// TS2 stock-H extraction adapter. The guest call remains unchanged; host-only
// context makes the physical pass an explicit, resumable boundary for G1.
#pragma once

#include "ps2_runtime.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

// EE1P2: this header is the split120 product path and stays compiled in
// release builds. Only the prediction skip/fallback counters are diagnostic
// (read solely by the PS2X_TS2_GATE print); PS2X_ENABLE_TS2_DIAG gates them.
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

inline thread_local uint32_t guestThread = 0;
inline thread_local bool guestInterrupt = false;
inline thread_local std::unordered_map<uint32_t, GuestContext> contexts;
inline thread_local std::unordered_map<uint32_t, PredictorIdentity> predictors;
inline thread_local uint64_t scopeErrors = 0;
inline thread_local std::unordered_map<uint32_t, uint32_t> macroHalves;
inline thread_local std::unordered_map<uint32_t, bool> predictionHelper;
inline thread_local uint64_t predictionSkips = 0;
inline thread_local uint64_t predictionFallbacks = 0;

inline bool enabled() noexcept
{
    static const bool on = [] {
        const char *mode = std::getenv("PS2X_SSX3_SIM_MODE");
        return mode && (std::strcmp(mode, "split60_v1") == 0 ||
                        std::strcmp(mode, "split120_render60_v1") == 0);
    }();
    return on;
}

