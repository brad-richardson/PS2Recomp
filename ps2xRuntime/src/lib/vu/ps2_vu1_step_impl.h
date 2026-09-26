#ifndef PS2_VU1_STEP_IMPL_H
#define PS2_VU1_STEP_IMPL_H

// VR1: the per-pair issue step of VU1Interpreter::run(), moved verbatim into an
// always-inline template so the generated VU1 programs (PS2X_VU1_RECOMP_DIR) and
// the interpreter share one copy of the scoreboard and cycle accounting. With
// kStatic the pair comes from a constexpr table and the executors are inlined
// with constant instruction words; the interpreter passes its decode-cache entry.

#include "runtime/ps2_vu1.h"
#include "runtime/ps2_memory.h"
#include "ps2_vu1_entry_trace.h"
#include "ps2_vu1_trace.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace ps2_vu1_step_detail
{
    constexpr uint8_t laneForComponent(uint32_t component)
    {
        return static_cast<uint8_t>(1u << (3u - component));
    }

    // E57: index of the first clear bit below `count`, or -1 when all
    // `count` entries are valid (same choice as a first-free linear scan).
    inline int firstFreeEntry(uint32_t validMask, uint32_t count)
    {
        const uint32_t freeBits = ~validMask & ((1u << count) - 1u);
        return freeBits != 0u ? std::countr_zero(freeBits) : -1;
    }
}

using ps2_vu1_step_detail::firstFreeEntry;
using ps2_vu1_step_detail::laneForComponent;

PS2X_VU1_ALWAYS_INLINE inline uint64_t VU1Interpreter::calculatePairReadyCycle(const DecodedInstructionPair &decoded) const
{
    // E57: lane tests as selects (no per-lane branches); the maximum over
    // the same set of ready cycles as the per-lane loop.
    uint64_t ready = m_cycle;
    const InstructionUsage *usages[2] = {
        &decoded.upperUsage,
        &decoded.lowerUsage};
    for (const InstructionUsage *usage : usages)
    {
        for (uint32_t index = 0; index < usage->vfReadCount; ++index)
        {
            const VfAccess &access = usage->vfRead[index];
            const std::array<uint64_t, 4> &regReady = m_vfReady[access.reg];
            for (uint32_t component = 0; component < 4u; ++component)
            {
                const uint64_t laneReady =
                    (access.lanes & laneForComponent(component)) != 0u ? regReady[component] : 0u;
                ready = std::max(ready, laneReady);
            }
        }
        for (uint32_t pending = usage->viRead & 0xFFFEu; pending != 0u; pending &= pending - 1u)
            ready = std::max(ready, m_viReady[std::countr_zero(pending)]);
        for (uint32_t component = 0; component < 4u; ++component)
        {
            const uint64_t laneReady =
                (usage->accRead & laneForComponent(component)) != 0u ? m_accReady[component] : 0u;
            ready = std::max(ready, laneReady);
        }
    }

    if (decoded.lowerUsage.pipeline == PipelineFdiv && m_fdiv.valid)
        ready = std::max(ready, m_fdiv.readyCycle);
    if (decoded.lowerUsage.pipeline == PipelineEfu)
        ready = std::max(ready, m_efuResourceReady);
    if (decoded.lowerUsage.waitQ && m_fdiv.valid)
        ready = std::max(ready, m_fdiv.readyCycle);
    if (decoded.lowerUsage.waitP)
    {
        for (const ScalarPipelineEntry &entry : m_efu)
            if (entry.valid)
                ready = std::max(ready, entry.readyCycle);
    }
    if (decoded.lowerUsage.pipeline == PipelineXgkick && m_xgkick.active)
        ready = std::max(ready, m_cycle + 1u);
    return ready;
}

