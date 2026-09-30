// TS2 stock-H extraction adapter. The guest call remains unchanged; host-only
// context makes the physical pass an explicit, resumable boundary for G1.
#pragma once

#include "ps2_runtime.h"
#include "ps2_ts2_splitsites.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

// EE1P2: this header is the split120 product path and stays compiled in
// release builds. Only the TS3 case census is diagnostic; PS2X_ENABLE_TS2_DIAG
// gates it.
#ifndef PS2X_ENABLE_TS2_DIAG
#define PS2X_ENABLE_TS2_DIAG 0
#endif

namespace ps2_ts2_split60
{
struct GuestContext
{
    uint64_t macroOrdinal = 0;
    uint32_t halfIndex = 0;
    uint32_t rider = 0;
    uint32_t riderIndex = 0;
    uint32_t predictorEpoch = 0;
    uint32_t continuation = 0;
    uint32_t stockHNumerator = 1;
    uint32_t stockHDenominator = 60;
    // TS3: the 111408 selector (P+0xde0) read at begin(). Only cases 0/1/2
    // were ever observed and converted to H/2; 3/4/5 run once at full H.
    // Unreadable addresses read as 0 (converted), preserving old behavior.
    uint32_t selectorCase = 0;
    bool active = false;
    bool restartCheckpointPending = false;
};
struct PredictorIdentity
{
    uint32_t context = 0;
    uint32_t epoch = 0;
};

// TT1: one plain static instead of nine `inline thread_local`s. The old
// thread_locals cost 1.64 ms/frame of tlsdesc_resolver_dynamic + __tls_init
// on the Odin GameThread (MV3 Part 2), plus an unordered_map hash lookup on
// every intercepted guest load. Audit (TT1 REPORT): every entry point below
// runs on the EE executor thread only — halfLoad via READ32 from EE
// recompiled code and EE-thread syscall stubs, the rest via EeScheduler /
// dispatchGuestBranch / eeCheckpointDue. The MTVU worker, GS worker, audio
// and IOP threads touch none of this state (the mtvu::sync TLS samples in
// the MV3 profile are ps2_mtvu.h's own t_* vars, untouched here).
struct ThreadData
{
    GuestContext ctx;
    uint32_t half = 0;
    bool helper = false;
};
struct State
{
    uint32_t guestThread = 0;
    bool guestInterrupt = false;
    uint64_t scopeErrors = 0;
    // Guest-thread-keyed records. Occupancy in practice is 1 (the main guest
    // thread runs the rider pass); the linear scan hits slot 0. Overflow
    // keeps exact map semantics past the flat slots; it is cold-only.
    static constexpr uint32_t kThreadSlots = 8;
    struct ThreadSlot
    {
        bool used = false;
        uint32_t key = 0;
        ThreadData data;
    };
    ThreadSlot threads[kThreadSlots];
    std::unordered_map<uint32_t, ThreadData> threadOverflow;
    // Rider-address-keyed predictor identities (<= ~6 riders per event).
    static constexpr uint32_t kRiderSlots = 16;
    struct RiderSlot
    {
        bool used = false;
        uint32_t key = 0;
        PredictorIdentity id;
    };
    RiderSlot riders[kRiderSlots];
    std::unordered_map<uint32_t, PredictorIdentity> riderOverflow;
    // HL1: halfLoad runs on every guest 32-bit load, so its findThread scan
    // per load is the #1 GameThread cost. This cache holds the current guest
    // thread's record and refreshes only when the scheduler switches guest
    // threads (observed via the key check) or getThread() inserts. Slot
    // addresses are fixed, and unordered_map never invalidates pointers on
    // insert/rehash (nothing erases), so the cached pointer is exact.
    uint32_t cachedKey = 0;
    ThreadData *cachedData = nullptr;
    bool cachedValid = false;
    // HL1: diagnostic counters (PS2X_TS2_HALFLOAD_COUNT=1, default off). A
    // plain env knob, not TS2_DIAG, so instrumented builds work where diag
    // taps compile out. Same EE-thread-only owner as the rest of State.
    uint64_t halfLoadCalls = 0;
    uint64_t halfLoadActive = 0;
    uint64_t halfLoadConverted = 0;
    uint64_t halfLoadRefreshes = 0;
    uint64_t halfLoadLastBin = 0;
#if PS2X_ENABLE_TS2_DIAG
    uint64_t ts3CaseCounts[7] = {};
    uint64_t ts3CbCounts[5] = {};
    uint64_t ts3CaseFirstTick[7] = {};
    uint64_t ts3LastPrintBin = 0;
    bool ts3PrintArmed = false;
#endif
    ThreadData *findThread(uint32_t key) noexcept
    {
        for (uint32_t i = 0; i < kThreadSlots; ++i)
            if (threads[i].used && threads[i].key == key) return &threads[i].data;
        const auto it = threadOverflow.find(key);
        return it == threadOverflow.end() ? nullptr : &it->second;
    }
    ThreadData &getThread(uint32_t key) noexcept
    {
        if (ThreadData *d = findThread(key)) return *d;
        cachedValid = false; // the insert below may create the cached key
        for (uint32_t i = 0; i < kThreadSlots; ++i)
        {
            if (threads[i].used) continue;
            threads[i].used = true;
            threads[i].key = key;
            return threads[i].data;
        }
        return threadOverflow[key];
    }
    PredictorIdentity &getRider(uint32_t key) noexcept
    {
        for (uint32_t i = 0; i < kRiderSlots; ++i)
            if (riders[i].used && riders[i].key == key) return riders[i].id;
        const auto it = riderOverflow.find(key);
        if (it != riderOverflow.end()) return it->second;
        for (uint32_t i = 0; i < kRiderSlots; ++i)
        {
            if (riders[i].used) continue;
            riders[i].used = true;
            riders[i].key = key;
            return riders[i].id;
        }
        return riderOverflow[key];
    }
};
inline State g_state;

inline bool enabled() noexcept
{
    static const bool on = [] {
        const char *mode = std::getenv("PS2X_SSX3_SIM_MODE");
        return mode && (std::strcmp(mode, "split60_v1") == 0 ||
                        std::strcmp(mode, "split120_render60_v1") == 0);
    }();
    return on;
}

// TT1/GT1(b): one plain load. The guarded function-local static cost an
// ldarb + ~6 insns on EVERY guest load even with SIM off (GT1 §4). The flag
// is filled by setThread()/begin() (EE thread; setThread precedes all guest
// execution, and conversions need an active context from begin), so the
// value is always correct where observed; see the fill points.
inline bool g_halfModeFlag = false;
inline bool g_halfModeDone = false;
inline void initHalfMode() noexcept
{
    if (g_halfModeDone) return;
    const char *mode = std::getenv("PS2X_SSX3_SIM_MODE");
    g_halfModeFlag = mode && std::strcmp(mode, "split120_render60_v1") == 0;
    g_halfModeDone = true;
}
inline bool halfMode() noexcept
{
    return g_halfModeFlag;
}

// TS3: run unconverted selector cases (3/4/5) once per stock update at full
// H. This is the product default in split mode; PS2X_TS3_NOFIX=1 restores the
// pre-fix behavior (every case twice, every reviewed site at H/2) for A/B.
inline bool fixUnconverted() noexcept
{
    static const bool on = [] {
        const char *v = std::getenv("PS2X_TS3_NOFIX");
        return !(v && v[0] == '1' && v[1] == '\0');
    }();
    return on;
}

inline bool countEnabled() noexcept
{
    static const bool on = [] {
        const char *s = std::getenv("PS2X_TS2_HALFLOAD_COUNT");
        return s && s[0] == '1' && s[1] == '\0';
    }();
    return on;
}

// HL1: PS2X_TS2_HL1_CACHE=0 restores the legacy per-load findThread scan
// (default on). Exact either way; the knob exists for A/B and fallback.
inline bool cacheDisabled() noexcept
{
    static const bool off = [] {
        const char *s = std::getenv("PS2X_TS2_HL1_CACHE");
        return s && s[0] == '0' && s[1] == '\0';
    }();
    return off;
}

inline uint32_t read32(const uint8_t *ram, uint32_t address) noexcept
{
    uint32_t value = 0;
    address &= 0x1fffffffu;
    if (ram && address <= 0x02000000u - 4u)
        std::memcpy(&value, ram + address, 4);
    return value;
}

#if PS2X_ENABLE_TS2_DIAG
// TS3: opt-in per-case/callback census. Observation only (guest untouched),
// works in stock and split modes. Counts are cumulative per guest thread;
// the print fires every 600 vsync ticks plus one line per first sighting of
// an unconverted case, so a full race stays under ~40 lines.
inline bool caseCountEnabled() noexcept
{
    static const bool on = [] {
        const char *v = std::getenv("PS2X_TS2_CASE_COUNT");
        return v && v[0] == '1' && v[1] == '\0';
    }();
    return on;
}
// TT1: census arrays live in State above (same EE-thread-only owner).

inline void ts3PrintCounts(uint64_t tick) noexcept
{
    State &s = g_state;
    std::fprintf(stderr,
        "ts2-case-count tick=%llu c0=%llu c1=%llu c2=%llu c3=%llu c4=%llu c5=%llu cOther=%llu cb13ebec=%llu cb13858c=%llu cb13b038=%llu cb13b094=%llu cb13b534=%llu\n",
        static_cast<unsigned long long>(tick),
        static_cast<unsigned long long>(s.ts3CaseCounts[0]),
        static_cast<unsigned long long>(s.ts3CaseCounts[1]),
        static_cast<unsigned long long>(s.ts3CaseCounts[2]),
        static_cast<unsigned long long>(s.ts3CaseCounts[3]),
        static_cast<unsigned long long>(s.ts3CaseCounts[4]),
        static_cast<unsigned long long>(s.ts3CaseCounts[5]),
        static_cast<unsigned long long>(s.ts3CaseCounts[6]),
        static_cast<unsigned long long>(s.ts3CbCounts[0]),
        static_cast<unsigned long long>(s.ts3CbCounts[1]),
        static_cast<unsigned long long>(s.ts3CbCounts[2]),
        static_cast<unsigned long long>(s.ts3CbCounts[3]),
        static_cast<unsigned long long>(s.ts3CbCounts[4]));
}

inline void noteTs3Counts(const uint8_t *ram, R5900Context *ctx,
                          uint32_t source, uint32_t target,
                          bool isCall, bool isIndirect, uint64_t tick) noexcept
{
    if (!ctx) return;
    State &s = g_state;
    if (isCall && !isIndirect && source == 0x128ddcu && target == 0x1216e0u)
    {
        const uint32_t r = getRegU32(ctx, 4) & 0x1fffffffu;
        const uint32_t p = read32(ram, r + 0x77cu) & 0x1fffffffu;
        const uint32_t c = read32(ram, p + 0xde0u);
        const uint32_t slot = (c <= 5u) ? c : 6u;
        ++s.ts3CaseCounts[slot];
        if (c >= 3u && s.ts3CaseFirstTick[slot] == 0u)
        {
            s.ts3CaseFirstTick[slot] = tick ? tick : 1u;
            std::fprintf(stderr, "ts2-case-first case=%u tick=%llu\n",
                         c, static_cast<unsigned long long>(tick));
        }
    }
    if (isCall && isIndirect)
    {
        switch (source)
        {
        case 0x13ebecu: ++s.ts3CbCounts[0]; break;
        case 0x13858cu: ++s.ts3CbCounts[1]; break;
        case 0x13b038u: ++s.ts3CbCounts[2]; break;
        case 0x13b094u: ++s.ts3CbCounts[3]; break;
        case 0x13b534u: ++s.ts3CbCounts[4]; break;
        default: break;
        }
    }
    const uint64_t bin = tick / 600u;
    if (!s.ts3PrintArmed || bin != s.ts3LastPrintBin)
    {
        s.ts3PrintArmed = true;
        s.ts3LastPrintBin = bin;
        ts3PrintCounts(tick);
    }
}
#else
inline bool caseCountEnabled() noexcept
{
    return false;
}
inline void noteTs3Counts(const uint8_t *, R5900Context *,
                          uint32_t, uint32_t,
                          bool, bool, uint64_t) noexcept
{
}
#endif // PS2X_ENABLE_TS2_DIAG

// Called only after the first rider pass has fully returned to 128dec.
inline bool beginSecondHalf() noexcept
{
    State &s = g_state;
    if (!halfMode() || s.guestInterrupt) return false;
    uint32_t &half = s.getThread(s.guestThread).half;
    if (half == 0) { half = 1; return true; }
    half = 0;
    return false;
}

inline void noteHelperCall(uint32_t source, uint32_t target) noexcept
{
    if (!halfMode() || target != 0x1139a0u) return;
    State &s = g_state;
    if (source == 0x1132e0u) s.getThread(s.guestThread).helper = true;
    if (source == 0x113844u || source == 0x113880u)
        s.getThread(s.guestThread).helper = false;
}

inline uint32_t halfLoad(uint32_t pc, uint32_t address, uint32_t bits) noexcept
{
    if (!halfMode()) return bits;
    State &s = g_state;
    if (s.guestInterrupt) return bits;
    if (countEnabled()) ++s.halfLoadCalls;
    const ThreadData *td;
    if (cacheDisabled())
    {
        td = s.findThread(s.guestThread);
    }
    else
    {
        if (!s.cachedValid || s.cachedKey != s.guestThread)
        {
            s.cachedData = s.findThread(s.guestThread);
            s.cachedKey = s.guestThread;
            s.cachedValid = true;
            if (countEnabled()) ++s.halfLoadRefreshes;
        }
        td = s.cachedData;
    }
    if (td == nullptr || !td->ctx.active) return bits;
    if (countEnabled()) ++s.halfLoadActive;
    // TS3: cases 4/5 reach converted sites through the shared 121aa0/113648
    // helpers. They run once at full H, so their loads keep stock values.
    if (fixUnconverted() && td->ctx.selectorCase >= 3u) return bits;
    address &= 0x1fffffffu;
    // Only reviewed, twice-executed instruction sites are converted. The
    // predictor's call to the shared 1139a0 helper retains stock values.
    // (A missing helper record reads false, as the old map's operator[] did.)
    const bool helper = td->helper;
    uint32_t out = bits;
    switch (pc)
    {
    case ps2_ts2_splitsites::kSite113808: if (address == 0x49b494u) out = 0x3c088889u; break;
    case ps2_ts2_splitsites::kSite113860: if (address == 0x49b498u) out = 0x3ba3d70au; break;
    case ps2_ts2_splitsites::kSite113888: if (address == 0x49b49cu) out = 0x42efffffu; break;
    case ps2_ts2_splitsites::kSite1139a4: if (address == 0x49b4a0u && !helper) out = 0x3c088889u; break;
    case ps2_ts2_splitsites::kSite1139c4: if (address == 0x49b4a4u && !helper) out = 0xbadaa2bdu; break;
    case ps2_ts2_splitsites::kSite1139dc: if (address == 0x49b4a8u && !helper) out = 0xc17d5556u; break;
    case ps2_ts2_splitsites::kSite113a0c: if (address == 0x49b4acu && !helper) out = 0xc0e2aaabu; break;
    case ps2_ts2_splitsites::kSite121e64: if (address == 0x49b828u) out = 0x3c088889u; break;
    case ps2_ts2_splitsites::kSite137d68: if (address == 0x49be9cu) out = 0x3c088889u; break;
    case ps2_ts2_splitsites::kSite139a48: if (address == 0x49bf1cu) out = 0x3c088889u; break;
    case ps2_ts2_splitsites::kSite13d8e4: if (address == 0x49c08cu) out = 0x3c088889u; break;
    case ps2_ts2_splitsites::kSite13ee80: if (address == 0x49c12cu) out = 0x3c088889u; break;
    default: break;
    }
    if (out != bits && countEnabled()) ++s.halfLoadConverted;
    return out;
}

// Called per dispatch with the current vsync tick; prints cumulative
// counters every 300 ticks (~10 lines per boot).
inline void countTick(uint64_t tick) noexcept
{
    if (!countEnabled()) return;
    State &s = g_state;
    const uint64_t bin = tick / 300u;
    if (bin == s.halfLoadLastBin) return;
    s.halfLoadLastBin = bin;
    std::fprintf(stderr,
                 "ts2-halfload tick=%llu calls=%llu active=%llu converted=%llu refreshes=%llu cache=%s\n",
                 static_cast<unsigned long long>(tick),
                 static_cast<unsigned long long>(s.halfLoadCalls),
                 static_cast<unsigned long long>(s.halfLoadActive),
                 static_cast<unsigned long long>(s.halfLoadConverted),
                 static_cast<unsigned long long>(s.halfLoadRefreshes),
                 cacheDisabled() ? "off" : "on");
    std::fflush(stderr);
}

inline void setThread(uint32_t id, bool interrupt) noexcept
{
    // Fill first, unconditionally: the scheduler calls this before any guest
    // code runs, so the halfMode flag is correct for every guarded load.
    initHalfMode();
    if (enabled()) { g_state.guestThread = id; g_state.guestInterrupt = interrupt; }
}

// The catch-up scanner belongs to the stock-rate predictor. The first half
// services it; the second reuses the result if the front already covers the
// caller's requested threshold. An uncovered front must still catch up.
inline bool skipSecondHalfPrediction(const uint8_t *ram, R5900Context *ctx,
                                    uint32_t source, uint32_t target) noexcept
{
    if (!halfMode()) return false;
    State &s = g_state;
    if (s.guestInterrupt || target != 0x113200u || !ctx ||
        (source != 0x113744u && source != 0x113770u &&
         source != 0x1137a8u && source != 0x1137d8u)) return false;
    const ThreadData *td = s.findThread(s.guestThread);
    if (td == nullptr || !td->ctx.active || td->ctx.halfIndex != 1)
        return false;
    const uint32_t c = getRegU32(ctx, 4) & 0x1fffffffu;
    if (!ram || c > 0x02000000u - 0xa4u) return false;
    const uint32_t frontBits = read32(ram, c + 0x98u);
    const uint32_t baseBits = read32(ram, c + 0xa0u);
    float front = 0.0f, base = 0.0f;
    std::memcpy(&front, &frontBits, 4);
    std::memcpy(&base, &baseBits, 4);
    if (front < base + ctx->f[22])
        return false;
    return true;
}

inline void begin(const uint8_t *ram, R5900Context *ctx) noexcept
{
    // Belt-and-braces for non-scheduler flows: conversions need an active
    // context, so the flag is filled before any converting load can run.
    initHalfMode();
    if (!enabled() || !ctx) return;
    State &s = g_state;
    if (s.guestInterrupt) return;
    ThreadData &td = s.getThread(s.guestThread);
    GuestContext &current = td.ctx;
    if (current.active) { ++s.scopeErrors; return; }
    const uint32_t remaining = getRegU32(ctx, 16);
    const uint32_t total = getRegU32(ctx, 18);
    if (remaining + 1u == total)
    {
        if (td.half == 0) ++current.macroOrdinal;
        current.riderIndex = 0;
    }
    else
    {
        ++current.riderIndex;
    }
    current.halfIndex = td.half;
    current.rider = getRegU32(ctx, 4) & 0x1fffffffu;
    // TS3: record the 111408 selector fresh every half, so a rider entering
    // or leaving a converted case mid-macro is decided per half, not per macro.
    current.selectorCase =
        read32(ram, (read32(ram, current.rider + 0x77cu) & 0x1fffffffu) + 0xde0u);
    const uint32_t predictorContext = read32(ram, current.rider + 0x788u) & 0x1fffffffu;
    PredictorIdentity &identity = s.getRider(current.rider);
    if (identity.context != predictorContext)
    {
        identity.context = predictorContext;
        ++identity.epoch;
    }
    current.predictorEpoch = identity.epoch;
    current.continuation = 0x128de4u;
    current.active = true;
}

inline void finish() noexcept
{
    if (!enabled()) return;
    State &s = g_state;
    if (s.guestInterrupt) return;
    ThreadData *td = s.findThread(s.guestThread);
    // A helper-only record (no begin) reads inactive, as the old map's
    // missing key did; either way this is a scope error.
    if (td == nullptr || !td->ctx.active) { ++s.scopeErrors; return; }
    td->ctx.active = false;
}

// TS3: unconverted cases run once per stock update. In half 1 the dispatch
// skips the 128ddc->1216e0 call for that rider (1216e0 is a pure wrapper
// around the 111408 dispatch, and the 128de4 continuation ignores its return
// value), then still runs finishIfContinuation so the half 1->0 flip on the
// last rider is preserved. Half 0 runs the handler at full H (halfLoad above
// keeps stock values for these cases). Transitions are per half: the case
// recorded at this half's begin() decides, so a rider that changes case
// mid-macro runs or skips each half correctly.
inline bool skipSecondHalfUnconverted() noexcept
{
    if (!halfMode() || !fixUnconverted()) return false;
    State &s = g_state;
    if (s.guestInterrupt) return false;
    const ThreadData *td = s.findThread(s.guestThread);
    if (td == nullptr || !td->ctx.active || td->ctx.halfIndex != 1)
        return false;
    return td->ctx.selectorCase >= 3u;
}

// The canonical generated 128af0 loop resumes at 128de4 after each rider.
// On the last rider of half 0, re-arm its existing backward branch with the
// original rider list. This uses the generated continuation, including when
// the callee yielded and the scheduler later resumes it.
inline void finishIfContinuation(R5900Context *ctx) noexcept
{
    if (!enabled() || !ctx || ctx->pc != 0x128de4u) return;
    State &s = g_state;
    if (s.guestInterrupt) return;
    ThreadData *td = s.findThread(s.guestThread);
    if (td == nullptr || !td->ctx.active) return;
    const uint32_t remaining = getRegU32(ctx, 16);
    const uint32_t total = getRegU32(ctx, 18);
    finish();
    if (!halfMode() || remaining != 0u || total == 0u) return;
    // The record exists: active required begin(), which always creates it.
    // finish() above only clears active, so td stays valid.
    uint32_t &half = td->half;
    if (half == 0u)
    {
        half = 1u;
        td->ctx.restartCheckpointPending = true;
        uint64_t firstRider = 0;
        std::memcpy(&firstRider, &ctx->r[29], sizeof(firstRider));
        const uint64_t remainingRiders = total;
        // Match SET_GPR_U64's low-half write while keeping this header usable
        // before ps2_runtime_macros.h defines the generated-code macros.
        std::memcpy(&ctx->r[16], &remainingRiders, sizeof(remainingRiders));
        std::memcpy(&ctx->r[17], &firstRider, sizeof(firstRider));
    }
    else
    {
        half = 0u;
    }
}

// The generated loop charges a checkpoint on its backward edge. G2d's
// second-half restart jumped directly to 128dd8, so skip only that extra
// checkpoint; all ordinary rider-loop checkpoints remain intact.
inline bool consumeRestartCheckpoint() noexcept
{
    // EE1P2: the sole caller gates on halfMode(), so only the interrupt
    // check remains here; behaviour is identical in all modes.
    State &s = g_state;
    if (s.guestInterrupt) return false;
    ThreadData *td = s.findThread(s.guestThread);
    if (td == nullptr || !td->ctx.restartCheckpointPending) return false;
    td->ctx.restartCheckpointPending = false;
    return true;
}
} // namespace ps2_ts2_split60
