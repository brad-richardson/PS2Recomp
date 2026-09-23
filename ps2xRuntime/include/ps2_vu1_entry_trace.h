// E37 DEV-ONLY VU1 entry trace behind PS2X_VU1_ENTRY_TRACE=<file>.
//
// Master gate: PS2X_VU1_ENTRY_TRACE names the text file receiving one
// block per listed startPC. Optional binds:
//   PS2X_VU1_ENTRY_TRACE_PCS=0x40,0x10 (byte PCs, 0x-hex or decimal,
//     comma-separated, max 16)
//   PS2X_VU1_ENTRY_TRACE_VSYNC=<n> (default 0)
// For the FIRST MSCAL at each listed startPC at or after vsync n, the
// block holds:
//   1. `vumem`: the full 16 KB VU1 data memory at MSCAL entry, one line
//      per qword: `vumem <row-dec> <w0> <w1> <w2> <w3>` (8-digit hex).
//   2. `vif`: every VIF1 command since the previous MSCAL/MSCALF/MSCNT
//      boundary (the packet that fed this program), caller-formatted
//      `vif ...` lines (cap 4096 per block).
//   3. `pair`: caller-formatted `pair ...` lines for every executed pair
//      (cap 16384 per block; the VU side stops after the first arrival
//      at 0x418 plus 3 loop iterations).
// Unset/empty (default) = one relaxed atomic check per tap; zero
// guest-visible behavior change, no I/O.
//
// Correlation model (single-threaded, synchronous): the VIF1 interpreter
// appends `vif` lines as it decodes and calls noteMscalEntry at each
// MSCAL/MSCALF/MSCNT (after appending that command's own line); the VU1
// interpreter's execute() picks up the arm via takeArm and streams pair
// lines via appendPair, closing the block with finishEntry. Windows are
// cut at EeScheduler VBlankStart (noteVsync), mirroring ps2_gfx_stats.

