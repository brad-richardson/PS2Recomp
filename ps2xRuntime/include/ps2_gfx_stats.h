// E33 DEV-ONLY per-vsync VU1/GIF/GS census behind PS2X_GFX_STATS=<file>.
//
// Master gate: PS2X_GFX_STATS names the text file receiving one line per
// guest vsync. Unset/empty (default) = every tap compiles to one relaxed
// atomic check; zero guest-visible behavior change, no I/O.
// Optional binds: PS2X_GFX_STATS_FROM / PS2X_GFX_STATS_TO (inclusive vsync
// window; default 0..2^64-1). Hard cap: 20,000 lines, then the file goes
// quiet (windows keep resetting so the steady state is unaffected).
//
// One line per guest vsync carries: MSCAL/MSCNT counts, VU1 cycles consumed,
// programs that spent their cycle budget without reaching an E-bit, max
// cycles of a single program, XGKICKs, GIF packets/bytes per PATH1/2/3, GS
// draw kicks per path with vertex count and screen-space xyz min/max, and
// the top 5 (TBP0, PRIM) tuples by draw count.
//
// Windows are cut at EeScheduler VBlankStart (noteVsync): the emitted line
// is labeled with the vsync the window belongs to. Events before the first
// observed VBlank open no window and are not counted. The trailing partial
// window at shutdown is not flushed.

#pragma once

#include "runtime/gs/ps2_gif_arbiter.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace ps2_gfx_stats
{

inline constexpr uint64_t kMaxLines = 20000ull;

namespace detail
{

    struct PathWindow
    {
        uint64_t packets = 0u;
        uint64_t bytes = 0u;
        uint64_t draws = 0u;
        uint64_t vertices = 0u;
        bool hasBox = false;
        float xMin = 0.0f;
        float xMax = 0.0f;
        float yMin = 0.0f;
        float yMax = 0.0f;
        double zMin = 0.0;
        double zMax = 0.0;
    };

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

        uint64_t mscal = 0u;
        uint64_t mscnt = 0u;
        uint64_t vuCycles = 0u;
        uint64_t vuExhausted = 0u;
        uint64_t vuMaxCycles = 0u;
        uint64_t xgkick = 0u;
        PathWindow paths[3];
        std::map<std::pair<uint32_t, uint32_t>, uint64_t> topPairs;
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
        const char *file = std::getenv("PS2X_GFX_STATS");
        if (!file || file[0] == '\0')
        {
            return;
        }
        s.path = file;
        uint64_t from = 0u;
        uint64_t to = ~0ull;
        if (const char *env = std::getenv("PS2X_GFX_STATS_FROM"))
        {
            if (!parseU64(env, from))
            {
                return;
            }
        }
        if (const char *env = std::getenv("PS2X_GFX_STATS_TO"))
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
            // Unwritable file: stay enabled (counters still prove the
            // mechanism) but never retry the open per vsync.
            s.capped = true;
        }
    }

    inline void formatBox(char *dst, size_t size, const PathWindow &w)
    {
        if (!w.hasBox)
        {
            std::snprintf(dst, size, "none");
            return;
        }
        std::snprintf(dst, size, "%.2f,%.2f,%.2f,%.2f,%.2f,%.2f",
                      static_cast<double>(w.xMin), static_cast<double>(w.xMax),
                      static_cast<double>(w.yMin), static_cast<double>(w.yMax),
                      w.zMin, w.zMax);
    }

    inline std::string formatLine(uint64_t tick, const State &s)
    {
        char b1[160], b2[160], b3[160];
        formatBox(b1, sizeof(b1), s.paths[0]);
        formatBox(b2, sizeof(b2), s.paths[1]);
        formatBox(b3, sizeof(b3), s.paths[2]);

        std::string top = "none";
        if (!s.topPairs.empty())
        {
            std::vector<std::pair<std::pair<uint32_t, uint32_t>, uint64_t>> sorted(
                s.topPairs.begin(), s.topPairs.end());
            std::sort(sorted.begin(), sorted.end(),
                      [](const auto &a, const auto &b)
                      {
                          if (a.second != b.second)
                          {
                              return a.second > b.second;
                          }
                          return a.first < b.first;
                      });
            top.clear();
            for (size_t i = 0; i < sorted.size() && i < 5u; ++i)
            {
                char entry[64];
                std::snprintf(entry, sizeof(entry), "%u:%u=%llu",
                              sorted[i].first.first, sorted[i].first.second,
                              static_cast<unsigned long long>(sorted[i].second));
                if (i != 0u)
                {
                    top += ',';
                }
                top += entry;
            }
        }

        char line[2048];
        std::snprintf(line, sizeof(line),
                      "vsync=%llu mscal=%llu mscnt=%llu vu_cycles=%llu vu_exhausted=%llu "
                      "vu_maxcyc=%llu xgkick=%llu "
                      "p1_pkt=%llu p1_bytes=%llu p2_pkt=%llu p2_bytes=%llu p3_pkt=%llu p3_bytes=%llu "
                      "d1_n=%llu d1_vert=%llu d1_box=%s "
                      "d2_n=%llu d2_vert=%llu d2_box=%s "
                      "d3_n=%llu d3_vert=%llu d3_box=%s "
                      "top=%s",
                      static_cast<unsigned long long>(tick),
                      static_cast<unsigned long long>(s.mscal),
                      static_cast<unsigned long long>(s.mscnt),
                      static_cast<unsigned long long>(s.vuCycles),
                      static_cast<unsigned long long>(s.vuExhausted),
                      static_cast<unsigned long long>(s.vuMaxCycles),
                      static_cast<unsigned long long>(s.xgkick),
                      static_cast<unsigned long long>(s.paths[0].packets),
                      static_cast<unsigned long long>(s.paths[0].bytes),
                      static_cast<unsigned long long>(s.paths[1].packets),
                      static_cast<unsigned long long>(s.paths[1].bytes),
                      static_cast<unsigned long long>(s.paths[2].packets),
                      static_cast<unsigned long long>(s.paths[2].bytes),
                      static_cast<unsigned long long>(s.paths[0].draws),
                      static_cast<unsigned long long>(s.paths[0].vertices), b1,
                      static_cast<unsigned long long>(s.paths[1].draws),
                      static_cast<unsigned long long>(s.paths[1].vertices), b2,
                      static_cast<unsigned long long>(s.paths[2].draws),
                      static_cast<unsigned long long>(s.paths[2].vertices), b3,
                      top.c_str());
        return std::string(line);
    }

    inline void resetWindowLocked(State &s)
    {
        s.mscal = 0u;
        s.mscnt = 0u;
        s.vuCycles = 0u;
        s.vuExhausted = 0u;
        s.vuMaxCycles = 0u;
        s.xgkick = 0u;
        s.paths[0] = PathWindow{};
        s.paths[1] = PathWindow{};
        s.paths[2] = PathWindow{};
        s.topPairs.clear();
    }

    inline size_t pathIndex(GifPathId path)
    {
        switch (path)
        {
        case GifPathId::Path1:
            return 0u;
        case GifPathId::Path2:
            return 1u;
        case GifPathId::Path3:
        default:
            return 2u;
        }
    }

} // namespace detail

