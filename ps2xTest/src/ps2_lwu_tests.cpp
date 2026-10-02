#include "MiniTest.h"
#include "ps2_runtime_macros.h"
#include "e54d_lwu_snippets.h"

#include <array>
#include <cstdint>
#include <cstring>

namespace
{
    constexpr uint64_t upper = 0x1122334455667788ull;

    void seed(R5900Context &ctx)
    {
        ctx.r[4] = _mm_set_epi64x(0, 0x10);
        ctx.r[5] = _mm_set_epi64x(static_cast<int64_t>(upper), 0xdeadbeefdeadbeefull);
        ctx.r[0] = _mm_setzero_si128();
    }

    uint64_t high64(const R5900Context &ctx, int reg)
    {
        return static_cast<uint64_t>(PS2_EXTRACT_EPI64_1(ctx.r[reg]));
    }

    void writeWord(std::array<uint8_t, 32> &ram, uint32_t word)
    {
        std::memcpy(ram.data() + 0x14, &word, sizeof(word));
    }
}

void register_ps2_lwu_tests()
{
    MiniTest::Case("Ps2Lwu", [](TestCase &tc)
    {
        tc.Run("decoded LWU zero extends high-bit word and preserves upper half", [](TestCase &t)
        {
            for (const uint32_t word : {0x80000000u, 0xffffffffu, 0x12345678u})
            {
                R5900Context ctx{};
                std::array<uint8_t, 32> ram{};
                seed(ctx);
                writeWord(ram, word);
                E54D_LWU(&ctx, ram.data());
                t.Equals(GPR_U64((&ctx), 5), static_cast<uint64_t>(word), "LWU low 64");
                t.Equals(high64(ctx, 5), upper, "LWU upper 64 preserved");
            }
        });
        tc.Run("decoded LW still sign extends", [](TestCase &t)
        {
            R5900Context ctx{};
            std::array<uint8_t, 32> ram{};
            seed(ctx);
            writeWord(ram, 0x80000000u);
            E54D_LW(&ctx, ram.data());
            t.Equals(GPR_U64((&ctx), 5), 0xffffffff80000000ull, "LW low 64");
            t.Equals(high64(ctx, 5), upper, "LW upper 64 preserved");
        });
        tc.Run("decoded LBU and LHU remain unsigned", [](TestCase &t)
        {
            R5900Context ctx{};
            std::array<uint8_t, 32> ram{};
            seed(ctx);
            writeWord(ram, 0x00008080u);
            E54D_LBU(&ctx, ram.data());
            t.Equals(GPR_U64((&ctx), 5), 0x80ull, "LBU low 64");
            t.Equals(high64(ctx, 5), upper, "LBU upper 64 preserved");
            E54D_LHU(&ctx, ram.data());
            t.Equals(GPR_U64((&ctx), 5), 0x8080ull, "LHU low 64");
            t.Equals(high64(ctx, 5), upper, "LHU upper 64 preserved");
        });
        tc.Run("decoded LWU to zero register stays zero", [](TestCase &t)
        {
            R5900Context ctx{};
            std::array<uint8_t, 32> ram{};
            seed(ctx);
            writeWord(ram, 0xffffffffu);
            E54D_LWU_ZERO(&ctx, ram.data());
            t.Equals(GPR_U64((&ctx), 0), 0ull, "$zero low 64");
            t.Equals(high64(ctx, 0), 0ull, "$zero upper 64");
        });
    });
}
