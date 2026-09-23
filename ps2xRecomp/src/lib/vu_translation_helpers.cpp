#include "ps2recomp/code_generator.h"
#include "ps2recomp/codegen_helpers.h"
#include "ps2recomp/instructions.h"
#include "ps2recomp/types.h"
#include <fmt/format.h>
#include <sstream>
#include <cmath>

namespace ps2recomp
{
    std::string CodeGenerator::translateVU_VADD_Field(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        uint8_t field = inst.function & 0x3;
        std::string shuffle_pattern = fmt::format("_MM_SHUFFLE({},{},{},{})", field, field, field, field);
        return fmt::format("{{ __m128 res = PS2_VADD(ctx->vu0_vf[{}], _mm_shuffle_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}], {})); __m128i mask = _mm_set_epi32({}, {}, {}, {}); ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}", vfs, vft, vft, shuffle_pattern, (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0, (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0, vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VSUB_Field(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        uint8_t field = inst.function & 0x3;
        std::string shuffle_pattern = fmt::format("_MM_SHUFFLE({},{},{},{})", field, field, field, field);
        return fmt::format("{{ __m128 res = PS2_VSUB(ctx->vu0_vf[{}], _mm_shuffle_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}], {})); __m128i mask = _mm_set_epi32({}, {}, {}, {}); ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}", vfs, vft, vft, shuffle_pattern, (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0, (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0, vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VMUL_Field(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        uint8_t field = inst.function & 0x3;
        std::string shuffle_pattern = fmt::format("_MM_SHUFFLE({},{},{},{})", field, field, field, field);
        return fmt::format("{{ __m128 res = PS2_VMUL(ctx->vu0_vf[{}], _mm_shuffle_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}], {})); __m128i mask = _mm_set_epi32({}, {}, {}, {}); ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}", vfs, vft, vft, shuffle_pattern, (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0, (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0, vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VADD(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 res = PS2_VADD(ctx->vu0_vf[{}], ctx->vu0_vf[{}]); __m128i mask = _mm_set_epi32({}, {}, {}, {}); ctx->vu0_vf[{}] = PS2_VBLEND(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}", vfs, vft, (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0, (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0, vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VSUB(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 res = PS2_VSUB(ctx->vu0_vf[{}], ctx->vu0_vf[{}]); __m128i mask = _mm_set_epi32({}, {}, {}, {}); ctx->vu0_vf[{}] = PS2_VBLEND(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}", vfs, vft, (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0, (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0, vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VMUL(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 res = PS2_VMUL(ctx->vu0_vf[{}], ctx->vu0_vf[{}]); __m128i mask = _mm_set_epi32({}, {}, {}, {}); ctx->vu0_vf[{}] = PS2_VBLEND(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}", vfs, vft, (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0, (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0, vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VDIV(const Instruction &inst)
    {
        // PCSX2 _vuDIV: Q = fs.fsf / ft.ftf; zero divisor -> +/-FMAX (sign fs^ft).
        return fmt::format("ctx->vu0_q = Ps2VuDiv(Ps2VuLane(ctx->vu0_vf[{}], {}), Ps2VuLane(ctx->vu0_vf[{}], {}));",
                           inst.rd, inst.vectorInfo.fsf, inst.rt, inst.vectorInfo.ftf);
    }

    std::string CodeGenerator::translateVU_VSQRT(const Instruction &inst)
    {
        // PCSX2 _vuSQRT: Q = sqrt(|ft.ftf|).
        return fmt::format("ctx->vu0_q = Ps2VuSqrt(Ps2VuLane(ctx->vu0_vf[{}], {}));", inst.rt, inst.vectorInfo.ftf);
    }

    std::string CodeGenerator::translateVU_VRSQRT(const Instruction &inst)
    {
        // PCSX2 _vuRSQRT: Q = fs.fsf / sqrt(|ft.ftf|); ft = 0 -> +/-FMAX (or +/-0 when fs = 0).
        return fmt::format("ctx->vu0_q = Ps2VuRsqrt(Ps2VuLane(ctx->vu0_vf[{}], {}), Ps2VuLane(ctx->vu0_vf[{}], {}));",
                           inst.rd, inst.vectorInfo.fsf, inst.rt, inst.vectorInfo.ftf);
    }

    std::string CodeGenerator::translateVU_VMTIR(const Instruction &inst)
    {
        uint8_t fsf = inst.vectorInfo.fsf;
        return fmt::format("{{ uint32_t bits; float src = _mm_cvtss_f32(_mm_shuffle_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}], _MM_SHUFFLE(0,0,0,{}))); std::memcpy(&bits, &src, sizeof(bits)); ctx->vi[{}] = (uint16_t)(bits & 0xFFFF); }}", inst.rd, inst.rd, fsf, inst.rt);
    }

    std::string CodeGenerator::translateVU_VMFIR(const Instruction &inst)
    {
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ uint32_t tmp = (uint32_t)(int32_t)(int16_t)ctx->vi[{}]; float val; std::memcpy(&val, &tmp, sizeof(val)); "
                           "__m128 res = _mm_set1_ps(val); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           inst.rd,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           inst.rt, inst.rt);
    }

    std::string CodeGenerator::translateVU_VILWR(const Instruction &inst)
    {
        // VILWR.f it, (is): VU0 data word at vi[is]*16 + field (PCSX2 _vuILWR; the
        // last selected field wins).
        const uint8_t mask = inst.vectorInfo.vectorField;
        if (inst.rt == 0 || mask == 0)
            return "// VILWR to vi0 ignored";
        const uint32_t offset = (mask & 0x1) ? 12u : (mask & 0x2) ? 8u : (mask & 0x4) ? 4u : 0u;
        return fmt::format("{{ uint16_t v; std::memcpy(&v, Ps2Vu0DataAt(runtime, (uint32_t)ctx->vi[{}] << 4) + {}, sizeof(v)); ctx->vi[{}] = v; }}",
                           inst.rd, offset, inst.rt);
    }

    std::string CodeGenerator::translateVU_VISWR(const Instruction &inst)
    {
        // VISWR.f it, (is): store vi[it] (zero-extended) to each selected field of
        // the VU0 data qword at vi[is]*16 (PCSX2 _vuISWR).
        const uint8_t mask = inst.vectorInfo.vectorField;
        std::string stores;
        const uint32_t offsets[4] = {0u, 4u, 8u, 12u};
        const uint8_t bits[4] = {0x8, 0x4, 0x2, 0x1};
        for (int i = 0; i < 4; ++i)
        {
            if (mask & bits[i])
                stores += fmt::format(" std::memcpy(p + {}, &v, sizeof(v));", offsets[i]);
        }
        return fmt::format("{{ uint8_t *p = Ps2Vu0DataAt(runtime, (uint32_t)ctx->vi[{}] << 4); uint32_t v = ctx->vi[{}];{} }}",
                           inst.rd, inst.rt, stores);
    }

    std::string CodeGenerator::translateVU_VIADD(const Instruction &inst)
    {
        return fmt::format("ctx->vi[{}] = ctx->vi[{}] + ctx->vi[{}];", inst.sa, inst.rd, inst.rt); // vid, vis, vit
    }

    std::string CodeGenerator::translateVU_VISUB(const Instruction &inst)
    {
        return fmt::format("ctx->vi[{}] = ctx->vi[{}] - ctx->vi[{}];", inst.sa, inst.rd, inst.rt); // vid, vis, vit
    }

    std::string CodeGenerator::translateVU_VIADDI(const Instruction &inst)
    {
        int32_t imm5 = (inst.sa & 0x10) ? static_cast<int32_t>(inst.sa | ~0x1F) : static_cast<int32_t>(inst.sa);
        return fmt::format("ctx->vi[{}] = ctx->vi[{}] + {};", inst.rt, inst.rd, imm5); // vit, vis, imm5
    }

    std::string CodeGenerator::translateVU_VIAND(const Instruction &inst)
    {
        return fmt::format("ctx->vi[{}] = ctx->vi[{}] & ctx->vi[{}];", inst.sa, inst.rd, inst.rt); // vid, vis, vit
    }

    std::string CodeGenerator::translateVU_VIOR(const Instruction &inst)
    {
        return fmt::format("ctx->vi[{}] = ctx->vi[{}] | ctx->vi[{}];", inst.sa, inst.rd, inst.rt); // vid, vis, vit
    }

    std::string CodeGenerator::translateVU_VCALLMS(const Instruction &inst)
    {
        // VCALLMS calls a VU0 microprogram at the specified immediate address.
        // VU0 micro memory is 4KB = 512 instructions (8 bytes each). Index is 0-511.
        uint16_t instr_index = static_cast<uint16_t>((inst.raw >> 6) & 0x1FF); // imm15[8:0]
        uint32_t target_byte_addr = static_cast<uint32_t>(instr_index) << 3;   // Convert instruction index to byte address

        return fmt::format(
            "{{ "
            "    ctx->vu0_tpc = 0x{:X}; " // Set target program counter
            "    runtime->executeVU0Microprogram(rdram, ctx, 0x{:X}); "
            "}}",
            target_byte_addr, target_byte_addr);
    }

    std::string CodeGenerator::translateVU_VCALLMSR(const Instruction &inst)
    {
        // VCALLMSR starts VU0 at CMSAR0 (PCSX2 COP2.cpp: vu0ExecMicro(VI[CMSAR0]));
        // the encoding's is field is always 27 (CMSAR0).
        (void)inst;
        return "{ const uint32_t target_byte_addr = (ctx->vu0_cmsar0 & 0x1FFu) << 3; "
               "ctx->vu0_pc = target_byte_addr; "
               "runtime->vu0StartMicroProgram(rdram, ctx, target_byte_addr); }";
    }

    std::string CodeGenerator::translateVU_VRNEXT(const Instruction &inst)
    {
        // PCSX2 _vuRNEXT: advance the 23-bit LFSR, write R to ft (nothing when ft = vf0).
        if (inst.rt == 0)
            return "// VRNEXT to vf0 ignored";
        return fmt::format("{{ const uint32_t r = Ps2VuAdvanceR(ctx); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], _mm_castsi128_ps(_mm_set1_epi32((int32_t)r)), {}); }}",
                           inst.rt, inst.rt, codegen::vuMaskExpr(inst.vectorInfo.vectorField));
    }

    std::string CodeGenerator::translateVU_VMADD_Field(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        uint8_t field = inst.function & 0x3; // Extract field from function code

        // Pre-construct the shuffle pattern to avoid format string issues
        std::string shuffle_pattern = fmt::format("_MM_SHUFFLE({},{},{},{})", field, field, field, field);

        return fmt::format("{{ __m128 mul_res = PS2_VMUL(ctx->vu0_vf[{}], _mm_shuffle_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}], {})); "
                           "__m128 res = PS2_VADD(ctx->vu0_acc, mul_res); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs, vft, vft, shuffle_pattern,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VMSUB_Field(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        uint8_t field = inst.function & 0x3; // Extract field from function code

        std::string shuffle_pattern = fmt::format("_MM_SHUFFLE({},{},{},{})", field, field, field, field);

        return fmt::format("{{ __m128 mul_res = PS2_VMUL(ctx->vu0_vf[{}], _mm_shuffle_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}], {})); "
                           "__m128 res = PS2_VSUB(ctx->vu0_acc, mul_res); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs, vft, vft, shuffle_pattern,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VMINI_Field(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        uint8_t field = inst.function & 0x3;

        std::string shuffle_pattern = fmt::format("_MM_SHUFFLE({},{},{},{})", field, field, field, field);

        return fmt::format("{{ __m128 res = Ps2VuMin(ctx->vu0_vf[{}], _mm_shuffle_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}], {})); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs, vft, vft, shuffle_pattern,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VMAX_Field(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        uint8_t field = inst.function & 0x3;

        std::string shuffle_pattern = fmt::format("_MM_SHUFFLE({},{},{},{})", field, field, field, field);

        return fmt::format("{{ __m128 res = Ps2VuMax(ctx->vu0_vf[{}], _mm_shuffle_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}], {})); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs, vft, vft, shuffle_pattern,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VMADD(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 mul_res = PS2_VMUL(ctx->vu0_vf[{}], ctx->vu0_vf[{}]); "
                           "__m128 res = PS2_VADD(ctx->vu0_acc, mul_res); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs, vft,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VMADDq(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 mul_res = PS2_VMUL(ctx->vu0_vf[{}], _mm_set1_ps(ctx->vu0_q)); "
                           "__m128 res = PS2_VADD(ctx->vu0_acc, mul_res); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VMADDi(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 mul_res = PS2_VMUL(ctx->vu0_vf[{}], _mm_set1_ps(ctx->vu0_i)); "
                           "__m128 res = PS2_VADD(ctx->vu0_acc, mul_res); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VMAX(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 res = Ps2VuMax(ctx->vu0_vf[{}], ctx->vu0_vf[{}]); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs, vft,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VMAXi(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 res = Ps2VuMax(ctx->vu0_vf[{}], _mm_set1_ps(ctx->vu0_i)); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VMINIi(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 res = Ps2VuMin(ctx->vu0_vf[{}], _mm_set1_ps(ctx->vu0_i)); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VMULi(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 res = PS2_VMUL(ctx->vu0_vf[{}], _mm_set1_ps(ctx->vu0_i)); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VMULq(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 res = PS2_VMUL(ctx->vu0_vf[{}], _mm_set1_ps(ctx->vu0_q)); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VOPMSUB(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 fs_yzx = _mm_shuffle_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}], _MM_SHUFFLE(3,0,2,1)); "
                           "__m128 ft_zxy = _mm_shuffle_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}], _MM_SHUFFLE(3,1,0,2)); "
                           "__m128 mul_res = PS2_VMUL(fs_yzx, ft_zxy); "
                           "__m128 res = PS2_VSUB(ctx->vu0_acc, mul_res); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs, vfs, vft, vft,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VADDq(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 res = PS2_VADD(ctx->vu0_vf[{}], _mm_set1_ps(ctx->vu0_q)); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VADDi(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 res = PS2_VADD(ctx->vu0_vf[{}], _mm_set1_ps(ctx->vu0_i)); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VMSUB(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 mul_res = PS2_VMUL(ctx->vu0_vf[{}], ctx->vu0_vf[{}]); "
                           "__m128 res = PS2_VSUB(ctx->vu0_acc, mul_res); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs, vft,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VMINI(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 res = Ps2VuMin(ctx->vu0_vf[{}], ctx->vu0_vf[{}]); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs, vft,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VSUBi(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 res = PS2_VSUB(ctx->vu0_vf[{}], _mm_set1_ps(ctx->vu0_i)); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VSUBq(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 res = PS2_VSUB(ctx->vu0_vf[{}], _mm_set1_ps(ctx->vu0_q)); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VMSUBq(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 mul_res = PS2_VMUL(ctx->vu0_vf[{}], _mm_set1_ps(ctx->vu0_q)); "
                           "__m128 res = PS2_VSUB(ctx->vu0_acc, mul_res); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VMSUBi(const Instruction &inst)
    {
        uint8_t vfd = inst.sa;
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 mul_res = PS2_VMUL(ctx->vu0_vf[{}], _mm_set1_ps(ctx->vu0_i)); "
                           "__m128 res = PS2_VSUB(ctx->vu0_acc, mul_res); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs,
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           vfd, vfd);
    }

    std::string CodeGenerator::translateVU_VADDA_Field(const Instruction &inst)
    {
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        uint8_t field = inst.function & 0x3;
        std::string shuffle_pattern = fmt::format("_MM_SHUFFLE({},{},{},{})", field, field, field, field);

        return fmt::format("{{ __m128 res = PS2_VADD(ctx->vu0_vf[{}], _mm_shuffle_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}], {})); "
                           "ctx->vu0_acc = _mm_blendv_ps(ctx->vu0_acc, res, {}); }}",
                           vfs, vft, vft, shuffle_pattern, codegen::vuMaskExpr(dest_mask));
    }

    std::string CodeGenerator::translateVU_VSUBA_Field(const Instruction &inst)
    {
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        uint8_t field = inst.function & 0x3;
        std::string shuffle_pattern = fmt::format("_MM_SHUFFLE({},{},{},{})", field, field, field, field);

        return fmt::format("{{ __m128 res = PS2_VSUB(ctx->vu0_vf[{}], _mm_shuffle_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}], {})); "
                           "ctx->vu0_acc = _mm_blendv_ps(ctx->vu0_acc, res, {}); }}",
                           vfs, vft, vft, shuffle_pattern, codegen::vuMaskExpr(dest_mask));
    }

    std::string CodeGenerator::translateVU_VMADDA_Field(const Instruction &inst)
    {
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        uint8_t field = inst.function & 0x3;
        std::string shuffle_pattern = fmt::format("_MM_SHUFFLE({},{},{},{})", field, field, field, field);

        return fmt::format("{{ __m128 mul_res = PS2_VMUL(ctx->vu0_vf[{}], _mm_shuffle_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}], {})); "
                           "__m128 res = PS2_VADD(ctx->vu0_acc, mul_res); "
                           "ctx->vu0_acc = _mm_blendv_ps(ctx->vu0_acc, res, {}); }}",
                           vfs, vft, vft, shuffle_pattern, codegen::vuMaskExpr(dest_mask));
    }

    std::string CodeGenerator::translateVU_VMSUBA_Field(const Instruction &inst)
    {
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        uint8_t field = inst.function & 0x3;
        std::string shuffle_pattern = fmt::format("_MM_SHUFFLE({},{},{},{})", field, field, field, field);

        return fmt::format("{{ __m128 mul_res = PS2_VMUL(ctx->vu0_vf[{}], _mm_shuffle_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}], {})); "
                           "__m128 res = PS2_VSUB(ctx->vu0_acc, mul_res); "
                           "ctx->vu0_acc = _mm_blendv_ps(ctx->vu0_acc, res, {}); }}",
                           vfs, vft, vft, shuffle_pattern, codegen::vuMaskExpr(dest_mask));
    }

    std::string CodeGenerator::translateVU_VMULA_Field(const Instruction &inst)
    {
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        uint8_t field = inst.function & 0x3;
        std::string shuffle_pattern = fmt::format("_MM_SHUFFLE({},{},{},{})", field, field, field, field);

        return fmt::format("{{ __m128 res = PS2_VMUL(ctx->vu0_vf[{}], _mm_shuffle_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}], {})); "
                           "ctx->vu0_acc = _mm_blendv_ps(ctx->vu0_acc, res, {}); }}",
                           vfs, vft, vft, shuffle_pattern, codegen::vuMaskExpr(dest_mask));
    }

    std::string CodeGenerator::translateVU_VADDA(const Instruction &inst)
    {
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 res = PS2_VADD(ctx->vu0_vf[{}], ctx->vu0_vf[{}]); ctx->vu0_acc = _mm_blendv_ps(ctx->vu0_acc, res, {}); }}",
                           vfs, vft, codegen::vuMaskExpr(dest_mask));
    }

    std::string CodeGenerator::translateVU_VADDAq(const Instruction &inst)
    {
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 res = PS2_VADD(ctx->vu0_vf[{}], _mm_set1_ps(ctx->vu0_q)); ctx->vu0_acc = _mm_blendv_ps(ctx->vu0_acc, res, {}); }}",
                           vfs, codegen::vuMaskExpr(dest_mask));
    }

    std::string CodeGenerator::translateVU_VADDAi(const Instruction &inst)
    {
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 res = PS2_VADD(ctx->vu0_vf[{}], _mm_set1_ps(ctx->vu0_i)); ctx->vu0_acc = _mm_blendv_ps(ctx->vu0_acc, res, {}); }}",
                           vfs, codegen::vuMaskExpr(dest_mask));
    }

    std::string CodeGenerator::translateVU_VSUBA(const Instruction &inst)
    {
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 res = PS2_VSUB(ctx->vu0_vf[{}], ctx->vu0_vf[{}]); ctx->vu0_acc = _mm_blendv_ps(ctx->vu0_acc, res, {}); }}",
                           vfs, vft, codegen::vuMaskExpr(dest_mask));
    }

    std::string CodeGenerator::translateVU_VSUBAq(const Instruction &inst)
    {
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 res = PS2_VSUB(ctx->vu0_vf[{}], _mm_set1_ps(ctx->vu0_q)); ctx->vu0_acc = _mm_blendv_ps(ctx->vu0_acc, res, {}); }}",
                           vfs, codegen::vuMaskExpr(dest_mask));
    }

    std::string CodeGenerator::translateVU_VSUBAi(const Instruction &inst)
    {
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 res = PS2_VSUB(ctx->vu0_vf[{}], _mm_set1_ps(ctx->vu0_i)); ctx->vu0_acc = _mm_blendv_ps(ctx->vu0_acc, res, {}); }}",
                           vfs, codegen::vuMaskExpr(dest_mask));
    }

    std::string CodeGenerator::translateVU_VMADDA(const Instruction &inst)
    {
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 mul_res = PS2_VMUL(ctx->vu0_vf[{}], ctx->vu0_vf[{}]); __m128 res = PS2_VADD(ctx->vu0_acc, mul_res); ctx->vu0_acc = _mm_blendv_ps(ctx->vu0_acc, res, {}); }}",
                           vfs, vft, codegen::vuMaskExpr(dest_mask));
    }

    std::string CodeGenerator::translateVU_VMADDAq(const Instruction &inst)
    {
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 mul_res = PS2_VMUL(ctx->vu0_vf[{}], _mm_set1_ps(ctx->vu0_q)); __m128 res = PS2_VADD(ctx->vu0_acc, mul_res); ctx->vu0_acc = _mm_blendv_ps(ctx->vu0_acc, res, {}); }}",
                           vfs, codegen::vuMaskExpr(dest_mask));
    }

    std::string CodeGenerator::translateVU_VMADDAi(const Instruction &inst)
    {
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 mul_res = PS2_VMUL(ctx->vu0_vf[{}], _mm_set1_ps(ctx->vu0_i)); __m128 res = PS2_VADD(ctx->vu0_acc, mul_res); ctx->vu0_acc = _mm_blendv_ps(ctx->vu0_acc, res, {}); }}",
                           vfs, codegen::vuMaskExpr(dest_mask));
    }

    std::string CodeGenerator::translateVU_VMSUBA(const Instruction &inst)
    {
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 mul_res = PS2_VMUL(ctx->vu0_vf[{}], ctx->vu0_vf[{}]); __m128 res = PS2_VSUB(ctx->vu0_acc, mul_res); ctx->vu0_acc = _mm_blendv_ps(ctx->vu0_acc, res, {}); }}",
                           vfs, vft, codegen::vuMaskExpr(dest_mask));
    }

    std::string CodeGenerator::translateVU_VMSUBAq(const Instruction &inst)
    {
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 mul_res = PS2_VMUL(ctx->vu0_vf[{}], _mm_set1_ps(ctx->vu0_q)); __m128 res = PS2_VSUB(ctx->vu0_acc, mul_res); ctx->vu0_acc = _mm_blendv_ps(ctx->vu0_acc, res, {}); }}",
                           vfs, codegen::vuMaskExpr(dest_mask));
    }

    std::string CodeGenerator::translateVU_VMSUBAi(const Instruction &inst)
    {
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 mul_res = PS2_VMUL(ctx->vu0_vf[{}], _mm_set1_ps(ctx->vu0_i)); __m128 res = PS2_VSUB(ctx->vu0_acc, mul_res); ctx->vu0_acc = _mm_blendv_ps(ctx->vu0_acc, res, {}); }}",
                           vfs, codegen::vuMaskExpr(dest_mask));
    }

    std::string CodeGenerator::translateVU_VMULA(const Instruction &inst)
    {
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 res = PS2_VMUL(ctx->vu0_vf[{}], ctx->vu0_vf[{}]); ctx->vu0_acc = _mm_blendv_ps(ctx->vu0_acc, res, {}); }}",
                           vfs, vft, codegen::vuMaskExpr(dest_mask));
    }

    std::string CodeGenerator::translateVU_VMULAq(const Instruction &inst)
    {
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 res = PS2_VMUL(ctx->vu0_vf[{}], _mm_set1_ps(ctx->vu0_q)); ctx->vu0_acc = _mm_blendv_ps(ctx->vu0_acc, res, {}); }}",
                           vfs, codegen::vuMaskExpr(dest_mask));
    }

    std::string CodeGenerator::translateVU_VMULAi(const Instruction &inst)
    {
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 res = PS2_VMUL(ctx->vu0_vf[{}], _mm_set1_ps(ctx->vu0_i)); ctx->vu0_acc = _mm_blendv_ps(ctx->vu0_acc, res, {}); }}",
                           vfs, codegen::vuMaskExpr(dest_mask));
    }

    std::string CodeGenerator::translateVU_VOPMULA(const Instruction &inst)
    {
        uint8_t vfs = inst.rd;
        uint8_t vft = inst.rt;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        return fmt::format("{{ __m128 fs_yzx = _mm_shuffle_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}], _MM_SHUFFLE(3,0,2,1)); "
                           "__m128 ft_zxy = _mm_shuffle_ps(ctx->vu0_vf[{}], ctx->vu0_vf[{}], _MM_SHUFFLE(3,1,0,2)); "
                           "__m128 res = PS2_VMUL(fs_yzx, ft_zxy); "
                           "ctx->vu0_acc = _mm_blendv_ps(ctx->vu0_acc, res, {}); }}",
                           vfs, vfs, vft, vft, codegen::vuMaskExpr(dest_mask));
    }

    std::string CodeGenerator::translateVU_VITOF(const Instruction &inst, int shift)
    {
        uint8_t vfs = inst.rd;
        uint8_t dest_mask = inst.vectorInfo.vectorField;
        float scale = (shift == 0) ? 1.0f : (1.0f / static_cast<float>(1 << shift));

        return fmt::format("{{ __m128i src = _mm_castps_si128(ctx->vu0_vf[{}]); "
                           "__m128 res = _mm_cvtepi32_ps(src); "
                           "res = _mm_mul_ps(res, _mm_set1_ps({})); "
                           "__m128i mask = _mm_set_epi32({}, {}, {}, {}); "
                           "ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], res, _mm_castsi128_ps(mask)); }}",
                           vfs, codegen::formatFloatLiteral(scale),
                           (dest_mask & 0x1) ? -1 : 0, (dest_mask & 0x2) ? -1 : 0,
                           (dest_mask & 0x4) ? -1 : 0, (dest_mask & 0x8) ? -1 : 0,
                           inst.rt, inst.rt);
    }

    std::string CodeGenerator::translateVU_VFTOI(const Instruction &inst, int shift)
    {
        // PCSX2 floatToInt<shift>: scale, truncate, saturate by sign at 2^31.
        if (inst.rt == 0)
            return "// VFTOI to vf0 ignored";
        return fmt::format("ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], Ps2VuFtoi(ctx->vu0_vf[{}], {}), {});",
                           inst.rt, inst.rt, inst.rd, shift, codegen::vuMaskExpr(inst.vectorInfo.vectorField));
    }

    std::string CodeGenerator::translateVU_VLQI(const Instruction &inst)
    {
        // VLQI.dest ft, (is++): load from VU0 data at vi[is]*16, then vi[is]++
        // (PCSX2 _vuLQI; no increment for vi0, no load into vf0).
        const uint8_t ft = inst.rt, is = inst.rd;
        std::string load = (ft == 0) ? std::string() :
            fmt::format(" ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], _mm_loadu_ps(reinterpret_cast<const float *>(Ps2Vu0DataAt(runtime, (uint32_t)ctx->vi[{}] << 4))), {});",
                        ft, ft, is, codegen::vuMaskExpr(inst.vectorInfo.vectorField));
        std::string inc = (is == 0) ? std::string() : fmt::format(" ctx->vi[{}] = (uint16_t)(ctx->vi[{}] + 1);", is, is);
        return "{" + load + inc + " }";
    }

    std::string CodeGenerator::translateVU_VSQI(const Instruction &inst)
    {
        // VSQI.dest fs, (it++): fs = bits 15:11, it = bits 20:16. Store to VU0 data
        // at vi[it]*16, then vi[it]++ (PCSX2 _vuSQI).
        const uint8_t fs = inst.rd, it = inst.rt;
        std::string inc = (it == 0) ? std::string() : fmt::format(" ctx->vi[{}] = (uint16_t)(ctx->vi[{}] + 1);", it, it);
        return fmt::format("{{ float *p = reinterpret_cast<float *>(Ps2Vu0DataAt(runtime, (uint32_t)ctx->vi[{}] << 4)); "
                           "_mm_storeu_ps(p, _mm_blendv_ps(_mm_loadu_ps(p), ctx->vu0_vf[{}], {}));{} }}",
                           it, fs, codegen::vuMaskExpr(inst.vectorInfo.vectorField), inc);
    }

    std::string CodeGenerator::translateVU_VLQD(const Instruction &inst)
    {
        // VLQD.dest ft, (--is): vi[is]-- (not vi0), then load from VU0 data at
        // vi[is]*16 (PCSX2 _vuLQD).
        const uint8_t ft = inst.rt, is = inst.rd;
        std::string dec = (is == 0) ? std::string() : fmt::format(" ctx->vi[{}] = (uint16_t)(ctx->vi[{}] - 1);", is, is);
        std::string load = (ft == 0) ? std::string() :
            fmt::format(" ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], _mm_loadu_ps(reinterpret_cast<const float *>(Ps2Vu0DataAt(runtime, (uint32_t)ctx->vi[{}] << 4))), {});",
                        ft, ft, is, codegen::vuMaskExpr(inst.vectorInfo.vectorField));
        return "{" + dec + load + " }";
    }

    std::string CodeGenerator::translateVU_VSQD(const Instruction &inst)
    {
        // VSQD.dest fs, (--it): vi[it]-- (not vi0), then store to VU0 data at
        // vi[it]*16 (PCSX2 _vuSQD).
        const uint8_t fs = inst.rd, it = inst.rt;
        std::string dec = (it == 0) ? std::string() : fmt::format(" ctx->vi[{}] = (uint16_t)(ctx->vi[{}] - 1);", it, it);
        return fmt::format("{{{} float *p = reinterpret_cast<float *>(Ps2Vu0DataAt(runtime, (uint32_t)ctx->vi[{}] << 4)); "
                           "_mm_storeu_ps(p, _mm_blendv_ps(_mm_loadu_ps(p), ctx->vu0_vf[{}], {})); }}",
                           dec, it, fs, codegen::vuMaskExpr(inst.vectorInfo.vectorField));
    }

    std::string CodeGenerator::translateVU_VRGET(const Instruction &inst)
    {
        // PCSX2 _vuRGET: write R to ft.
        if (inst.rt == 0)
            return "// VRGET to vf0 ignored";
        return fmt::format("ctx->vu0_vf[{}] = _mm_blendv_ps(ctx->vu0_vf[{}], _mm_castsi128_ps(_mm_set1_epi32((int32_t)Ps2VuR(ctx))), {});",
                           inst.rt, inst.rt, codegen::vuMaskExpr(inst.vectorInfo.vectorField));
    }

    std::string CodeGenerator::translateVU_VRINIT(const Instruction &inst)
    {
        // PCSX2 _vuRINIT: R = 0x3F800000 | (fs.fsf & 0x7FFFFF).
        return fmt::format("Ps2VuSetR(ctx, Ps2VuLane(ctx->vu0_vf[{}], {}));", inst.rd, inst.vectorInfo.fsf);
    }

    std::string CodeGenerator::translateVU_VRXOR(const Instruction &inst)
    {
        // PCSX2 _vuRXOR: R = 0x3F800000 | ((R ^ fs.fsf) & 0x7FFFFF).
        return fmt::format("Ps2VuSetR(ctx, Ps2VuR(ctx) ^ Ps2VuLane(ctx->vu0_vf[{}], {}));", inst.rd, inst.vectorInfo.fsf);
    }

}