#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace ps2_vu1_entry_trace
{

inline constexpr uint32_t kMaxPcs = 16u;
inline constexpr uint32_t kMaxVifLive = 8192u;
inline constexpr uint32_t kMaxVifPerBlock = 4096u;
inline constexpr uint32_t kMaxPairsPerBlock = 16384u;
inline constexpr uint64_t kMaxLines = 100000ull;
inline constexpr uint32_t kVuMemWords = 4096u; // 16 KB / 4
inline constexpr uint32_t kLoopHeadPc = 0x418u;

struct State
{
    std::mutex mutex;
    bool initDone = false;
    bool enabled = false;
    std::string path;
    std::vector<uint32_t> pcs;
    uint64_t vsyncGate = 0u;
    uint64_t curTick = 0u;
    bool curTickValid = false;
    std::vector<bool> captured; // per pcs index
    std::vector<std::string> vifLive;
    uint32_t vifLiveDropped = 0u;
    // Frozen at the triggering MSCAL; consumed by finishEntry.
    bool frozenValid = false;
    uint32_t frozenIdx = 0u;
    uint32_t frozenPc = 0u;
    uint64_t frozenVsync = 0u;
    std::vector<std::string> frozenVif;
    uint32_t frozenVifDropped = 0u;
    std::vector<uint32_t> frozenVmem; // kVuMemWords, empty when unsnapshotted
    // Pair-stream arm, picked up by the next execute() for the same PC.
    bool pendingValid = false;
    uint32_t pendingPc = 0u;
    uint32_t pendingIdx = 0u;
    std::ofstream out;
    bool outOpen = false;
    bool truncatedOnce = false;
    uint64_t linesWritten = 0u;
    bool capped = false;
};

namespace detail
{

    inline State &state()
    {
        static State s;
        return s;
    }

    inline std::atomic<bool> &initDone()
    {
        static std::atomic<bool> done{false};
        return done;
    }

    inline std::atomic<bool> &enabledFlag()
    {
        static std::atomic<bool> on{false};
        return on;
    }

    inline bool parseU64(const char *text, uint64_t &out)
    {
        if (!text || text[0] == '\0')
        {
            return false;
        }
        uint64_t value = 0u;
        for (const char *p = text; *p != '\0'; ++p)
        {
            if (*p < '0' || *p > '9')
            {
                return false;
            }
            value = value * 10u + static_cast<uint64_t>(*p - '0');
        }
        out = value;
        return true;
    }

    // One "0x40" / "64" item; leading/trailing spaces tolerated.
    inline bool parsePcItem(const char *begin, const char *end, uint32_t &out)
    {
        while (begin < end && (*begin == ' ' || *begin == '\t'))
        {
            ++begin;
        }
        while (end > begin && (end[-1] == ' ' || end[-1] == '\t'))
        {
            --end;
        }
        if (begin >= end)
        {
            return false;
        }
        uint32_t base = 10u;
        if (end - begin > 2 && begin[0] == '0' && (begin[1] == 'x' || begin[1] == 'X'))
        {
            base = 16u;
            begin += 2;
        }
        uint32_t value = 0u;
        for (const char *p = begin; p < end; ++p)
        {
            uint32_t digit = 0u;
            if (*p >= '0' && *p <= '9')
            {
                digit = static_cast<uint32_t>(*p - '0');
            }
            else if (base == 16u && *p >= 'a' && *p <= 'f')
            {
                digit = static_cast<uint32_t>(*p - 'a') + 10u;
            }
            else if (base == 16u && *p >= 'A' && *p <= 'F')
            {
                digit = static_cast<uint32_t>(*p - 'A') + 10u;
            }
            else
            {
                return false;
            }
            if (digit >= base)
            {
                return false;
            }
            value = value * base + digit;
        }
        out = value;
        return true;
    }

    inline void initLocked(State &s)
    {
        s.initDone = true;
        const char *file = std::getenv("PS2X_VU1_ENTRY_TRACE");
        if (!file || file[0] == '\0')
        {
            return;
        }
        s.path = file;
        if (const char *pcs = std::getenv("PS2X_VU1_ENTRY_TRACE_PCS"))
        {
            const char *itemBegin = pcs;
            for (const char *p = pcs;; ++p)
            {
                if (*p == ',' || *p == '\0')
                {
                    uint32_t pc = 0u;
                    if (parsePcItem(itemBegin, p, pc) && s.pcs.size() < kMaxPcs)
                    {
                        s.pcs.push_back(pc);
                    }
                    itemBegin = p + 1;
                    if (*p == '\0')
                    {
                        break;
                    }
                }
            }
        }
        if (const char *vsync = std::getenv("PS2X_VU1_ENTRY_TRACE_VSYNC"))
        {
            uint64_t gate = 0u;
            if (parseU64(vsync, gate))
            {
                s.vsyncGate = gate;
            }
        }
        if (s.pcs.empty())
        {
            return; // No targets: stay off (nothing could ever arm).
        }
        s.captured.assign(s.pcs.size(), false);
        s.enabled = true;
        enabledFlag().store(true, std::memory_order_relaxed);
    }

    inline void ensureInit()
    {
        if (initDone().load(std::memory_order_relaxed))
        {
            return;
        }
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.initDone)
        {
            initDone().store(true, std::memory_order_relaxed);
            return;
        }
        initLocked(s);
        initDone().store(true, std::memory_order_relaxed);
    }

    inline void openLocked(State &s, bool truncate)
    {
        if (s.outOpen || s.capped || s.path.empty())
        {
            return;
        }
        s.out.open(s.path, std::ios::out | (truncate ? std::ios::trunc : std::ios::app));
        s.outOpen = s.out.is_open();
        if (!s.outOpen)
        {
            s.capped = true;
        }
    }

    inline void writeLineLocked(State &s, const std::string &line)
    {
        if (s.capped)
        {
            return;
        }
        if (!s.outOpen)
        {
            openLocked(s, !s.truncatedOnce);
            s.truncatedOnce = true;
            if (!s.outOpen)
            {
                return;
            }
        }
        s.out << line << '\n';
        ++s.linesWritten;
        if (s.linesWritten >= kMaxLines)
        {
            s.capped = true;
            s.out.close();
            s.outOpen = false;
        }
    }

} // namespace detail

