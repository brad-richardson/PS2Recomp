// E36 DEV-ONLY per-program VU1 trace behind PS2X_VU1_TRACE=<file>.
//
// Master gate: PS2X_VU1_TRACE names the text file receiving one short
// `census` line per budget-exhausted VU1 program plus one multi-line
// `detail` block for the first 40 distinct startPCs that exhaust (one
// block per startPC; repeats of an already-detailed startPC are census
// only). Unset/empty (default) = every tap compiles to one relaxed
// atomic check; zero guest-visible behavior change, no I/O.
// Optional binds: PS2X_VU1_TRACE_FROM / PS2X_VU1_TRACE_TO (inclusive
// guest-vsync window; default 0..2^64-1). Hard cap: 100,000 lines, then
// the file goes quiet (windows keep resetting so the steady state is
// unaffected).
//
// Correlation model (single-threaded, synchronous): the VIF1 interpreter
// calls noteMscal at each MSCAL/MSCALF/MSCNT, stashing the VIF1 register
// snapshot; the VU1 interpreter's execute()/resume() consumes it via
// consumeContext. execute() also keys detailed-trace arming/dedupe on
// its startPC (resume() keeps the program's startPC).
//
// Windows are cut at EeScheduler VBlankStart (noteVsync), mirroring
// ps2_gfx_stats. Events before the first observed VBlank open no window.

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

namespace ps2_vu1_trace
{

inline constexpr uint64_t kMaxLines = 100000ull;
inline constexpr uint32_t kMaxDetailPrograms = 40u;

struct MscalContext
{
    bool isMscnt = false;
    uint32_t startPC = 0u; // MSCAL imm*8; undefined for MSCNT
    uint32_t top = 0u;
    uint32_t itop = 0u;
    uint32_t base = 0u;
    uint32_t ofst = 0u;
    uint32_t tops = 0u; // pre-update: the state this MSCAL acted on
    uint32_t itops = 0u;
    bool dbf = false; // pre-update
    uint64_t vsync = 0u;
};

namespace detail
{

    struct State
    {
        std::mutex mutex;
        bool initDone = false;
        bool enabled = false;
        std::string path;
        uint64_t from = 0u;
        uint64_t to = ~0ull;
        std::ofstream out;
        bool outOpen = false;
        uint64_t linesWritten = 0u;
        bool capped = false;
        uint64_t curTick = 0u;
        bool curTickValid = false;
        bool pendingValid = false;
        MscalContext pending{};
        std::vector<uint32_t> detailedPCs;
    };

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

    inline void initLocked(State &s)
    {
        s.initDone = true;
        const char *file = std::getenv("PS2X_VU1_TRACE");
        if (!file || file[0] == '\0')
        {
            return;
        }
        s.path = file;
        uint64_t from = 0u;
        uint64_t to = ~0ull;
        if (const char *env = std::getenv("PS2X_VU1_TRACE_FROM"))
        {
            if (!parseU64(env, from))
            {
                return;
            }
        }
        if (const char *env = std::getenv("PS2X_VU1_TRACE_TO"))
        {
            if (!parseU64(env, to))
            {
                return;
            }
        }
        s.from = from;
        s.to = to;
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

    inline void openLocked(State &s)
    {
        if (s.outOpen || s.capped || s.path.empty())
        {
            return;
        }
        s.out.open(s.path, std::ios::out | std::ios::trunc);
        s.outOpen = s.out.is_open();
        if (!s.outOpen)
        {
            // Unwritable file: stay enabled (arming still proves the
            // mechanism) but never retry the open per program.
            s.capped = true;
        }
    }

    inline bool inWindowLocked(const State &s)
    {
        return s.curTickValid && s.curTick >= s.from && s.curTick <= s.to;
    }

    // Caller holds s.mutex. Emits one line; enforces the line cap.
    inline void writeLineLocked(State &s, const std::string &line)
    {
        if (s.capped)
        {
            return;
        }
        openLocked(s);
        if (!s.outOpen)
        {
            return;
        }
        s.out << line << '\n' << std::flush;
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
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.curTickValid && tick == s.curTick)
    {
        return;
    }
    s.curTick = tick;
    s.curTickValid = true;
    s.pendingValid = false;
}

inline void noteMscal(bool isMscnt, uint32_t startPC,
                      uint32_t top, uint32_t itop,
                      uint32_t base, uint32_t ofst, uint32_t tops, uint32_t itops,
                      bool dbf)
{
    if (!enabled())
    {
        return;
    }
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.pending.isMscnt = isMscnt;
    s.pending.startPC = startPC;
    s.pending.top = top & 0x3FFu;
    s.pending.itop = itop & 0x3FFu;
    s.pending.base = base & 0x3FFu;
    s.pending.ofst = ofst & 0x3FFu;
    s.pending.tops = tops & 0x3FFu;
    s.pending.itops = itops & 0x3FFu;
    s.pending.dbf = dbf;
    s.pending.vsync = s.curTickValid ? s.curTick : 0u;
    s.pendingValid = true;
}

// Consumed by VU1Interpreter::execute/resume. False = no MSCAL context
// pending (stale window, VU0 unit, or direct callers).
inline bool consumeContext(MscalContext &out)
{
    if (!enabled())
    {
        return false;
    }
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.pendingValid)
    {
        return false;
    }
    out = s.pending;
    s.pendingValid = false;
    return true;
}

// Should this program record a PC histogram? True only inside the vsync
// window, for startPCs without a detail block yet, while fewer than 40
// distinct startPCs have one.
inline bool armFor(uint32_t startPC)
{
    if (!enabled())
    {
        return false;
    }
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!detail::inWindowLocked(s))
    {
        return false;
    }
    if (s.detailedPCs.size() >= kMaxDetailPrograms)
    {
        return false;
    }
    for (uint32_t pc : s.detailedPCs)
    {
        if (pc == startPC)
        {
            return false;
        }
    }
    return true;
}

