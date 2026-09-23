// E43 DEV-ONLY draw-record census + mode-6 producer watch + h394 hash-call
// log behind PS2X_E43_TRACE=<file>.
//
// Master gate: PS2X_E43_TRACE names the text file receiving every line
// kind below. Unset/empty (default) = one relaxed atomic check per tap;
// zero guest-visible behavior change, no I/O.
// Optional binds: PS2X_E43_TRACE_FROM / PS2X_E43_TRACE_TO (inclusive
// guest-vsync window for the census + producer watch; default 0..2^64-1,
// vsync = the shared E41 VBlank mirror via noteVsync()),
// PS2X_E43_H394_FROM / PS2X_E43_H394_TO (window for the h394 hash-call
// log; default 1270..1400 — wide on purpose: run-to-run tick reach
// varies, e42a died at ~1300; the analyzer buckets settled vsyncs),
// PS2X_E43_PROD_MODE (0-15 or "any"; default 6).
// Hard caps: 40000 lines total, then quiet; dprod 4096; h394 1024;
// first-8-records-per-(vsync,mode) rule bounds drecs; learned producer
// pages stop at 64 (matching continues).
//
// Line kinds:
//   drec vsync=<t> src=0x<dispatch source pc> mode=<m| X> count=<n>
//     One per (vsync, mode) with >0 walked records, flushed when the tick
//     advances (out-of-window calls still advance the flush so the window
//     edge is not lost). mode is a1&0xF; X = a1>15 (should not happen).
//   drecs vsync=<t> src=0x<> mode=<m> addr=0x<record s3> wmode=<m2>
//     w0=0x<> w1=0x<> w2=0x<> w3=0x<>
//     First 8 records per (vsync, mode). w0..w3 are the 16 bytes at s3
//     (wmode = (w0>>6)&0xF, self-check against mode); `-` when unreadable.
//   dprod vsync=<t> addr=0x<store addr> value=0x<lane> mode=<bits>
//     pc=0x<guest pc|-> ra=0x<guest ra|-> fn=<host sym> src=macro|fast
//     raw=0x<unfolded addr>
//     Guest stores whose 32-bit lane has bits 6-9 == PROD_MODE ("any" =
//     every lane) into a learned producer page (4 KB page of a
//     walker-source record base, folded). Macro path carries guest
//     pc/ra + __func__; inlined fast path carries dladdr fn only; a
//     thread-local guard keeps macro-path stores to one line.
//   h394 vsync=<t> s1=0x<record ptr> w0=.. w1=.. w2=.. w3=.. hash=0x<NN>
//     ret=0x<v0 on return> stores=<0/1>
//     One per sub_00362DE8@0x362F68 -> func_394ED0 dispatch in-window:
//     the 4 source words, the delay-slot-masked hash (a2), v0 after the
//     synchronous return, and whether any WRITE* macro ran with guest pc
//     in [0x394FDC,0x394FEC) during the call (the SDL/SDR/SW sequence).

#pragma once

#include "runtime/ps2_memory.h" // PS2_RAM_SIZE / PS2_RAM_MASK
#include "ps2_runtime.h" // R5900Context / getRegU32
#include "ps2_e41_trace.h" // lastVsyncTick (shared VBlank mirror)

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>

namespace ps2_e43_trace
{

inline constexpr uint64_t kMaxLines = 40000ull;
inline constexpr uint64_t kFlushEvery = 128ull;
inline constexpr uint64_t kMaxDprodLines = 4096ull;
inline constexpr uint64_t kMaxH394Lines = 1024ull;
inline constexpr int kDrecsPerMode = 8;
inline constexpr int kMaxProdPages = 64;
inline constexpr uint32_t kWalkerSource = 0x00363CF4u;
inline constexpr uint32_t kWalkerTarget = 0x00364CD0u;
inline constexpr uint32_t kH394Source = 0x00362F68u;
inline constexpr uint32_t kH394Target = 0x00394ED0u;
inline constexpr uint32_t kH394StoreLo = 0x00394FDCu;
inline constexpr uint32_t kH394StoreHi = 0x00394FECu; // exclusive
inline constexpr uint32_t kInvalidPage = 0xFFFFFFFFu;

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
        uint64_t h394From = 1270u;
        uint64_t h394To = 1400u;
        uint32_t prodMode = 6u; // 16 = any
        std::ofstream out;
        bool outOpen = false;
        bool capped = false;
        uint64_t linesWritten = 0u;
        uint64_t dprodLines = 0u;
        uint64_t h394Lines = 0u;
        // drec aggregation for the in-progress tick.
        bool tickValid = false;
        uint64_t curTick = 0u;
        uint64_t counts[17] = {0u};
        uint64_t recsEmitted[17] = {0u};
        // h394 in-flight call (EE is single-threaded; depth guards nesting).
        bool h394Busy = false;
        int h394Nested = 0;
        uint64_t h394Tick = 0u;
        uint32_t h394S1 = 0u;
        uint32_t h394Words[4] = {0u, 0u, 0u, 0u};
        bool h394WordsOk = false;
        uint32_t h394Hash = 0u;
        bool h394StoresFired = false;
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

