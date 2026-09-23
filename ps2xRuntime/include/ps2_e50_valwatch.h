// E50 DEV-ONLY value-match store watch behind PS2X_E50_VALWATCH=<file>.
//
// Logs every guest WRITE32/64/128 (the generated-code store macros) any of
// whose 32-bit lanes equals one of PS2X_E50_VALWATCH_VALUES (comma list of
// hex words, max 8) during vsyncs PS2X_E50_VALWATCH_FROM..TO (inclusive,
// default all). One line per matching lane:
//   valwatch vsync=<n> addr=0x<lane addr> value=0x<word> width=<bytes>
//     pc=0x<guest pc> ra=0x<> fn=<host func> a0..s7 (low 32 hex)
// Cap 512 lines. Unset (default) = one relaxed atomic check per store; the
// value compare is lock-free, the lock is taken only on a match. Inlined
// constant-address FAST_WRITE sequences bypass the macros (stated gap).

#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>

#include "ps2_runtime.h"

namespace ps2_e50_valwatch
{

inline constexpr int kMaxValues = 8;
inline constexpr uint64_t kMaxLines = 512ull;

namespace detail
{
    struct State
    {
        std::mutex mutex;
        std::string path;
        std::ofstream out;
        bool outOpen = false;
        uint64_t from = 0u;
        uint64_t to = ~0ull;
        uint64_t lines = 0u;
        bool capped = false;
    };

    inline State &state()
    {
        static State s;
        return s;
    }

    inline std::atomic<int> &count()
    {
        static std::atomic<int> n{-1}; // -1 = not initialised
        return n;
    }

    inline uint32_t *values()
    {
        static uint32_t v[kMaxValues] = {};
        return v;
    }

    inline bool parseU64(const char *text, uint64_t &out)
    {
        if (!text || !*text)
            return false;
        uint64_t v = 0u;
        for (const char *p = text; *p; ++p)
        {
            if (*p < '0' || *p > '9')
                return false;
            v = v * 10u + static_cast<uint64_t>(*p - '0');
        }
        out = v;
        return true;
    }

    // "3ee33810,0xbe73a2e6" -> values; returns the count (0 on any error).
    inline int parseValues(const char *text, uint32_t *out)
    {
        if (!text || !*text)
            return 0;
        int n = 0;
        const char *p = text;
        while (*p)
        {
            if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
                p += 2;
            uint32_t v = 0u;
            int digits = 0;
            for (;; ++p)
            {
                const char c = *p;
                uint32_t d;
                if (c >= '0' && c <= '9')
                    d = static_cast<uint32_t>(c - '0');
                else if (c >= 'a' && c <= 'f')
                    d = static_cast<uint32_t>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F')
                    d = static_cast<uint32_t>(c - 'A' + 10);
                else
                    break;
                v = (v << 4) | d;
                ++digits;
            }
            if (digits == 0 || digits > 8 || n == kMaxValues)
                return 0;
            out[n++] = v;
            if (*p == ',')
                ++p;
            else if (*p)
                return 0;
        }
        return n;
    }

    inline void init()
    {
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (count().load(std::memory_order_relaxed) >= 0)
            return;
        int n = 0;
        const char *file = std::getenv("PS2X_E50_VALWATCH");
        if (file && *file)
        {
            n = parseValues(std::getenv("PS2X_E50_VALWATCH_VALUES"), values());
            uint64_t v = 0u;
            if (parseU64(std::getenv("PS2X_E50_VALWATCH_FROM"), v))
                s.from = v;
            if (parseU64(std::getenv("PS2X_E50_VALWATCH_TO"), v))
                s.to = v;
            s.path = file;
        }
        count().store(n, std::memory_order_release);
    }

