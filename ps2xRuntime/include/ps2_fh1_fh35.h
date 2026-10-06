#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace ps2_fh1
{
// FH35 opt-in guest-affecting conversions. Callers and exact incoming words
// constrain the trail correction; the normal emitter remains untouched.
inline uint32_t fh35Bits(float f) noexcept
{
    uint32_t u; std::memcpy(&u, &f, sizeof(u)); return u;
}
inline float fh35Float(uint32_t u) noexcept
{
    float f; std::memcpy(&f, &u, sizeof(f)); return f;
}
inline bool directTrailCaller(uint32_t sourcePc) noexcept
{
    return sourcePc == 0x345e4cu || sourcePc == 0x345fdcu ||
           sourcePc == 0x34600cu || sourcePc == 0x346028u;
}
inline float directTrailDt(uint32_t sourcePc, float incoming) noexcept
{
    return directTrailCaller(sourcePc) && fh35Bits(incoming) == 0x3c088889u
               ? fh35Float(0x3c888889u) : incoming;
}
inline float chase2T(float z, float lo, float hi) noexcept
{
    if (!std::isfinite(z) || !std::isfinite(lo) || !std::isfinite(hi) || !(hi > lo))
        return -1.0f;
    z = std::fabs(z);
    return z <= lo ? 0.0f : z >= hi ? 1.0f : (z-lo)/(hi-lo);
}
// Endpoint words are the camera group's existing rooted pool words. Return
// false at endpoints or for another caller's parameters, preserving that
// invocation exactly. At interior t, interpolate STOCK retentions before sqrt.
inline bool chase2Retention(float k0, float k1, float t, float &out) noexcept
{
    if (!(t > 0.0f && t < 1.0f)) return false;
    uint32_t r0, r1;
    if (fh35Bits(k0) == 0x3f6c0535u && fh35Bits(k1) == 0x3f7c217au)
        { r0=0x3f59999au; r1=0x3f7851ecu; }
    else if (fh35Bits(k0) == 0x3f055b3fu && fh35Bits(k1) == 0x3f72dce8u)
        { r0=0x3e8aefe0u; r1=0x3f666666u; }
    else return false;
    // Explicit intermediate rounding matches the guest's separate mul/add.
    const float a=fh35Float(r0), b=fh35Float(r1);
    volatile float weighted=(b-a)*t;
    out=std::sqrt(a+weighted);
    return true;
}
}
