// VX1: the VU1-only half of the static engine (was in ps2_vu1_core.cpp and
// ps2_vu1_recomp.cpp): construction, execute/resume with the E36/E37 trace
// and VRB1 capture hooks, XGKICK/PATH1, the trace helpers, VR2 stage-4 block
// planning and the hash-build block/direct counters. The shared engine is
// VuCore (ps2_vu_core.cpp, ps2_vu_recomp.cpp, ps2_vu_{upper,lower}.cpp).

#include "runtime/ps2_vu1.h"
#include "runtime/gs/ps2_gif_arbiter.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"
#include "ps2_gfx_stats.h"
#include "ps2_vu_detail.h"
#include "ps2_vu1_entry_trace.h"
#include "ps2_vu1_trace.h"
#include "ps2_vu1cap.h"
#include "ps2_vu_step_impl.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sstream>
#include <ps2_log.h>

namespace
{
    constexpr uint8_t laneBit(uint32_t component)
    {
        return static_cast<uint8_t>(1u << (3u - component));
    }
}

VU1Interpreter::VU1Interpreter()
{
    const char *flagElide = std::getenv("PS2X_VU1_FLAG_ELIDE");
    m_flagElideRequested = flagElide != nullptr && std::strcmp(flagElide, "1") == 0;
    reset();
}

void VU1Interpreter::execute(uint8_t *vuCode, uint32_t codeSize,
                             uint8_t *vuData, uint32_t dataSize,
                             GS &gs, PS2Memory *memory,
                             uint32_t startPC, uint32_t top, uint32_t itop,
                             uint32_t maxCycles)
{
#if PS2X_ENABLE_DET_HASH_TAP
    ++m_programStartCount;
#endif
    beginProgram(startPC, top, itop);
    // E36: key trace arming/dedupe on the MSCAL startPC; pick up the VIF1
    // snapshot stashed by noteMscal (absent for direct callers).
    m_traceProgramPC = startPC & microAddressMask();
    m_traceCtxValid = ps2_vu1_trace::consumeContext(m_traceCtx);
    // E37: pick up the entry-trace arm stashed by the VIF1 MSCAL hook.
    m_entryArmed = false;
    if (ps2_vu1_entry_trace::enabled() &&
        ps2_vu1_entry_trace::takeArm(m_traceProgramPC))
    {
        m_entryArmed = true;
        m_entryTarget = m_traceProgramPC;
        m_entryIdx = ps2_vu1_entry_trace::armIndex(m_traceProgramPC);
        m_entryPairs = 0u;
        m_entryArrivals = 0u;
        m_entryLines.clear();
        m_entryStoreValid = false;
    }
    // VRB1 DEV-ONLY capture (default off; local branch, never pushed).
    const bool vcap = ps2_vu1cap::on();
    if (vcap)
        ps2_vu1cap::Capture::instance().begin(
            memory ? memory->gs().vsyncTick.load(std::memory_order_relaxed) : 0u,
            startPC & microAddressMask(), m_state, m_cycle, top, itop,
            vuCode, codeSize, vuData, dataSize, false);
    run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
    if (vcap)
        ps2_vu1cap::Capture::instance().end(m_state, m_cycle, vuData);
}

void VU1Interpreter::resume(uint8_t *vuCode, uint32_t codeSize,
                            uint8_t *vuData, uint32_t dataSize,
                            GS &gs, PS2Memory *memory,
                            uint32_t top, uint32_t itop, uint32_t maxCycles)
{
    beginResume(top, itop);
    // E36: an MSCNT continues the program keyed at the last execute().
    m_traceCtxValid = ps2_vu1_trace::consumeContext(m_traceCtx);
    // E37: pair streams always close inside the arming run(); a resume
    // never continues one.
    m_entryArmed = false;
    // VRB1 DEV-ONLY capture: resumes are counted, never recorded (format v1
    // holds MSCAL runs only; VP1 saw 0 resumes in t1714-3000).
    const bool vcap = ps2_vu1cap::on();
    if (vcap)
        ps2_vu1cap::Capture::instance().begin(
            memory ? memory->gs().vsyncTick.load(std::memory_order_relaxed) : 0u,
            m_state.pc, m_state, m_cycle, top, itop,
            vuCode, codeSize, vuData, dataSize, true);
    run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
    if (vcap)
        ps2_vu1cap::Capture::instance().end(m_state, m_cycle, vuData);
}

// E36: arm the dev-only per-program trace (one member branch per pair
// when disarmed; histogram vectors only when armed).
void VU1Interpreter::beginRunTrace(uint32_t codeSize, const uint8_t *vuData, uint32_t dataSize)
{
    m_traceArmed = false;
    m_traceCountKicks = ps2_vu1_trace::enabled();
    if (m_traceCountKicks)
    {
        m_traceXgkick = 0u;
    }
    if (m_traceCountKicks && ps2_vu1_trace::armFor(m_traceProgramPC))
    {
        m_traceArmed = true;
        const size_t pairCount = codeSize / 8u;
        m_traceHist.assign(pairCount, 0u);
        m_traceTaken.assign(pairCount, 0u);
        snapshotTraceHeaders(vuData, dataSize);
    }
}

// VF1 Part 3: the six pinned images with no in-image FS*/FM* readers.
// Require a compiled-in match; unknown/new images retain exact flags.
// (run() cleared m_elideFmacFlags before this call.)
void VU1Interpreter::selectFlagElision(const RecompProgram *recomp)
{
    if (m_flagElideRequested && recomp != nullptr)
    {
        switch (recomp->hash)
        {
        case 0x00df9699b042f1b9ull:
        case 0x77b7173b0915df5full:
        case 0xa56458ed3544b351ull:
        case 0xad77d08a166608edull:
        case 0xb3bdaeaef374c80bull:
        case 0xf587398bb6fc4651ull:
            m_elideFmacFlags = true;
            break;
        default: // a214cc509116aaa4 has 13 MAC-reader sites.
            break;
        }
    }
}

