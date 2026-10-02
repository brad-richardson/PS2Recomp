#include "MiniTest.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"
#include "ps2recomp/code_generator.h"
#include "ps2recomp/instructions.h"
#include "ps2recomp/types.h"

#include <array>
#include <string>

using namespace ps2recomp;

namespace
{
    std::string emitCop0(uint32_t format, uint32_t reg, uint32_t rt)
    {
        CodeGenerator generator({}, {});
        Instruction inst{};
        inst.opcode = OPCODE_COP0;
        inst.rs = format;
        inst.rd = reg;
        inst.rt = rt;
        return generator.translateInstruction(inst);
    }
}

void register_ps2_ee_count_tests()
{
    MiniTest::Case("Ps2EeCount", [](TestCase &tc)
    {
        tc.Run("shared cycle clock, write epoch and wrap", [](TestCase &t)
        {
            PS2Runtime runtime;
            R5900Context first{};
            R5900Context second{};
            std::array<uint8_t, 16> ram{};
            runtime.eeScheduler().reset(ram.data(), first);

            runtime.eeScheduler().accountCycles(100);
            t.Equals(runtime.readEeCount(&first), 0x64u, "cycle 100");
            runtime.eeScheduler().accountCycles(40);
            t.Equals(runtime.readEeCount(&first), 0x8cu, "cycle 140");
            runtime.eeScheduler().accountCycles(60);
            runtime.writeEeCount(&first, 0xfffffff0u);
            t.Equals(first.cop0_count, 0xfffffff0u, "write mirror");
            runtime.eeScheduler().accountCycles(40);
            t.Equals(runtime.readEeCount(&second), 0x18u, "cycle 240 and u32 wrap across contexts");
            t.Equals(second.cop0_count, 0x18u, "second context mirror");

            t.Equals(runtime.readEeCount(&first), 0x19u, "same cycle first repeat");
            t.Equals(runtime.readEeCount(&first), 0x1au, "same cycle second repeat");
            const uint64_t zeroBefore = GPR_U64((&first), 0);
            (void)runtime.readEeCount(&first);
            t.Equals(runtime.readEeCount(&second), 0x1cu, "MFC0 to zero still charged one tick");
            t.Equals(GPR_U64((&first), 0), zeroBefore, "zero register stays zero");

            runtime.eeScheduler().accountCycles(80); // same accounting path used by idle fast-forward
            t.Equals(runtime.readEeCount(&second), 0x6cu, "idle-style cycle charge");
        });

        tc.Run("Count emitter calls and neighboring COP0 emissions", [](TestCase &t)
        {
            t.Equals(emitCop0(COP0_MF, COP0_REG_COUNT, 5),
                     std::string("SET_GPR_S32(ctx, 5, (int32_t)runtime->readEeCount(ctx));"),
                     "MFC0 Count helper");
            t.Equals(emitCop0(COP0_MF, COP0_REG_COUNT, 0),
                     std::string("(void)runtime->readEeCount(ctx);"),
                     "MFC0 Count zero still evaluates helper");
            t.Equals(emitCop0(COP0_MT, COP0_REG_COUNT, 7),
                     std::string("runtime->writeEeCount(ctx, GPR_U32(ctx, 7));"),
                     "MTC0 Count helper");
            t.Equals(emitCop0(COP0_MF, COP0_REG_COMPARE, 5),
                     std::string("SET_GPR_S32(ctx, 5, (int32_t)ctx->cop0_compare);"),
                     "MFC0 Compare unchanged");
            t.Equals(emitCop0(COP0_MT, COP0_REG_COMPARE, 7),
                     std::string("ctx->cop0_compare = GPR_U32(ctx, 7); ctx->cop0_cause &= ~0x8000;"),
                     "MTC0 Compare and Cause unchanged");
            t.Equals(emitCop0(COP0_MT, COP0_REG_CAUSE, 7),
                     std::string("ctx->cop0_cause = (ctx->cop0_cause & ~0x00000300) | (GPR_U32(ctx, 7) & 0x00000300);"),
                     "MTC0 Cause unchanged");
        });
    });
}
