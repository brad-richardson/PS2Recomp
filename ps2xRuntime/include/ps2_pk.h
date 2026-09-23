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

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>

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

    constexpr uint64_t kLineCap = 2000000ull;

    // Returns false once the cap is hit (caller stops logging).
    inline bool checkCap()
    {
        const uint64_t n = lineCount().fetch_add(1u, std::memory_order_relaxed);
        if (n == kLineCap)
        {
            std::cerr << "[pklog] TRUNCATED at " << kLineCap << " lines" << std::endl;
            return false;
        }
        return n < kLineCap;
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
        std::lock_guard<std::mutex> lock(logMutex());
        std::cerr << "[pk] idx=" << idx << " tick=" << tick << " fnv=" << std::hex << fnv << std::dec
                  << " len=" << sizeBytes << " src=" << src << std::endl;
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
        std::lock_guard<std::mutex> lock(logMutex());
        std::cerr << "[pk] idx=" << idx << " tick=" << tick << " fnv=" << std::hex << fnv << std::dec
                  << " len=" << sizeBytes << " src=img" << std::endl;
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
        std::cerr << "[csr] idx=" << idx << " tick=" << tick << " value=" << std::hex << value
                  << " pc=" << pc << " addr=" << addr << std::dec << std::endl;
    }
} // namespace ps2_pk