void VU1Interpreter::endRunTrace(const uint8_t *vuCode, uint32_t codeSize, PS2Memory *memory,
                                 uint64_t cyclesUsed, bool budgetExhausted)
{
    // E36: dev-only per-program trace. Census for every exhausted program
    // in the window; a detail block for the first 40 distinct startPCs.
    if (ps2_vu1_trace::enabled() && budgetExhausted)
    {
        ps2_vu1_trace::noteCensus(m_traceProgramPC, cyclesUsed, m_traceXgkick);
        if (m_traceArmed)
        {
            std::string block;
            buildTraceDetail(vuCode, codeSize, memory, cyclesUsed, block);
            ps2_vu1_trace::emitDetail(m_traceProgramPC, block);
        }
    }
    // E37: close a still-open pair stream (stop condition never hit:
    // 0x418 never arrived, or the program/budget ended first).
    if (m_entryArmed)
    {
        m_entryArmed = false;
        ps2_vu1_entry_trace::finishEntry(m_entryIdx, m_entryLines,
                                         m_entryPairs, m_entryArrivals);
        m_entryLines.clear();
    }
    m_traceArmed = false;
    m_traceCountKicks = false;
}

// PS2X_VU1_RECOMP_STATS=1: the VU1-only counters after VuCore's [vu1-recomp]
// line (hash builds; same lines as before VX1).
void VU1Interpreter::printRecompStats() const
{
#if PS2X_ENABLE_DET_HASH_TAP
    // VR2 2D: block counters are kept in hash builds only (no per-entry
    // read-modify-write on the hot path).
    std::fprintf(stderr, "[vu1-blocks] on=%d entries=%llu pairs=%llu nostall_misses=%llu miss_off=%llu miss_branch=%llu miss_end=%llu miss_budget=%llu plaintail_misses=%llu\n",
                 m_blocksOn ? 1 : 0, static_cast<unsigned long long>(m_blockEntries),
                 static_cast<unsigned long long>(m_blockPairs),
                 static_cast<unsigned long long>(m_blockNoStallMisses),
                 static_cast<unsigned long long>(m_blockMissOff),
                 static_cast<unsigned long long>(m_blockMissBranch),
                 static_cast<unsigned long long>(m_blockMissEnd),
                 static_cast<unsigned long long>(m_blockMissBudget),
                 static_cast<unsigned long long>(m_blockPlainTailMisses));
    std::fprintf(stderr, "[vu1-blocks] gen_pairs=%llu block_pairs=%llu pair_share=%.4f gen_cycles=%llu block_cycles=%llu cycle_share=%.4f\n",
                 static_cast<unsigned long long>(m_genIssuedPairs),
                 static_cast<unsigned long long>(m_blockIssuedPairs),
                 m_genIssuedPairs != 0u ? static_cast<double>(m_blockIssuedPairs) / static_cast<double>(m_genIssuedPairs) : 0.0,
                 static_cast<unsigned long long>(m_genIssuedCycles),
                 static_cast<unsigned long long>(m_blockIssuedCycles),
                 m_genIssuedCycles != 0u ? static_cast<double>(m_blockIssuedCycles) / static_cast<double>(m_genIssuedCycles) : 0.0);
#endif
#if PS2X_ENABLE_DET_HASH_TAP
    const uint64_t pairCycles = m_vbDirectCycles + m_vbQueuedCycles;
    const uint64_t flagWrites = m_vbDirectFlagWrites + m_vbQueuedFlagWrites;
    std::fprintf(stderr, "[vu1-direct] direct_cycles=%llu queued_cycles=%llu direct_share=%.4f flag_direct=%llu flag_queued=%llu flag_direct_share=%.4f\n",
                 static_cast<unsigned long long>(m_vbDirectCycles),
                 static_cast<unsigned long long>(m_vbQueuedCycles),
                 pairCycles != 0u ? static_cast<double>(m_vbDirectCycles) / static_cast<double>(pairCycles) : 0.0,
                 static_cast<unsigned long long>(m_vbDirectFlagWrites),
                 static_cast<unsigned long long>(m_vbQueuedFlagWrites),
                 flagWrites != 0u ? static_cast<double>(m_vbDirectFlagWrites) / static_cast<double>(flagWrites) : 0.0);
    const uint64_t vfWrites = m_vbDirectVfWrites + m_vbQueuedVfWrites;
    std::fprintf(stderr, "[vu1-direct] vf_direct=%llu vf_queued=%llu vf_direct_share=%.4f\n",
                 static_cast<unsigned long long>(m_vbDirectVfWrites),
                 static_cast<unsigned long long>(m_vbQueuedVfWrites),
                 vfWrites != 0u ? static_cast<double>(m_vbDirectVfWrites) / static_cast<double>(vfWrites) : 0.0);
#endif
}

void VU1Interpreter::progressXgkick()
{
    if (!m_xgkick.active || !m_activeVuData || m_activeVuDataSize == 0u)
        return;

    ++m_xgkick.cycleCredit;
    while (m_xgkick.active && m_xgkick.cycleCredit >= 2u)
    {
        m_xgkick.cycleCredit -= 2u;
        if (m_xgkick.copiedBytes > XgkickPipeline::kBufferSize - 16u)
        {
            reportReservedInstruction(false, 0xFFFFFFFBu);
            m_xgkick.active = false;
            return;
        }

        const uint32_t qwordOffset = m_xgkick.copiedBytes;
        const uint32_t first = (m_xgkick.sourceAddress + m_xgkick.copiedBytes) % m_activeVuDataSize;
        if (first + 16u <= m_activeVuDataSize)
        {
            // E57: no wrap inside this qword, so (first + i) is the same byte
            // the per-byte modulo below selects.
            std::memcpy(m_xgkick.packet.data() + m_xgkick.copiedBytes, m_activeVuData + first, 16u);
        }
        else
        {
            for (uint32_t i = 0; i < 16u; ++i)
            {
                const uint32_t source = (m_xgkick.sourceAddress + m_xgkick.copiedBytes + i) % m_activeVuDataSize;
                m_xgkick.packet[m_xgkick.copiedBytes + i] = m_activeVuData[source];
            }
        }
        m_xgkick.copiedBytes += 16u;

        if (m_xgkick.currentTagEnd == 0u)
        {
            uint64_t tagLo = 0;
            std::memcpy(&tagLo, m_xgkick.packet.data() + qwordOffset, sizeof(tagLo));
            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint32_t format = static_cast<uint32_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;

            uint64_t tagBytes = 16u;
            if (format == 0u)
                tagBytes += static_cast<uint64_t>(nloop) * nreg * 16u;
            else if (format == 1u)
                tagBytes += ((static_cast<uint64_t>(nloop) * nreg + 1u) & ~1ull) * 8u;
            else if (format == 2u)
                tagBytes += static_cast<uint64_t>(nloop) * 16u;
            else
            {
                reportReservedInstruction(false, 0xFFFFFFF8u);
                m_xgkick.active = false;
                return;
            }

            if (tagBytes > XgkickPipeline::kBufferSize - qwordOffset)
            {
                reportReservedInstruction(false, 0xFFFFFFFBu);
                m_xgkick.active = false;
                return;
            }
            m_xgkick.currentTagEnd = qwordOffset + static_cast<uint32_t>(tagBytes);
            m_xgkick.currentTagEop = ((tagLo >> 15) & 1u) != 0u;
            if (m_xgkick.currentTagEop)
                m_xgkick.totalBytes = m_xgkick.currentTagEnd;
        }

        if (m_xgkick.copiedBytes >= m_xgkick.currentTagEnd)
        {
            if (m_xgkick.currentTagEop)
                finishXgkick();
            else
            {
                // The next transferred qword is another GIFtag.
                m_xgkick.currentTagEnd = 0u;
                m_xgkick.currentTagEop = false;
            }
        }
    }
}