    // Learned producer pages: lock-free (atomic slots, 0xFFFFFFFF = free).
    // Written only by the rare walker tap; read per store. A torn/duplicated
    // entry at worst misses or double-counts one page; matching is exact.
    inline std::atomic<uint32_t> &prodPageSlot(int i)
    {
        static std::atomic<uint32_t> pages[kMaxProdPages];
        return pages[i];
    }

    inline std::atomic<uint32_t> &prodPageCount()
    {
        static std::atomic<uint32_t> n{0u};
        return n;
    }

    // Suppress guard: WRITE-macro producer stores reach Ps2FastWrite* too.
    inline bool &prodSuppressed()
    {
        thread_local bool suppressed = false;
        return suppressed;
    }

    struct ScopedProdSuppress
    {
        bool active = false;
        explicit ScopedProdSuppress(bool on) : active(on)
        {
            if (on)
            {
                prodSuppressed() = true;
            }
        }
        ~ScopedProdSuppress()
        {
            if (active)
            {
                prodSuppressed() = false;
            }
        }
    };

    // H394 store watch: set around the synchronous func_394ED0 call.
    inline bool &h394Watch()
    {
        thread_local bool armed = false;
        return armed;
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
        const char *file = std::getenv("PS2X_E43_TRACE");
        if (!file || file[0] == '\0')
        {
            return;
        }
        s.path = file;
        uint64_t from = 0u, to = ~0ull, hfrom = 1270u, hto = 1400u;
        if (const char *env = std::getenv("PS2X_E43_TRACE_FROM"))
        {
            if (!parseU64(env, from))
            {
                return;
            }
        }
        if (const char *env = std::getenv("PS2X_E43_TRACE_TO"))
        {
            if (!parseU64(env, to))
            {
                return;
            }
        }
        if (const char *env = std::getenv("PS2X_E43_H394_FROM"))
        {
            if (!parseU64(env, hfrom))
            {
                return;
            }
        }
        if (const char *env = std::getenv("PS2X_E43_H394_TO"))
        {
            if (!parseU64(env, hto))
            {
                return;
            }
        }
        uint32_t prodMode = 6u;
        if (const char *env = std::getenv("PS2X_E43_PROD_MODE"))
        {
            if (std::strcmp(env, "any") == 0)
            {
                prodMode = 16u;
            }
            else
            {
                uint64_t m = 0u;
                if (!parseU64(env, m) || m > 15u)
                {
                    return;
                }
                prodMode = static_cast<uint32_t>(m);
            }
        }
        s.from = from;
        s.to = to;
        s.h394From = hfrom;
        s.h394To = hto;
        s.prodMode = prodMode;
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

    inline void emitLocked(State &s, const char *line)
    {
        if (!s.enabled || s.capped)
        {
            return;
        }
        if (s.linesWritten >= kMaxLines)
        {
            s.capped = true;
            return;
        }
        if (!s.outOpen)
        {
            s.out.open(s.path, std::ios::out | std::ios::trunc);
            if (!s.out.is_open())
            {
                s.enabled = false;
                enabledFlag().store(false, std::memory_order_relaxed);
                return;
            }
            s.outOpen = true;
        }
        s.out << line << '\n';
        ++s.linesWritten;
        if ((s.linesWritten % kFlushEvery) == 0u)
        {
            s.out.flush();
        }
    }

    // Emits pending drec counts for s.curTick (call with s.mutex held).
    // Caller decides window relevance; out-of-window ticks emit nothing
    // but still reset the aggregation.
    inline void flushDrecLocked(State &s)
    {
        if (!s.tickValid)
        {
            return;
        }
        const bool inWindow = s.enabled && !s.capped && s.curTick >= s.from && s.curTick <= s.to;
        if (inWindow)
        {
            for (int m = 0; m < 17; ++m)
            {
                if (s.counts[m] == 0u)
                {
                    continue;
                }
                char line[128];
                if (m < 16)
                {
                    std::snprintf(line, sizeof(line), "drec vsync=%llu src=0x%08x mode=%d count=%llu",
                                  static_cast<unsigned long long>(s.curTick),
                                  kWalkerSource,
                                  m, static_cast<unsigned long long>(s.counts[m]));
                }
                else
                {
                    std::snprintf(line, sizeof(line), "drec vsync=%llu src=0x%08x mode=X count=%llu",
                                  static_cast<unsigned long long>(s.curTick),
                                  kWalkerSource,
                                  static_cast<unsigned long long>(s.counts[m]));
                }
                emitLocked(s, line);
            }
        }
        s.tickValid = false;
        for (int m = 0; m < 17; ++m)
        {
            s.counts[m] = 0u;
            s.recsEmitted[m] = 0u;
        }
    }

} // namespace detail

// Hot-path gate: one relaxed atomic load when disabled.
inline bool enabled()
{
    detail::ensureInit();
    return detail::enabledFlag().load(std::memory_order_relaxed);
}

// Lock-free producer-page match (folded 4 KB page). No state, no locks.
inline bool isProdPage(uint32_t addr)
{
    const uint32_t page = (addr & 0x0FFFFFFFu) >> 12;
    const uint32_t n = detail::prodPageCount().load(std::memory_order_relaxed);
    const uint32_t lim = n < kMaxProdPages ? n : static_cast<uint32_t>(kMaxProdPages);
    for (uint32_t i = 0u; i < lim; ++i)
    {
        if (detail::prodPageSlot(static_cast<int>(i)).load(std::memory_order_relaxed) == page)
        {
            return true;
        }
    }
    return false;
}

// Learns the folded 4 KB page of a walker-source record base. Lock-free;
// duplicates may waste slots (bounded by the 64-page cap).
inline void learnProdPage(uint32_t addr)
{
    const uint32_t page = (addr & 0x0FFFFFFFu) >> 12;
    uint32_t n = detail::prodPageCount().load(std::memory_order_relaxed);
    for (uint32_t i = 0u; i < n && i < static_cast<uint32_t>(kMaxProdPages); ++i)
    {
        if (detail::prodPageSlot(static_cast<int>(i)).load(std::memory_order_relaxed) == page)
        {
            return;
        }
    }
    if (n >= static_cast<uint32_t>(kMaxProdPages))
    {
        return;
    }
    // Claim a fresh slot; a lost race only duplicates the page.
    if (!detail::prodPageCount().compare_exchange_strong(n, n + 1u, std::memory_order_relaxed))
    {
        return;
    }
    detail::prodPageSlot(static_cast<int>(n)).store(page, std::memory_order_relaxed);
}

// Reads 4 little-endian words at folded addr; false when out of RAM range.
inline bool readRecWords(const uint8_t *rdram, uint32_t addr, uint32_t out[4])
{
    if (rdram == nullptr)
    {
        return false;
    }
    const uint32_t folded = addr & 0x0FFFFFFFu;
    const uint32_t off = folded & PS2_RAM_MASK;
    if (off + 16u > PS2_RAM_SIZE)
    {
        return false;
    }
    for (int i = 0; i < 4; ++i)
    {
        uint32_t w = 0u;
        std::memcpy(&w, rdram + off + static_cast<uint32_t>(i) * 4u, sizeof(w));
        out[i] = w;
    }
    return true;
}

// Walker census tap: call from dispatchGuestBranch when
// targetPc == 0x364CD0 (any source; the src field separates the walker
// @0x363CF4 from sub_00364360 @0x364460). Reads call args off the shared
// guest register file (a1 = mode, s3 = record ptr).
inline void noteWalkerCall(const uint8_t *rdram, const R5900Context *ctx, uint32_t sourcePc)
{
    if (!enabled())
    {
        return;
    }
    const uint64_t tick = ps2_e41_trace::lastVsyncTick();
    const uint32_t a1 = (ctx != nullptr) ? getRegU32(ctx, 5) : 0u;
    const uint32_t s3 = (ctx != nullptr) ? getRegU32(ctx, 19) : 0u;
    const int bucket = (a1 <= 15u) ? static_cast<int>(a1) : 16;
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.enabled || s.capped)
    {
        return;
    }
    if (!s.tickValid || tick != s.curTick)
    {
        detail::flushDrecLocked(s);
        s.tickValid = true;
        s.curTick = tick;
    }
    ++s.counts[bucket];
    if (sourcePc == kWalkerSource)
    {
        learnProdPage(s3);
    }
    if (tick < s.from || tick > s.to)
    {
        return;
    }
    if (s.recsEmitted[bucket] >= static_cast<uint64_t>(kDrecsPerMode))
    {
        return;
    }
    ++s.recsEmitted[bucket];
    uint32_t w[4] = {0u, 0u, 0u, 0u};
    const bool ok = readRecWords(rdram, s3, w);
    char modeBuf[8];
    if (bucket == 16)
    {
        std::snprintf(modeBuf, sizeof(modeBuf), "X");
    }
    else
    {
        std::snprintf(modeBuf, sizeof(modeBuf), "%d", bucket);
    }
    char line[256];
    if (ok)
    {
        std::snprintf(line, sizeof(line),
                      "drecs vsync=%llu src=0x%08x mode=%s addr=0x%08x wmode=%u "
                      "w0=0x%08x w1=0x%08x w2=0x%08x w3=0x%08x",
                      static_cast<unsigned long long>(tick), sourcePc, modeBuf,
                      s3, (w[0] >> 6) & 0xFu,
                      w[0], w[1], w[2], w[3]);
    }
    else
    {
        std::snprintf(line, sizeof(line),
                      "drecs vsync=%llu src=0x%08x mode=%s addr=0x%08x wmode=- "
                      "w0=- w1=- w2=- w3=-",
                      static_cast<unsigned long long>(tick), sourcePc, modeBuf, s3);
    }
    detail::emitLocked(s, line);
}