PS2X_VU1_ALWAYS_INLINE inline void VU1Interpreter::markPairWrites(const DecodedInstructionPair &decoded)
{
    const VfAccess lowerWrite = decoded.lowerUsage.vfWrite;
    if (lowerWrite.reg != 0u &&
        decoded.suppressedLowerVf != lowerWrite.reg)
    {
        const uint32_t latency = decoded.lowerUsage.vfLatency != 0u
                                     ? decoded.lowerUsage.vfLatency
                                     : decoded.lowerUsage.latency;
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((lowerWrite.lanes & laneForComponent(component)) != 0u)
                m_vfReady[lowerWrite.reg][component] = m_cycle + latency;
        }
    }

    const VfAccess upperWrite = decoded.upperUsage.vfWrite;
    if (upperWrite.reg != 0u)
    {
        const uint32_t latency = decoded.upperUsage.vfLatency != 0u
                                     ? decoded.upperUsage.vfLatency
                                     : decoded.upperUsage.latency;
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((upperWrite.lanes & laneForComponent(component)) != 0u)
                m_vfReady[upperWrite.reg][component] = m_cycle + latency;
        }
    }

    for (uint32_t pending = decoded.lowerUsage.viWrite & 0xFFFEu; pending != 0u; pending &= pending - 1u)
        m_viReady[std::countr_zero(pending)] = m_cycle + (decoded.lowerUsage.viLatency != 0u ? decoded.lowerUsage.viLatency : decoded.lowerUsage.latency);
    for (uint32_t component = 0; component < 4u; ++component)
    {
        if ((decoded.upperUsage.accWrite & laneForComponent(component)) != 0u)
            m_accReady[component] = m_cycle + kAccForwardLatency;
    }
}

// VB1: the commit of a queued write, applied at issue. A fresh sequence number
// retires any older queued write to the same lanes (commit skips entries that
// are not the latest), exactly as a newer queued write would.
PS2X_VU1_ALWAYS_INLINE inline void VU1Interpreter::directVfWrite(uint8_t reg, uint8_t laneMask,
                                                                  const float value[4], uint32_t latency)
{
    if (reg == 0u || laneMask == 0u)
        return;
    const uint64_t sequence = ++m_nextWriteSequence;
    for (uint32_t component = 0; component < 4u; ++component)
    {
        if ((laneMask & laneForComponent(component)) != 0u)
        {
            m_state.vf[reg][component] = value[component];
            m_vfLatestWrite[reg][component] = sequence;
        }
    }
    noteDirect(m_cycle + latency);
}

PS2X_VU1_ALWAYS_INLINE inline void VU1Interpreter::directViWrite(uint8_t reg, int32_t value, uint32_t latency)
{
    if (reg == 0u)
        return;
    m_viLatestWrite[reg] = ++m_nextWriteSequence;
    m_state.vi[reg] = static_cast<int16_t>(value);
    noteDirect(m_cycle + latency);
}

PS2X_VU1_ALWAYS_INLINE inline void VU1Interpreter::directAccWrite(uint8_t laneMask, const float value[4], uint32_t latency)
{
    if (laneMask == 0u)
        return;
    const uint64_t sequence = ++m_nextWriteSequence;
    for (uint32_t component = 0; component < 4u; ++component)
    {
        if ((laneMask & laneForComponent(component)) != 0u)
        {
            m_state.acc[component] = value[component];
            m_accLatestWrite[component] = sequence;
        }
    }
    noteDirect(m_cycle + latency);
}

// VB1 d3: older queued flag entries may still be in flight when a flag write
// is applied at issue. In queue order they land first and the newer write then
// overwrites MAC, status bits 0-3 and CLIP, while sticky bits (6-11) OR in and
// the FDIV D/I update commutes with both. So the older entries keep only their
// sticky OR. The static map guarantees no flag reader issues before the newer
// entry would land, so nobody sees the order change. A queued FSSET (replaces
// the sticky bits, which does not commute) keeps the whole pair queued.
PS2X_VU1_ALWAYS_INLINE inline bool VU1Interpreter::flagQueueAllowsDirect() const
{
    for (uint32_t pending = m_flagValidMask; pending != 0u; pending &= pending - 1u)
    {
        if (m_flagPipeline[std::countr_zero(pending)].writesSticky)
            return false;
    }
    return true;
}

