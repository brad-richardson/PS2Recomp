#pragma once

// GB2 Part 3: submitted-packet + guest-CSR-read log for divergence
// bisection (header-only, ps2_e4.h pattern). Armed by PS2X_PKLOG=1 only.
//
// Packets (every submitted packet, arbiter-input order):
//   [pk] idx=<n> tick=<t> fnv=<hex32> len=<bytes> src=<1|2|3|img|packed>
// CSR/SIGLBLID guest loads (every read COUNTED; a line is emitted only on
// (pc,value) change, collapsing spin loops — idx gaps are spin lengths):
//   [csr] idx=<read ordinal> tick=<t> value=<hex> pc=<hex> addr=<hex>
// Both streams cap at 2M lines with a TRUNCATED marker (boot-log safety).
//
// Also carries the Part 3 candidate-fix flag: PS2X_GS_CSR_DRAIN=1 (dev,
// default off). When on AND the GS queue is on, a guest CSR/SIGLBLID load
// drains the queue with a Fence first (GB1 §2c completion-visibility).
//
// Part 4: (a) all lines go to ./ps2_pklog.txt (CWD; the boot wrapper gives
// each boot its own cwd) under one mutex — concurrent stderr writers tore
// lines in Part 3; (b) PS2X_PKCAP=<idx,...> dumps full bytes for listed
// packet idx as [pkbytes] with the source-memory classification
// (base=rdram+0xPHYS | spad+0xOFF | other) so a differing word maps back
// to the VIF1 DMA source address for the PS2X_DIAG_WATCH one-word watch.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

namespace ps2_pk
{
    inline bool enabled()
    {
        static const bool on = [] {
            const char *env = std::getenv("PS2X_PKLOG");
            return env && std::strcmp(env, "1") == 0;
        }();
        return on;
    }

    inline bool csrDrainEnabled()
    {
        static const bool on = [] {
            const char *env = std::getenv("PS2X_GS_CSR_DRAIN");
            return env && std::strcmp(env, "1") == 0;
        }();
        return on;
    }

    inline uint32_t fnv1a32(const uint8_t *data, size_t size, uint32_t seed = 2166136261u)
    {
        uint32_t hash = seed;
        for (size_t i = 0; i < size; ++i)
        {
            hash ^= data[i];
            hash *= 16777619u;
        }
        return hash;
    }

    inline std::atomic<uint64_t> &pkIndex()
    {
        static std::atomic<uint64_t> idx{0};
        return idx;
    }

    inline std::atomic<uint64_t> &csrIndex()
    {
        static std::atomic<uint64_t> idx{0};
        return idx;
    }

    inline std::atomic<uint64_t> &lineCount()
    {
        static std::atomic<uint64_t> n{0};
        return n;
    }

    inline std::mutex &logMutex()
    {
        static std::mutex m;
        return m;
    }

    // Dedicated file (./ps2_pklog.txt, opened once under logMutex). Falls
    // back to stderr if the open fails. Caller must hold logMutex().
    inline std::ostream &logStream()
    {
        static std::ofstream f;
        static bool opened = false;
        if (!opened)
        {
            opened = true;
            f.open("ps2_pklog.txt", std::ios::out | std::ios::trunc);
            if (f.is_open())
                std::cerr << "[pklog] writing to ./ps2_pklog.txt" << std::endl;
            else
                std::cerr << "[pklog] WARN: cannot open ./ps2_pklog.txt, using stderr" << std::endl;
        }
        if (f.is_open())
            return f;
        return std::cerr;
    }

    constexpr uint64_t kLineCap = 2000000ull;

    // Returns false once the cap is hit (caller stops logging).
    inline bool checkCap()
    {
        const uint64_t n = lineCount().fetch_add(1u, std::memory_order_relaxed);
        if (n == kLineCap)
        {
            std::lock_guard<std::mutex> lock(logMutex());
            logStream() << "[pklog] TRUNCATED at " << kLineCap << " lines" << std::endl;
            return false;
        }
        return n < kLineCap;
    }

    // PS2X_PKCAP=<idx,...>: packet indexes whose full bytes are dumped.
    inline const std::vector<uint64_t> &captureSet()
    {
        static const std::vector<uint64_t> set = [] {
            std::vector<uint64_t> out;
            if (const char *env = std::getenv("PS2X_PKCAP"))
            {
                std::string s(env);
                size_t pos = 0;
                while (pos <= s.size() && out.size() < 64u)
                {
                    const size_t comma = s.find(',', pos);
                    const std::string tok =
                        s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                    char *end = nullptr;
                    const unsigned long long v = std::strtoull(tok.c_str(), &end, 0);
                    if (end != tok.c_str() && *end == '\0')
                        out.push_back(static_cast<uint64_t>(v));
                    if (comma == std::string::npos)
                        break;
                    pos = comma + 1;
                }
            }
            return out;
        }();
        return set;
    }

    inline bool captureWanted(uint64_t idx)
    {
        if (!enabled())
            return false;
        for (const uint64_t v : captureSet())
        {
            if (v == idx)
                return true;
        }
        return false;
    }

    // Source-memory bases, set from each submit site (game thread only).
    struct Bases
    {
        const uint8_t *rdram = nullptr;
        uint32_t rdramSize = 0;
        const uint8_t *spad = nullptr;
        uint32_t spadSize = 0;
    };