// One relaxed check per call; no I/O when the flag is unset.
inline bool enabled()
{
    detail::ensureInit();
    return detail::enabledFlag().load(std::memory_order_relaxed);
}

inline void noteVsync(uint64_t tick)
{
    if (!enabled())
    {
        return;
    }
    State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.curTickValid && tick == s.curTick)
    {
        return;
    }
    s.curTick = tick;
    s.curTickValid = true;
}

// A caller-formatted `vif ...` line for the in-progress packet.
inline void appendVif(const std::string &line)
{
    if (!enabled())
    {
        return;
    }
    State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.vifLive.size() < kMaxVifLive)
    {
        s.vifLive.push_back(line);
    }
    else
    {
        ++s.vifLiveDropped;
    }
}

// Called by the VIF1 interpreter at each MSCAL/MSCALF/MSCNT, after it has
// appended that command's own `vif` line. Snapshots VU1 data memory and
// freezes the packet log when this is the first in-window MSCAL for a
// listed startPC; always resets the live log and arms the pair stream
// for a freshly frozen target. Not for MSCNT (no startPC of its own).
inline void noteMscalEntry(uint32_t startPC, bool isMscnt,
                           const uint8_t *vuData, uint32_t dataSize)
{
    if (!enabled())
    {
        return;
    }
    State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!isMscnt && s.curTickValid && s.curTick >= s.vsyncGate)
    {
        for (size_t i = 0u; i < s.pcs.size(); ++i)
        {
            if (s.pcs[i] == startPC && !s.captured[i] && !s.frozenValid)
            {
                s.captured[i] = true;
                s.frozenValid = true;
                s.frozenIdx = static_cast<uint32_t>(i);
                s.frozenPc = startPC;
                s.frozenVsync = s.curTick;
                s.frozenVif = s.vifLive;
                s.frozenVifDropped = s.vifLiveDropped;
                if (s.frozenVif.size() > kMaxVifPerBlock)
                {
                    s.frozenVifDropped += static_cast<uint32_t>(s.frozenVif.size() - kMaxVifPerBlock);
                    s.frozenVif.resize(kMaxVifPerBlock);
                }
                s.frozenVmem.assign(kVuMemWords, 0u);
                if (vuData != nullptr && dataSize >= kVuMemWords * 4u)
                {
                    std::memcpy(s.frozenVmem.data(), vuData, kVuMemWords * 4u);
                }
                s.pendingValid = true;
                s.pendingPc = startPC;
                s.pendingIdx = static_cast<uint32_t>(i);
                break;
            }
        }
    }
    s.vifLive.clear();
    s.vifLiveDropped = 0u;
}

// Called by VU1Interpreter::execute (VU1 only). True once, for the
// execute() synchronously following a freezing MSCAL.
inline bool takeArm(uint32_t startPC)
{
    if (!enabled())
    {
        return false;
    }
    State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.pendingValid || s.pendingPc != startPC)
    {
        return false;
    }
    s.pendingValid = false;
    return true;
}

inline uint32_t armIndex(uint32_t startPC)
{
    State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    for (size_t i = 0u; i < s.pcs.size(); ++i)
    {
        if (s.pcs[i] == startPC)
        {
            return static_cast<uint32_t>(i);
        }
    }
    return 0u;
}

