// SSX 3 (SLUS_207.72) 0x3E3968: the comparator EA's CD file layer passes to
// qsort/bsearch for its file table (key = u32 name hash at +0).
//
//   lwu $v1, 0($a0); lwu $v0, 0($a1); dsubu $v1, $v1, $v0
//   bltz $v1 -> return -1; bgtz $v1 -> return 1; else ++[0x51ED80], return 0
//
// Codegen trees made before the recompiler tested BLTZ/BGTZ on the full
// 64-bit register compare the low 32 bits of the difference instead. That is
// not an order: the table sorts into a rotated sequence and bsearch misses
// data/config/banks.inf (so no SFX bank is ever loaded), GRNT_ZOE.BNK,
// GRNT_ARI.BNK, SLUSOVF.BIG, NETCNF.IRX and EZMIDI.IRX (AU9). This override
// restores the guest semantics until the codegen is regenerated.

#include "ssx3_file_key_compare.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <cstdint>

namespace
{
    constexpr uint32_t kCompareAddr = 0x003E3968u;
    constexpr uint32_t kMatchCounterAddr = 0x0051ED80u;
}

namespace ps2_ssx3_file_key
{
    void compare(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        const uint64_t a = READ32(GPR_U32(ctx, 4));
        const uint64_t b = READ32(GPR_U32(ctx, 5));
        const uint64_t diff = a - b;
        SET_GPR_U64(ctx, 3, diff);
        SET_GPR_U64(ctx, 2, a);
        if (static_cast<int64_t>(diff) < 0)
        {
            SET_GPR_S32(ctx, 2, -1);
        }
        else
        {
            SET_GPR_S32(ctx, 4, static_cast<int32_t>(0x00520000u));
            if (static_cast<int64_t>(diff) > 0)
            {
                SET_GPR_S32(ctx, 2, 1);
            }
            else
            {
                const uint32_t count = READ32(kMatchCounterAddr) + 1u;
                WRITE32(kMatchCounterAddr, count);
                SET_GPR_S32(ctx, 3, static_cast<int32_t>(count));
                SET_GPR_S32(ctx, 2, 0);
            }
        }
        ctx->pc = GPR_U32(ctx, 31);
    }

    void apply(PS2Runtime &runtime)
    {
        runtime.replaceFunction(kCompareAddr, &compare);
    }
}