void VU1Interpreter::finishXgkick()
{
    if (!m_xgkick.active)
        return;

    // VRB1 DEV-ONLY capture (default off): record this run's XGKICK bytes.
    if (ps2_vu1cap::recording())
        ps2_vu1cap::Capture::instance().xgk(m_xgkick.packet.data(), m_xgkick.totalBytes);
    if (m_activeMemory)
        m_activeMemory->submitGifPacket(GifPathId::Path1, m_xgkick.packet.data(), m_xgkick.totalBytes);
    else if (m_activeGs)
        m_activeGs->processGIFPacket(m_xgkick.packet.data(), m_xgkick.totalBytes);
    m_xgkick.active = false;
}

void VU1Interpreter::startXgkick(uint32_t qwordAddress)
{
    if (!m_activeVuData || m_activeVuDataSize < 16u)
        return;

    // E33: one relaxed check when stats are off.
    ps2_gfx_stats::noteXgkick();
    // E36: per-program XGKICK count for the dev-only trace. Counted on
    // every enabled run (not just histogram-armed ones) so each census
    // line carries its own program's count.
    if (m_traceCountKicks)
    {
        ++m_traceXgkick;
    }

    const uint32_t sourceAddress = (qwordAddress * 16u) % m_activeVuDataSize;
    m_xgkick.reset();
    m_xgkick.active = true;
    m_xgkick.sourceAddress = sourceAddress;
    m_xgkick.cycleCredit = 1u; // XGKICK's issue cycle counts toward PATH1.
    m_xgkick.issueCycle = m_cycle;
}

