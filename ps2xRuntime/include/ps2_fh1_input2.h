#pragma once

// INP2: 0x321298's digital record in stock half-frame units. All persistent
// accounting remains in the guest record, so event flips and savestates do
// not lose a half update. The private guard word's tag identifies old states.
#include <algorithm>
#include <array>
#include <cstdint>

namespace ps2_fh1
{
inline constexpr uint32_t kInput2Tag = 0x40000000u;
struct Input2Record
{
    uint32_t raw, press, release, level, repeat, countdown, guard;
};
static_assert(sizeof(Input2Record) == 28);

inline void input2Step(Input2Record &r, float raw, unsigned halfStep) noexcept
{
    uint32_t remaining = r.guard & ~kInput2Tag;
    if ((r.guard & kInput2Tag) == 0u)
    {
        // Legacy post-call age 0..2 suppresses the next 3-age stock calls;
        // age >= 3 is already eligible. Legacy countdown pulses on reaching 0.
        remaining = r.guard < 3u ? (4u-r.guard)*2u : 0u;
        r.countdown = std::min(r.countdown, 24u)*2u;
    }
    r.press = r.release = r.repeat = 0u;
    remaining = remaining > halfStep ? remaining-halfStep : 0u;
    const uint32_t level = raw > 0.f ? 1u : 0u;
    if (remaining == 0u && level != r.level)
    {
        r.level = level;
        r.press = level;
        r.release = 1u-level;
        remaining = 8u; // four stock frames between transitions
    }
    if (!r.level)
        r.countdown = 0u;
    else if (r.press)
    {
        r.repeat = 1u;
        r.countdown = 48u; // initial 24-stock-frame repeat delay
    }
    else if (r.countdown == 0u)
    {
        r.repeat = 1u;
        r.countdown = 48u; // legacy held record whose first repeat is due
    }
    else if (r.countdown <= halfStep)
    {
        r.repeat = 1u;
        r.countdown = 24u; // subsequent 12-stock-frame delay
    }
    else
        r.countdown -= halfStep;
    r.guard = kInput2Tag | remaining;
}

// INP3 prototype: phase is parked only across the directional decision block
// 133648..133c40 on odd updates. The tag carries the original phase in guest
// RAM, including across a scheduler checkpoint; no host-side pending map.
inline constexpr uint32_t kInputChainHold = 0x80000000u;
inline uint32_t inputChainPark(uint32_t phase) noexcept
{
    return phase <= 3u ? phase | kInputChainHold : phase;
}
// INP5: a phase-3 tracker with no target, current rotation, accumulated
// rotation or offsets can admit an initial direction without advancing a
// pending recognition recurrence. Preserve signed zero as numerical zero.
inline bool inputChainNeutral(uint32_t phase, const std::array<uint32_t, 8> &angles) noexcept
{
    return phase == 3u && std::all_of(angles.begin(), angles.end(),
        [](uint32_t bits) { return (bits & 0x7fffffffu) == 0u; });
}
inline uint32_t inputChainRestore(uint32_t phase) noexcept
{
    return (phase & ~3u) == kInputChainHold ? phase & 3u : phase;
}
}
