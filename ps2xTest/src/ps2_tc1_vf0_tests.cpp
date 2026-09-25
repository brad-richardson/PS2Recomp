// TC1: VU0 vf0 is hardwired to (0,0,0,1); the emitter must not emit stores to ctx->vu0_vf[0].
//
// Hardware ignores writes to vf0 (PCSX2 VU0.cpp: LQC2 still performs the memory read into a
// dummy, QMTC2 returns early for _Fs_ == 0; VUmicroMem.cpp vuMemReset pins VF[0] = (0,0,0,1)).
// SSX 3's VU0 save/restore (sub_003FE828) executes `lqc2 $vf0, 0($s0)` at 0x3fe9bc, which the
// old emitter translated into a real store.
#include "MiniTest.h"
#include "ps2recomp/code_generator.h"
#include "ps2recomp/instructions.h"
#include "ps2recomp/r5900_decoder.h"
#include "ps2recomp/types.h"
#include "ps2recomp/Translators/instruction_translator.h"
#include "ps2recomp/Translators/vu_translator.h"

#include <string>
#include <vector>

using namespace ps2recomp;

namespace
{
    bool emitsVfStore(const std::string &code, int reg)
    {
        return code.find("vu0_vf[" + std::to_string(reg) + "] =") != std::string::npos;
    }

    CodeGenerator makeGenerator()
    {
        static const std::vector<Symbol> symbols;
        static const std::vector<Section> sections;
        return CodeGenerator(symbols, sections);
    }
}

void register_ps2_tc1_vf0_tests()
{
    MiniTest::Case("TC1Vf0ReadOnly", [](TestCase &tc)
                   {
        tc.Run("TC1: LQC2 to vf0 keeps the load but drops the store", [](TestCase &t) {
            R5900Decoder decoder;
            // SSX 3 sub_003FE828: lqc2 $vf0, 0x0($s0) at 0x3fe9bc, then lqc2 $vf1 at 0x3fe9c0.
            const Instruction toVf0 = decoder.decodeInstruction(0x3fe9bc, 0xda000000, false);
            const Instruction toVf1 = decoder.decodeInstruction(0x3fe9c0, 0xda010010, false);
            t.Equals(toVf0.opcode, static_cast<uint32_t>(OPCODE_LDC2), "0xda000000 must decode as LQC2");
            t.Equals(toVf0.rt, 0u, "rt of the SSX3 encoding must be vf0");
            CodeGenerator cg = makeGenerator();
            InstructionTranslator translator(cg);
            const MemoryAccessHint hint{};
            const std::string vf0 = translator.translate(toVf0, hint);
            t.IsFalse(emitsVfStore(vf0, 0), "LQC2 $vf0 must not emit a store to vu0_vf[0]: " + vf0);
            t.IsTrue(vf0.find("READ128") != std::string::npos,
                     "LQC2 $vf0 must still emit the memory read (PCSX2 reads into a dummy): " + vf0);
            const std::string vf1 = translator.translate(toVf1, hint);
            t.IsTrue(emitsVfStore(vf1, 1), "LQC2 $vf1 must still emit its store: " + vf1);
        });

        tc.Run("TC1: QMTC2 to vf0 is skipped", [](TestCase &t) {
            CodeGenerator cg = makeGenerator();
            VuTranslator translator(cg);
            Instruction toVf0{};
            toVf0.rs = COP2_QMTC2;
            toVf0.rt = 2;
            toVf0.rd = 0;
            const std::string vf0 = translator.translate(toVf0);
            t.IsFalse(emitsVfStore(vf0, 0), "QMTC2 $vf0 must not emit a store to vu0_vf[0]: " + vf0);
            Instruction toVf1 = toVf0;
            toVf1.rd = 1;
            const std::string vf1 = translator.translate(toVf1);
            t.IsTrue(emitsVfStore(vf1, 1), "QMTC2 $vf1 must still emit its store: " + vf1);
        });

        tc.Run("TC1: VU macro ops with dest vf0 emit no store", [](TestCase &t) {
            CodeGenerator cg = makeGenerator();
            // sa-dest convention (VADD family, MADD family, OPM...): VADD + VMADDq.
            Instruction vadd{};
            vadd.sa = 0;
            vadd.rd = 1;
            vadd.rt = 2;
            vadd.vectorInfo.vectorField = 0xF;
            t.IsFalse(emitsVfStore(cg.translateVU_VADD(vadd), 0), "VADD $vf0 must not emit a store");
            vadd.sa = 1;
            t.IsTrue(emitsVfStore(cg.translateVU_VADD(vadd), 1), "VADD $vf1 must still emit its store");
            Instruction maddq = vadd;
            maddq.sa = 0;
            t.IsFalse(emitsVfStore(cg.translateVU_VMADDq(maddq), 0), "VMADDq $vf0 must not emit a store");
            maddq.sa = 3;
            t.IsTrue(emitsVfStore(cg.translateVU_VMADDq(maddq), 3), "VMADDq $vf3 must still emit its store");
            // rt-dest convention: VITOF + VMFIR.
            Instruction vitof{};
            vitof.rt = 0;
            vitof.rd = 1;
            vitof.vectorInfo.vectorField = 0xF;
            t.IsFalse(emitsVfStore(cg.translateVU_VITOF(vitof, 0), 0), "VITOF $vf0 must not emit a store");
            vitof.rt = 4;
            t.IsTrue(emitsVfStore(cg.translateVU_VITOF(vitof, 0), 4), "VITOF $vf4 must still emit its store");
            Instruction vmfir{};
            vmfir.rt = 0;
            vmfir.rd = 1;
            vmfir.vectorInfo.vectorField = 0xF;
            t.IsFalse(emitsVfStore(cg.translateVU_VMFIR(vmfir), 0), "VMFIR $vf0 must not emit a store");
            vmfir.rt = 5;
            t.IsTrue(emitsVfStore(cg.translateVU_VMFIR(vmfir), 5), "VMFIR $vf5 must still emit its store");
        }); });
}
