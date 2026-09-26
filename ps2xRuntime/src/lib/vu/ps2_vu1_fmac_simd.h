#ifndef PS2_VU1_FMAC_SIMD_H
#define PS2_VU1_FMAC_SIMD_H

// VR4 D1: the exact FMAC core in vector form. It computes, for the upper ops
// that end in applyFmacDest/applyFmacDestAcc, exactly what the scalar path
// (execUpperImpl's normalized operands + normalizeFmacResult +
// calculateFmacProductSticky + updateFmacFlags + applyDest) computes:
//   - each operand register is normalized once for all four lanes
//     (normalizeOperand: exponent 0 -> signed zero, exponent 0xFF -> signed
//     0x7F7FFFFF), then shared by the float op, the exact result and the
//     product sticky;
//   - the float result uses the same expression per lane (no contraction);
//   - the exact result is the same double expression per lane
//     (calculateFmacExactResults), and a float x float product is exact in
//     double, so the product sticky reuses it;
//   - classification is normalizeFmacExactResult without branches: S from the
//     sign bit, then Z (magnitude 0), O (> FLT_MAX) or U|Z (< FLT_MIN), with
//     the same value overrides;
//   - MAC/status are assembled from the dest lanes as updateFmacFlags does,
//     and committed through the same commitFmacFlags.
// The scalar code stays as the reference (PS2X_VU1_FMAC_SIMD off, and the
// bit-for-bit unit test "VR4 vector FMAC core matches the scalar reference").

#include "runtime/ps2_vu1.h"
#include "ps2_vu1_detail.h"
#include "ps2_vu1_fmac_impl.h"

#include <cstring>
#include <limits>

#if PS2X_VU1_FMAC_SIMD_AVAILABLE

namespace ps2_vu1_fmac_simd
{
    typedef float v4f __attribute__((vector_size(16)));
    typedef uint32_t v4u __attribute__((vector_size(16)));
    typedef int32_t v4i __attribute__((vector_size(16)));
    typedef float v2f __attribute__((vector_size(8)));
    typedef int32_t v2i __attribute__((vector_size(8)));
    typedef double v2d __attribute__((vector_size(16)));
    typedef int64_t v2l __attribute__((vector_size(16)));

    enum Arith : int
    {
        kAdd,
        kSub,
        kMul,
        kMadd,
        kMsub,
        kOpmsub, // acc - vs.yzx * vt.zxy, w = 0
        kOpmula  // vs.yzx * vt.zxy, w = 0
    };

    // Second operand: kBc0..kBc3 = vt lane broadcast, then Q, I, the vt
    // vector, and the OPMSUB/OPMULA cross product.
    enum Src : int
    {
        kBc0 = 0,
        kBc3 = 3,
        kQ,
        kI,
        kVec,
        kCross
    };

    PS2X_VU1_ALWAYS_INLINE inline v4u load4(const float *p)
    {
        v4u v;
        std::memcpy(&v, p, sizeof(v));
        return v;
    }

    // normalizeOperand on four lanes.
    PS2X_VU1_ALWAYS_INLINE inline v4u normalize4(v4u bits)
    {
        const v4u sign = bits & 0x80000000u;
        const v4u exponent = bits & 0x7F800000u;
        const v4u zero = (v4u)(exponent == 0u);
        const v4u inf = (v4u)(exponent == 0x7F800000u);
        bits = (bits & ~zero) | (sign & zero);
        bits = (bits & ~inf) | ((sign | 0x7F7FFFFFu) & inf);
        return bits;
    }

    PS2X_VU1_ALWAYS_INLINE inline v2d lo2(v4f v)
    {
        return __builtin_convertvector(__builtin_shufflevector(v, v, 0, 1), v2d);
    }

    PS2X_VU1_ALWAYS_INLINE inline v2d hi2(v4f v)
    {
        return __builtin_convertvector(__builtin_shufflevector(v, v, 2, 3), v2d);
    }

    PS2X_VU1_ALWAYS_INLINE inline v4i join(v2l lo, v2l hi)
    {
        const v2i l = __builtin_convertvector(lo, v2i);
        const v2i h = __builtin_convertvector(hi, v2i);
        return __builtin_shufflevector(l, h, 0, 1, 2, 3);
    }

    // normalizeFmacExactResult's tests on four exact results, as lane masks.
    struct Class4
    {
        v4i neg, zero, over, under;
    };