// Called by the VU1 interpreter when its pair stream for a frozen target
// ends (stop condition, program end, or budget end). Writes the whole
// block: header, vumem, vif, pairs. No-op unless this index froze one.
inline void finishEntry(uint32_t idx, const std::vector<std::string> &pairs,
                        uint32_t pairCount, uint32_t arrivals)
{
    if (!enabled())
    {
        return;
    }
    State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.frozenValid || s.frozenIdx != idx)
    {
        return;
    }
    s.frozenValid = false;
    char head[128];
    std::snprintf(head, sizeof(head), "entry startPC=0x%x vsync=%llu",
                  s.frozenPc, static_cast<unsigned long long>(s.frozenVsync));
    detail::writeLineLocked(s, std::string(head));
    char row[64];
    for (uint32_t r = 0u; r < kVuMemWords / 4u; ++r)
    {
        std::snprintf(row, sizeof(row), "vumem %u %08x %08x %08x %08x", r,
                      s.frozenVmem[r * 4u], s.frozenVmem[r * 4u + 1u],
                      s.frozenVmem[r * 4u + 2u], s.frozenVmem[r * 4u + 3u]);
        detail::writeLineLocked(s, std::string(row));
    }
    for (const std::string &line : s.frozenVif)
    {
        detail::writeLineLocked(s, line);
    }
    if (s.frozenVifDropped != 0u)
    {
        char drop[64];
        std::snprintf(drop, sizeof(drop), "vif +dropped=%u", s.frozenVifDropped);
        detail::writeLineLocked(s, std::string(drop));
    }
    size_t kept = pairs.size();
    if (kept > kMaxPairsPerBlock)
    {
        kept = kMaxPairsPerBlock;
    }
    for (size_t i = 0u; i < kept; ++i)
    {
        detail::writeLineLocked(s, pairs[i]);
    }
    char tail[128];
    std::snprintf(tail, sizeof(tail), "endentry startPC=0x%x pairs=%u arrivals=%u%s",
                  s.frozenPc, pairCount, arrivals,
                  pairs.size() > kMaxPairsPerBlock ? " capped=1" : "");
    detail::writeLineLocked(s, std::string(tail));
    if (s.outOpen)
    {
        s.out << std::flush;
    }
}

// Test hooks. Production code paths never call these.
inline bool parsePcsForTest(const char *text, std::vector<uint32_t> &out)
{
    if (!text)
    {
        return false;
    }
    out.clear();
    const char *itemBegin = text;
    for (const char *p = text;; ++p)
    {
        if (*p == ',' || *p == '\0')
        {
            uint32_t pc = 0u;
            if (!detail::parsePcItem(itemBegin, p, pc))
            {
                return false;
            }
            out.push_back(pc);
            itemBegin = p + 1;
            if (*p == '\0')
            {
                break;
            }
        }
    }
    return true;
}

inline bool configureForTest(const char *path, const std::vector<uint32_t> &pcs,
                             uint64_t vsyncGate = 0u)
{
    if (!path || path[0] == '\0' || pcs.empty() || pcs.size() > kMaxPcs)
    {
        return false;
    }
    State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.outOpen)
    {
        s.out.close();
        s.outOpen = false;
    }
    s.path = path;
    s.pcs = pcs;
    s.vsyncGate = vsyncGate;
    s.curTickValid = false;
    s.curTick = 0u;
    s.captured.assign(pcs.size(), false);
    s.vifLive.clear();
    s.vifLiveDropped = 0u;
    s.frozenValid = false;
    s.frozenVif.clear();
    s.frozenVmem.clear();
    s.pendingValid = false;
    s.truncatedOnce = false;
    s.linesWritten = 0u;
    s.capped = false;
    s.initDone = true;
    detail::initDone().store(true, std::memory_order_relaxed);
    s.enabled = true;
    detail::enabledFlag().store(true, std::memory_order_relaxed);
    return true;
}

inline void clearForTest()
{
    State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.outOpen)
    {
        s.out.close();
        s.outOpen = false;
    }
    s.path.clear();
    s.pcs.clear();
    s.vsyncGate = 0u;
    s.curTickValid = false;
    s.curTick = 0u;
    s.captured.clear();
    s.vifLive.clear();
    s.vifLiveDropped = 0u;
    s.frozenValid = false;
    s.frozenVif.clear();
    s.frozenVifDropped = 0u;
    s.frozenVmem.clear();
    s.pendingValid = false;
    s.truncatedOnce = false;
    s.linesWritten = 0u;
    s.capped = false;
    s.initDone = true;
    detail::initDone().store(true, std::memory_order_relaxed);
    s.enabled = false;
    detail::enabledFlag().store(false, std::memory_order_relaxed);
}

} // namespace ps2_vu1_entry_trace