// Shared producer-lane emitter. Lanes = fully-contained 32-bit words of
// [addr, addr+size); sub-word stores read the containing word back.
// prodMode 16 = any mode. Returns true when a line was emitted.
inline bool emitProdLanes(uint64_t tick, uint32_t addr, uint32_t size,
                          const uint8_t *rdram, uint32_t prodMode,
                          uint32_t pc, uint32_t ra, const char *fn, const char *src)
{
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.enabled || s.capped)
    {
        return false;
    }
    if (tick < s.from || tick > s.to)
    {
        return false;
    }
    if (s.dprodLines >= kMaxDprodLines)
    {
        return false;
    }
    if (rdram == nullptr)
    {
        return false;
    }
    const uint32_t folded = addr & 0x0FFFFFFFu;
    // Candidate lane bases: every 4-aligned word overlapped by the store
    // (sub-word stores test their containing word).
    const uint32_t first = folded & ~3u;
    const uint32_t last = (folded + size - 1u) & ~3u;
    bool emitted = false;
    for (uint32_t base = first; base <= last; base += 4u)
    {
        const uint32_t off = base & PS2_RAM_MASK;
        if (off + 4u > PS2_RAM_SIZE)
        {
            continue;
        }
        uint32_t w = 0u;
        std::memcpy(&w, rdram + off, sizeof(w));
        const uint32_t mode = (w >> 6) & 0xFu;
        if (prodMode != 16u && mode != prodMode)
        {
            continue;
        }
        ++s.dprodLines;
        char line[256];
        std::snprintf(line, sizeof(line),
                      "dprod vsync=%llu addr=0x%08x value=0x%08x mode=%u pc=0x%08x ra=0x%08x fn=%s src=%s raw=0x%08x",
                      static_cast<unsigned long long>(tick),
                      base, w, mode, pc, ra,
                      fn != nullptr ? fn : "-",
                      src != nullptr ? src : "-",
                      addr);
        detail::emitLocked(s, line);
        emitted = true;
        if (s.capped)
        {
            return true;
        }
    }
    return emitted;
}

