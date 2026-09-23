// E50: R5900 FPU semantics the SSX 3 camera depends on.
//
// SSX 3's sincos (guest 0x31be50) range-reduces with
//   q = CVT.W.S(a * 2/pi +/- 0.5)       (EE CVT.W.S truncates toward zero)
// and forms cos(x) as
//   SQRT.S $f1, $f5   with f5 = 1 - sin(x)^2   (0x46050044; EE SQRT.S reads
//                                               ft and takes sqrt(|ft|))
// References: PCSX2 pcsx2/FPU.cpp CVT_W ((s32) cast, saturating) and SQRT_S
// (sqrt(fabs(Ft)), +/-0 preserved). The camera yaw at Select Character is
// pi/2 (T65 / E50 Part 1).
#include "MiniTest.h"
#include "ps2recomp/code_generator.h"
#include "ps2recomp/instructions.h"
#include "ps2recomp/r5900_decoder.h"
#include "ps2recomp/types.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

using namespace ps2recomp;

namespace
{
    float fbits(uint32_t u)
    {
        float f;
        std::memcpy(&f, &u, sizeof(f));
        return f;
    }

    // Register index the generator reads for the SQRT.S at guest 0x31beec.
    int generatedSqrtOperand()
    {
        R5900Decoder decoder;
        CodeGenerator generator({}, {});
        const Instruction inst = decoder.decodeInstruction(0x31beecu, 0x46050044u);
        const std::string code = generator.translateInstruction(inst);
        const std::string key = "FPU_SQRT_S(";
        const size_t at = code.find(key);
        if (at == std::string::npos)
            return -1;
        const size_t lb = code.find("ctx->f[", at);
        if (lb == std::string::npos)
            return -1;
        return std::atoi(code.c_str() + lb + 7);
    }

    struct SinCos
    {
        float s;
        float c;
    };

    // Replay of guest 0x31be50 using the runtime's FPU macros and the
    // generator's SQRT.S operand choice (the generated code's semantics).
    SinCos replayGuestSinCos(float angle, int sqrtOperand)
    {
        float f[32] = {};
        const float K0 = fbits(0x3f22f983u); // 2/pi
        const float K1 = fbits(0x3fc90fdbu); // pi/2
        const float K2 = fbits(0x3638ef1fu);
        const float K3 = fbits(0xb9500d03u);
        const float K4 = fbits(0x3c088889u);
        const float K5 = fbits(0xbe2aaaabu);
        f[12] = angle;
        f[0] = 0.0f;
        const bool neg = f[12] < f[0];
        f[1] = FPU_MUL_S(f[12], K0);
        f[0] = 0.5f;
        f[1] = neg ? FPU_SUB_S(f[1], f[0]) : FPU_ADD_S(f[1], f[0]);
        f[0] = K1;
        const int32_t q = FPU_CVT_W_S(f[1]);
        f[1] = FPU_CVT_S_W(q);
        f[6] = K2;
        const int32_t quad = q & 3;
        f[0] = FPU_MUL_S(f[1], f[0]);
        f[2] = K3;
        f[3] = K4;
        f[4] = K5;
        f[12] = FPU_SUB_S(f[12], f[0]);
        f[5] = 1.0f;
        f[1] = FPU_MUL_S(f[12], f[12]);
        f[6] = FPU_MUL_S(f[1], f[6]);
        f[2] = FPU_ADD_S(f[6], f[2]);
        f[6] = FPU_MUL_S(f[2], f[1]);
        f[3] = FPU_ADD_S(f[6], f[3]);
        f[6] = FPU_MUL_S(f[3], f[1]);
        f[4] = FPU_ADD_S(f[6], f[4]);
        f[6] = FPU_MUL_S(f[4], f[1]);
        f[0] = FPU_ADD_S(f[6], f[5]);
        f[6] = FPU_MUL_S(f[0], f[12]); // sin(x)
        f[1] = FPU_MUL_S(f[6], f[6]);
        f[5] = FPU_SUB_S(f[5], f[1]); // 1 - sin(x)^2
        f[1] = FPU_SQRT_S(f[sqrtOperand < 0 ? 0 : sqrtOperand]); // cos(x)
        switch (quad)
        {
        case 0:
            return {f[6], f[1]};
        case 1:
            return {f[1], -f[6]};
        case 2:
            return {-f[6], -f[1]};
        default:
            return {-f[1], f[6]};
        }
    }
}