    inline void emitLocked(State &s, const char *line)
    {
        if (s.capped)
            return;
        if (!s.outOpen)
        {
            s.out.open(s.path, std::ios::out | std::ios::trunc);
            s.outOpen = s.out.is_open();
            if (!s.outOpen)
            {
                s.capped = true;
                return;
            }
        }
        s.out << line << '\n' << std::flush;
        if (++s.lines >= kMaxLines)
        {
            s.capped = true;
            s.out.close();
            s.outOpen = false;
            count().store(0, std::memory_order_relaxed);
        }
    }
} // namespace detail

// One relaxed check per store once initialised.
inline bool armed()
{
    int n = detail::count().load(std::memory_order_relaxed);
    if (n < 0)
    {
        detail::init();
        n = detail::count().load(std::memory_order_relaxed);
    }
    return n > 0;
}

// Lock-free lane compare; returns the bitmask of matching lanes.
inline uint32_t matchLanes(uint32_t width, uint64_t lo, uint64_t hi)
{
    const int n = detail::count().load(std::memory_order_relaxed);
    const uint32_t *v = detail::values();
    const uint32_t lanes = width >= 16u ? 4u : (width >= 8u ? 2u : 1u);
    uint32_t mask = 0u;
    for (uint32_t i = 0u; i < lanes; ++i)
    {
        const uint32_t w = static_cast<uint32_t>(i < 2u ? (lo >> (32u * i)) : (hi >> (32u * (i - 2u))));
        for (int k = 0; k < n; ++k)
        {
            if (w == v[k])
            {
                mask |= 1u << i;
                break;
            }
        }
    }
    return mask;
}

inline void noteStore(uint64_t vsync, uint32_t addr, uint32_t width, uint64_t lo, uint64_t hi,
                      uint32_t pc, uint32_t ra, const char *fn, const R5900Context *ctx)
{
    const uint32_t mask = matchLanes(width, lo, hi);
    if (mask == 0u)
        return;
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (vsync < s.from || vsync > s.to || s.capped)
        return;
    static const int kRegs[24] = {4, 5, 6, 7, 2, 3, 8, 9, 10, 11, 12, 13, 14, 15, 24, 25,
                                  16, 17, 18, 19, 20, 21, 22, 23};
    static const char *const kNames[24] = {"a0", "a1", "a2", "a3", "v0", "v1", "t0", "t1",
                                           "t2", "t3", "t4", "t5", "t6", "t7", "t8", "t9",
                                           "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7"};
    for (uint32_t i = 0u; i < 4u; ++i)
    {
        if ((mask & (1u << i)) == 0u)
            continue;
        const uint32_t w = static_cast<uint32_t>(i < 2u ? (lo >> (32u * i)) : (hi >> (32u * (i - 2u))));
        char line[768];
        int k = std::snprintf(line, sizeof(line),
                              "valwatch vsync=%llu addr=0x%08x value=0x%08x width=%u pc=0x%08x ra=0x%08x fn=%s",
                              static_cast<unsigned long long>(vsync), addr + 4u * i, w, width, pc, ra,
                              fn ? fn : "?");
        for (int r = 0; r < 24 && k > 0 && k < static_cast<int>(sizeof(line)); ++r)
        {
            const uint32_t rv = ctx ? getRegU32(ctx, kRegs[r]) : 0u;
            k += std::snprintf(line + k, sizeof(line) - static_cast<size_t>(k), " %s=0x%08x", kNames[r], rv);
        }
        detail::emitLocked(s, line);
    }
}

inline void noteStoreCtx(const PS2Runtime *runtime, const R5900Context *ctx, uint32_t addr, uint32_t width,
                         uint64_t lo, uint64_t hi, const char *fn)
{
    if (matchLanes(width, lo, hi) == 0u)
        return;
    const uint64_t vsync = runtime ? runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed) : 0u;
    const uint32_t pc = ctx ? ctx->pc : 0u;
    const uint32_t ra = ctx ? getRegU32(ctx, 31) : 0u;
    noteStore(vsync, addr, width, lo, hi, pc, ra, fn, ctx);
}

// Test hooks.
inline bool configureForTest(const char *path, const char *valuesCsv, uint64_t from, uint64_t to)
{
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.outOpen)
    {
        s.out.close();
        s.outOpen = false;
    }
    const int n = detail::parseValues(valuesCsv, detail::values());
    s.path = path ? path : "";
    s.from = from;
    s.to = to;
    s.lines = 0u;
    s.capped = false;
    detail::count().store(n, std::memory_order_release);
    return n > 0;
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
    s.lines = 0u;
    s.capped = false;
    detail::count().store(0, std::memory_order_release);
}

} // namespace ps2_e50_valwatch