    PS2X_VU1_ALWAYS_INLINE inline Class4 classify(v2d lo, v2d hi)
    {
        const double maximum = static_cast<double>(std::numeric_limits<float>::max());
        const double minimum = static_cast<double>(std::numeric_limits<float>::min());
        const v2l loBits = (v2l)lo;
        const v2l hiBits = (v2l)hi;
        const v2d loMag = (v2d)(loBits & 0x7FFFFFFFFFFFFFFFll);
        const v2d hiMag = (v2d)(hiBits & 0x7FFFFFFFFFFFFFFFll);
        Class4 c;
        c.neg = join((v2l)(loBits < 0), (v2l)(hiBits < 0));
        c.zero = join((v2l)(loMag == 0.0), (v2l)(hiMag == 0.0));
        c.over = join((v2l)(loMag > maximum), (v2l)(hiMag > maximum));
        c.under = join((v2l)(loMag < minimum), (v2l)(hiMag < minimum)) & ~c.zero;
        return c;
    }

    // Per-lane flag nibble (Z 1, S 2, U 4, O 8), as normalizeFmacExactResult.
    PS2X_VU1_ALWAYS_INLINE inline v4i flags4(const Class4 &c)
    {
        return (c.neg & 2) | (c.zero & 1) | (c.over & 8) | (c.under & 5);
    }

    PS2X_VU1_ALWAYS_INLINE inline v4i destMask(uint8_t dest)
    {
        const v4i lanes = {8, 4, 2, 1};
        return (v4i)((lanes & static_cast<int32_t>(dest)) != 0);
    }

    PS2X_VU1_ALWAYS_INLINE inline uint32_t orLanes(v4i v)
    {
        return static_cast<uint32_t>(v[0] | v[1] | v[2] | v[3]);
    }
}

// VR4 D1: one vector FMAC. kAcc = the ACC is the destination (…A ops).
template <int kArith, int kSrc, bool kAcc>
PS2X_VU1_ALWAYS_INLINE inline void VU1Interpreter::fmacSimd(uint32_t instr)
{
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
    using namespace ps2_vu1_fmac_simd;
    const uint8_t dest = DEST(instr);
    // Scalar path with dest 0: no lane, no flag write, no store.
    if (dest == 0u)
        return;
    constexpr bool kCrossOp = kArith == kOpmsub || kArith == kOpmula;
    constexpr bool kUsesAcc = kArith == kMadd || kArith == kMsub || kArith == kOpmsub;
    // calculateFmacProductSticky's product-sum set: MADD/MSUB in every form
    // (bc, q, i, vector; FD and ACC destinations) and OPMSUB, not OPMULA.
    constexpr bool kProductSum = kArith == kMadd || kArith == kMsub || kArith == kOpmsub;

    v4f vs = (v4f)normalize4(load4(m_state.vf[FS(instr)]));
    v4f b;
    if constexpr (kSrc >= kBc0 && kSrc <= kBc3)
    {
        const v4f vt = (v4f)normalize4(load4(m_state.vf[FT(instr)]));
        b = __builtin_shufflevector(vt, vt, kSrc, kSrc, kSrc, kSrc);
    }
    else if constexpr (kSrc == kQ || kSrc == kI)
    {
        const float s = normalizeOperand(kSrc == kQ ? m_state.q : m_state.i);
        b = v4f{s, s, s, s};
    }
    else if constexpr (kSrc == kVec)
    {
        b = (v4f)normalize4(load4(m_state.vf[FT(instr)]));
    }
    else
    {
        static_assert(kSrc == kCross, "unknown FMAC source");
        const v4f vt = (v4f)normalize4(load4(m_state.vf[FT(instr)]));
        vs = __builtin_shufflevector(vs, vs, 1, 2, 0, 3);
        b = __builtin_shufflevector(vt, vt, 2, 0, 1, 3);
    }
    v4f acc = v4f{0.0f, 0.0f, 0.0f, 0.0f};
    if constexpr (kUsesAcc)
        acc = (v4f)normalize4(load4(m_state.acc));

    // Float result, same expression per lane as execUpperImpl.
    v4f result;
    if constexpr (kArith == kAdd)
        result = vs + b;
    else if constexpr (kArith == kSub)
        result = vs - b;
    else if constexpr (kArith == kMul || kArith == kOpmula)
    {
        const v4f product = vs * b;
        result = product;
    }
    else if constexpr (kArith == kMadd)
    {
        const v4f product = vs * b;
        result = acc + product;
    }
    else
    {
        const v4f product = vs * b;
        result = acc - product;
    }
    if constexpr (kCrossOp)
        result[3] = 0.0f;

    // Exact results, same double expression per lane as
    // calculateFmacExactResults (the cross ops' w lane is exactly 0.0).
    const v2d sLo = lo2(vs), sHi = hi2(vs);
    const v2d bLo = lo2(b), bHi = hi2(b);
    v2d eLo, eHi;
    v2d pLo = {0.0, 0.0}, pHi = {0.0, 0.0};
    if constexpr (kArith == kAdd)
    {
        eLo = sLo + bLo;
        eHi = sHi + bHi;
    }
    else if constexpr (kArith == kSub)
    {
        eLo = sLo - bLo;
        eHi = sHi - bHi;
    }
    else
    {
        pLo = sLo * bLo;
        pHi = sHi * bHi;
        if constexpr (kArith == kMul || kArith == kOpmula)
        {
            eLo = pLo;
            eHi = pHi;
        }
        else
        {
            const v2d aLo = lo2(acc), aHi = hi2(acc);
            if constexpr (kArith == kMadd)
            {
                eLo = aLo + pLo;
                eHi = aHi + pHi;
            }
            else
            {
                eLo = aLo - pLo;
                eHi = aHi - pHi;
            }
        }
    }
    if constexpr (kCrossOp)
        eHi[1] = 0.0;

    const v4i dm = destMask(dest);
    const Class4 c = classify(eLo, eHi);
    const v4i laneFlags = flags4(c) & dm;

    // normalizeFmacExactResult's value overrides.
    v4u bits = (v4u)result;
    const v4u signBits = (v4u)c.neg & 0x80000000u;
    const v4u toZero = (v4u)(c.zero | c.under);
    const v4u toMax = (v4u)c.over;
    bits = (bits & ~toZero) | (signBits & toZero);
    bits = (bits & ~toMax) | ((signBits | 0x7F7FFFFFu) & toMax);

    // calculateFmacProductSticky: the flags of the same double products (the
    // cross ops' w lane included: vs.w * vt.w).
    uint32_t extraSticky = 0u;
    if constexpr (kProductSum)
        extraSticky = orLanes(flags4(classify(pLo, pHi)) & dm);

    // updateFmacFlags: MAC bit (lane) per Z, << 4 per S, << 8 per U, << 12
    // per O, with lane x = 8 .. w = 1; status = OR of the lane flags.
    const v4i spread = (laneFlags & 1) | ((laneFlags & 2) << 3) | ((laneFlags & 4) << 6) |
                       ((laneFlags & 8) << 9);
    const v4i shifts = {3, 2, 1, 0};
    const uint32_t mac = orLanes(spread << shifts);
    const uint32_t status = orLanes(laneFlags);
    commitFmacFlags(mac, status, extraSticky);

    float out[4];
    std::memcpy(out, &bits, sizeof(out));
    if constexpr (kAcc)
        applyDestAcc(out, dest);
    else
        applyDest(m_state.vf[FD(instr)], out, dest);
}

