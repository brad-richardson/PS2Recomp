// TS2 stock-H extraction adapter. The guest call remains unchanged; host-only
// context makes the physical pass an explicit, resumable boundary for G1.
#pragma once

#include "ps2_runtime.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

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

inline uint32_t read32(const uint8_t *ram, uint32_t address) noexcept
{
    uint32_t value = 0;
    address &= 0x1fffffffu;
    if (ram && address <= 0x02000000u - 4u)
        std::memcpy(&value, ram + address, 4);
    return value;
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
    if (front < base + ctx->f[22]) { ++predictionFallbacks; return false; }
    ++predictionSkips;
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
    if (!halfMode() || guestInterrupt) return false;
    auto it = contexts.find(guestThread);
    if (it == contexts.end() || !it->second.restartCheckpointPending) return false;
    it->second.restartCheckpointPending = false;
    return true;
}
} // namespace ps2_ts2_split60