// WRITE-macro path (has guest ctx): producer watch + h394 store flag.
// Call AFTER the store is visible in rdram. precomputedProd comes from
// the macro's isProdPage pre-check (which also holds the suppress guard).
inline void noteWriteCtx(const R5900Context *ctx,
                         const uint8_t *rdram, uint32_t addr, uint32_t size,
                         const char *fn, bool precomputedProd)
{
    if (detail::h394Watch())
    {
        const uint32_t pc = (ctx != nullptr) ? ctx->pc : 0u;
        detail::State &s = detail::state();
        std::lock_guard<std::mutex> lock(s.mutex);
        if (pc >= kH394StoreLo && pc < kH394StoreHi)
        {
            s.h394StoresFired = true;
        }
    }
    if (!precomputedProd)
    {
        return;
    }
    const uint32_t pc = (ctx != nullptr) ? ctx->pc : 0u;
    const uint32_t ra = (ctx != nullptr) ? getRegU32(ctx, 31) : 0u;
    uint32_t prodMode = 6u;
    {
        detail::State &s = detail::state();
        std::lock_guard<std::mutex> lock(s.mutex);
        prodMode = s.prodMode;
    }
    emitProdLanes(ps2_e41_trace::lastVsyncTick(), addr, size, rdram, prodMode, pc, ra, fn, "macro");
}