// VR2: evaluated where an FMAC/CLIP flag write happens (at most once per
// pair, before any lower op of the pair queues a flag entry), so pairs without
// a flag write never load m_flagValidMask. Same value as at pair start: only
// the stall's commits change the flag queue before the upper executes.
PS2X_VU1_ALWAYS_INLINE inline bool VU1Interpreter::directFlagsNow() const
{
    return m_directFlags && (m_flagValidMask == 0u || flagQueueAllowsDirect());
}

PS2X_VU1_ALWAYS_INLINE inline void VU1Interpreter::demoteQueuedFlags(bool macStatus, bool clip)
{
    for (uint32_t pending = m_flagValidMask; pending != 0u; pending &= pending - 1u)
    {
        FlagPipelineEntry &entry = m_flagPipeline[std::countr_zero(pending)];
        if (macStatus)
        {
            entry.writesMac = false;
            if (entry.writesStatus)
            {
                entry.writesStatus = false;
                entry.writesStickyOr = true;
            }
        }
        if (clip)
            entry.writesClip = false;
    }
}

// VR1 g5: moved from ps2_vu1_core.cpp and inlined, with commitReadyPipelines()'
// own early-return gate checked at the call site (same condition, so the same
// calls do work).
PS2X_VU1_ALWAYS_INLINE inline void VU1Interpreter::advanceOneCycle()
{
    ++m_cycle;
    m_state.cycles = m_cycle;
    // LSU commits become visible at the cycle boundary before PATH1 consumes
    // its next qword from VU memory.
    if (m_cycle >= m_nextCommitCycle)
        commitReadyPipelines();
    if (m_xgkick.active)
        progressXgkick();
}