void register_ps2_fpu_semantics_tests()
{
    MiniTest::Case("Ps2FpuSemantics", [](TestCase &tc)
    {
        tc.Run("SQRT.S reads ft (0x46050044 = sqrt.s $f1, $f5)", [](TestCase &t)
        {
            R5900Decoder decoder;
            CodeGenerator generator({}, {});
            const Instruction inst = decoder.decodeInstruction(0x31beecu, 0x46050044u);
            const std::string code = generator.translateInstruction(inst);
            t.IsTrue(code.find("ctx->f[1] = FPU_SQRT_S(ctx->f[5]);") != std::string::npos,
                     "SQRT.S must take its operand from ft (f5), got: " + code);
        });

        tc.Run("SQRT.S takes sqrt(|ft|) and keeps signed zero", [](TestCase &t)
        {
            t.IsTrue(FPU_SQRT_S(4.0f) == 2.0f, "sqrt(4) = 2");
            t.IsTrue(FPU_SQRT_S(-4.0f) == 2.0f, "EE SQRT.S of a negative is sqrt(|x|), not NaN");
            t.IsTrue(!std::isnan(FPU_SQRT_S(-7.4e-6f)), "tiny negative 1 - sin^2 must not become NaN");
            t.IsTrue(std::signbit(FPU_SQRT_S(-0.0f)) && FPU_SQRT_S(-0.0f) == 0.0f, "sqrt(-0) = -0");
        });

        tc.Run("RSQRT.S computes fs / sqrt(ft)", [](TestCase &t)
        {
            // rsqrt.s $f3, $f1, $f2: fd=3, fs=1, ft=2, funct 0x16.
            const uint32_t raw = 0x46000000u | (2u << 16) | (1u << 11) | (3u << 6) | 0x16u;
            R5900Decoder decoder;
            CodeGenerator generator({}, {});
            const std::string code = generator.translateInstruction(decoder.decodeInstruction(0x1000u, raw));
            t.IsTrue(code.find("ctx->f[3] = FPU_RSQRT_S(ctx->f[1], ctx->f[2]);") != std::string::npos,
                     "RSQRT.S must read fs and ft, got: " + code);
            t.IsTrue(FPU_RSQRT_S(6.0f, 4.0f) == 3.0f, "6 / sqrt(4) = 3");
            t.IsTrue(FPU_RSQRT_S(6.0f, -4.0f) == 3.0f, "negative ft uses |ft|");
            t.IsTrue(FPU_RSQRT_S(1.0f, 0.0f) == fbits(0x7F7FFFFFu), "ft = +0 gives +FMAX");
            t.IsTrue(FPU_RSQRT_S(1.0f, -0.0f) == fbits(0xFF7FFFFFu), "ft = -0 gives -FMAX");
        });

        tc.Run("CVT.W.S truncates toward zero", [](TestCase &t)
        {
            t.Equals(static_cast<int>(FPU_CVT_W_S(1.5f)), 1, "1.5 -> 1 (EE truncates)");
            t.Equals(static_cast<int>(FPU_CVT_W_S(-1.5f)), -1, "-1.5 -> -1");
            t.Equals(static_cast<int>(FPU_CVT_W_S(2.7f)), 2, "2.7 -> 2");
            t.Equals(static_cast<int>(FPU_CVT_W_S(-0.7f)), 0, "-0.7 -> 0");
            t.Equals(static_cast<int>(FPU_CVT_W_S(3.0e9f)), 0x7FFFFFFF, "overflow saturates positive");
            t.Equals(static_cast<int>(FPU_CVT_W_S(-3.0e9f)), static_cast<int>(0x80000000u), "overflow saturates negative");
        });

        tc.Run("guest sincos 0x31be50 at the SC camera yaw (pi/2) gives cos 0", [](TestCase &t)
        {
            const int op = generatedSqrtOperand();
            const float yaw = fbits(0x3fc90fdbu); // pi/2
            const SinCos r = replayGuestSinCos(yaw, op);
            t.IsTrue(std::fabs(r.s - 1.0f) < 1e-6f, "sin(pi/2) = 1, got " + std::to_string(r.s));
            t.IsTrue(std::fabs(r.c) < 1e-6f,
                     "cos(pi/2) = 0 (the recomp produced -0.797886 -> camera R + 0.4438 I), got " +
                         std::to_string(r.c));
            const SinCos n = replayGuestSinCos(-yaw, op);
            t.IsTrue(std::fabs(n.c) < 1e-6f, "cos(-pi/2) = 0, got " + std::to_string(n.c));
            float worst = 0.0f;
            for (int i = -6283; i <= 6283; ++i)
            {
                const float a = static_cast<float>(i) * 0.001f;
                const SinCos v = replayGuestSinCos(a, op);
                worst = std::max(worst, std::fabs(v.c - std::cos(a)));
                worst = std::max(worst, std::fabs(v.s - std::sin(a)));
            }
            t.IsTrue(worst < 1e-5f, "sincos error over [-2pi, 2pi] < 1e-5, got " + std::to_string(worst));
        });
    });
}