// Inlined fast-write path (no guest ctx): dladdr host fn, pc/ra unknown.
// Silent while a WRITE-macro tap holds the suppress guard.
__attribute__((noinline)) inline void noteProdSite(const uint8_t *rdram,
                                                   uint32_t addr, uint32_t size)
{
    if (!enabled() || detail::prodSuppressed())
    {
        return;
    }
    if (!isProdPage(addr))
    {
        return;
    }
    void *ra0 = __builtin_return_address(0);
    char fn0[256];
    ps2_e41_trace::resolveHostSym(ra0, fn0, sizeof(fn0));
    uint32_t prodMode = 6u;
    {
        detail::State &s = detail::state();
        std::lock_guard<std::mutex> lock(s.mutex);
        prodMode = s.prodMode;
    }
    emitProdLanes(ps2_e41_trace::lastVsyncTick(), addr, size, rdram, prodMode,
                  0u, 0u, fn0, "fast");
}

// H394 arm check for the dispatch site (cheap compares first, atomics last).
inline bool h394CallArmed(uint32_t sourcePc, uint32_t targetPc)
{
    if (sourcePc != kH394Source || targetPc != kH394Target)
    {
        return false;
    }
    if (!enabled())
    {
        return false;
    }
    const uint64_t tick = ps2_e41_trace::lastVsyncTick();
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.enabled && !s.capped && tick >= s.h394From && tick <= s.h394To &&
           s.h394Lines < kMaxH394Lines;
}

// H394 pre-call capture (args still intact; clobbered by the call).
inline void h394Pre(const uint8_t *rdram, const R5900Context *ctx)
{
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.enabled || s.capped)
    {
        return;
    }
    if (s.h394Busy)
    {
        ++s.h394Nested;
        return;
    }
    s.h394Busy = true;
    s.h394Tick = ps2_e41_trace::lastVsyncTick();
    s.h394S1 = (ctx != nullptr) ? getRegU32(ctx, 17) : 0u;
    s.h394Hash = (ctx != nullptr) ? (getRegU32(ctx, 6) & 0xFFu) : 0u;
    s.h394WordsOk = readRecWords(rdram, s.h394S1, s.h394Words);
    if (!s.h394WordsOk)
    {
        s.h394Words[0] = s.h394Words[1] = s.h394Words[2] = s.h394Words[3] = 0u;
    }
    s.h394StoresFired = false;
    detail::h394Watch() = true;
}

