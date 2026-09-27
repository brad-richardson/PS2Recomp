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

inline void finishIfContinuation(uint32_t pc) noexcept
{
    if (!enabled() || pc != 0x128de4u) return;
    const auto it = contexts.find(guestThread);
    if (it != contexts.end() && it->second.active) finish();
}
} // namespace ps2_ts2_split60