// VR2 stage 4: generated block functions. PS2X_VU1_BLOCKS=1 turns them on
// (default off until proven); off, a block leader's entry takes its pair
// function.
bool VU1Interpreter::blocksEnabled()
{
    static const bool enabled = []
    {
        const char *value = std::getenv("PS2X_VU1_BLOCKS");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

// VR2 stage 4: block plans for the emitter. Leaders: static branch targets,
// the pair after every branch's delay slot (fall-through, BAL/JALR returns)
// and after every E-bit delay slot (packed program starts), and pair 0. A
// block runs from its leader while pairs are plain: it stops before a
// reserved, XGKICK (unbounded stall) or D/T-bit (runtime halt) pair, ends
// after a branch or E-bit pair plus its delay slot (both left out when the
// slot is not plain or is itself a branch), never wraps, and holds at most
// kMaxBlockPairs. At least two pairs, else the leader keeps its pair function.
//
// noStall[k]: pair k's scoreboard read can be skipped. Every VF lane, VI and
// ACC it reads is either last written inside the block by pair p with
// k - p >= that write's latency (each pair takes at least one cycle), or not
// written in the block and k >= 3: every write issued before the block lands
// within kDirectMaxLatency (VF 4, VI <= 4, ACC 1), i.e. by entry + 3, and
// pair k issues at entry + k or later. FDIV/EFU/WAITQ/WAITP pairs keep the
// read (their Q/P pipelines are not tracked here). This mirrors
// markPairWrites: lower VF write unless suppressed, upper VF write, VI writes,
// ACC at kAccForwardLatency. A hash build counts violations
// (m_blockNoStallMisses).
//
// maxCycles: sum over pairs of 1 + worst stall (0 when noStall; 13
// FDIV/WAITQ; 54 EFU/WAITP; 4 otherwise) plus kDirectMaxLatency, so no stall
// reaches budgetEnd and every direct write of the block lands inside it.
void VU1Interpreter::planRecompBlocks(const uint8_t *vuCode, uint32_t codeSize,
                                      std::vector<RecompBlockPlan> &blocks) const
{
    // GV2: 32 covers the full a56458 31-pair loop (0x0628-0x0718) in one block.
    constexpr uint32_t kMaxBlockPairs = 32u;
    const uint32_t pairs = codeSize / 8u;
    std::vector<DecodedInstructionPair> d(pairs);
    std::vector<uint8_t> branch(pairs, 0u), plain(pairs, 0u), leader(pairs, 0u);
    for (uint32_t i = 0; i < pairs; ++i)
    {
        d[i] = decodeInstructionPair(vuCode, i * 8u);
        plain[i] = !d[i].upperUsage.reserved && !d[i].lowerUsage.reserved &&
                   d[i].lowerUsage.pipeline != PipelineXgkick && !d[i].dBit && !d[i].tBit;
        if (d[i].iBit)
            continue;
        const uint8_t opHi = static_cast<uint8_t>((d[i].lower >> 25) & 0x7Fu);
        const bool jump = opHi == 0x24u || opHi == 0x25u;
        const bool uncond = opHi == 0x20u || opHi == 0x21u;
        const bool cond = opHi == 0x28u || opHi == 0x29u || (opHi >= 0x2Cu && opHi <= 0x2Fu);
        branch[i] = jump || uncond || cond;
        if (uncond || cond)
        {
            const int32_t imm = static_cast<int32_t>(d[i].lower << 21) >> 21;
            const uint32_t pc = (i * 8u + 8u + static_cast<uint32_t>(imm * 8)) & microAddressMask();
            if (pc + 8u <= codeSize)
                leader[pc / 8u] = 1u;
        }
    }
    leader[0] = 1u;
    for (uint32_t i = 0; i + 2u < pairs; ++i)
        if (branch[i] != 0u || d[i].eBit)
            leader[i + 2u] = 1u;

    blocks.clear();
    for (uint32_t s = 0; s < pairs; ++s)
    {
        if (leader[s] == 0u)
            continue;
        RecompBlockPlan block;
        block.start = s;
        for (uint32_t q = s; q < pairs && block.pairs.size() < kMaxBlockPairs && plain[q] != 0u; ++q)
        {
            if (branch[q] != 0u || d[q].eBit)
            {
                const uint32_t slot = q + 1u;
                if (slot < pairs && plain[slot] != 0u && branch[slot] == 0u &&
                    block.pairs.size() + 2u <= kMaxBlockPairs)
                {
                    block.pairs.push_back(q);
                    block.pairs.push_back(slot);
                }
                break;
            }
            block.pairs.push_back(q);
        }
        if (block.pairs.size() < 2u)
            continue;

        // Latest in-block writer per VF lane / VI / ACC lane: (pair position + 1, latency).
        std::array<std::array<std::pair<uint32_t, uint32_t>, 4>, 32> vfWriter{};
        std::array<std::pair<uint32_t, uint32_t>, 16> viWriter{};
        std::array<std::pair<uint32_t, uint32_t>, 4> accWriter{};
        uint32_t cycles = kDirectMaxLatency;
        for (uint32_t k = 0; k < block.pairs.size(); ++k)
        {
            const DecodedInstructionPair &p = d[block.pairs[k]];
            const auto ready = [&](const std::pair<uint32_t, uint32_t> &w)
            {
                return w.first != 0u ? k - (w.first - 1u) >= w.second : k >= 3u;
            };
            bool quiet = p.lowerUsage.pipeline != PipelineFdiv && p.lowerUsage.pipeline != PipelineEfu &&
                         !p.lowerUsage.waitQ && !p.lowerUsage.waitP;
            for (const InstructionUsage *usage : {&p.upperUsage, &p.lowerUsage})
            {
                for (uint32_t r = 0; r < usage->vfReadCount; ++r)
                    for (uint32_t c = 0; c < 4u; ++c)
                        if ((usage->vfRead[r].lanes & laneBit(c)) != 0u)
                            quiet = quiet && ready(vfWriter[usage->vfRead[r].reg][c]);
                for (uint32_t v = usage->viRead & 0xFFFEu; v != 0u; v &= v - 1u)
                    quiet = quiet && ready(viWriter[std::countr_zero(v)]);
                for (uint32_t c = 0; c < 4u; ++c)
                    if ((usage->accRead & laneBit(c)) != 0u)
                        quiet = quiet && ready(accWriter[c]);
            }
            block.noStall.push_back(quiet ? 1u : 0u);
            // VR4 D2: plain tail = neither the branch/E-bit pair nor its delay slot.
            const bool ender = branch[block.pairs[k]] != 0u || p.eBit;
            const bool slot = k > 0u && (branch[block.pairs[k - 1u]] != 0u || d[block.pairs[k - 1u]].eBit);
            block.plainTail.push_back(!ender && !slot ? 1u : 0u);
            const uint32_t worst = quiet ? 0u
                                   : (p.lowerUsage.pipeline == PipelineEfu || p.lowerUsage.waitP)    ? 54u
                                   : (p.lowerUsage.pipeline == PipelineFdiv || p.lowerUsage.waitQ) ? 13u
                                                                                                    : 4u;
            cycles += 1u + worst;

            const auto mark = [&](const VfAccess &w, uint32_t latency)
            {
                for (uint32_t c = 0; c < 4u; ++c)
                    if ((w.lanes & laneBit(c)) != 0u)
                        vfWriter[w.reg][c] = {k + 1u, latency};
            };
            const VfAccess lw = p.lowerUsage.vfWrite;
            if (lw.reg != 0u && p.suppressedLowerVf != lw.reg)
                mark(lw, p.lowerUsage.vfLatency != 0u ? p.lowerUsage.vfLatency : p.lowerUsage.latency);
            if (p.upperUsage.vfWrite.reg != 0u)
                mark(p.upperUsage.vfWrite, p.upperUsage.vfLatency != 0u ? p.upperUsage.vfLatency : p.upperUsage.latency);
            for (uint32_t v = p.lowerUsage.viWrite & 0xFFFEu; v != 0u; v &= v - 1u)
                viWriter[std::countr_zero(v)] = {k + 1u, p.lowerUsage.viLatency != 0u ? p.lowerUsage.viLatency
                                                                                       : p.lowerUsage.latency};
            for (uint32_t c = 0; c < 4u; ++c)
                if ((p.upperUsage.accWrite & laneBit(c)) != 0u)
                    accWriter[c] = {k + 1u, kAccForwardLatency};
        }
        block.maxCycles = cycles;
        blocks.push_back(std::move(block));
    }
}

// E36: dev-only trace helpers. The mini-decoders below mirror the
// dispatch in execLower (bits 31:25 primary opcode, Lower1 funct in the
// low 6 bits, Lower1-special funct2 per DobieStation). They name loop
// control, integer couners, flag tests and XGKICK/X TOP ops; anything
// else prints as opHi/funct hex. Upper pairs print raw except NOP.

namespace
{
    struct TraceLowerDesc
    {
        std::string text;
        const char *branchOp = nullptr; // set for B/BAL/JR/JALR/IBcc
        bool targetStatic = false;
        int16_t imm = 0;
    };

    const char *traceLower1SpecialName(uint8_t funct2)
    {
        switch (funct2)
        {
        case 0x30: return "MOVE";
        case 0x31: return "MR32";
        case 0x34: return "LQI";
        case 0x35: return "SQI";
        case 0x36: return "LQD";
        case 0x37: return "SQD";
        case 0x38: return "DIV";
        case 0x39: return "SQRT";
        case 0x3A: return "RSQRT";
        case 0x3B: return "WAITQ";
        case 0x3C: return "MTIR";
        case 0x3D: return "MFIR";
        case 0x3E: return "ILWR";
        case 0x3F: return "ISWR";
        case 0x40: return "RNEXT";
        case 0x41: return "RGET";
        case 0x42: return "RINIT";
        case 0x43: return "RXOR";
        case 0x64: return "MFP";
        case 0x68: return "XTOP";
        case 0x69: return "XITOP";
        case 0x6C: return "XGKICK";
        case 0x70: return "ESADD";
        case 0x71: return "ERSADD";
        case 0x72: return "ELENG";
        case 0x73: return "ERLENG";
        case 0x74: return "EATANxy";
        case 0x75: return "EATANxz";
        case 0x76: return "ESUM";
        case 0x78: return "ESQRT";
        case 0x79: return "ERSQRT";
        case 0x7A: return "ERCPR";
        case 0x7B: return "WAITP";
        case 0x7C: return "ESIN";
        case 0x7D: return "EATAN";
        case 0x7E: return "EEXP";
        default: return nullptr;
        }
    }

    bool traceIsFlagOpName(const std::string &text)
    {
        static const char *kFlagOps[] = {
            "FCAND", "FSAND", "FMAND", "FMOR", "FMEQ",
            "FCEQ", "FSEQ", "FCSET", "FSSET", "FCOR", "FSOR", "FCGET",
        };
        for (const char *op : kFlagOps)
        {
            if (text.compare(0, std::strlen(op), op) == 0)
            {
                return true;
            }
        }
        return false;
    }

    TraceLowerDesc traceDescribeLower(uint32_t w)
    {
        TraceLowerDesc d;
        char buf[96];
        if (w == 0x00000000u || w == 0x8000033Cu)
        {
            d.text = "NOP";
            return d;
        }
        const uint32_t opHi = (w >> 25) & 0x7Fu;
        const uint8_t it = VIT(w);
        const uint8_t is = VIS(w);
        const uint8_t id = VID(w);
        const int16_t imm = IMM11(w);
        switch (opHi)
        {
        case 0x00:
            std::snprintf(buf, sizeof(buf), "LQ vf%u,%d(vi%u)", FT(w), imm, is);
            d.text = buf;
            return d;
        case 0x01:
            std::snprintf(buf, sizeof(buf), "SQ vf%u,%d(vi%u)", FS(w), imm, it);
            d.text = buf;
            return d;
        case 0x04:
            std::snprintf(buf, sizeof(buf), "ILW vi%u,%d(vi%u)", it, imm, is);
            d.text = buf;
            return d;
        case 0x05:
            std::snprintf(buf, sizeof(buf), "ISW vi%u,%d(vi%u)", it, imm, is);
            d.text = buf;
            return d;
        case 0x08:
        case 0x09:
        {
            const int imm15 = (int)(int16_t)((w & 0x7FFu) | ((w >> 10) & 0x7800u));
            std::snprintf(buf, sizeof(buf), "%s vi%u,vi%u,%d",
                          opHi == 0x08 ? "IADDIU" : "ISUBIU", it, is, imm15);
            d.text = buf;
            return d;
        }
        case 0x10:
            std::snprintf(buf, sizeof(buf), "FCEQ 0x%x", w & 0xFFFFFFu);
            d.text = buf;
            return d;
        case 0x11:
            std::snprintf(buf, sizeof(buf), "FCSET 0x%x", w & 0xFFFFFFu);
            d.text = buf;
            return d;
        case 0x12:
        case 0x13:
        case 0x14:
        case 0x15:
        case 0x16:
        case 0x17:
        {
            static const char *kNames[] = {"?", "?", "FCAND", "FCOR", "FSEQ", "FSSET", "FSAND", "FSOR"};
            std::snprintf(buf, sizeof(buf), "%s vi%u,0x%x", kNames[opHi - 0x10], it, w & 0x7FFu);
            d.text = buf;
            return d;
        }
        case 0x18:
            std::snprintf(buf, sizeof(buf), "FMEQ vi%u,vi%u", it, is);
            d.text = buf;
            return d;
        case 0x1A:
            std::snprintf(buf, sizeof(buf), "FMAND vi%u,vi%u", it, is);
            d.text = buf;
            return d;
        case 0x1B:
            std::snprintf(buf, sizeof(buf), "FMOR vi%u,vi%u", it, is);
            d.text = buf;
            return d;
        case 0x1C:
            std::snprintf(buf, sizeof(buf), "FCGET vi%u", it);
            d.text = buf;
            return d;
        case 0x20:
            std::snprintf(buf, sizeof(buf), "B %d", imm);
            d.text = buf;
            d.branchOp = "B";
            d.targetStatic = true;
            d.imm = imm;
            return d;
        case 0x21:
            std::snprintf(buf, sizeof(buf), "BAL vi%u,%d", it, imm);
            d.text = buf;
            d.branchOp = "BAL";
            d.targetStatic = true;
            d.imm = imm;
            return d;
        case 0x24:
            std::snprintf(buf, sizeof(buf), "JR (vi%u)", is);
            d.text = buf;
            d.branchOp = "JR";
            return d;
        case 0x25:
            std::snprintf(buf, sizeof(buf), "JALR vi%u,(vi%u)", it, is);
            d.text = buf;
            d.branchOp = "JALR";
            return d;
        case 0x28:
        case 0x29:
        case 0x2C:
        case 0x2D:
        case 0x2E:
        case 0x2F:
        {
            const char *name = "?";
            switch (opHi)
            {
            case 0x28: name = "IBEQ"; break;
            case 0x29: name = "IBNE"; break;
            case 0x2C: name = "IBLTZ"; break;
            case 0x2D: name = "IBGTZ"; break;
            case 0x2E: name = "IBLEZ"; break;
            case 0x2F: name = "IBGEZ"; break;
            }
            if (opHi == 0x28 || opHi == 0x29)
            {
                std::snprintf(buf, sizeof(buf), "%s vi%u,vi%u,%d", name, it, is, imm);
            }
            else
            {
                std::snprintf(buf, sizeof(buf), "%s vi%u,%d", name, is, imm);
            }
            d.text = buf;
            d.branchOp = name;
            d.targetStatic = true;
            d.imm = imm;
            return d;
        }
        case 0x40:
        {
            const uint8_t funct = w & 0x3Fu;
            if (funct == 0x30 || funct == 0x31 || funct == 0x32 ||
                funct == 0x34 || funct == 0x35)
            {
                const char *name = "?";
                switch (funct)
                {
                case 0x30: name = "IADD"; break;
                case 0x31: name = "ISUB"; break;
                case 0x32: name = "IADDI"; break;
                case 0x34: name = "IAND"; break;
                case 0x35: name = "IOR"; break;
                }
                if (funct == 0x32)
                {
                    const int imm5 = (int)((int32_t)((w >> 6) & 0x1F) << 27 >> 27);
                    std::snprintf(buf, sizeof(buf), "IADDI vi%u,vi%u,%d", it, is, imm5);
                }
                else
                {
                    std::snprintf(buf, sizeof(buf), "%s vi%u,vi%u,vi%u", name, id, is, it);
                }
                d.text = buf;
                return d;
            }
            if (funct >= 0x3Cu)
            {
                const uint8_t funct2 = (uint8_t)((w & 0x3u) | ((w >> 4) & 0x7Cu));
                const char *name = traceLower1SpecialName(funct2);
                if (name != nullptr)
                {
                    if (funct2 == 0x68 || funct2 == 0x69)
                    {
                        std::snprintf(buf, sizeof(buf), "%s vi%u", name, it);
                    }
                    else if (funct2 == 0x6C)
                    {
                        std::snprintf(buf, sizeof(buf), "XGKICK (vi%u)", is);
                    }
                    else if (funct2 == 0x3C || funct2 == 0x3D)
                    {
                        std::snprintf(buf, sizeof(buf), "%s vi%u,vf%u", name,
                                      funct2 == 0x3C ? id : it,
                                      funct2 == 0x3C ? FS(w) : FT(w));
                    }
                    else
                    {
                        std::snprintf(buf, sizeof(buf), "%s vf%u,vf%u", name, FT(w), FS(w));
                    }
                    d.text = buf;
                    return d;
                }
            }
            std::snprintf(buf, sizeof(buf), "lower1:0x%02x", funct);
            d.text = buf;
            return d;
        }
        default:
            std::snprintf(buf, sizeof(buf), "opHi=0x%02x", opHi);
            d.text = buf;
            return d;
        }
    }
} // namespace

// E37 DEV-ONLY entry pair line. Called post-exec, pre-revert: m_state
// holds this pair's computed values; oldVi/oldVf are the pre-exec
// (post-stall) state. Loads report the memory row read; stores report
// the exact payload queued by queueStore (lanes merge at commit, one
// cycle later, mirroring the interpreter model).
void VU1Interpreter::recordEntryPair(uint32_t pc, uint32_t lo, uint32_t up,
                                     const uint8_t *vuData, uint32_t dataSize,
                                     const int32_t oldVi[16], const uint32_t oldVf[32][4])
{
    uint32_t newVf[32][4];
    std::memcpy(newVf, m_state.vf, sizeof(newVf));
    const int32_t *newVi = m_state.vi;

    std::ostringstream s;
    s << std::hex;
    char num[64];
    std::snprintf(num, sizeof(num), "pair pc=0x%x up=%08x lo=%08x", pc, up, lo);
    s << num;
    TraceLowerDesc ld = traceDescribeLower(lo);
    s << " " << ld.text << " | up ";
    if (up == 0x000002FFu)
    {
        s << "NOP";
    }
    else
    {
        std::snprintf(num, sizeof(num), "0x%08x", up);
        s << num;
    }
    for (uint32_t r = 1u; r < 16u; ++r)
    {
        if (oldVi[r] != newVi[r])
        {
            std::snprintf(num, sizeof(num), " | vi%u:%04x->%04x", r,
                          static_cast<uint32_t>(oldVi[r]) & 0xFFFFu,
                          static_cast<uint32_t>(newVi[r]) & 0xFFFFu);
            s << num;
        }
    }
    static const char kLane[4] = {'x', 'y', 'z', 'w'};
    for (uint32_t r = 1u; r < 32u; ++r)
    {
        for (uint32_t c = 0u; c < 4u; ++c)
        {
            if (oldVf[r][c] != newVf[r][c])
            {
                std::snprintf(num, sizeof(num), " | vf%u.%c:%08x->%08x", r,
                              kLane[c], oldVf[r][c], newVf[r][c]);
                s << num;
            }
        }
    }
    // VU data memory traffic. Address math mirrors execLower.
    const uint32_t opHi = (lo >> 25) & 0x7Fu;
    bool isLoad = (opHi == 0x00u || opHi == 0x04u);
    bool isStore = (opHi == 0x01u || opHi == 0x05u);
    uint32_t memRow = 0u;
    bool haveRow = false;
    if (opHi == 0x00u || opHi == 0x01u || opHi == 0x04u || opHi == 0x05u)
    {
        const int32_t base = (opHi == 0x01u || opHi == 0x05u)
                                 ? oldVi[VIT(lo)]
                                 : oldVi[VIS(lo)];
        memRow = static_cast<uint32_t>(base + IMM11(lo)) & 0x3FFu;
        haveRow = true;
    }
    else if (opHi == 0x40u && (lo & 0x3Fu) >= 0x3Cu)
    {
        const uint8_t funct2 = static_cast<uint8_t>((lo & 0x3u) | ((lo >> 4) & 0x7Cu));
        if (funct2 == 0x34u || funct2 == 0x3Eu) // LQI, ILWR
        {
            memRow = static_cast<uint32_t>(static_cast<uint16_t>(oldVi[VIS(lo)])) & 0x3FFu;
            haveRow = true;
            isLoad = true;
        }
        else if (funct2 == 0x36u) // LQD (pre-decrement)
        {
            memRow = static_cast<uint32_t>(static_cast<uint16_t>(oldVi[VIS(lo)] - 1)) & 0x3FFu;
            haveRow = true;
            isLoad = true;
        }
        else if (funct2 == 0x35u || funct2 == 0x3Fu) // SQI, ISWR
        {
            memRow = static_cast<uint32_t>(static_cast<uint16_t>(oldVi[VIT(lo)])) & 0x3FFu;
            haveRow = true;
            isStore = true;
        }
        else if (funct2 == 0x37u) // SQD (pre-decrement)
        {
            memRow = static_cast<uint32_t>(static_cast<uint16_t>(oldVi[VIT(lo)] - 1)) & 0x3FFu;
            haveRow = true;
            isStore = true;
        }
    }
    if (isLoad && haveRow && vuData != nullptr &&
        static_cast<uint64_t>(memRow) * 16u + 16u <= dataSize)
    {
        uint32_t words[4]{};
        std::memcpy(words, vuData + memRow * 16u, sizeof(words));
        std::snprintf(num, sizeof(num), " | rd %u:%08x %08x %08x %08x", memRow,
                      words[0], words[1], words[2], words[3]);
        s << num;
    }
    if (isStore && m_entryStoreValid)
    {
        std::snprintf(num, sizeof(num), " | wr %u:%08x %08x %08x %08x",
                      m_entryStoreAddr / 16u,
                      m_entryStoreWords[0], m_entryStoreWords[1],
                      m_entryStoreWords[2], m_entryStoreWords[3]);
        s << num;
    }
    m_entryLines.push_back(s.str());
    ++m_entryPairs;
    if (pc == ps2_vu1_entry_trace::kLoopHeadPc)
    {
        ++m_entryArrivals;
    }
    // Stop after the first 0x418 arrival plus 3 loop iterations (4th
    // arrival recorded), or at the pair cap; execution continues.
    if (m_entryArrivals >= 4u ||
        m_entryPairs >= ps2_vu1_entry_trace::kMaxPairsPerBlock)
    {
        m_entryArmed = false;
        ps2_vu1_entry_trace::finishEntry(m_entryIdx, m_entryLines,
                                         m_entryPairs, m_entryArrivals);
        m_entryLines.clear();
    }
}

void VU1Interpreter::snapshotTraceHeaders(const uint8_t *vuData, uint32_t dataSize)
{
    m_traceTopQw.fill(0u);
    m_traceItopQw.fill(0u);
    if (vuData == nullptr || dataSize == 0u)
    {
        return;
    }
    for (uint32_t slot = 0u; slot < 2u; ++slot)
    {
        const uint32_t row = (slot == 0u ? m_state.top : m_state.itop) & 0x3FFu;
        const uint64_t base = static_cast<uint64_t>(row) * 16u;
        std::array<uint32_t, 32> &dst = (slot == 0u ? m_traceTopQw : m_traceItopQw);
        for (uint32_t i = 0u; i < 32u; ++i)
        {
            const uint64_t off = base + static_cast<uint64_t>(i) * 4u;
            if (off + 4u <= dataSize)
            {
                uint32_t word = 0u;
                std::memcpy(&word, vuData + off, sizeof(word));
                dst[i] = word;
            }
        }
    }
}

void VU1Interpreter::buildTraceDetail(const uint8_t *vuCode, uint32_t codeSize,
                                      PS2Memory *memory, uint64_t cyclesUsed,
                                      std::string &out)
{
    std::ostringstream s;
    s << std::hex;
    const uint32_t pcMask = microAddressMask();
    char num[32];

    // Header: MSCAL context + run totals.
    const char *ctxName = "none";
    if (m_traceCtxValid)
    {
        ctxName = m_traceCtx.isMscnt ? "mscnt" : "mscal";
    }
    std::snprintf(num, sizeof(num), "0x%x", m_traceProgramPC);
    s << "detail startPC=" << num;
    if (m_traceCtxValid)
    {
        s << " top=" << std::dec << m_traceCtx.top
          << " itop=" << m_traceCtx.itop
          << " base=" << m_traceCtx.base
          << " ofst=" << m_traceCtx.ofst
          << " tops=" << m_traceCtx.tops
          << " itops=" << m_traceCtx.itops
          << " dbf=" << (m_traceCtx.dbf ? 1 : 0) << std::hex;
    }
    else
    {
        s << " top=- itop=- base=- ofst=- tops=- itops=- dbf=-";
    }
    s << " ctx=" << ctxName;
    s << std::dec << " cycles=" << cyclesUsed << " xgkick=" << m_traceXgkick << "\n";

    // Input header qwords at TOP and ITOP (program-start snapshot).
    s << "topq";
    for (uint32_t w : m_traceTopQw)
    {
        std::snprintf(num, sizeof(num), " %08x", w);
        s << num;
    }
    s << "\nitopq";
    for (uint32_t w : m_traceItopQw)
    {
        std::snprintf(num, sizeof(num), " %08x", w);
        s << num;
    }
    s << "\nvi";
    for (uint32_t r = 0u; r < 16u; ++r)
    {
        std::snprintf(num, sizeof(num), " %08x", static_cast<uint32_t>(m_state.vi[r]));
        s << num;
    }
    std::snprintf(num, sizeof(num), " mac=%08x status=%08x clip=%08x",
                  m_state.mac, m_state.status, m_state.clip);
    s << num;
    std::snprintf(num, sizeof(num), " endpc=0x%x", m_state.pc);
    s << num << "\n";

    // PC-visit and branch-taken histograms, top 10 each.
    std::vector<std::pair<uint32_t, uint32_t>> hist; // (count, pc)
    std::vector<std::pair<uint32_t, uint32_t>> taken;
    for (size_t i = 0u; i < m_traceHist.size(); ++i)
    {
        if (m_traceHist[i] != 0u)
        {
            hist.emplace_back(m_traceHist[i], static_cast<uint32_t>(i * 8u));
        }
        if (i < m_traceTaken.size() && m_traceTaken[i] != 0u)
        {
            taken.emplace_back(m_traceTaken[i], static_cast<uint32_t>(i * 8u));
        }
    }
    const auto byCountDesc = [](const auto &a, const auto &b)
    {
        if (a.first != b.first)
        {
            return a.first > b.first;
        }
        return a.second < b.second;
    };
    std::sort(hist.begin(), hist.end(), byCountDesc);
    std::sort(taken.begin(), taken.end(), byCountDesc);
    s << "hist";
    if (hist.empty())
    {
        s << " none";
    }
    for (size_t i = 0u; i < hist.size() && i < 10u; ++i)
    {
        std::snprintf(num, sizeof(num), " 0x%x=%u", hist[i].second, hist[i].first);
        s << num;
    }
    s << "\ntaken";
    if (taken.empty())
    {
        s << " none";
    }
    for (size_t i = 0u; i < taken.size() && i < 10u; ++i)
    {
        std::snprintf(num, sizeof(num), " 0x%x=%u", taken[i].second, taken[i].first);
        s << num;
    }
    s << "\n";

    // Hottest backward branch: max taken count among taken branches whose
    // static target runs backward (or is the branch itself); JR/JALR are
    // dynamic and only win when no static-backward branch took.
    bool haveBranch = false;
    bool branchBackward = false;
    uint32_t branchPc = 0u;
    TraceLowerDesc branchDesc;
    uint32_t branchTaken = 0u;
    uint32_t branchTarget = 0u;
    bool branchTargetKnown = false;
    // `taken` is hottest-first: the first entry is the hottest taken
    // branch overall; the first static-backward entry is the hottest loop.
    for (const auto &t : taken)
    {
        const uint32_t pc = t.second;
        if (pc + 8u > codeSize)
        {
            continue;
        }
        uint32_t lo = 0u;
        std::memcpy(&lo, vuCode + pc, sizeof(lo));
        TraceLowerDesc d = traceDescribeLower(lo);
        if (d.branchOp == nullptr)
        {
            continue;
        }
        bool backward = false;
        uint32_t target = 0u;
        bool known = false;
        if (d.targetStatic)
        {
            target = (pc + 8u + static_cast<uint32_t>(d.imm * 8)) & pcMask;
            known = true;
            backward = target <= pc;
        }
        if (!haveBranch || (backward && !branchBackward))
        {
            haveBranch = true;
            branchBackward = backward;
            branchPc = pc;
            branchDesc = d;
            branchTaken = t.first;
            branchTarget = target;
            branchTargetKnown = known;
        }
    }
    uint32_t branchVisits = 0u;
    if (haveBranch && branchPc / 8u < m_traceHist.size())
    {
        branchVisits = m_traceHist[branchPc / 8u];
    }
    // Flag ops inside the loop span (static target..branch PC).
    std::string bodyFlags = "none";
    if (haveBranch && branchTargetKnown)
    {
        std::string acc;
        for (uint32_t pc = branchTarget; pc <= branchPc && pc + 8u <= codeSize; pc += 8u)
        {
            uint32_t lo = 0u;
            std::memcpy(&lo, vuCode + pc, sizeof(lo));
            TraceLowerDesc d = traceDescribeLower(lo);
            if (traceIsFlagOpName(d.text) && acc.find(d.text.substr(0, d.text.find(' '))) == std::string::npos)
            {
                if (!acc.empty())
                {
                    acc += ",";
                }
                acc += d.text.substr(0, d.text.find(' '));
            }
        }
        if (!acc.empty())
        {
            bodyFlags = acc;
        }
    }
    if (haveBranch)
    {
        uint32_t lo = 0u;
        std::memcpy(&lo, vuCode + branchPc, sizeof(lo));
        const uint8_t vis = VIS(lo);
        const uint8_t vit = VIT(lo);
        std::snprintf(num, sizeof(num), "0x%x", branchPc);
        s << "branch pc=" << num << " op=" << branchDesc.branchOp;
        const uint32_t opHi = (lo >> 25) & 0x7Fu;
        if (opHi == 0x20)
        {
            std::snprintf(num, sizeof(num), " imm=%d", branchDesc.imm);
            s << " is=- it=-" << num;
        }
        else if (opHi == 0x21 || opHi == 0x24 || opHi == 0x25)
        {
            s << " is=" << std::dec << static_cast<unsigned>(vis)
              << " it=" << static_cast<unsigned>(vit) << std::hex;
        }
        else
        {
            s << " is=" << std::dec << static_cast<unsigned>(vis)
              << " it=" << static_cast<unsigned>(vit);
            std::snprintf(num, sizeof(num), " imm=%d", branchDesc.imm);
            s << num << std::hex;
        }
        if (branchTargetKnown)
        {
            std::snprintf(num, sizeof(num), " target=0x%x", branchTarget);
            s << num;
        }
        else
        {
            s << " target=dyn";
        }
        s << std::dec << " taken=" << branchTaken << " visits=" << branchVisits;
        s << " vi_is=" << m_state.vi[vis] << " vi_it=" << m_state.vi[vit];
        s << " bodyflags=" << bodyFlags << "\n";
    }
    else
    {
        s << "branch none\n";
    }

    // Loop body disassembly: the static span target..PC plus the branch
    // delay slot. Spans over 32 PCs keep the 32 ending at the delay slot
    // (branch context over loop head); the flag-op scan above already
    // covers the whole span. Without a static branch, the 32 most-visited
    // PCs in ascending order.
    std::vector<uint32_t> bodyPcs;
    if (haveBranch && branchTargetKnown)
    {
        std::vector<uint32_t> span;
        for (uint32_t pc = branchTarget; pc <= branchPc && span.size() < 4096u; pc += 8u)
        {
            span.push_back(pc);
        }
        if (branchPc + 8u <= codeSize)
        {
            span.push_back(branchPc + 8u); // delay slot
        }
        const size_t keep = 32u;
        const size_t skip = span.size() > keep ? span.size() - keep : 0u;
        for (size_t i = skip; i < span.size(); ++i)
        {
            bodyPcs.push_back(span[i]);
        }
    }
    else
    {
        std::vector<std::pair<uint32_t, uint32_t>> byPc = hist;
        std::sort(byPc.begin(), byPc.end(),
                  [](const auto &a, const auto &b)
                  { return a.second < b.second; });
        for (size_t i = 0u; i < byPc.size() && bodyPcs.size() < 32u; ++i)
        {
            bodyPcs.push_back(byPc[i].second);
        }
    }
    for (uint32_t pc : bodyPcs)
    {
        if (pc + 8u > codeSize)
        {
            continue;
        }
        uint32_t lo = 0u, up = 0u;
        std::memcpy(&lo, vuCode + pc, sizeof(lo));
        std::memcpy(&up, vuCode + pc + 4u, sizeof(up));
        DecodedInstructionPair decoded = getDecodedInstructionPairForPc(vuCode, codeSize, memory, pc);
        TraceLowerDesc ld = traceDescribeLower(lo);
        std::string upText = (up == 0x000002FFu) ? "NOP" : "upper";
        std::snprintf(num, sizeof(num), "0x%x", pc);
        s << "body " << num;
        std::snprintf(num, sizeof(num), " lo=0x%08x up=0x%08x", lo, up);
        s << num;
        if (decoded.eBit)
        {
            s << " E";
        }
        if (decoded.dBit)
        {
            s << " D";
        }
        if (decoded.tBit)
        {
            s << " T";
        }
        if (decoded.iBit)
        {
            s << " I";
        }
        s << " " << ld.text << " | " << upText << "\n";
    }
    // Entry path: visited PCs outside the emitted body (cap 16, ascending),
    // i.e. how the program reaches the loop and arms its limit/counter.
    s << "entry";
    {
        std::vector<bool> inBody(codeSize / 8u + 1u, false);
        for (uint32_t pc : bodyPcs)
        {
            if (pc / 8u < inBody.size())
            {
                inBody[pc / 8u] = true;
            }
        }
        std::vector<std::pair<uint32_t, uint32_t>> outside; // (pc, count)
        for (size_t i = 0u; i < m_traceHist.size(); ++i)
        {
            if (m_traceHist[i] != 0u && (i >= inBody.size() || !inBody[i]))
            {
                outside.emplace_back(static_cast<uint32_t>(i * 8u), m_traceHist[i]);
            }
        }
        std::sort(outside.begin(), outside.end(),
                  [](const auto &a, const auto &b)
                  { return a.first < b.first; });
        if (outside.empty())
        {
            s << " none";
        }
        for (size_t i = 0u; i < outside.size() && i < 16u; ++i)
        {
            std::snprintf(num, sizeof(num), " 0x%x=%u", outside[i].first, outside[i].second);
            s << num;
        }
        if (outside.size() > 16u)
        {
            std::snprintf(num, sizeof(num), " +%u more", static_cast<unsigned>(outside.size() - 16u));
            s << num;
        }
    }
    s << "\n";
    out = s.str();
}