// One relaxed check per call; no I/O when the flag is unset.
inline bool enabled()
{
    detail::ensureInit();
    return detail::enabledFlag().load(std::memory_order_relaxed);
}

inline void noteMscal()
{
    if (!enabled())
    {
        return;
    }
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    ++s.mscal;
}

inline void noteMscnt()
{
    if (!enabled())
    {
        return;
    }
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    ++s.mscnt;
}

inline void noteXgkick()
{
    if (!enabled())
    {
        return;
    }
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    ++s.xgkick;
}

inline void noteVuRun(uint64_t cyclesUsed, bool budgetExhausted)
{
    if (!enabled())
    {
        return;
    }
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.vuCycles += cyclesUsed;
    if (cyclesUsed > s.vuMaxCycles)
    {
        s.vuMaxCycles = cyclesUsed;
    }
    if (budgetExhausted)
    {
        ++s.vuExhausted;
    }
}

inline void noteGifPacket(GifPathId path, uint32_t bytes)
{
    if (!enabled())
    {
        return;
    }
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    detail::PathWindow &w = s.paths[detail::pathIndex(path)];
    ++w.packets;
    w.bytes += bytes;
}

inline void noteDraw(GifPathId path, uint32_t vertexCount,
                     float xMin, float xMax, float yMin, float yMax,
                     double zMin, double zMax, uint32_t tbp0, uint32_t prim)
{
    if (!enabled())
    {
        return;
    }
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    detail::PathWindow &w = s.paths[detail::pathIndex(path)];
    ++w.draws;
    w.vertices += vertexCount;
    if (!w.hasBox)
    {
        w.hasBox = true;
        w.xMin = xMin;
        w.xMax = xMax;
        w.yMin = yMin;
        w.yMax = yMax;
        w.zMin = zMin;
        w.zMax = zMax;
    }
    else
    {
        if (xMin < w.xMin)
        {
            w.xMin = xMin;
        }
        if (xMax > w.xMax)
        {
            w.xMax = xMax;
        }
        if (yMin < w.yMin)
        {
            w.yMin = yMin;
        }
        if (yMax > w.yMax)
        {
            w.yMax = yMax;
        }
        if (zMin < w.zMin)
        {
            w.zMin = zMin;
        }
        if (zMax > w.zMax)
        {
            w.zMax = zMax;
        }
    }
    ++s.topPairs[std::make_pair(tbp0, prim)];
}

// Cut the current window at guest vsync `tick`: emit the line for the
// window that just ended (when inside FROM..TO and under the line cap),
// then start a fresh window for `tick`.
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
    if (s.curTickValid)
    {
        const uint64_t window = s.curTick;
        if (window >= s.from && window <= s.to && !s.capped)
        {
            detail::openLocked(s);
            if (s.outOpen)
            {
                // Flush every line: one small write per vsync keeps the
                // file tail-able during long boots and readable in tests.
                s.out << detail::formatLine(window, s) << '\n' << std::flush;
                ++s.linesWritten;
                if (s.linesWritten >= kMaxLines)
                {
                    s.capped = true;
                    s.out.close();
                    s.outOpen = false;
                }
            }
        }
    }
    detail::resetWindowLocked(s);
    s.curTick = tick;
    s.curTickValid = true;
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
    detail::resetWindowLocked(s);
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
    detail::resetWindowLocked(s);
    s.initDone = true;
    detail::initDone().store(true, std::memory_order_relaxed);
    s.enabled = false;
    detail::enabledFlag().store(false, std::memory_order_relaxed);
}

} // namespace ps2_gfx_stats
