#include "MiniTest.h"
#include "ps2_runtime_macros.h"
#include "ps2recomp/Emitters/control_flow_emitter.h"
#include "ps2recomp/instructions.h"
#include "ps2recomp/r5900_decoder.h"

#include <array>
#include <cstdint>
#include <string>

using namespace ps2recomp;

namespace
{
    struct BranchCase
    {
        const char* name;
        uint32_t opcode;
        uint32_t rt;
        const char* predicate;
        bool likely;
        bool link;
    };

    constexpr std::array<BranchCase, 12> branches{{
        {"BLTZ", OPCODE_REGIMM, REGIMM_BLTZ, "< 0", false, false},
        {"BLTZL", OPCODE_REGIMM, REGIMM_BLTZL, "< 0", true, false},
        {"BLTZAL", OPCODE_REGIMM, REGIMM_BLTZAL, "< 0", false, true},
        {"BLTZALL", OPCODE_REGIMM, REGIMM_BLTZALL, "< 0", true, true},
        {"BGEZ", OPCODE_REGIMM, REGIMM_BGEZ, ">= 0", false, false},
        {"BGEZL", OPCODE_REGIMM, REGIMM_BGEZL, ">= 0", true, false},
        {"BGEZAL", OPCODE_REGIMM, REGIMM_BGEZAL, ">= 0", false, true},
        {"BGEZALL", OPCODE_REGIMM, REGIMM_BGEZALL, ">= 0", true, true},
        {"BLEZ", OPCODE_BLEZ, 0, "<= 0", false, false},
        {"BLEZL", OPCODE_BLEZL, 0, "<= 0", true, false},
        {"BGTZ", OPCODE_BGTZ, 0, "> 0", false, false},
        {"BGTZL", OPCODE_BGTZL, 0, "> 0", true, false},
    }};

    bool predicate(const R5900Context* ctx, int family)
    {
        switch (family)
        {
        case 0: return GPR_S64(ctx, 4) < 0;
        case 1: return GPR_S64(ctx, 4) >= 0;
        case 2: return GPR_S64(ctx, 4) <= 0;
        default: return GPR_S64(ctx, 4) > 0;
        }
    }
}

void register_ps2_signed_branch_tests()
{
    MiniTest::Case("Ps2SignedBranch", [](TestCase& tc)
    {
        tc.Run("decoded signed variants emit low-64 predicate and retain likely/link shape", [](TestCase& t)
        {
            R5900Decoder decoder;
            CodeGenerator generator({}, {});
            Function function{};
            CodeGenerator::AnalysisResult analysis{};
            for (const auto& entry : branches)
            {
                const uint32_t raw = (entry.opcode << 26) | (4u << 21) | (entry.rt << 16) | 2u;
                const Instruction branch = decoder.decodeInstruction(0x100000u, raw);
                const Instruction delay = decoder.decodeInstruction(0x100004u, 0u);
                const std::string code = ControlFlowEmitter(generator, branch, delay, function, analysis).emit();
                t.IsTrue(code.find(std::string("GPR_S64(ctx, 4) ") + entry.predicate) != std::string::npos,
                         std::string(entry.name) + " signed low-64 expression");
                t.IsFalse(code.find("GPR_S32(ctx, 4)") != std::string::npos,
                          std::string(entry.name) + " lacks low-32 expression");
                t.Equals(code.find("if (branch_taken_0x100000) {\n") != std::string::npos,
                         true, std::string(entry.name) + " retains branch block");
                t.Equals(code.find("SET_GPR_U32(ctx, 31, 0x100008u);") != std::string::npos,
                         entry.link, std::string(entry.name) + " retains link write");
                if (entry.likely)
                    t.IsTrue(code.find("if (branch_taken_0x100000) {\n") < code.find("ctx->pc = 0x10000Cu;"),
                             std::string(entry.name) + " retains likely taken block");
            }
        });
        tc.Run("compiled predicates use signed low 64 and ignore upper GPR half", [](TestCase& t)
        {
            struct Vector { uint64_t low; uint64_t upper; std::array<bool, 4> expected; };
            constexpr std::array<Vector, 6> vectors{{
                {0x0000000180000000ull, 0x1122334455667788ull, {false, true, false, true}},
                {0xffffffff00000000ull, 0x1122334455667788ull, {true, false, true, false}},
                {0, 0x1122334455667788ull, {false, true, true, false}},
                {0x000000007fffffffull, 0x1122334455667788ull, {false, true, false, true}},
                {0xffffffff80000000ull, 0x1122334455667788ull, {true, false, true, false}},
                {1, 0xffffffffffffffffull, {false, true, false, true}},
            }};
            for (const auto& vector : vectors)
            {
                R5900Context ctx{};
                ctx.r[4] = _mm_set_epi64x(static_cast<int64_t>(vector.upper), static_cast<int64_t>(vector.low));
                for (int family = 0; family < 4; ++family)
                    t.Equals(predicate(&ctx, family), vector.expected[family], "signed low-64 result");
            }
        });
    });
}