// One census line per budget-exhausted program inside the window.
inline void noteCensus(uint32_t startPC, uint64_t cyclesUsed, uint32_t xgkicks)
{
    if (!enabled())
    {
        return;
    }
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!detail::inWindowLocked(s))
    {
        return;
    }
    char line[160];
    std::snprintf(line, sizeof(line),
                  "census vsync=%llu startPC=0x%x cycles=%llu xgkick=%u",
                  static_cast<unsigned long long>(s.curTick),
                  startPC,
                  static_cast<unsigned long long>(cyclesUsed),
                  xgkicks);
    detail::writeLineLocked(s, std::string(line));
}

// A pre-formatted multi-line detail block (may contain '\n'). Written
// once per distinct startPC; further calls for the same startPC, calls
// past the 40-program cap, and calls outside the window are dropped.
// Returns true when the block was written.
inline bool emitDetail(uint32_t startPC, const std::string &block)
{
    if (!enabled())
    {
        return false;
    }
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!detail::inWindowLocked(s))
    {
        return false;
    }
    for (uint32_t pc : s.detailedPCs)
    {
        if (pc == startPC)
        {
            return false;
        }
    }
    if (s.detailedPCs.size() >= kMaxDetailPrograms)
    {
        return false;
    }
    // Count embedded newlines toward the line cap.
    uint64_t lines = 1u;
    for (char c : block)
    {
        if (c == '\n')
        {
            ++lines;
        }
    }
    if (s.linesWritten + lines > kMaxLines)
    {
        s.capped = true;
        if (s.outOpen)
        {
            s.out.close();
            s.outOpen = false;
        }
        return false;
    }
    openLocked(s);
    if (!s.outOpen)
    {
        return false;
    }
    s.out << block;
    if (block.empty() || block.back() != '\n')
    {
        s.out << '\n';
    }
    s.out << std::flush;
    s.linesWritten += lines;
    s.detailedPCs.push_back(startPC);
    return true;
}

// Test hooks. Production code paths never call these.
inline bool configureForTest(const char *path, uint64_t from = 0u, uint64_t to = ~0ull)
{
    if (!path || path[0] == '\0' || from > to)
    {
        return false;
    }
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.outOpen)
    {
        s.out.close();
        s.outOpen = false;
    }
    s.path = path;
    s.from = from;
    s.to = to;
    s.linesWritten = 0u;
    s.capped = false;
    s.curTickValid = false;
    s.curTick = 0u;
    s.pendingValid = false;
    s.detailedPCs.clear();
    s.initDone = true;
    detail::initDone().store(true, std::memory_order_relaxed);
    s.enabled = true;
    detail::enabledFlag().store(true, std::memory_order_relaxed);
    return true;
}

inline void clearForTest()
{
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.outOpen)
    {
        s.out.close();
        s.outOpen = false;
    }
    s.path.clear();
    s.from = 0u;
    s.to = ~0ull;
    s.linesWritten = 0u;
    s.capped = false;
    s.curTickValid = false;
    s.curTick = 0u;
    s.pendingValid = false;
    s.detailedPCs.clear();
    s.initDone = true;
    detail::initDone().store(true, std::memory_order_relaxed);
    s.enabled = false;
    detail::enabledFlag().store(false, std::memory_order_relaxed);
}

} // namespace ps2_vu1_trace
