// Copied from the ssx3 repo, local/research/RZV1/tooling/rz_core.inc (RZV1 Part 1: bit-exact on Turnip, Metal, CPU).
// RZV1: microVU float ops (round toward zero, flush to zero, results clamped
// to +-FLT_MAX, microVU's DIV-by-zero rule) for a GPU shader. One source,
// compiled as C (CPU check), GLSL 450 (SPIR-V for Turnip) and MSL (Metal);
// the dialect prelude defines uint, F2U/U2F, F2I, U2FLT, PREC, MSB, UMUL.
//
// VARIANT selects the arithmetic:
//   0 INT   integer emulation: mantissa products/sums truncated in uint math
//           (no dependence on the GPU's float rounding, contraction or FTZ)
//   1 EMU   NRV1 §7: hardware RNE add/mul + TwoSum / Dekker TwoProduct error,
//           stepped one ulp toward zero; divide by exact-remainder correction
//   2 EMUF  as EMU, but the product error from fma(a, b, -p)
//   3 RNE   plain precise float ops (the "ordinary shader" baseline)
//   4 FAST  plain float ops, no precise (compiler may contract MUL+ADD)
#define SIGNB 0x80000000u
#define FMAXB 0x7f7fffffu
#define ONEB 0x3f800000u

// ---------------------------------------------------------------- integer emulation
uint i_mul(uint a, uint b)
{
    uint s = (a ^ b) & SIGNB;
    uint ea = (a >> 23) & 0xffu;
    uint eb = (b >> 23) & 0xffu;
    if (ea == 255u || eb == 255u)
        return s | FMAXB; // inf/NaN operand: clamped (never seen in the captures)
    if (ea == 0u || eb == 0u)
        return s; // FTZ: a zero/denormal operand gives a signed zero
    uint ma = (a & 0x7fffffu) | 0x800000u;
    uint mb = (b & 0x7fffffu) | 0x800000u;
    uint hi;
    uint lo;
    UMUL(ma, mb, hi, lo); // product in [2^46, 2^48)
    int e = I32C(ea) + I32C(eb) - 127;
    uint m;
    if (hi >= 0x8000u)
    {
        m = (hi << 8) | (lo >> 24);
        e = e + 1;
    }
    else
        m = (hi << 9) | (lo >> 23);
    if (e >= 255)
        return s | FMAXB; // RZ overflow = FLT_MAX (also the clamp)
    if (e <= 0)
        return s; // RZ underflow is a denormal: flushed
    return s | (U32C(e) << 23) | (m & 0x7fffffu);
}

uint i_add(uint a, uint b)
{
    uint ea = (a >> 23) & 0xffu;
    uint eb = (b >> 23) & 0xffu;
    if (ea == 255u)
        return (a & SIGNB) | FMAXB;
    if (eb == 255u)
        return (b & SIGNB) | FMAXB;
    if (eb == 0u)
    {
        if (ea == 0u)
            return a & b & SIGNB; // (+-0) + (+-0): -0 only if both negative
        return a;
    }
    if (ea == 0u)
        return b;
    if ((b & 0x7fffffffu) > (a & 0x7fffffffu))
    {
        uint t = a;
        a = b;
        b = t;
        t = ea;
        ea = eb;
        eb = t;
    }
    uint s = a & SIGNB;
    uint A = ((a & 0x7fffffu) | 0x800000u) << 7;
    uint mb = ((b & 0x7fffffu) | 0x800000u) << 7;
    uint d = ea - eb;
    uint B = 0u;
    uint sticky = 1u;
    if (d < 31u)
    {
        B = mb >> d;
        sticky = ((B << d) != mb) ? 1u : 0u;
    }
    uint N;
    if (((a ^ b) & SIGNB) == 0u)
        N = A + B; // floor of the exact sum in units of 2^-7 ulp(a); later shifts floor again
    else
    {
        N = A - B - sticky; // floor(A - B_exact) for any later right shift (sticky only when d > 7)
        if (N == 0u)
            return 0u; // exact cancellation: +0 under RZ
    }
    int msb = MSB(N);
    int sh = msb - 23;
    int e = I32C(ea) + sh - 7;
    uint m = (sh >= 0) ? (N >> U32C(sh)) : (N << U32C(-sh));
    if (e >= 255)
        return s | FMAXB;
    if (e <= 0)
        return s;
    return s | (U32C(e) << 23) | (m & 0x7fffffu);
}

