#ifndef PS2_VU1_RECOMP_GEN_H
#define PS2_VU1_RECOMP_GEN_H

// VR1: everything a generated VU1 image (vu1_<hash>.cpp) needs. VX1: the
// shared part is ps2_vu_recomp_gen_common.h; this adds the VU1 class and
// the stage-4 block guard (VU1 images only). The image text is unchanged
// by VX1 (it names VU1Interpreter and the PS2X_VU1_* macros below).

#include "runtime/ps2_vu1.h"
#include "ps2_vu_recomp_gen_common.h"

#define PS2X_VU1_MUSTTAIL PS2X_VU_MUSTTAIL
#define PS2X_VU1_NOINLINE PS2X_VU_NOINLINE

// VR2 stage 4: a block function runs its pairs back to back with the per-pair
// guards hoisted here. Entry state the emitter's analysis assumes: no branch
// pending (the block starts a pc sequence; a leader reached as a delay slot
// takes the pair function), no E-bit or halt pending, and every stall plus
// the last direct landing inside the budget. m_blocksOn implies m_directRunOk.
PS2X_VU1_ALWAYS_INLINE inline bool VU1Interpreter::recompBlockReady(const RunContext &ctx, uint32_t maxCycles,
                                                                     uint32_t pairs)
{
    if (!m_blocksOn || m_state.branchPending || m_state.ebit || m_state.haltAfterDelaySlot ||
        m_cycle + maxCycles > ctx.budgetEnd)
    {
#if PS2X_ENABLE_DET_HASH_TAP
        ++(!m_blocksOn                                          ? m_blockMissOff
           : m_state.branchPending                              ? m_blockMissBranch
           : m_state.ebit || m_state.haltAfterDelaySlot         ? m_blockMissEnd
                                                                : m_blockMissBudget);
#endif
        return false;
    }
    // Counted in every build: the tests read m_blockEntries (on path only).
    ++m_blockEntries;
#if PS2X_ENABLE_DET_HASH_TAP
    m_blockPairs += pairs;
#else
    (void)pairs;
#endif
    return true;
}


#endif
