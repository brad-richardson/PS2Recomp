// E52: R5900 FPU and COP2 (VU0 macro) translation semantics vs PCSX2.
//
// Every numeric test runs the generator's own translation of an encoding
// (e52_snippets.h, written at build time by tools/e52_snippet_gen.cpp) on a
// fresh R5900Context and compares the result bits with PCSX2's.
//
// Reference: PCSX2 9056c0834 (the T-lane tree on bytesize).
// - EE FPU: pcsx2/FPU.cpp (fpuDouble operands, checkOverflow/checkUnderflow
//   results, CVT_W, DIV_S, SQRT_S, fp_max/fp_min, C_cond_S).
// - VU0 macro ops: pcsx2/VUops.cpp (vuDouble, VU_MAC_UPDATE in VUflags.cpp,
//   floatToInt/intToFloat, _vuCLIP, _vuDIV, _vuSQRT, _vuRSQRT, _vuSQI,
//   AdvanceLFSR/_vuRINIT/_vuRNEXT), pcsx2/VU0.cpp (CFC2/CTC2),
//   pcsx2/COP2.cpp (VCALLMSR = vu0ExecMicro(VI[CMSAR0])).
// - Host rounding: Pcsx2Config.cpp sets EE and VU control registers to
//   round-toward-zero with DAZ/FTZ (DEFAULT_FPU_FP_CONTROL_REGISTER); the EE
//   recompiler switches to round-to-nearest only for DIV.S (FPUDivFPCR).
//
// Names say which PCSX2 rule a test checks. Numeric tests run under the EE
// thread's PS2 FP mode (ps2_fpmode::ScopedPs2Mode: round toward zero + FZ),
// which is what EeScheduler::run() sets for generated code (E53).
// Assertions for per-op Inf/NaN operand clamping (audit rule G2, deferred by
// the E53 brief) run only with PS2X_TEST_DEFERRED=1.
#include "MiniTest.h"
#include "ps2recomp/code_generator.h"
#include "ps2recomp/instructions.h"
#include "ps2recomp/r5900_decoder.h"
#include "ps2_runtime_macros.h"
#include "ps2_fpmode.h"
#include "ps2_runtime.h"

#include <cfenv>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

#include "e52_snippets.h"

using namespace ps2recomp;

namespace
{
    uint32_t ub(float f)
    {
        uint32_t u;
        std::memcpy(&u, &f, sizeof(u));
        return u;
    }

    float fb(uint32_t u)
    {
        float f;
        std::memcpy(&f, &u, sizeof(f));
        return f;
    }

    std::string hex(uint32_t u)
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "0x%08x", u);
        return buf;
    }

    std::unique_ptr<R5900Context> freshCtx()
    {
        auto ctx = std::make_unique<R5900Context>();
        ctx->vu0_vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
        return ctx;
    }

    void setVf(R5900Context &c, int r, uint32_t x, uint32_t y, uint32_t z, uint32_t w)
    {
        c.vu0_vf[r] = _mm_castsi128_ps(_mm_set_epi32(static_cast<int>(w), static_cast<int>(z),
                                                     static_cast<int>(y), static_cast<int>(x)));
    }

    uint32_t lane(const __m128 &v, int i)
    {
        uint32_t out[4];
        _mm_storeu_si128(reinterpret_cast<__m128i *>(out), _mm_castps_si128(v));
        return out[i];
    }

    void setF(R5900Context &c, int r, uint32_t bits) { c.f[r] = fb(bits); }
    uint32_t getF(const R5900Context &c, int r) { return ub(c.f[r]); }

    // Runs a two-operand FPU snippet (fd=1, fs=2, ft=3) and returns f1's bits.
    template <typename Fn>
    uint32_t fpu2(Fn fn, uint32_t fs, uint32_t ft)
    {
        auto c = freshCtx();
        setF(*c, 2, fs);
        setF(*c, 3, ft);
        // Keep the compiler from folding the known inputs (e.g. x + -0 -> x),
        // which would skip the host FPU and its flush-to-zero.
#if defined(__GNUC__) || defined(__clang__)
        __asm__ __volatile__("" : : "r"(c.get()) : "memory");
#endif
        fn(c.get(), nullptr, nullptr);
        return getF(*c, 1);
    }

    std::string translate(uint32_t address, uint32_t word)
    {
        R5900Decoder decoder;
        CodeGenerator generator({}, {});
        return generator.translateInstruction(decoder.decodeInstruction(address, word));
    }

    bool deferredEnabled()
    {
        const char *v = std::getenv("PS2X_TEST_DEFERRED");
        return v && v[0] == '1';
    }

    constexpr uint32_t FMAX = 0x7F7FFFFFu;
    constexpr uint32_t NFMAX = 0xFF7FFFFFu;
    constexpr uint32_t ONE = 0x3F800000u;

    // LX1c: the fpmode scopes manage MXCSR only on x86, never the x87
    // control word — and glibc's fegetround() reads the x87 word. Assert
    // on the word the scopes actually write (MXCSR RC bits 14:13).
    bool roundTowardZero()
    {
#ifdef PS2X_FPMODE_X86
        return ((ps2_fpmode::readControl() >> 13) & 3u) == 3u;
#else
        return std::fegetround() == FE_TOWARDZERO;
#endif
    }

    bool roundToNearest()
    {
#ifdef PS2X_FPMODE_X86
        return ((ps2_fpmode::readControl() >> 13) & 3u) == 0u;
#else
        return std::fegetround() == FE_TONEAREST;
#endif
    }
}