bool gt64(uint ah, uint al, uint bh, uint bl)
{
    return ah > bh || (ah == bh && al > bl);
}

// a / b with microVU's rule for a zero-exponent divisor.
uint i_div(uint a, uint b)
{
    uint s = (a ^ b) & SIGNB;
    uint ea = (a >> 23) & 0xffu;
    uint eb = (b >> 23) & 0xffu;
    if (eb == 0u)
        return s | FMAXB;
    if (ea == 0u)
        return s;
    if (ea == 255u)
        return s | FMAXB;
    if (eb == 255u)
        return s;
    uint ma = (a & 0x7fffffu) | 0x800000u;
    uint mb = (b & 0x7fffffu) | 0x800000u;
    bool big = ma >= mb;
    uint k = big ? 23u : 24u;
    int e = I32C(ea) - I32C(eb) + (big ? 127 : 126);
    // numerator N = ma << k (< 2^48) as hi:lo; quotient mantissa = floor(N / mb) in [2^23, 2^24)
    uint nhi = ma >> (32u - k);
    uint nlo = ma << k;
    float qf = (U2FLT(ma) / U2FLT(mb)) * (big ? 8388608.0f : 16777216.0f);
    uint q = U32C(F2I(qf));
    if (q < 0x800000u)
        q = 0x800000u;
    if (q > 0xffffffu)
        q = 0xffffffu;
    for (int i = 0; i < 4; ++i)
    {
        uint ph;
        uint pl;
        UMUL(q, mb, ph, pl);
        if (gt64(ph, pl, nhi, nlo))
            q = q - 1u;
    }
    for (int i = 0; i < 4; ++i)
    {
        uint ph;
        uint pl;
        UMUL(q + 1u, mb, ph, pl);
        if (!gt64(ph, pl, nhi, nlo))
            q = q + 1u;
    }
    if (e >= 255)
        return s | FMAXB;
    if (e <= 0)
        return s;
    return s | (U32C(e) << 23) | (q & 0x7fffffu);
}

// ---------------------------------------------------------------- float emulation
uint clampb(uint u)
{
    // microVU's result clamp: +-inf and NaN -> +-FLT_MAX by sign
    if ((u & 0x7f800000u) == 0x7f800000u)
        return (u & SIGNB) | FMAXB;
    return u;
}
// one ulp toward zero (the magnitude's bit pattern minus one)
uint step0(uint u)
{
    return u - 1u;
}

uint e_add(uint ab, uint bb)
{
    PREC float a = U2F(ab);
    PREC float b = U2F(bb);
    PREC float s = a + b;
    uint su = F2U(s);
    if ((su & 0x7f800000u) == 0x7f800000u)
        return clampb(su);
    PREC float bv = s - a;
    PREC float av = s - bv;
    PREC float da = a - av;
    PREC float db = b - bv;
    PREC float err = da + db;
    uint eu = F2U(err);
    if ((eu & 0x7fffffffu) != 0u && ((eu ^ su) & SIGNB) != 0u && (su & 0x7fffffffu) != 0u)
        su = step0(su);
    return su;
}

// exact product error a*b - p (|a|, |b| below 2^100 after scaling)
float two_prod_err(float a, float b, float p)
{
#if VARIANT == 2
    PREC float np = -p;
    PREC float r = fma(a, b, np);
    return r;
#else
    PREC float ca = 4097.0f * a;
    PREC float ta = ca - a;
    PREC float ah = ca - ta;
    PREC float al = a - ah;
    PREC float cb = 4097.0f * b;
    PREC float tb = cb - b;
    PREC float bh = cb - tb;
    PREC float bl = b - bh;
    PREC float x1 = ah * bh;
    PREC float x2 = x1 - p;
    PREC float x3 = ah * bl;
    PREC float x4 = x2 + x3;
    PREC float x5 = al * bh;
    PREC float x6 = x4 + x5;
    PREC float x7 = al * bl;
    PREC float r = x6 + x7;
    return r;
#endif
}

// scale so the Veltkamp split can't overflow (exact: powers of two, inputs normal)
uint scale_down(uint u)
{
    uint e = (u >> 23) & 0xffu;
    if (e > 200u)
        return u - (64u << 23);
    return u;
}