template <bool kStatic, int kBlockMap, bool kNoStall>
PS2X_VU1_ALWAYS_INLINE inline bool VU1Interpreter::issuePair(const DecodedInstructionPair &decoded, RunContext &ctx)
{
    constexpr bool kBlock = kBlockMap >= 0;
#if PS2X_ENABLE_DET_HASH_TAP
    const uint64_t vbStartCycle = m_cycle;
#endif
    // VR2: the dev-only E36/E37 traces run in the interpreter only (run()
    // sends an armed run there), so generated pairs skip their checks.
    const uint32_t traceIssuePc = m_state.pc;
    const uint32_t traceIssueIdx = traceIssuePc / 8u;
    if constexpr (!kStatic)
    {
        if (m_traceArmed && traceIssueIdx < m_traceHist.size())
        {
            ++m_traceHist[traceIssueIdx];
        }
    }
    // A branch "takes" when this pair newly raises branchPending (or
    // retargets it). A delay-slot branch to the identical target is
    // indistinguishable here; noted, negligible for hot-loop readout.
    const bool traceWasBranchPending = m_state.branchPending;
    const uint32_t traceWasBranchTarget = m_state.branchTarget;

    // VR2 stage 4: inside a block the entry guard bounds every stall below
    // budgetEnd, so the budget branches never fire there; a kNoStall pair's
    // reads are all ready (the emitter's proof), so it skips the scoreboard.
    if constexpr (!kNoStall)
    {
        uint64_t readyCycle = calculatePairReadyCycle(decoded);
        while (readyCycle > m_cycle)
        {
            if (!kBlock && readyCycle >= ctx.budgetEnd)
            {
                advanceTo(ctx.budgetEnd);
                break;
            }
            advanceTo(readyCycle);
            readyCycle = calculatePairReadyCycle(decoded);
        }
    }
#if PS2X_ENABLE_DET_HASH_TAP
    else if (calculatePairReadyCycle(decoded) > m_cycle)
        ++m_blockNoStallMisses; // must stay 0: the emitter's no-stall proof failed
#endif
    if constexpr (!kBlock)
    {
        if (m_cycle >= ctx.budgetEnd)
            return true;
    }

    // VB1: commit this pair's writes at issue when they land inside the
    // budget (all within kDirectMaxLatency). VF writes also need the static
    // map's no-supersede bit, VI writes latency 1 (ILW/ILWR stay queued),
    // flag writes the map's no-reader bit and no queued FSSET
    // (directFlagsNow).
    // VR2 stage 4: a block's guard implies both (m_blocksOn needs
    // m_directRunOk; its cycle bound includes the last landing), and its map
    // byte is the emitter's buildDirectFlagMap of the same code bytes.
    const bool direct = kBlock || (m_directRunOk && m_cycle + kDirectMaxLatency <= ctx.budgetEnd);
    const uint8_t directMap = kBlock ? static_cast<uint8_t>(kBlockMap)
                              : direct && m_directFlagSafe != nullptr ? m_directFlagSafe[m_state.pc >> 3]
                                                                     : 0u;
    const bool directUpperVf = (directMap & kDirectMapUpperVf) != 0u;
    const bool directLowerVf = (directMap & kDirectMapLowerVf) != 0u;
    const bool directVi = direct &&
                          (decoded.lowerUsage.viLatency != 0u ? decoded.lowerUsage.viLatency
                                                              : decoded.lowerUsage.latency) <= 1u;
    m_directStores = direct;
    m_directFlags = (directMap & kDirectMapFlags) != 0u;

    // E37: pre-exec snapshot for the pair line (post-stall state). VR1: the
    // snapshot lives in members, written and read only while m_entryArmed
    // (was two zero-initialized stack arrays, 576 B cleared on every pair).
    uint32_t entryPc = 0u, entryLo = 0u, entryUp = 0u;
    if (!kStatic && m_entryArmed)
    {
        entryPc = m_state.pc;
        std::memcpy(m_entryOldVi, m_state.vi, sizeof(m_entryOldVi));
        std::memcpy(m_entryOldVf, m_state.vf, sizeof(m_entryOldVf));
        std::memcpy(&entryLo, ctx.vuCode + m_state.pc, sizeof(entryLo));
        std::memcpy(&entryUp, ctx.vuCode + m_state.pc + sizeof(entryLo), sizeof(entryUp));
        m_entryStoreValid = false;
    }

    uint8_t writtenVi = 0u;
    int32_t oldVi = 0;
    if (const uint32_t viWrites = decoded.lowerUsage.viWrite & 0xFFFEu; viWrites != 0u)
    {
        // E57: lowest written VI register (same pick as the 1..15 scan).
        writtenVi = static_cast<uint8_t>(std::countr_zero(viWrites));
        oldVi = m_state.vi[writtenVi];
    }

    const VfAccess upperWrite = decoded.upperUsage.vfWrite;
    const VfAccess lowerWrite = decoded.lowerUsage.vfWrite;
    const bool hasUpperWrite = upperWrite.reg != 0u;
    const bool hasLowerWrite = lowerWrite.reg != 0u && decoded.suppressedLowerVf != lowerWrite.reg;
    const bool hasDistinctLowerWrite = hasLowerWrite && (!hasUpperWrite || lowerWrite.reg != upperWrite.reg);
    float oldUpperVf[4]{};
    float newUpperVf[4]{};
    float oldLowerVf[4]{};
    float newLowerVf[4]{};
    float oldAcc[4]{};
    float newAcc[4]{};
    if (hasUpperWrite)
        std::memcpy(oldUpperVf, m_state.vf[upperWrite.reg], sizeof(oldUpperVf));
    if (hasDistinctLowerWrite)
        std::memcpy(oldLowerVf, m_state.vf[lowerWrite.reg], sizeof(oldLowerVf));
    if (decoded.upperUsage.accWrite != 0u)
        std::memcpy(oldAcc, m_state.acc, sizeof(oldAcc));

    if (decoded.iBit)
    {
        if constexpr (kStatic) execUpperImpl(decoded.upper); else execUpper(decoded.upper);
        float immediate = 0.0f;
        std::memcpy(&immediate, &decoded.lower, sizeof(immediate));
        m_state.i = normalizeOperand(immediate);
    }
    else if (decoded.upperVfShadowReg != 0u)
    {
        float oldVf[4]{};
        float upperVf[4]{};
        std::memcpy(oldVf,
                    m_state.vf[decoded.upperVfShadowReg],
                    sizeof(oldVf));
        if constexpr (kStatic) execUpperImpl(decoded.upper); else execUpper(decoded.upper);
        std::memcpy(upperVf,
                    m_state.vf[decoded.upperVfShadowReg],
                    sizeof(upperVf));
        std::memcpy(m_state.vf[decoded.upperVfShadowReg],
                    oldVf,
                    sizeof(oldVf));
        if constexpr (kStatic) execLowerImpl(decoded.lower, ctx.vuData, ctx.dataSize, *ctx.gs, ctx.memory, decoded.upper); else execLower(decoded.lower, ctx.vuData, ctx.dataSize, *ctx.gs, ctx.memory, decoded.upper);
        std::memcpy(m_state.vf[decoded.upperVfShadowReg],
                    upperVf,
                    sizeof(upperVf));
    }
    else
    {
        if constexpr (kStatic) execUpperImpl(decoded.upper); else execUpper(decoded.upper);
        if constexpr (kStatic) execLowerImpl(decoded.lower, ctx.vuData, ctx.dataSize, *ctx.gs, ctx.memory, decoded.upper); else execLower(decoded.lower, ctx.vuData, ctx.dataSize, *ctx.gs, ctx.memory, decoded.upper);
    }

    m_directStores = false;
    m_directFlags = false;
    m_viBranchBackupValid = false;

    if (!kStatic && m_traceArmed && m_state.branchPending &&
        (!traceWasBranchPending || m_state.branchTarget != traceWasBranchTarget) &&
        traceIssueIdx < m_traceTaken.size())
    {
        ++m_traceTaken[traceIssueIdx];
    }

    // E37: post-exec register state, before the revert/queue block
    // below restores the pipelined values.
    if (!kStatic && m_entryArmed)
    {
        recordEntryPair(entryPc, entryLo, entryUp, ctx.vuData, ctx.dataSize,
                        m_entryOldVi, m_entryOldVf);
    }

    if (hasUpperWrite)
    {
        std::memcpy(newUpperVf, m_state.vf[upperWrite.reg], sizeof(newUpperVf));
        std::memcpy(m_state.vf[upperWrite.reg], oldUpperVf, sizeof(oldUpperVf));
        const uint32_t latency =
            decoded.upperUsage.vfLatency != 0u
                ? decoded.upperUsage.vfLatency
                : decoded.upperUsage.latency;
#if PS2X_ENABLE_DET_HASH_TAP
        if (m_unit == Unit::VU1)
            ++(directUpperVf ? m_vbDirectVfWrites : m_vbQueuedVfWrites);
#endif
        if (directUpperVf)
            directVfWrite(upperWrite.reg, upperWrite.lanes, newUpperVf, latency);
        else
            queueVfWrite(upperWrite.reg, upperWrite.lanes, newUpperVf, latency);
    }
    if (hasDistinctLowerWrite)
    {
        std::memcpy(newLowerVf, m_state.vf[lowerWrite.reg], sizeof(newLowerVf));
        std::memcpy(m_state.vf[lowerWrite.reg], oldLowerVf, sizeof(oldLowerVf));
        const uint32_t latency = decoded.lowerUsage.vfLatency != 0u
                                     ? decoded.lowerUsage.vfLatency
                                     : decoded.lowerUsage.latency;
#if PS2X_ENABLE_DET_HASH_TAP
        if (m_unit == Unit::VU1)
            ++(directLowerVf ? m_vbDirectVfWrites : m_vbQueuedVfWrites);
#endif
        if (directLowerVf)
            directVfWrite(lowerWrite.reg, lowerWrite.lanes, newLowerVf, latency);
        else
            queueVfWrite(lowerWrite.reg, lowerWrite.lanes, newLowerVf, latency);
    }
    if (decoded.upperUsage.accWrite != 0u)
    {
        std::memcpy(newAcc, m_state.acc, sizeof(newAcc));
        std::memcpy(m_state.acc, oldAcc, sizeof(oldAcc));
        // ACC is forwarded to the next upper instruction. Its arithmetic
        // flags still use the normal four-cycle FMAC timeline.
        if (direct)
            directAccWrite(decoded.upperUsage.accWrite, newAcc, kAccForwardLatency);
        else
            queueAccWrite(decoded.upperUsage.accWrite, newAcc,
                          kAccForwardLatency);
    }
    if (writtenVi != 0u)
    {
        const int32_t newVi = m_state.vi[writtenVi];
        m_state.vi[writtenVi] = oldVi;
        const uint32_t latency =
            decoded.lowerUsage.viLatency != 0u
                ? decoded.lowerUsage.viLatency
                : decoded.lowerUsage.latency;
        if (directVi)
            directViWrite(writtenVi, newVi, latency);
        else
            queueViWrite(writtenVi, newVi, latency);
    }

    markPairWrites(decoded);
    if (writtenVi != 0u && decoded.lowerUsage.delaysNextBranchRead)
        recordViWriteForBranch(writtenVi, oldVi);

    m_state.vf[0][0] = 0.0f;
    m_state.vf[0][1] = 0.0f;
    m_state.vf[0][2] = 0.0f;
    m_state.vf[0][3] = 1.0f;
    m_state.vi[0] = 0;

    // VR2: generated images always cover the whole VU1 micro memory
    // (lookupRecompProgram), so their code size and address mask are constants.
    const uint32_t codeSize = kStatic ? kRecompCodeSize : ctx.codeSize;
    uint32_t nextPc = m_state.pc + 8u;
    if (nextPc >= codeSize)
        nextPc = 0u;
    m_state.pc = nextPc;

    if (m_state.branchPending)
    {
        if (m_state.branchDelay == 0u)
        {
            m_state.pc = m_state.branchTarget & (kStatic ? kRecompCodeSize - 1u : microAddressMask());
            m_state.branchPending = false;
        }
        else
        {
            --m_state.branchDelay;
        }
    }

    const bool dHalt = decoded.dBit && m_state.dBitEnabled;
    const bool tHalt = decoded.tBit && m_state.tBitEnabled;
    const bool haltBit = dHalt || tHalt;
    const bool haltBranch = haltBit && decoded.lowerUsage.pipeline == PipelineBranch;

    if (m_state.haltAfterDelaySlot)
    {
        m_state.stoppedByD = m_pendingHaltD;
        m_state.stoppedByT = m_pendingHaltT;
        ctx.programEnded = true;
    }
    else if (m_state.ebit)
        ctx.programEnded = true;
    else if (haltBit && !haltBranch)
    {
        m_state.stoppedByD = dHalt;
        m_state.stoppedByT = tHalt;
        ctx.programEnded = true;
    }
    else if (decoded.eBit)
        m_state.ebit = true;
    else if (haltBranch)
    {
        m_state.haltAfterDelaySlot = true;
        m_pendingHaltD = dHalt;
        m_pendingHaltT = tHalt;
    }

    advanceOneCycle();
#if PS2X_ENABLE_DET_HASH_TAP
    if (m_unit == Unit::VU1)
        (direct ? m_vbDirectCycles : m_vbQueuedCycles) += m_cycle - vbStartCycle;
    if constexpr (kStatic)
    {
        ++m_genIssuedPairs;
        m_genIssuedCycles += m_cycle - vbStartCycle;
        if constexpr (kBlock)
        {
            ++m_blockIssuedPairs;
            m_blockIssuedCycles += m_cycle - vbStartCycle;
        }
    }
#endif
    return ctx.programEnded;
}

// VR1: the checks at the top of run()'s loop, so a generated pair can hand off
// to the next one without returning to run(). When it returns false run()
// re-evaluates its loop header.
// VR2: only the stop request is left. The budget: issuePair stops a pair
// that starts at or past budgetEnd before it changes any state (the stall
// loop and the budget check come first), so the next pair returns true to
// run() exactly where this check would have returned false. The commit: a
// no-op here, since advanceOneCycle committed everything ready at m_cycle and
// nothing is queued in between. The pc bound: generated images cover the
// whole 16 KiB micro memory and every pc issuePair produces (pc + 8 wrapped
// at the code size, or a branch target masked to it) is an in-range multiple
// of 8.
PS2X_VU1_ALWAYS_INLINE inline bool VU1Interpreter::recompChainReady(RunContext &)
{
    return !m_stopRequested;
}

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