inline bool halfMode() noexcept
{
    static const bool on = [] {
        const char *mode = std::getenv("PS2X_SSX3_SIM_MODE");
        return mode && std::strcmp(mode, "split120_render60_v1") == 0;
    }();
    return on;
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
inline thread_local uint64_t ts3CaseCounts[7] = {};
inline thread_local uint64_t ts3CbCounts[5] = {};
inline thread_local uint64_t ts3CaseFirstTick[7] = {};
inline thread_local uint64_t ts3LastPrintBin = 0;
inline thread_local bool ts3PrintArmed = false;

inline void ts3PrintCounts(uint64_t tick) noexcept
{
    std::fprintf(stderr,
        "ts2-case-count tick=%llu c0=%llu c1=%llu c2=%llu c3=%llu c4=%llu c5=%llu cOther=%llu cb13ebec=%llu cb13858c=%llu cb13b038=%llu cb13b094=%llu cb13b534=%llu\n",
        static_cast<unsigned long long>(tick),
        static_cast<unsigned long long>(ts3CaseCounts[0]),
        static_cast<unsigned long long>(ts3CaseCounts[1]),
        static_cast<unsigned long long>(ts3CaseCounts[2]),
        static_cast<unsigned long long>(ts3CaseCounts[3]),
        static_cast<unsigned long long>(ts3CaseCounts[4]),
        static_cast<unsigned long long>(ts3CaseCounts[5]),
        static_cast<unsigned long long>(ts3CaseCounts[6]),
        static_cast<unsigned long long>(ts3CbCounts[0]),
        static_cast<unsigned long long>(ts3CbCounts[1]),
        static_cast<unsigned long long>(ts3CbCounts[2]),
        static_cast<unsigned long long>(ts3CbCounts[3]),
        static_cast<unsigned long long>(ts3CbCounts[4]));
}

inline void noteTs3Counts(const uint8_t *ram, R5900Context *ctx,
                          uint32_t source, uint32_t target,
                          bool isCall, bool isIndirect, uint64_t tick) noexcept
{
    if (!ctx) return;
    if (isCall && !isIndirect && source == 0x128ddcu && target == 0x1216e0u)
    {
        const uint32_t r = getRegU32(ctx, 4) & 0x1fffffffu;
        const uint32_t p = read32(ram, r + 0x77cu) & 0x1fffffffu;
        const uint32_t c = read32(ram, p + 0xde0u);
        const uint32_t slot = (c <= 5u) ? c : 6u;
        ++ts3CaseCounts[slot];
        if (c >= 3u && ts3CaseFirstTick[slot] == 0u)
        {
            ts3CaseFirstTick[slot] = tick ? tick : 1u;
            std::fprintf(stderr, "ts2-case-first case=%u tick=%llu\n",
                         c, static_cast<unsigned long long>(tick));
        }
    }
    if (isCall && isIndirect)
    {
        switch (source)
        {
        case 0x13ebecu: ++ts3CbCounts[0]; break;
        case 0x13858cu: ++ts3CbCounts[1]; break;
        case 0x13b038u: ++ts3CbCounts[2]; break;
        case 0x13b094u: ++ts3CbCounts[3]; break;
        case 0x13b534u: ++ts3CbCounts[4]; break;
        default: break;
        }
    }
    const uint64_t bin = tick / 600u;
    if (!ts3PrintArmed || bin != ts3LastPrintBin)
    {
        ts3PrintArmed = true;
        ts3LastPrintBin = bin;
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
    if (!halfMode() || guestInterrupt) return false;
    uint32_t &half = macroHalves[guestThread];
    if (half == 0) { half = 1; return true; }
    half = 0;
    return false;
}

inline void noteHelperCall(uint32_t source, uint32_t target) noexcept
{
    if (!halfMode() || target != 0x1139a0u) return;
    if (source == 0x1132e0u) predictionHelper[guestThread] = true;
    if (source == 0x113844u || source == 0x113880u)
        predictionHelper[guestThread] = false;
}

inline uint32_t halfLoad(uint32_t pc, uint32_t address, uint32_t bits) noexcept
{
    if (!halfMode() || guestInterrupt) return bits;
    auto it = contexts.find(guestThread);
    if (it == contexts.end() || !it->second.active) return bits;
    // TS3: cases 4/5 reach converted sites through the shared 121aa0/113648
    // helpers. They run once at full H, so their loads keep stock values.
    if (fixUnconverted() && it->second.selectorCase >= 3u) return bits;
    address &= 0x1fffffffu;
    // Only reviewed, twice-executed instruction sites are converted. The
    // predictor's call to the shared 1139a0 helper retains stock values.
    switch (pc)
    {
    case 0x113808u: if (address == 0x49b494u) return 0x3c088889u; break;
    case 0x113860u: if (address == 0x49b498u) return 0x3ba3d70au; break;
    case 0x113888u: if (address == 0x49b49cu) return 0x42efffffu; break;
    case 0x1139a4u: if (address == 0x49b4a0u && !predictionHelper[guestThread]) return 0x3c088889u; break;
    case 0x1139c4u: if (address == 0x49b4a4u && !predictionHelper[guestThread]) return 0xbadaa2bdu; break;
    case 0x1139dcu: if (address == 0x49b4a8u && !predictionHelper[guestThread]) return 0xc17d5556u; break;
    case 0x113a0cu: if (address == 0x49b4acu && !predictionHelper[guestThread]) return 0xc0e2aaabu; break;
    case 0x121e64u: if (address == 0x49b828u) return 0x3c088889u; break;
    case 0x137d68u: if (address == 0x49be9cu) return 0x3c088889u; break;
    case 0x139a48u: if (address == 0x49bf1cu) return 0x3c088889u; break;
    case 0x13d8e4u: if (address == 0x49c08cu) return 0x3c088889u; break;
    case 0x13ee80u: if (address == 0x49c12cu) return 0x3c088889u; break;
    default: break;
    }
    return bits;
}

inline void setThread(uint32_t id, bool interrupt) noexcept
{
    if (enabled()) { guestThread = id; guestInterrupt = interrupt; }
}

// The catch-up scanner belongs to the stock-rate predictor. The first half
// services it; the second reuses the result if the front already covers the
// caller's requested threshold. An uncovered front must still catch up.
inline bool skipSecondHalfPrediction(const uint8_t *ram, R5900Context *ctx,
                                    uint32_t source, uint32_t target) noexcept
{
    if (!halfMode() || guestInterrupt || target != 0x113200u || !ctx ||
        (source != 0x113744u && source != 0x113770u &&
         source != 0x1137a8u && source != 0x1137d8u)) return false;
    const auto it = contexts.find(guestThread);
    if (it == contexts.end() || !it->second.active || it->second.halfIndex != 1)
        return false;
    const uint32_t c = getRegU32(ctx, 4) & 0x1fffffffu;
    if (!ram || c > 0x02000000u - 0xa4u) return false;
    const uint32_t frontBits = read32(ram, c + 0x98u);
    const uint32_t baseBits = read32(ram, c + 0xa0u);
    float front = 0.0f, base = 0.0f;
    std::memcpy(&front, &frontBits, 4);
    std::memcpy(&base, &baseBits, 4);
    if (front < base + ctx->f[22])
    {
#if PS2X_ENABLE_TS2_DIAG
        ++predictionFallbacks;
#endif
        return false;
    }
#if PS2X_ENABLE_TS2_DIAG
    ++predictionSkips;
#endif
    return true;
}

inline void begin(const uint8_t *ram, R5900Context *ctx) noexcept
{
    if (!enabled() || !ctx || guestInterrupt) return;
    GuestContext &current = contexts[guestThread];
    if (current.active) { ++scopeErrors; return; }
    const uint32_t remaining = getRegU32(ctx, 16);
    const uint32_t total = getRegU32(ctx, 18);
    if (remaining + 1u == total)
    {
        if (macroHalves[guestThread] == 0) ++current.macroOrdinal;
        current.riderIndex = 0;
    }
    else
    {
        ++current.riderIndex;
    }
    current.halfIndex = macroHalves[guestThread];
    current.rider = getRegU32(ctx, 4) & 0x1fffffffu;
    // TS3: record the 111408 selector fresh every half, so a rider entering
    // or leaving a converted case mid-macro is decided per half, not per macro.
    current.selectorCase =
        read32(ram, (read32(ram, current.rider + 0x77cu) & 0x1fffffffu) + 0xde0u);
    const uint32_t predictorContext = read32(ram, current.rider + 0x788u) & 0x1fffffffu;
    PredictorIdentity &identity = predictors[current.rider];
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
    if (!enabled() || guestInterrupt) return;
    auto it = contexts.find(guestThread);
    if (it == contexts.end() || !it->second.active) { ++scopeErrors; return; }
    it->second.active = false;
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
    if (!halfMode() || guestInterrupt || !fixUnconverted()) return false;
    const auto it = contexts.find(guestThread);
    if (it == contexts.end() || !it->second.active || it->second.halfIndex != 1)
        return false;
    return it->second.selectorCase >= 3u;
}

// The canonical generated 128af0 loop resumes at 128de4 after each rider.
// On the last rider of half 0, re-arm its existing backward branch with the
// original rider list. This uses the generated continuation, including when
// the callee yielded and the scheduler later resumes it.
inline void finishIfContinuation(R5900Context *ctx) noexcept
{
    if (!enabled() || !ctx || ctx->pc != 0x128de4u || guestInterrupt) return;
    const auto it = contexts.find(guestThread);
    if (it == contexts.end() || !it->second.active) return;
    const uint32_t remaining = getRegU32(ctx, 16);
    const uint32_t total = getRegU32(ctx, 18);
    finish();
    if (!halfMode() || remaining != 0u || total == 0u) return;
    uint32_t &half = macroHalves[guestThread];
    if (half == 0u)
    {
        half = 1u;
        it->second.restartCheckpointPending = true;
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
    if (guestInterrupt) return false;
    auto it = contexts.find(guestThread);
    if (it == contexts.end() || !it->second.restartCheckpointPending) return false;
    it->second.restartCheckpointPending = false;
    return true;
}
} // namespace ps2_ts2_split60