// H394 post-call: v0 is the return value (synchronous dispatch).
inline void h394Post(const R5900Context *ctx)
{
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.enabled)
    {
        detail::h394Watch() = false;
        return;
    }
    if (s.h394Nested > 0)
    {
        --s.h394Nested;
        return;
    }
    if (!s.h394Busy)
    {
        detail::h394Watch() = false;
        return;
    }
    s.h394Busy = false;
    detail::h394Watch() = false;
    if (s.capped || s.h394Lines >= kMaxH394Lines)
    {
        return;
    }
    if (s.h394Tick < s.h394From || s.h394Tick > s.h394To)
    {
        return;
    }
    const uint32_t ret = (ctx != nullptr) ? getRegU32(ctx, 2) : 0u;
    ++s.h394Lines;
    char line[256];
    if (s.h394WordsOk)
    {
        std::snprintf(line, sizeof(line),
                      "h394 vsync=%llu s1=0x%08x w0=0x%08x w1=0x%08x w2=0x%08x w3=0x%08x "
                      "hash=0x%02x ret=0x%08x stores=%d",
                      static_cast<unsigned long long>(s.h394Tick),
                      s.h394S1, s.h394Words[0], s.h394Words[1],
                      s.h394Words[2], s.h394Words[3],
                      s.h394Hash, ret, s.h394StoresFired ? 1 : 0);
    }
    else
    {
        std::snprintf(line, sizeof(line),
                      "h394 vsync=%llu s1=0x%08x w0=- w1=- w2=- w3=- "
                      "hash=0x%02x ret=0x%08x stores=%d",
                      static_cast<unsigned long long>(s.h394Tick),
                      s.h394S1, s.h394Hash, ret, s.h394StoresFired ? 1 : 0);
    }
    detail::emitLocked(s, line);
}

// Test hooks (mirror ps2_e41_trace.h conventions).
inline void configureForTest(const char *path, uint64_t from, uint64_t to)
{
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.initDone = true;
    detail::initDone().store(true, std::memory_order_relaxed);
    s.path = path != nullptr ? path : "";
    s.from = from;
    s.to = to;
    s.h394From = from;
    s.h394To = to;
    s.prodMode = 6u;
    s.enabled = !s.path.empty();
    detail::enabledFlag().store(s.enabled, std::memory_order_relaxed);
    s.outOpen = false;
    s.capped = false;
    s.linesWritten = 0u;
    s.dprodLines = 0u;
    s.h394Lines = 0u;
    s.tickValid = false;
    s.curTick = 0u;
    for (int m = 0; m < 17; ++m)
    {
        s.counts[m] = 0u;
        s.recsEmitted[m] = 0u;
    }
    s.h394Busy = false;
    s.h394Nested = 0;
    s.h394StoresFired = false;
    detail::prodPageCount().store(0u, std::memory_order_relaxed);
    for (int i = 0; i < kMaxProdPages; ++i)
    {
        detail::prodPageSlot(i).store(kInvalidPage, std::memory_order_relaxed);
    }
    detail::h394Watch() = false;
    detail::prodSuppressed() = false;
}

inline void clearForTest()
{
    detail::State &s = detail::state();
    std::lock_guard<std::mutex> lock(s.mutex);
    detail::flushDrecLocked(s);
    if (s.outOpen)
    {
        s.out.flush();
        s.out.close();
        s.outOpen = false;
    }
    s.initDone = false;
    detail::initDone().store(false, std::memory_order_relaxed);
    s.enabled = false;
    detail::enabledFlag().store(false, std::memory_order_relaxed);
    s.path.clear();
    s.from = 0u;
    s.to = ~0ull;
    s.h394From = 1270u;
    s.h394To = 1400u;
    s.prodMode = 6u;
    s.capped = false;
    s.linesWritten = 0u;
    s.dprodLines = 0u;
    s.h394Lines = 0u;
    s.tickValid = false;
    s.curTick = 0u;
    for (int m = 0; m < 17; ++m)
    {
        s.counts[m] = 0u;
        s.recsEmitted[m] = 0u;
    }
    s.h394Busy = false;
    s.h394Nested = 0;
    s.h394StoresFired = false;
    detail::prodPageCount().store(0u, std::memory_order_relaxed);
    for (int i = 0; i < kMaxProdPages; ++i)
    {
        detail::prodPageSlot(i).store(kInvalidPage, std::memory_order_relaxed);
    }
    detail::h394Watch() = false;
    detail::prodSuppressed() = false;
}

} // namespace ps2_e43_trace
