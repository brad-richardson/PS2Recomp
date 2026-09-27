// Bounded, opt-in TS2 G2b trajectory and event-cadence evidence.
#pragma once

#include "ps2_runtime.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ps2_ts2_g2b
{
inline bool enabled() noexcept
{
    static const bool on = [] {
        const char *v = std::getenv("PS2X_TS2_G2B");
        return v && v[0] == '1' && v[1] == '\0';
    }();
    return on;
}

inline uint32_t rd(const uint8_t *ram, uint32_t addr) noexcept
{
    uint32_t v = 0;
    addr &= 0x1fffffffu;
    if (ram && addr <= 0x02000000u - 4u) std::memcpy(&v, ram + addr, 4);
    return v;
}

inline uint32_t fb(float f) noexcept
{
    uint32_t bits = 0;
    std::memcpy(&bits, &f, 4);
    return bits;
}

inline constexpr uint32_t kPlayerP = 0x01464e30u;
inline constexpr uint32_t kPlayerR = 0x01465c40u;
inline constexpr uint32_t kBins = 100u;
inline std::array<std::atomic<uint32_t>, kBins> rng0{};
inline std::array<std::atomic<uint32_t>, kBins> rng1{};
inline std::array<std::atomic<uint32_t>, kBins> triggerCountdown{};
inline std::array<std::atomic<uint32_t>, kBins> triggerOther{};

inline void noteBranch(uint64_t tick, uint32_t source, uint32_t target,
                       R5900Context *ctx) noexcept
{
    if (!enabled() || tick < 1744u || tick > 4000u || !ctx) return;
    const uint32_t bin = static_cast<uint32_t>(tick / 60u);
    if (bin >= kBins) return;
    if (target == 0x317a08u)
    {
        const uint32_t state = getRegU32(ctx, 4) & 0x1fffffffu;
        if (state == 0x4ff018u) rng0[bin].fetch_add(1, std::memory_order_relaxed);
        if (state == 0x4ff030u) rng1[bin].fetch_add(1, std::memory_order_relaxed);
    }
    if (source == 0x1114e4u && target == 0x139c88u)
        triggerCountdown[bin].fetch_add(1, std::memory_order_relaxed);
    if (source == 0x13ac64u && target == 0x105d98u)
        triggerOther[bin].fetch_add(1, std::memory_order_relaxed);
}

inline void noteState(const uint8_t *ram, R5900Context *ctx, uint64_t tick,
                      uint32_t source, uint32_t target) noexcept
{
    if (!enabled() || source != 0x128e54u || target != 0x121750u ||
        !ctx || getRegU32(ctx, 4) != kPlayerR || tick < 1744u || tick > 4000u)
        return;
    if ((tick % 30u) != 0u && (tick < 2200u || tick > 2290u)) return;
    const uint32_t r = kPlayerR;
    if ((rd(ram, r + 0x77cu) & 0x1fffffffu) != kPlayerP) return;
    const uint32_t c = r ? (rd(ram, r + 0x788u) & 0x1fffffffu) : 0u;
    std::fprintf(stderr,
        "ts2-g2b-state tick=%llu p=%08x r=%08x c=%08x mode=%08x pos=%08x,%08x,%08x vel=%08x,%08x,%08x scale=%08x ta0=%08x ta4=%08x\n",
        static_cast<unsigned long long>(tick), kPlayerP, r, c,
        rd(ram, kPlayerP + 0xde0u),
        rd(ram, r + 0x110u), rd(ram, r + 0x114u), rd(ram, r + 0x118u),
        rd(ram, r + 0x1e0u), rd(ram, r + 0x1e4u), rd(ram, r + 0x1e8u),
        rd(ram, r + 0x300u), rd(ram, c + 0xa0u), rd(ram, c + 0xa4u));
    if (tick % 60u == 0u)
    {
        const uint32_t bin = static_cast<uint32_t>(tick / 60u - 1u);
        if (bin < kBins)
            std::fprintf(stderr,
                "ts2-g2b-cadence sec=%u rng0=%u rng1=%u countdown=%u other=%u\n",
                bin, rng0[bin].load(), rng1[bin].load(),
                triggerCountdown[bin].load(), triggerOther[bin].load());
    }
}

inline void notePredicate(const uint8_t *ram, R5900Context *ctx,
                          uint64_t tick, uint32_t pc) noexcept
{
    if (!enabled() || !ctx || tick < 2200u || tick > 2290u) return;
    const uint32_t r = rd(ram, kPlayerP + 0x24u) & 0x1fffffffu;
    // sub_00139C88 receives player P+0x20 as its s0 argument.
    if (getRegU32(ctx, 16) != kPlayerP + 0x20u) return;
    std::fprintf(stderr,
        "ts2-g2b-predicate tick=%llu pc=%08x r=%08x f0=%08x f1=%08x f20=%08x f21=%08x f22=%08x s3p30=%08x\n",
        static_cast<unsigned long long>(tick), pc, r,
        fb(ctx->f[0]), fb(ctx->f[1]), fb(ctx->f[20]),
        fb(ctx->f[21]), fb(ctx->f[22]),
        rd(ram, getRegU32(ctx, 19) + 0x30u));
}
} // namespace ps2_ts2_g2b