PS2X_VU1_ALWAYS_INLINE inline bool VU1Interpreter::fmacSimdDispatch(uint32_t instr)
{
    using namespace ps2_vu1_fmac_simd;
    const uint32_t op = instr & 0x3Fu;
    if (op < 0x3Cu)
    {
        switch (op)
        {
        case 0x00: fmacSimd<kAdd, 0, false>(instr); return true; // ADDbc
        case 0x01: fmacSimd<kAdd, 1, false>(instr); return true;
        case 0x02: fmacSimd<kAdd, 2, false>(instr); return true;
        case 0x03: fmacSimd<kAdd, 3, false>(instr); return true;
        case 0x04: fmacSimd<kSub, 0, false>(instr); return true; // SUBbc
        case 0x05: fmacSimd<kSub, 1, false>(instr); return true;
        case 0x06: fmacSimd<kSub, 2, false>(instr); return true;
        case 0x07: fmacSimd<kSub, 3, false>(instr); return true;
        case 0x08: fmacSimd<kMadd, 0, false>(instr); return true; // MADDbc
        case 0x09: fmacSimd<kMadd, 1, false>(instr); return true;
        case 0x0A: fmacSimd<kMadd, 2, false>(instr); return true;
        case 0x0B: fmacSimd<kMadd, 3, false>(instr); return true;
        case 0x0C: fmacSimd<kMsub, 0, false>(instr); return true; // MSUBbc
        case 0x0D: fmacSimd<kMsub, 1, false>(instr); return true;
        case 0x0E: fmacSimd<kMsub, 2, false>(instr); return true;
        case 0x0F: fmacSimd<kMsub, 3, false>(instr); return true;
        case 0x18: fmacSimd<kMul, 0, false>(instr); return true; // MULbc
        case 0x19: fmacSimd<kMul, 1, false>(instr); return true;
        case 0x1A: fmacSimd<kMul, 2, false>(instr); return true;
        case 0x1B: fmacSimd<kMul, 3, false>(instr); return true;
        case 0x1C: fmacSimd<kMul, kQ, false>(instr); return true;  // MULq
        case 0x1E: fmacSimd<kMul, kI, false>(instr); return true;  // MULi
        case 0x20: fmacSimd<kAdd, kQ, false>(instr); return true;  // ADDq
        case 0x21: fmacSimd<kMadd, kQ, false>(instr); return true; // MADDq
        case 0x22: fmacSimd<kAdd, kI, false>(instr); return true;  // ADDi
        case 0x23: fmacSimd<kMadd, kI, false>(instr); return true; // MADDi
        case 0x24: fmacSimd<kSub, kQ, false>(instr); return true;  // SUBq
        case 0x25: fmacSimd<kMsub, kQ, false>(instr); return true; // MSUBq
        case 0x26: fmacSimd<kSub, kI, false>(instr); return true;  // SUBi
        case 0x27: fmacSimd<kMsub, kI, false>(instr); return true; // MSUBi
        case 0x28: fmacSimd<kAdd, kVec, false>(instr); return true;  // ADD
        case 0x29: fmacSimd<kMadd, kVec, false>(instr); return true; // MADD
        case 0x2A: fmacSimd<kMul, kVec, false>(instr); return true;  // MUL
        case 0x2C: fmacSimd<kSub, kVec, false>(instr); return true;  // SUB
        case 0x2D: fmacSimd<kMsub, kVec, false>(instr); return true; // MSUB
        case 0x2E: fmacSimd<kOpmsub, kCross, false>(instr); return true; // OPMSUB
        default: return false;
        }
    }
    const uint32_t special = (instr & 0x3u) | ((instr >> 4) & 0x7Cu);
    switch (special)
    {
    case 0x00: fmacSimd<kAdd, 0, true>(instr); return true; // ADDAbc
    case 0x01: fmacSimd<kAdd, 1, true>(instr); return true;
    case 0x02: fmacSimd<kAdd, 2, true>(instr); return true;
    case 0x03: fmacSimd<kAdd, 3, true>(instr); return true;
    case 0x04: fmacSimd<kSub, 0, true>(instr); return true; // SUBAbc
    case 0x05: fmacSimd<kSub, 1, true>(instr); return true;
    case 0x06: fmacSimd<kSub, 2, true>(instr); return true;
    case 0x07: fmacSimd<kSub, 3, true>(instr); return true;
    case 0x08: fmacSimd<kMadd, 0, true>(instr); return true; // MADDAbc
    case 0x09: fmacSimd<kMadd, 1, true>(instr); return true;
    case 0x0A: fmacSimd<kMadd, 2, true>(instr); return true;
    case 0x0B: fmacSimd<kMadd, 3, true>(instr); return true;
    case 0x0C: fmacSimd<kMsub, 0, true>(instr); return true; // MSUBAbc
    case 0x0D: fmacSimd<kMsub, 1, true>(instr); return true;
    case 0x0E: fmacSimd<kMsub, 2, true>(instr); return true;
    case 0x0F: fmacSimd<kMsub, 3, true>(instr); return true;
    case 0x18: fmacSimd<kMul, 0, true>(instr); return true; // MULAbc
    case 0x19: fmacSimd<kMul, 1, true>(instr); return true;
    case 0x1A: fmacSimd<kMul, 2, true>(instr); return true;
    case 0x1B: fmacSimd<kMul, 3, true>(instr); return true;
    case 0x1C: fmacSimd<kMul, kQ, true>(instr); return true;  // MULAq
    case 0x1E: fmacSimd<kMul, kI, true>(instr); return true;  // MULAi
    case 0x20: fmacSimd<kAdd, kQ, true>(instr); return true;  // ADDAq
    case 0x21: fmacSimd<kMadd, kQ, true>(instr); return true; // MADDAq
    case 0x22: fmacSimd<kAdd, kI, true>(instr); return true;  // ADDAi
    case 0x23: fmacSimd<kMadd, kI, true>(instr); return true; // MADDAi
    case 0x24: fmacSimd<kSub, kQ, true>(instr); return true;  // SUBAq
    case 0x25: fmacSimd<kMsub, kQ, true>(instr); return true; // MSUBAq
    case 0x26: fmacSimd<kSub, kI, true>(instr); return true;  // SUBAi
    case 0x27: fmacSimd<kMsub, kI, true>(instr); return true; // MSUBAi
    case 0x28: fmacSimd<kAdd, kVec, true>(instr); return true;  // ADDA
    case 0x29: fmacSimd<kMadd, kVec, true>(instr); return true; // MADDA
    case 0x2A: fmacSimd<kMul, kVec, true>(instr); return true;  // MULA
    case 0x2C: fmacSimd<kSub, kVec, true>(instr); return true;  // SUBA
    case 0x2D: fmacSimd<kMsub, kVec, true>(instr); return true; // MSUBA
    case 0x2E: fmacSimd<kOpmula, kCross, true>(instr); return true; // OPMULA
    default: return false;
    }
}

#endif // PS2X_VU1_FMAC_SIMD_AVAILABLE

#endif