void register_ps2_fpu_cop2_audit_tests()
{
    MiniTest::Case("E52FpuCop2Audit", [](TestCase &tc)
    {
        tc.Run("E53 FP mode: PS2 scope = round toward zero + FZ; host scope inside it = IEEE; both restore", [](TestCase &t)
        {
            const uint64_t before = ps2_fpmode::readControl();
            volatile float one = 1.0f, tiny = fb(0x33C00000u), den = fb(0x1E3CE508u);
            {
                ps2_fpmode::ScopedPs2Mode ps2;
                t.IsTrue(roundTowardZero(), "PS2 scope rounds toward zero");
                t.IsTrue(ub(one + tiny) == 0x3F800000u, "PS2 scope: 1 + 0.75 ulp chops");
                t.IsTrue(ub(den * den) == 0u, "PS2 scope: denormal product flushes to 0");
                {
                    ps2_fpmode::ScopedHostMode host;
                    t.IsTrue(roundToNearest(), "host scope (GS) rounds to nearest");
                    t.IsTrue(ub(one + tiny) == 0x3F800001u, "host scope: 1 + 0.75 ulp rounds up");
                    t.IsTrue(ub(den * den) != 0u, "host scope: denormal product kept");
                }
                t.IsTrue(roundTowardZero(), "host scope restores PS2 mode");
            }
            t.IsTrue(ps2_fpmode::readControl() == before, "PS2 scope restores the thread's control word");
        });

        // ---- EE FPU (COP1) -------------------------------------------------

        tc.Run("FPU ADD/SUB/MUL.S round toward zero (PCSX2 FPUFPCR ChopZero)", [](TestCase &t)
        {
            ps2_fpmode::ScopedPs2Mode fpMode;
            uint32_t r = fpu2(E52_ADD_S, ONE, 0x33C00000u); // 1 + 0.75 ulp
            t.IsTrue(r == 0x3F800000u, "add.s 1 + 1.5*2^-24: PCSX2 0x3f800000, got " + hex(r));
            r = fpu2(E52_SUB_S, ONE, 0x33A00000u); // 1 - 1.25 ulp(below 1)
            t.IsTrue(r == 0x3F7FFFFEu, "sub.s 1 - 1.25*2^-24: PCSX2 0x3f7ffffe, got " + hex(r));
            r = fpu2(E52_MUL_S, 0x3F800001u, 0x3FC00001u); // 1.5 + 2.5 ulp
            t.IsTrue(r == 0x3FC00002u, "mul.s (1+2^-23)(1.5+2^-23): PCSX2 0x3fc00002, got " + hex(r));
        });

        tc.Run("FPU results clamp overflow to +/-FMAX (checkOverflow)", [](TestCase &t)
        {
            ps2_fpmode::ScopedPs2Mode fpMode;
            uint32_t r = fpu2(E52_ADD_S, FMAX, FMAX);
            t.IsTrue(r == FMAX, "add.s FMAX + FMAX: PCSX2 0x7f7fffff, got " + hex(r));
            r = fpu2(E52_MUL_S, NFMAX, 0x40000000u);
            t.IsTrue(r == NFMAX, "mul.s -FMAX * 2: PCSX2 0xff7fffff, got " + hex(r));
        });

        tc.Run("FPU operands: denormals as +/-0 (fpuDouble; Inf/NaN part deferred G2)", [](TestCase &t)
        {
            ps2_fpmode::ScopedPs2Mode fpMode;
            uint32_t r;
            if (deferredEnabled())
            {
                r = fpu2(E52_MUL_S, 0x7F800000u, 0x00000000u);
                t.IsTrue(r == 0x00000000u, "[deferred G2] mul.s 0x7f800000 * 0: PCSX2 FMAX*0 = +0, got " + hex(r));
            }
            r = fpu2(E52_ADD_S, 0x00400000u, 0x80000000u);
            t.IsTrue(r == 0x00000000u, "add.s denormal + -0: PCSX2 +0, got " + hex(r));
        });

        tc.Run("FPU results: denormals flush to signed zero (checkUnderflow)", [](TestCase &t)
        {
            ps2_fpmode::ScopedPs2Mode fpMode;
            uint32_t r = fpu2(E52_MUL_S, 0x1E3CE508u, 0x1E3CE508u); // 1e-20^2
            t.IsTrue(r == 0x00000000u, "mul.s 1e-20 * 1e-20: PCSX2 +0, got " + hex(r));
            r = fpu2(E52_MUL_S, 0x9E3CE508u, 0x1E3CE508u);
            t.IsTrue(r == 0x80000000u, "mul.s -1e-20 * 1e-20: PCSX2 -0, got " + hex(r));
        });

        tc.Run("FPU DIV.S by zero gives +/-FMAX by sign(fs)^sign(ft) (checkDivideByZero)", [](TestCase &t)
        {
            ps2_fpmode::ScopedPs2Mode fpMode;
            uint32_t r = fpu2(E52_DIV_S, ONE, 0x00000000u);
            t.IsTrue(r == FMAX, "1 / +0: PCSX2 +FMAX, got " + hex(r));
            r = fpu2(E52_DIV_S, ONE, 0x80000000u);
            t.IsTrue(r == NFMAX, "1 / -0: PCSX2 -FMAX, got " + hex(r));
            r = fpu2(E52_DIV_S, 0xBF800000u, 0x00000000u);
            t.IsTrue(r == NFMAX, "-1 / +0: PCSX2 -FMAX, got " + hex(r));
            r = fpu2(E52_DIV_S, 0x00000000u, 0x00000000u);
            t.IsTrue(r == FMAX, "0 / 0: PCSX2 +FMAX, got " + hex(r));
            r = fpu2(E52_DIV_S, ONE, 0x00000001u);
            t.IsTrue(r == FMAX, "1 / denormal: PCSX2 treats the divisor as 0 -> +FMAX, got " + hex(r));
        });

        tc.Run("FPU SQRT.S rounds toward zero (sqrtss under ChopZero)", [](TestCase &t)
        {
            ps2_fpmode::ScopedPs2Mode fpMode;
            auto c = freshCtx();
            setF(*c, 3, 0x40A00000u); // 5.0
            E52_SQRT_S(c.get());
            t.IsTrue(getF(*c, 1) == 0x400F1BBCu, "sqrt.s 5: PCSX2 0x400f1bbc, got " + hex(getF(*c, 1)));
        });

        tc.Run("FPU MADD.S/MADDA.S round the product before the add (no fused multiply-add)", [](TestCase &t)
        {
            ps2_fpmode::ScopedPs2Mode fpMode;
            // (1+2^-12)^2 = 1 + 2^-11 + 2^-24 rounds to 1 + 2^-11 in both modes;
            // ACC = -(1 + 2^-11) then gives +0. A fused FMA gives 2^-24.
            auto c = freshCtx();
            c->f_acc = fb(0xBF801000u);
            setF(*c, 2, 0x3F800800u);
            setF(*c, 3, 0x3F800800u);
            E52_MADD_S(c.get());
            t.IsTrue(getF(*c, 1) == 0x00000000u, "madd.s: PCSX2 +0, got " + hex(getF(*c, 1)));
            c->f_acc = fb(0xBF801000u);
            E52_MADDA_S(c.get());
            t.IsTrue(ub(c->f_acc) == 0x00000000u, "madda.s: PCSX2 ACC = +0, got " + hex(ub(c->f_acc)));
        });

        tc.Run("FPU MAX.S/MIN.S compare as sign-magnitude integers (fp_max/fp_min)", [](TestCase &t)
        {
            ps2_fpmode::ScopedPs2Mode fpMode;
            uint32_t r = fpu2(E52_MAX_S, 0x80000000u, 0x00000000u);
            t.IsTrue(r == 0x00000000u, "max.s(-0, +0): PCSX2 +0, got " + hex(r));
            r = fpu2(E52_MIN_S, 0x00000000u, 0x80000000u);
            t.IsTrue(r == 0x80000000u, "min.s(+0, -0): PCSX2 -0, got " + hex(r));
            r = fpu2(E52_MAX_S, 0xFFC00000u, ONE);
            t.IsTrue(r == ONE, "max.s(0xffc00000, 1): PCSX2 1.0 (a negative number), got " + hex(r));
        });

        tc.Run("FPU C.EQ/C.LE compare fpuDouble operands (denormal = 0, Inf pattern = FMAX)", [](TestCase &t)
        {
            ps2_fpmode::ScopedPs2Mode fpMode;
            auto cond = [](auto fn, uint32_t fs, uint32_t ft) {
                auto c = freshCtx();
                setF(*c, 2, fs);
                setF(*c, 3, ft);
                fn(c.get(), nullptr, nullptr);
                return (c->fcr31 & 0x800000u) != 0;
            };
            t.IsTrue(cond(E52_C_EQ_S, 0x00000001u, 0x00000000u), "c.eq.s(denormal, 0): PCSX2 true");
            t.IsTrue(cond(E52_C_LE_S, 0x7F800000u, FMAX), "c.le.s(0x7f800000, FMAX): PCSX2 true");
            t.IsTrue(cond(E52_C_EQ_S, 0x7FC00000u, 0x7FC00000u), "c.eq.s(0x7fc00000, same): PCSX2 true");
            t.IsTrue(cond(E52_C_LT_S, 0x3F800000u, 0x40000000u), "c.lt.s(1, 2): true (sanity)");
        });

        tc.Run("FPU CVT.S.W rounds toward zero above 2^24", [](TestCase &t)
        {
            ps2_fpmode::ScopedPs2Mode fpMode;
            uint32_t r = fpu2(E52_CVT_S_W, 16777219u, 0u);
            t.IsTrue(r == 0x4B800001u, "cvt.s.w 16777219: PCSX2 0x4b800001, got " + hex(r));
            r = fpu2(E52_CVT_S_W, 0x7FFFFFFFu, 0u);
            t.IsTrue(r == 0x4EFFFFFFu, "cvt.s.w 0x7fffffff: PCSX2 0x4effffff, got " + hex(r));
        });

        tc.Run("FPU CVT.W.S / ABS.S / NEG.S match PCSX2 (sanity, E50 fix)", [](TestCase &t)
        {
            ps2_fpmode::ScopedPs2Mode fpMode;
            t.IsTrue(fpu2(E52_CVT_W_S, 0xBFC00000u, 0u) == 0xFFFFFFFFu, "cvt.w.s -1.5 -> -1");
            t.IsTrue(fpu2(E52_CVT_W_S, 0x4F400000u, 0u) == 0x7FFFFFFFu, "cvt.w.s 3.2e9 -> 0x7fffffff");
            t.IsTrue(fpu2(E52_ABS_S, 0xFFC00001u, 0u) == 0x7FC00001u, "abs.s clears only the sign bit");
            t.IsTrue(fpu2(E52_NEG_S, 0x7F800000u, 0u) == 0xFF800000u, "neg.s flips only the sign bit");
        });

        // ---- COP2 / VU0 macro ----------------------------------------------

        tc.Run("VU0 VADD/VMUL round toward zero (VU0FPCR ChopZero)", [](TestCase &t)
        {
            ps2_fpmode::ScopedPs2Mode fpMode;
            auto c = freshCtx();
            setVf(*c, 3, ONE, ONE, ONE, ONE);
            setVf(*c, 4, 0x33C00000u, 0, 0, 0);
            E52_VADD(c.get()); // vf5 = vf3 + vf4
            t.IsTrue(lane(c->vu0_vf[5], 0) == 0x3F800000u, "vadd 1 + 1.5*2^-24: PCSX2 0x3f800000, got " + hex(lane(c->vu0_vf[5], 0)));
            setVf(*c, 3, 0x3F800001u, ONE, ONE, ONE);
            setVf(*c, 5, 0x3FC00001u, ONE, ONE, ONE);
            E52_VMUL(c.get()); // vf4 = vf3 * vf5
            t.IsTrue(lane(c->vu0_vf[4], 0) == 0x3FC00002u, "vmul: PCSX2 0x3fc00002, got " + hex(lane(c->vu0_vf[4], 0)));
        });

        tc.Run("VU0 VADD clamps overflow to FMAX (VU_MAC_UPDATE; Inf-operand part deferred G2)", [](TestCase &t)
        {
            ps2_fpmode::ScopedPs2Mode fpMode;
            auto c = freshCtx();
            setVf(*c, 3, FMAX, 0x7F800000u, ONE, ONE);
            setVf(*c, 4, FMAX, ONE, ONE, ONE);
            E52_VADD(c.get());
            t.IsTrue(lane(c->vu0_vf[5], 0) == FMAX, "vadd FMAX+FMAX: PCSX2 FMAX, got " + hex(lane(c->vu0_vf[5], 0)));
            if (deferredEnabled())
            {
                t.IsTrue(lane(c->vu0_vf[5], 1) == FMAX, "[deferred G2] vadd 0x7f800000+1: PCSX2 FMAX, got " + hex(lane(c->vu0_vf[5], 1)));
                setVf(*c, 3, 0x7F800000u, ONE, ONE, ONE);
                setVf(*c, 5, 0x00000000u, ONE, ONE, ONE);
                E52_VMUL(c.get());
                t.IsTrue(lane(c->vu0_vf[4], 0) == 0x00000000u, "[deferred G2] vmul 0x7f800000*0: PCSX2 +0, got " + hex(lane(c->vu0_vf[4], 0)));
            }
        });

        tc.Run("VU0 VMUL flushes denormal results to signed zero (VU_MAC_UPDATE exp 0)", [](TestCase &t)
        {
            ps2_fpmode::ScopedPs2Mode fpMode;
            auto c = freshCtx();
            setVf(*c, 3, 0x1E3CE508u, 0x9E3CE508u, ONE, ONE);
            setVf(*c, 5, 0x1E3CE508u, 0x1E3CE508u, ONE, ONE);
            E52_VMUL(c.get());
            t.IsTrue(lane(c->vu0_vf[4], 0) == 0x00000000u, "vmul 1e-20^2: PCSX2 +0, got " + hex(lane(c->vu0_vf[4], 0)));
            t.IsTrue(lane(c->vu0_vf[4], 1) == 0x80000000u, "vmul -1e-20*1e-20: PCSX2 -0, got " + hex(lane(c->vu0_vf[4], 1)));
        });

        tc.Run("VU0 VMADDw/VMADDAz: product rounded before the add; chop on the add", [](TestCase &t)
        {
            ps2_fpmode::ScopedPs2Mode fpMode;
            auto c = freshCtx();
            c->vu0_acc = _mm_castsi128_ps(_mm_set_epi32(0, 0, 0, static_cast<int>(0xBF801000u)));
            setVf(*c, 6, 0x3F800800u, 0, 0, 0);
            setVf(*c, 4, 0, 0, 0, 0x3F800800u);
            E52_VMADDW(c.get()); // vmaddw.x vf4, vf6, vf4w
            t.IsTrue(lane(c->vu0_vf[4], 0) == 0x00000000u, "vmaddw.x unfused: PCSX2 +0, got " + hex(lane(c->vu0_vf[4], 0)));
            c->vu0_acc = _mm_castsi128_ps(_mm_set_epi32(0, 0, 0, static_cast<int>(ONE)));
            setVf(*c, 6, 0x33C00000u, 0, 0, 0);
            setVf(*c, 4, 0, 0, ONE, 0);
            E52_VMADDAZ(c.get()); // vmaddaz.x ACC, vf6, vf4z
            t.IsTrue(lane(c->vu0_acc, 0) == 0x3F800000u, "vmaddaz.x 1 + 1.5*2^-24: PCSX2 0x3f800000, got " + hex(lane(c->vu0_acc, 0)));
        });

        tc.Run("VU0 VOPMULA/VOPMSUB lane mapping (sanity)", [](TestCase &t)
        {
            ps2_fpmode::ScopedPs2Mode fpMode;
            auto c = freshCtx();
            setVf(*c, 4, ub(1.0f), ub(2.0f), ub(3.0f), 0);
            setVf(*c, 5, ub(5.0f), ub(7.0f), ub(11.0f), 0);
            E52_VOPMULA(c.get()); // ACC = (4.y*5.z, 4.z*5.x, 4.x*5.y) = (22, 15, 7)
            t.IsTrue(lane(c->vu0_acc, 0) == ub(22.0f) && lane(c->vu0_acc, 1) == ub(15.0f) && lane(c->vu0_acc, 2) == ub(7.0f),
                     "vopmula lanes");
            E52_VOPMSUB(c.get()); // vf6 = ACC - (5.y*4.z, 5.z*4.x, 5.x*4.y) = (22-21, 15-11, 7-10)
            t.IsTrue(lane(c->vu0_vf[6], 0) == ub(1.0f) && lane(c->vu0_vf[6], 1) == ub(4.0f) && lane(c->vu0_vf[6], 2) == ub(-3.0f),
                     "vopmsub lanes");
        });

        tc.Run("VU0 VMAX/VMINI compare as sign-magnitude integers (fp_max/fp_min)", [](TestCase &t)
        {
            ps2_fpmode::ScopedPs2Mode fpMode;
            auto c = freshCtx();
            setVf(*c, 2, 0x80000000u, 0xFFC00000u, 0x00000000u, 0x7F800000u);
            setVf(*c, 3, 0x00000000u, ONE, 0x80000000u, ONE);
            E52_VMAX(c.get()); // vf6
            t.IsTrue(lane(c->vu0_vf[6], 0) == 0x00000000u, "vmax(-0,+0): PCSX2 +0, got " + hex(lane(c->vu0_vf[6], 0)));
            t.IsTrue(lane(c->vu0_vf[6], 1) == ONE, "vmax(0xffc00000,1): PCSX2 1.0, got " + hex(lane(c->vu0_vf[6], 1)));
            t.IsTrue(lane(c->vu0_vf[6], 3) == 0x7F800000u, "vmax(0x7f800000,1): PCSX2 0x7f800000, got " + hex(lane(c->vu0_vf[6], 3)));
            E52_VMINI(c.get()); // vf7
            t.IsTrue(lane(c->vu0_vf[7], 2) == 0x80000000u, "vmini(+0,-0): PCSX2 -0, got " + hex(lane(c->vu0_vf[7], 2)));
            t.IsTrue(lane(c->vu0_vf[7], 1) == 0xFFC00000u, "vmini(0xffc00000,1): PCSX2 0xffc00000, got " + hex(lane(c->vu0_vf[7], 1)));
        });

        tc.Run("VU0 VFTOI0 saturates by sign (floatToInt)", [](TestCase &t)
        {
            ps2_fpmode::ScopedPs2Mode fpMode;
            auto c = freshCtx();
            setVf(*c, 3, 0x4F32D05Eu /* 3e9 */, 0xCF32D05Eu /* -3e9 */, 0x7F800000u, 0x3FC00000u /* 1.5 */);
            E52_VFTOI0(c.get()); // vf8
            t.IsTrue(lane(c->vu0_vf[8], 0) == 0x7FFFFFFFu, "vftoi0 3e9: PCSX2 0x7fffffff, got " + hex(lane(c->vu0_vf[8], 0)));
            t.IsTrue(lane(c->vu0_vf[8], 1) == 0x80000000u, "vftoi0 -3e9: PCSX2 0x80000000, got " + hex(lane(c->vu0_vf[8], 1)));
            t.IsTrue(lane(c->vu0_vf[8], 2) == 0x7FFFFFFFu, "vftoi0 0x7f800000: PCSX2 0x7fffffff, got " + hex(lane(c->vu0_vf[8], 2)));
            t.IsTrue(lane(c->vu0_vf[8], 3) == 1u, "vftoi0 1.5 -> 1");
        });

        tc.Run("VU0 VITOF0 rounds toward zero above 2^24 (intToFloat)", [](TestCase &t)
        {
            ps2_fpmode::ScopedPs2Mode fpMode;
            auto c = freshCtx();
            setVf(*c, 1, 16777219u, 0x7FFFFFFFu, static_cast<uint32_t>(-16777219), 0x12345678u);
            E52_VITOF0(c.get()); // vitof0.xyz vf1, vf1
            t.IsTrue(lane(c->vu0_vf[1], 0) == 0x4B800001u, "vitof0 16777219: PCSX2 0x4b800001, got " + hex(lane(c->vu0_vf[1], 0)));
            t.IsTrue(lane(c->vu0_vf[1], 1) == 0x4EFFFFFFu, "vitof0 0x7fffffff: PCSX2 0x4effffff, got " + hex(lane(c->vu0_vf[1], 1)));
            t.IsTrue(lane(c->vu0_vf[1], 2) == 0xCB800001u, "vitof0 -16777219: PCSX2 0xcb800001, got " + hex(lane(c->vu0_vf[1], 2)));
            t.IsTrue(lane(c->vu0_vf[1], 3) == 0x12345678u, "vitof0.xyz leaves w");
        });

        tc.Run("VU0 VDIV (0x4a6303bc, 85 sites): x/0 = +/-FMAX, chop rounding (_vuDIV)", [](TestCase &t)
        {
            ps2_fpmode::ScopedPs2Mode fpMode;
            auto q = [](uint32_t ftx) {
                auto c = freshCtx();
                setVf(*c, 3, ftx, 0, 0, 0);
                E52_VDIV(c.get()); // Q = vf0.w / vf3.x
                return ub(c->vu0_q);
            };
            t.IsTrue(q(0x00000000u) == FMAX, "1 / +0: PCSX2 +FMAX, got " + hex(q(0x00000000u)));
            t.IsTrue(q(0x80000000u) == NFMAX, "1 / -0: PCSX2 -FMAX, got " + hex(q(0x80000000u)));
            t.IsTrue(q(0x00000001u) == FMAX, "1 / denormal: PCSX2 +FMAX, got " + hex(q(0x00000001u)));
            t.IsTrue(q(0x40400000u) == 0x3EAAAAAAu, "1 / 3: PCSX2 0x3eaaaaaa (chop), got " + hex(q(0x40400000u)));
        });

        tc.Run("VU0 VSQRT (0x4a0403bd, 285 sites): sqrt(|ft|), chop rounding (_vuSQRT)", [](TestCase &t)
        {
            ps2_fpmode::ScopedPs2Mode fpMode;
            auto q = [](uint32_t ftx) {
                auto c = freshCtx();
                setVf(*c, 4, ftx, 0, 0, 0);
                E52_VSQRT(c.get());
                return ub(c->vu0_q);
            };
            t.IsTrue(q(0xC0800000u) == 0x40000000u, "vsqrt -4: PCSX2 2.0, got " + hex(q(0xC0800000u)));
            t.IsTrue(q(0x40A00000u) == 0x400F1BBCu, "vsqrt 5: PCSX2 0x400f1bbc, got " + hex(q(0x40A00000u)));
            t.IsTrue(q(0x40800000u) == 0x40000000u, "vsqrt 4 = 2 (sanity)");
        });

        tc.Run("VU0 VRSQRT (0x4a6403be, 104 sites): Q = fs/sqrt(|ft|), ft = 0 gives +/-FMAX (_vuRSQRT)", [](TestCase &t)
        {
            ps2_fpmode::ScopedPs2Mode fpMode;
            auto q = [](uint32_t ftx) {
                auto c = freshCtx();
                setVf(*c, 4, ftx, 0, 0, 0);
                E52_VRSQRT(c.get()); // Q = vf0.w / sqrt(vf4.x)
                return ub(c->vu0_q);
            };
            t.IsTrue(q(0x40800000u) == 0x3F000000u, "1/sqrt(4) = 0.5 (sanity)");
            t.IsTrue(q(0x00000000u) == FMAX, "1/sqrt(+0): PCSX2 +FMAX, got " + hex(q(0x00000000u)));
            t.IsTrue(q(0xC0800000u) == 0x3F000000u, "1/sqrt(-4): PCSX2 0.5 (|ft|), got " + hex(q(0xC0800000u)));
        });

        tc.Run("VU0 VRSQRT reads fs (synthetic 0x4a020bbe: Q = vf1.x / sqrt(vf2.x))", [](TestCase &t)
        {
            ps2_fpmode::ScopedPs2Mode fpMode;
            auto c = freshCtx();
            setVf(*c, 1, ub(6.0f), 0, 0, 0);
            setVf(*c, 2, ub(4.0f), 0, 0, 0);
            E52_VRSQRT_FS(c.get());
            t.IsTrue(ub(c->vu0_q) == ub(3.0f), "6/sqrt(4): PCSX2 3.0, got " + hex(ub(c->vu0_q)));
        });

        tc.Run("VU0 VCLIPw flag bits: +x=bit0, -x=bit1, ..., against |ft.w| (_vuCLIP)", [](TestCase &t)
        {
            ps2_fpmode::ScopedPs2Mode fpMode;
            auto c = freshCtx();
            setVf(*c, 1, ub(2.0f), ub(-2.0f), ub(0.5f), 0);
            setVf(*c, 2, 0, 0, 0, ub(1.0f));
            E52_VCLIPW(c.get());
            t.IsTrue((c->vu0_clip_flags & 0x3F) == 0x09u, "clip (2,-2,0.5) vs w=1: PCSX2 0x09 (+x, -y), got " + hex(c->vu0_clip_flags & 0x3F));
            c->vu0_clip_flags = 0;
            setVf(*c, 2, 0, 0, 0, ub(-1.0f));
            E52_VCLIPW(c.get());
            t.IsTrue((c->vu0_clip_flags & 0x3F) == 0x09u, "clip vs w=-1 uses |w|: PCSX2 0x09, got " + hex(c->vu0_clip_flags & 0x3F));
        });

        tc.Run("VU0 VRINIT/VRNEXT: R = 0x3f800000|23 bits; RNEXT advances the LFSR and writes ft", [](TestCase &t)
        {
            ps2_fpmode::ScopedPs2Mode fpMode;
            auto c = freshCtx();
            setVf(*c, 3, 0x12345678u, 0, 0, 0);
            E52_VRINIT(c.get()); // R = 0x3fb45678
            setVf(*c, 3, 0, 0, 0, 0);
            E52_VRNEXT(c.get()); // vrnext.x vf3
            t.IsTrue(lane(c->vu0_vf[3], 0) == 0x3FE8ACF1u, "vrnext.x after rinit 0x12345678: PCSX2 0x3fe8acf1, got " + hex(lane(c->vu0_vf[3], 0)));
        });

        tc.Run("COP2 CTC2/CFC2 R keeps 23 bits (VU0.cpp CTC2/CFC2 REG_R)", [](TestCase &t)
        {
            ps2_fpmode::ScopedPs2Mode fpMode;
            auto c = freshCtx();
            SET_GPR_U32(c.get(), 2, 0xFFFFFFFFu);
            E52_CTC2_R(c.get());
            SET_GPR_U32(c.get(), 2, 0u);
            E52_CFC2_R(c.get());
            t.IsTrue(GPR_U32(c.get(), 2) == 0x007FFFFFu, "ctc2 R=~0 then cfc2: PCSX2 0x007fffff, got " + hex(GPR_U32(c.get(), 2)));
        });

        tc.Run("COP2 CFC2 VI1 zero-extends 16 bits (sanity)", [](TestCase &t)
        {
            ps2_fpmode::ScopedPs2Mode fpMode;
            auto c = freshCtx();
            c->vi[1] = 0xFFFFu;
            E52_CFC2_VI1(c.get());
            t.IsTrue(GPR_U64(c.get(), 3) == 0xFFFFull, "cfc2 vi1=0xffff -> 0xffff");
        });

        tc.Run("COP2 VSQI/VLQI/VILWR/VISWR address VU0 data memory with the right fields (_vuSQI/_vuLQI/_vuILWR/_vuISWR)", [](TestCase &t)
        {
            ps2_fpmode::ScopedPs2Mode fpMode;
            PS2Runtime runtime;
            t.IsTrue(runtime.memory().initialize(), "PS2Memory initialize");
            uint8_t *vu0 = runtime.memory().getVU0Data();
            uint8_t *rdram = runtime.memory().getRDRAM();
            t.IsTrue(vu0 != nullptr && rdram != nullptr, "VU0 data and RDRAM exist");
            if (!vu0 || !rdram)
                return;
            std::memset(vu0, 0, PS2_VU0_DATA_SIZE);
            std::memset(rdram, 0, 0x4000);
            auto c = freshCtx();
            c->vi[1] = 3;
            c->vi[2] = 7;
            setVf(*c, 1, 0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u);
            setVf(*c, 2, 0xA0000001u, 0xA0000002u, 0xA0000003u, 0xA0000004u);
            E52_VSQI(c.get(), &runtime, rdram); // vsqi.xyzw vf2, (vi1++)
            uint32_t w[4];
            std::memcpy(w, vu0 + 3 * 16, sizeof(w));
            t.IsTrue(w[0] == 0xA0000001u && w[1] == 0xA0000002u && w[2] == 0xA0000003u && w[3] == 0xA0000004u,
                     "vsqi stores vf2 at VU0 data qword vi1 = 3, got " + hex(w[0]));
            t.IsTrue(c->vi[1] == 4 && c->vi[2] == 7, "vsqi post-increments vi1 only");
            bool rdramClean = true;
            for (uint32_t i = 0; i < 0x4000; ++i)
                rdramClean = rdramClean && rdram[i] == 0;
            t.IsTrue(rdramClean, "vsqi must not write EE RAM 0x0000-0x3fff");
            c->vi[1] = 3;
            E52_VLQI(c.get(), &runtime, rdram); // vlqi.xyzw vf1, (vi1++)
            t.IsTrue(lane(c->vu0_vf[1], 0) == 0xA0000001u && lane(c->vu0_vf[1], 3) == 0xA0000004u,
                     "vlqi loads VU0 data qword vi1 = 3 into vf1");
            t.IsTrue(c->vi[1] == 4, "vlqi post-increments vi1");
            c->vi[1] = 3;
            E52_VILWR_Y(c.get(), &runtime, rdram); // vilwr.y vi2, (vi1)
            t.IsTrue(c->vi[2] == 0x0002u, "vilwr.y reads the low 16 bits of word y, got " + hex(c->vi[2]));
            c->vi[2] = 0xBEEF;
            E52_VISWR_Z(c.get(), &runtime, rdram); // viswr.z vi2, (vi1)
            std::memcpy(w, vu0 + 3 * 16, sizeof(w));
            t.IsTrue(w[2] == 0x0000BEEFu && w[1] == 0xA0000002u, "viswr.z stores vi2 zero-extended into word z only, got " + hex(w[2]));
        });

        tc.Run("COP2 VCALLMSR (0x4a00d839, 8 sites) starts at CMSAR0 (COP2.cpp: vu0ExecMicro(VI[CMSAR0]))", [](TestCase &t)
        {
            ps2_fpmode::ScopedPs2Mode fpMode;
            const std::string code = translate(0x1223f0u, 0x4a00d839u);
            t.IsTrue(code.find("vu0_cmsar0") != std::string::npos,
                     "vcallmsr must take its start from CMSAR0 (ctx->vu0_cmsar0); got: " + code);
            t.IsTrue(code.find("vi[27]") == std::string::npos,
                     "vcallmsr reads ctx->vi[27], past the 16-entry VI array; got: " + code);
        });

        tc.Run("COP2 VSQI (0x4be1137d, 14 sites) stores vf[fs] to VU0 data at vi[it]*16 (_vuSQI)", [](TestCase &t)
        {
            ps2_fpmode::ScopedPs2Mode fpMode;
            // vsqi.xyzw $vf2, ($vi1++): fs = 2 (bits 15:11), it = 1 (bits 20:16).
            const std::string code = translate(0x229fb0u, 0x4be1137du);
            t.IsTrue(code.find("vu0_vf[2]") != std::string::npos && code.find("vi[1]") != std::string::npos &&
                         code.find("vi[2]") == std::string::npos,
                     "vsqi must store vf2 at vi1 and post-increment vi1; got: " + code);
            t.IsTrue(code.find("WRITE128(addr") == std::string::npos,
                     "vsqi must target VU0 data memory, not EE address vi*16 via WRITE128; got: " + code);
        });

        tc.Run("COP2 CTC2 CMSAR1 (0x48c4f800, 1 site) starts a VU1 microprogram (VU0.cpp CTC2 REG_CMSAR1)", [](TestCase &t)
        {
            ps2_fpmode::ScopedPs2Mode fpMode;
            const std::string code = translate(0x3fed54u, 0x48c4f800u);
            t.IsTrue(code.find("runtime->") != std::string::npos,
                     "ctc2 to CMSAR1 must start VU1 at the written address; got: " + code);
        });
    });
}