uint e_mul(uint ab, uint bb)
{
    uint s = (ab ^ bb) & SIGNB;
    if ((ab & 0x7f800000u) == 0u || (bb & 0x7f800000u) == 0u)
        return s; // FTZ
    PREC float a = U2F(ab);
    PREC float b = U2F(bb);
    PREC float p = a * b;
    uint pu = F2U(p);
    if ((pu & 0x7f800000u) == 0x7f800000u)
        return clampb(pu);
    if ((pu & 0x7f800000u) == 0u)
        return s; // underflow: flushed (RZ never rounds up to the smallest normal)
    // scaled copies: the error's sign is unchanged by exact power-of-two scaling
    uint as2 = scale_down(ab);
    uint bs2 = scale_down(bb);
    uint ps2 = pu - (ab - as2) - (bb - bs2);
    float err = two_prod_err(U2F(as2), U2F(bs2), U2F(ps2));
    uint eu = F2U(err);
    if ((eu & 0x7fffffffu) != 0u && ((eu ^ pu) & SIGNB) != 0u)
        pu = step0(pu);
    return pu;
}

// is q * b > a exactly (all positive normals, q*b within [a/2, 2a])
bool over(uint qb, uint bb, uint ab)
{
    uint qs = scale_down(qb);
    uint bs = scale_down(bb);
    uint as2 = ab - (qb - qs) - (bb - bs);
    PREC float q = U2F(qs);
    PREC float b = U2F(bs);
    PREC float a = U2F(as2);
    PREC float p = q * b;
    float err = two_prod_err(q, b, p);
    PREC float d = p - a; // exact (Sterbenz)
    PREC float r = d + err;
    return r > 0.0f;
}

uint e_div(uint ab, uint bb)
{
    uint s = (ab ^ bb) & SIGNB;
    uint ea = (ab >> 23) & 0xffu;
    uint eb = (bb >> 23) & 0xffu;
    if (eb == 0u)
        return s | FMAXB;
    if (ea == 0u)
        return s;
    if (ea == 255u)
        return s | FMAXB;
    if (eb == 255u)
        return s;
    uint A = ab & 0x7fffffffu;
    uint B = bb & 0x7fffffffu;
    PREC float qf = U2F(A) / U2F(B);
    uint Q = F2U(qf) & 0x7fffffffu;
    if (Q >= 0x7f800000u)
        return s | FMAXB;
    if ((Q & 0x7f800000u) == 0u)
        return s;
    for (int i = 0; i < 3; ++i)
        if (over(Q, B, A))
            Q = Q - 1u;
    for (int i = 0; i < 3; ++i)
        if (Q + 1u < 0x7f800000u && !over(Q + 1u, B, A))
            Q = Q + 1u;
    return s | Q;
}

// ---------------------------------------------------------------- plain float
uint p_add(uint a, uint b)
{
    PREC float r = U2F(a) + U2F(b);
    return clampb(F2U(r));
}
uint p_mul(uint a, uint b)
{
    PREC float r = U2F(a) * U2F(b);
    return clampb(F2U(r));
}
uint p_div(uint a, uint b)
{
    if ((b & 0x7f800000u) == 0u)
        return ((a ^ b) & SIGNB) | FMAXB;
    PREC float r = U2F(a) / U2F(b);
    return clampb(F2U(r));
}

// ---------------------------------------------------------------- dispatch
uint vmul(uint a, uint b)
{
#if VARIANT == 0
    return i_mul(a, b);
#elif VARIANT == 1 || VARIANT == 2
    return e_mul(a, b);
#else
    return p_mul(a, b);
#endif
}
uint vadd(uint a, uint b)
{
#if VARIANT == 0
    return i_add(a, b);
#elif VARIANT == 1 || VARIANT == 2
    return e_add(a, b);
#else
    return p_add(a, b);
#endif
}
uint vdiv(uint a, uint b)
{
#if VARIANT == 0
    return i_div(a, b);
#elif VARIANT == 1 || VARIANT == 2
    return e_div(a, b);
#else
    return p_div(a, b);
#endif
}
// FTOIn: x * 2^n (exact or overflow to FLT_MAX), FCVTZS (toward zero, saturating)
uint ftoi(uint x, uint scaleBits)
{
    uint y = i_mul(x, scaleBits);
    float f = U2F(y);
    if (f >= 2147483648.0f)
        return 0x7fffffffu;
    if (f <= -2147483648.0f)
        return 0x80000000u;
    return U32C(F2I(f));
}