    inline Bases &bases()
    {
        static Bases b;
        return b;
    }

    inline void setBases(const uint8_t *rdram, uint32_t rdramSize, const uint8_t *spad,
                         uint32_t spadSize)
    {
        if (!enabled())
            return;
        Bases &b = bases();
        b.rdram = rdram;
        b.rdramSize = rdramSize;
        b.spad = spad;
        b.spadSize = spadSize;
    }

    inline void emitHex(std::ostream &os, const uint8_t *data, uint32_t sizeBytes)
    {
        static const char *hexd = "0123456789abcdef";
        uint32_t n = sizeBytes;
        constexpr uint32_t kCap = 4096u;
        if (n > kCap)
            n = kCap;
        for (uint32_t i = 0; i < n; ++i)
        {
            os.put(hexd[data[i] >> 4]);
            os.put(hexd[data[i] & 0xFu]);
        }
        if (sizeBytes > kCap)
            os << "...(+" << (sizeBytes - kCap) << ")";
    }

    // Caller must hold logMutex().
    inline void emitCapture(uint64_t idx, uint64_t tick, const char *src, const uint8_t *data,
                            uint32_t sizeBytes)
    {
        std::ostream &os = logStream();
        os << "[pkbytes] idx=" << idx << " tick=" << tick << " len=" << sizeBytes << " src=" << src
           << " base=";
        const Bases &b = bases();
        if (data && b.rdram && data >= b.rdram && data < b.rdram + b.rdramSize)
            os << "rdram+0x" << std::hex << static_cast<uint64_t>(data - b.rdram) << std::dec;
        else if (data && b.spad && data >= b.spad && data < b.spad + b.spadSize)
            os << "spad+0x" << std::hex << static_cast<uint64_t>(data - b.spad) << std::dec;
        else
            os << "other";
        os << " data=";
        if (data && sizeBytes != 0u)
            emitHex(os, data, sizeBytes);
        else
            os << "(null)";
        os << std::endl;
    }

    inline void noteSubmit(const char *src, const uint8_t *data, uint32_t sizeBytes, uint64_t tick)
    {
        if (!enabled())
        {
            return;
        }
        const uint64_t idx = pkIndex().fetch_add(1u, std::memory_order_relaxed);
        if (!checkCap())
        {
            return;
        }
        const uint32_t fnv = (data && sizeBytes != 0u) ? fnv1a32(data, sizeBytes) : 0u;
        const bool cap = captureWanted(idx);
        std::lock_guard<std::mutex> lock(logMutex());
        logStream() << "[pk] idx=" << idx << " tick=" << tick << " fnv=" << std::hex << fnv << std::dec
                    << " len=" << sizeBytes << " src=" << src << std::endl;
        if (cap)
            emitCapture(idx, tick, src, data, sizeBytes);
    }

    // Native image upload (P6): hashes setup regs + image bytes (same image
    // to a different target must hash differently).
    inline void noteSubmitNative(const uint64_t (&setupRegs)[4], const uint8_t *data, uint32_t sizeBytes,
                                 uint64_t tick)
    {
        if (!enabled())
        {
            return;
        }
        const uint64_t idx = pkIndex().fetch_add(1u, std::memory_order_relaxed);
        if (!checkCap())
        {
            return;
        }
        uint32_t fnv = fnv1a32(reinterpret_cast<const uint8_t *>(setupRegs), sizeof(setupRegs));
        if (data && sizeBytes != 0u)
        {
            fnv = fnv1a32(data, sizeBytes, fnv);
        }
        const bool cap = captureWanted(idx);
        std::lock_guard<std::mutex> lock(logMutex());
        logStream() << "[pk] idx=" << idx << " tick=" << tick << " fnv=" << std::hex << fnv << std::dec
                    << " len=" << sizeBytes << " src=img" << std::endl;
        if (cap)
            emitCapture(idx, tick, "img", data, sizeBytes);
    }

    inline void noteCsrRead(uint64_t tick, uint64_t value, uint32_t pc, uint32_t addr)
    {
        if (!enabled())
        {
            return;
        }
        const uint64_t idx = csrIndex().fetch_add(1u, std::memory_order_relaxed);
        // Collapse consecutive identical (pc,addr,value) spin reads; the idx
        // gap between emitted lines is the spin length. Guarded: Loads run
        // on the game thread, but be safe.
        static uint64_t lastValue = 0;
        static uint32_t lastPc = 0;
        static uint32_t lastAddr = 0;
        static bool haveLast = false;
        {
            std::lock_guard<std::mutex> lock(logMutex());
            if (haveLast && value == lastValue && pc == lastPc && addr == lastAddr)
            {
                return;
            }
            lastValue = value;
            lastPc = pc;
            lastAddr = addr;
            haveLast = true;
        }
        if (!checkCap())
        {
            return;
        }
        std::lock_guard<std::mutex> lock(logMutex());
        logStream() << "[csr] idx=" << idx << " tick=" << tick << " value=" << std::hex << value
                    << " pc=" << pc << " addr=" << addr << std::dec << std::endl;
    }
} // namespace ps2_pk
