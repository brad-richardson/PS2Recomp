#pragma once
#include <algorithm>
#include <cmath>

namespace ps2_fh1
{
// FH33: residual map of 113e80 + 121210, for a frozen target/mode:
// T(g) = max(0, g - clamp(a*g, b, c)). In its proportional interval
// [L=b/a,U=c/a], choose H(g)=sqrt(1-a)*g. Extend below L with
// H=T^n H T^-n, and above U with H=T^-n H T^n. This handles crossings
// of either clamp, unlike simply rescaling the existing half-dt step.
// H(H(g))=T(g) in real arithmetic; guest float stores add rounding.
inline double ground2Residual(double g, double a, double b, double c) noexcept
{
    if (!(g > 0.0)) return 0.0;
    if (!(a > 0.0 && a < 1.0 && b > 0.0 && c >= b / (1.0-a)) ||
        !std::isfinite(g+c))
        return g; // reject invalid guest inputs
    const double r = 1.0-a, q = std::sqrt(r), L = b/a, U = c/a;
    auto step = [=](double x) { return std::max(0.0, x-std::clamp(a*x,b,c)); };
    auto inverse = [=](double x) { return x < r*L ? x+b : (x <= r*U ? x/r : x+c); };
    if (g < L)
    {
        unsigned n = 0;
        while (g < L && n < 128) { g = inverse(g); ++n; }
        if (g < L) return g; // coefficients outside this game's bounded domain
        double h = q*g;
        while (n--) h = step(h);
        return h;
    }
    if (g > U)
    {
        const double n = std::ceil((g-U)/c);
        const double x = g-n*c;
        // First inverse can cross the proportional/cap boundary. Every
        // remaining inverse is in the cap region (q > r).
        return inverse(q*x)+(n-1.0)*c;
    }
    return q*g;
}
} // namespace ps2_fh1
