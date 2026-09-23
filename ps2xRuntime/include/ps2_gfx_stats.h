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
// E50 additions (appended to the line only when the window saw them, so
// pre-E50 lines are byte-identical):
//   dN_scr=<on>,<off>,<straddle>  per-path draws classified against the
//     drawing context's SCISSOR after subtracting XYOFFSET (noteDrawScreen;
//     `off` = the vertex bbox misses the scissor rect entirely, `on` = the
//     bbox lies inside it, `straddle` = the rest).
//   dN_t65=<verts>,<on>,<off>,<straddle>,<zero>,<adc>  T65's exact
//     definitions (PCSX2 T65-S2/S3): every kicked vertex, then each
//     completed prim: ADC prims counted and skipped, the rest classified
//     against [SCAX0, SCAX1+1] x [SCAY0, SCAY1+1] with inclusive edges;
//     zero = x0 == x1 or y0 == y1 (a subset of on/off/straddle).
//   pcs=<startPC>:<mscal>:<on>/<off>/<straddle>;...  per VU1 startPC
//     (byte PC, hex): MSCAL count and the PATH1 draws attributed to the
//     most recent MSCAL's program (noteMscalPc), ascending PC, max 64.
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
        uint64_t scr[3] = {0u, 0u, 0u}; // E50: on, off, straddle
        bool hasScr = false;
        // E50 T65-format: verts, on, off, straddle, zero-area, adc.
        uint64_t t65[6] = {0u, 0u, 0u, 0u, 0u, 0u};
        bool hasT65 = false;
    };

    struct PcWindow
    {
        uint64_t mscal = 0u;
        uint64_t scr[3] = {0u, 0u, 0u};
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
        // E50: per-startPC census; curPc persists across vsyncs (a program
        // started before the cut keeps kicking after it).
        std::map<uint32_t, PcWindow> pcs;
        uint32_t curPc = 0u;
        bool curPcValid = false;
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

        std::string e50;
        static const char *const kScrNames[3] = {"d1_scr", "d2_scr", "d3_scr"};
        for (size_t i = 0u; i < 3u; ++i)
        {
            if (!s.paths[i].hasScr)
            {
                continue;
            }
            char entry[96];
            std::snprintf(entry, sizeof(entry), " %s=%llu,%llu,%llu", kScrNames[i],
                          static_cast<unsigned long long>(s.paths[i].scr[0]),
                          static_cast<unsigned long long>(s.paths[i].scr[1]),
                          static_cast<unsigned long long>(s.paths[i].scr[2]));
            e50 += entry;
        }
        static const char *const kT65Names[3] = {"d1_t65", "d2_t65", "d3_t65"};
        for (size_t i = 0u; i < 3u; ++i)
        {
            if (!s.paths[i].hasT65)
            {
                continue;
            }
            const uint64_t *c = s.paths[i].t65;
            char entry[160];
            std::snprintf(entry, sizeof(entry), " %s=%llu,%llu,%llu,%llu,%llu,%llu", kT65Names[i],
                          static_cast<unsigned long long>(c[0]), static_cast<unsigned long long>(c[1]),
                          static_cast<unsigned long long>(c[2]), static_cast<unsigned long long>(c[3]),
                          static_cast<unsigned long long>(c[4]), static_cast<unsigned long long>(c[5]));
            e50 += entry;
        }
        if (!s.pcs.empty())
        {
            e50 += " pcs=";
            size_t n = 0u;
            for (const auto &pc : s.pcs)
            {
                if (n == 64u)
                {
                    e50 += ";+more";
                    break;
                }
                char entry[96];
                std::snprintf(entry, sizeof(entry), "%s0x%x:%llu:%llu/%llu/%llu",
                              n == 0u ? "" : ";", pc.first,
                              static_cast<unsigned long long>(pc.second.mscal),
                              static_cast<unsigned long long>(pc.second.scr[0]),
                              static_cast<unsigned long long>(pc.second.scr[1]),
                              static_cast<unsigned long long>(pc.second.scr[2]));
                e50 += entry;
                ++n;
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
        return std::string(line) + e50;
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
        s.pcs.clear();
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

// E50: screen class of one draw. Coordinates are GS primitive space in
// pixels (XYZ/16); ofx/ofy are raw XYOFFSET (12.4); the scissor is the
// inclusive pixel rect. 0 = on (bbox inside), 1 = off (bbox misses the
// rect), 2 = straddle.
inline constexpr uint32_t kScrOn = 0u;
inline constexpr uint32_t kScrOff = 1u;
inline constexpr uint32_t kScrStraddle = 2u;

inline uint32_t classifyScreen(float xMin, float xMax, float yMin, float yMax,
                               uint16_t ofx, uint16_t ofy,
                               uint16_t sx0, uint16_t sx1, uint16_t sy0, uint16_t sy1)
{
    const float ox = static_cast<float>(ofx) / 16.0f;
    const float oy = static_cast<float>(ofy) / 16.0f;
    const float x0 = xMin - ox;
    const float x1 = xMax - ox;
    const float y0 = yMin - oy;
    const float y1 = yMax - oy;
    // Pixels cover [s0, s1 + 1).
    const float left = static_cast<float>(sx0);
    const float right = static_cast<float>(sx1) + 1.0f;
    const float top = static_cast<float>(sy0);
    const float bottom = static_cast<float>(sy1) + 1.0f;
    if (x1 < left || x0 >= right || y1 < top || y0 >= bottom)
    {
        return kScrOff;
    }
    if (x0 >= left && x1 < right && y0 >= top && y1 < bottom)
    {
        return kScrOn;
    }
    return kScrStraddle;
}

// E50: T65's class (PCSX2 T65-S3): inclusive edges on [s0, s1 + 1].
// Returns kScrOn/kScrOff/kScrStraddle; zeroArea = x0 == x1 || y0 == y1.
inline uint32_t classifyT65(float xMin, float xMax, float yMin, float yMax,
                            uint16_t ofx, uint16_t ofy,
                            uint16_t sx0, uint16_t sx1, uint16_t sy0, uint16_t sy1,
                            bool &zeroArea)
{
    const float ox = static_cast<float>(ofx) / 16.0f;
    const float oy = static_cast<float>(ofy) / 16.0f;
    const float x0 = xMin - ox;
    const float x1 = xMax - ox;
    const float y0 = yMin - oy;
    const float y1 = yMax - oy;
    const float left = static_cast<float>(sx0);
    const float right = static_cast<float>(sx1) + 1.0f;
    const float top = static_cast<float>(sy0);
    const float bottom = static_cast<float>(sy1) + 1.0f;
    zeroArea = (xMin == xMax) || (yMin == yMax);
    if (x1 < left || x0 > right || y1 < top || y0 > bottom)
    {
        return 1u;
    }
    if (x0 >= left && x1 <= right && y0 >= top && y1 <= bottom)
    {
        return 0u;
    }
    return 2u;
}

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

// E50: the VU1 startPC of a VIF MSCAL/MSCALF (byte PC). Counts the
// MSCAL for that PC and attributes later PATH1 draws to it.
inline void noteMscalPc(uint32_t startPC)
{
    if (!enabled())
    {
        return;
    }
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.curPc = startPC;
    s.curPcValid = true;
    if (s.curTickValid)
    {
        ++s.pcs[startPC].mscal;
    }
}

// E51: latest MSCAL startPC (~0u when none seen or stats are off).
inline uint32_t currentPc()
{
    if (!enabled())
    {
        return ~0u;
    }
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.curPcValid ? s.curPc : ~0u;
}

// E50: screen class (classifyScreen) of the draw just counted by noteDraw.
inline void noteDrawScreen(GifPathId path, uint32_t cls)
{
    if (!enabled() || cls > 2u)
    {
        return;
    }
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    const size_t idx = detail::pathIndex(path);
    detail::PathWindow &w = s.paths[idx];
    w.hasScr = true;
    ++w.scr[cls];
    if (idx == 0u && s.curPcValid && s.curTickValid)
    {
        ++s.pcs[s.curPc].scr[cls];
    }
}

// E50 T65-format taps: one kicked vertex; one completed prim (adc = the
// kick did not draw; otherwise cls/zeroArea from classifyT65).
inline void noteT65Vertex(GifPathId path)
{
    if (!enabled())
    {
        return;
    }
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    detail::PathWindow &w = s.paths[detail::pathIndex(path)];
    w.hasT65 = true;
    ++w.t65[0];
}

inline void noteT65Prim(GifPathId path, bool adc, uint32_t cls, bool zeroArea)
{
    if (!enabled() || cls > 2u)
    {
        return;
    }
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    detail::PathWindow &w = s.paths[detail::pathIndex(path)];
    w.hasT65 = true;
    if (adc)
    {
        ++w.t65[5];
        return;
    }
    ++w.t65[1 + cls];
    if (zeroArea)
    {
        ++w.t65[4];
    }
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
    s.curPcValid = false;
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
    s.curPcValid = false;
    detail::resetWindowLocked(s);
    s.initDone = true;
    detail::initDone().store(true, std::memory_order_relaxed);
    s.enabled = false;
    detail::enabledFlag().store(false, std::memory_order_relaxed);
}

} // namespace ps2_gfx_stats
