#pragma once
#include <cmath>

namespace ps2_fh1
{
// NRM1 normcap: exact half-step map for the G18 bounded normal response.
// The cruise ground handler 0x13d818 maps rider-relative velocity w = vel-u
// through c = k*(w.n)*n with c.z clamped at -40, then rescales to |w|.
// Stock applies (k=-0.4) once per update; vnret roots k per half-step but
// each half keeps its own -40 cap, so a saturated pair applies about -80
// per stock period. The exact stock-equivalent composition (frozen unit
// normal, no intervening force):
//
//   half 1 (publish half): plain half-map (capped at -40, rescaled). The
//     hook leaves the guest value and only stores the used downward
//     budget used1, the cap distortion e1 = applied1.z-uncapped1.z
//     (> 0 iff half 1 binds), and mag1 = |w1|.
//   half 2 (hold half): d2 = (w2.n)-e1*n.z, c2 = k*d2*n,
//     c2.z capped at -(40-used1); rescaled to mag1 iff the pair binds
//     (e1 > 0 or half 2 caps), else to |w2| (plain).
//
// h2(h1(x)) equals the stock map in real arithmetic: corrections sum
// exactly since k_h*(2+k_h) = -0.4, the pair binds iff stock binds, and
// the single rescale matches stock's. Unbound pairs reproduce the plain
// half-map (the hook skips the write), so only bound pairs deviate from
// current 120 behavior. Verified offline on 4062 captured rows
// (tooling/check_halfmap.py).
inline constexpr uint32_t kNormcapGround = 0x13d818u;
inline constexpr uint32_t kNormcapPoolK = 0x49c124u;
inline constexpr int kNormcapSlots = 8;

// Pure half-map math (doubles). Returns false to leave the guest value.
// normcapFirst is the plain half-map (capped, rescaled); the hook keeps
// the guest value and only stores the budget triple.
inline bool normcapFirst(const double w[3], const double n[3], double k,
                         double out[3], double &used1, double &e1, double &mag1) noexcept
{
    const double d = w[0] * n[0] + w[1] * n[1] + w[2] * n[2];
    const double cx = k * d * n[0], cy = k * d * n[1];
    const double cz = k * d * n[2];
    const double az = cz < -40.0 ? -40.0 : cz;
    if (!std::isfinite(d + cx + cy + cz))
        return false;
    const double vx = w[0] + cx, vy = w[1] + cy, vz = w[2] + az;
    mag1 = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
    const double m = std::sqrt(vx * vx + vy * vy + vz * vz);
    if (m == 0.0)
    {
        out[0] = vx;
        out[1] = vy;
        out[2] = vz;
    }
    else
    {
        const double s = mag1 / m;
        out[0] = vx * s;
        out[1] = vy * s;
        out[2] = vz * s;
    }
    used1 = az < 0.0 ? -az : 0.0;
    e1 = az - cz;
    return std::isfinite(out[0] + out[1] + out[2] + used1 + e1 + mag1);
}

// binds reports whether the pair caps (half 1 bound, or half 2 caps
// against the remaining budget); only then does the hook write.
inline bool normcapSecond(const double w[3], const double n[3], double k,
                          double used1, double e1, double mag1,
                          double out[3], bool &binds) noexcept
{
    const double d = w[0] * n[0] + w[1] * n[1] + w[2] * n[2] - e1 * n[2];
    const double cx = k * d * n[0], cy = k * d * n[1];
    const double cz = k * d * n[2];
    const double cap = -(40.0 - used1);
    const double az = cz < cap ? cap : cz;
    if (!std::isfinite(d + cx + cy + cz + cap))
        return false;
    binds = (e1 > 0.0) || (cz < cap);
    const double vx = w[0] + cx, vy = w[1] + cy, vz = w[2] + az;
    const double m = std::sqrt(vx * vx + vy * vy + vz * vz);
    if (m == 0.0)
    {
        out[0] = vx;
        out[1] = vy;
        out[2] = vz;
        return true;
    }
    const double w2 = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
    const double s = (binds ? mag1 : w2) / m;
    out[0] = vx * s;
    out[1] = vy * s;
    out[2] = vz * s;
    return std::isfinite(out[0] + out[1] + out[2]);
}

// Stock single map (reference for tests).
inline bool normcapStock(const double w[3], const double n[3], double k, double out[3]) noexcept
{
    const double d = w[0] * n[0] + w[1] * n[1] + w[2] * n[2];
    const double cx = k * d * n[0], cy = k * d * n[1];
    double cz = k * d * n[2];
    if (cz < -40.0)
        cz = -40.0;
    const double vx = w[0] + cx, vy = w[1] + cy, vz = w[2] + cz;
    const double old = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
    const double m = std::sqrt(vx * vx + vy * vy + vz * vz);
    if (m == 0.0)
    {
        out[0] = vx;
        out[1] = vy;
        out[2] = vz;
        return true;
    }
    const double s = old / m;
    out[0] = vx * s;
    out[1] = vy * s;
    out[2] = vz * s;
    return std::isfinite(out[0] + out[1] + out[2]);
}
} // namespace ps2_fh1
