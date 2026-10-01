#ifndef PS2_VU1_H
#define PS2_VU1_H

// VX1: the static VU1 engine. The shared engine is VuCore (ps2_vu_core.h);
// this class adds what only VU1 has: XGKICK/PATH1 to the GIF, the E36/E37
// dev traces, VRB1 capture, VR2 stage-4 block functions, VF1 flag elision
// per image and the VB1 stats. Play runs VU1 on microVU; this engine is the
// PS2X_VU1_ENGINE=static path, the OM1 MISS fallback and the canonical
// register file (state()) microVU and savestates read.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ps2_vu1_trace.h"
#include "runtime/ps2_vu_core.h"

class VU1Interpreter final : public VuCore<VU1Interpreter>
{
    friend class VuCore<VU1Interpreter>;
    friend struct VuSavestate;
    template <uint64_t>
    friend struct VU1RecompImage;

public:
    static constexpr VuUnit kUnit = VuUnit::VU1;

    VU1Interpreter();

    void execute(uint8_t *vuCode, uint32_t codeSize,
                 uint8_t *vuData, uint32_t dataSize,
                 GS &gs, PS2Memory *memory = nullptr,
                 uint32_t startPC = 0, uint32_t top = 0, uint32_t itop = 0,
                 uint32_t maxCycles = 65536);

    void resume(uint8_t *vuCode, uint32_t codeSize,
                uint8_t *vuData, uint32_t dataSize,
                GS &gs, PS2Memory *memory = nullptr,
                uint32_t top = 0, uint32_t itop = 0, uint32_t maxCycles = 65536);

    // VR2 stage 4: -1 follows PS2X_VU1_BLOCKS (default off), 0 off, 1 on.
    void setBlocksForTest(int mode) { m_blocksOverride = mode; }
    uint64_t blockEntriesForTest() const { return m_blockEntries; }

    // Savestate format: the vu0 section writes this many zero bytes where the
    // vu1 section writes m_xgkick (VuSavestate; layout unchanged by VX1).
    static constexpr size_t kXgkickSavestateBytes = 0x10000u + 40u;

private:
    struct XgkickPipeline
    {
        static constexpr uint32_t kBufferSize = 0x10000u;
        std::array<uint8_t, kBufferSize> packet{};
        uint32_t sourceAddress = 0;
        uint32_t totalBytes = 0;
        uint32_t copiedBytes = 0;
        uint32_t currentTagEnd = 0;
        uint32_t cycleCredit = 0;
        uint64_t issueCycle = 0;
        bool active = false;
        bool currentTagEop = false;

        // NP1: per-execute reset used to value-init the whole struct (a 64 KiB
        // memset on every VU1/VU0 program). The packet bytes need no clearing:
        // every read is of this-transfer data (the tag parse reads the qword
        // just written; submit sends [0, totalBytes), all written after the
        // reset), and no tap hashes the staging buffer. Scalars only.
        void reset()
        {
            sourceAddress = 0;
            totalBytes = 0;
            copiedBytes = 0;
            currentTagEnd = 0;
            cycleCredit = 0;
            issueCycle = 0;
            active = false;
            currentTagEop = false;
        }
    };
    static_assert(sizeof(XgkickPipeline) == kXgkickSavestateBytes, "savestate xgkick blob size");

    bool m_flagElideRequested = false;
    XgkickPipeline m_xgkick{};

    // E36 DEV-ONLY per-program trace (PS2X_VU1_TRACE). Scratch for one
    // execute()/resume() pair; default-off cost is one member branch per
    // issued instruction pair. The XGKICK count resets on every run (not
    // just armed ones) so census lines never carry a stale count from an
    // earlier program; the histogram stays armed-gated (the costly part).
    bool m_entryArmed = false;
    uint32_t m_entryIdx = 0u;
    uint32_t m_entryTarget = 0u;
    uint32_t m_entryPairs = 0u;
    uint32_t m_entryArrivals = 0u;
    std::vector<std::string> m_entryLines;
    bool m_entryStoreValid = false;
    int32_t m_entryOldVi[16]{};
    uint32_t m_entryOldVf[32][4]{};
    uint32_t m_entryStoreAddr = 0u;
    uint32_t m_entryStoreWords[4]{};
    bool m_traceArmed = false;
    bool m_traceCountKicks = false;
    uint32_t m_traceProgramPC = 0u;
    ps2_vu1_trace::MscalContext m_traceCtx{};
    bool m_traceCtxValid = false;
    std::vector<uint32_t> m_traceHist;
    std::vector<uint32_t> m_traceTaken;
    uint32_t m_traceXgkick = 0u;
    std::array<uint32_t, 32> m_traceTopQw{};
    std::array<uint32_t, 32> m_traceItopQw{};

    // VR2 stage 4: entry guard of a generated block of pairs pairs whose
    // worst case (stalls and the last direct landing) is maxCycles.
    bool recompBlockReady(const RunContext &ctx, uint32_t maxCycles, uint32_t pairs);
    static bool blocksEnabled();
    void planRecompBlocks(const uint8_t *vuCode, uint32_t codeSize, std::vector<RecompBlockPlan> &blocks) const;
    bool m_blocksOn = false;
    int m_blocksOverride = -1;
    uint64_t m_blockEntries = 0;
    uint64_t m_blockPairs = 0;
    uint64_t m_blockNoStallMisses = 0; // hash builds: the no-stall proof failed (must stay 0)
    uint64_t m_blockPlainTailMisses = 0; // VR4 D2, hash builds: the plain-tail proof failed (must stay 0)
    // Guard misses by reason (knob off / branch pending / E-bit or halt pending / budget).
    uint64_t m_blockMissOff = 0, m_blockMissBranch = 0, m_blockMissEnd = 0, m_blockMissBudget = 0;
    // Hash builds: generated pairs and their cycles in blocks (totals: VuCore).
    uint64_t m_blockIssuedPairs = 0, m_blockIssuedCycles = 0;
#if PS2X_ENABLE_DET_HASH_TAP
    // VB1 stats (hash builds only; printed with PS2X_VU1_RECOMP_STATS=1):
    // VU1 cycles spent in pairs issued with direct commits (stall included)
    // vs queued, and FMAC/CLIP flag writes applied directly vs queued.
    uint64_t m_vbDirectCycles = 0;
    uint64_t m_vbQueuedCycles = 0;
    uint64_t m_vbDirectFlagWrites = 0;
    uint64_t m_vbQueuedFlagWrites = 0;
    uint64_t m_vbDirectVfWrites = 0;
    uint64_t m_vbQueuedVfWrites = 0;
#endif

    // VuCore::run hooks (VU1 only).
    void beginRunTrace(uint32_t codeSize, const uint8_t *vuData, uint32_t dataSize);
    void selectFlagElision(const RecompProgram *recomp);
    bool traceActive() const { return m_traceArmed || m_entryArmed; }
    void endRunTrace(const uint8_t *vuCode, uint32_t codeSize, PS2Memory *memory, uint64_t cyclesUsed,
                     bool budgetExhausted);
    void printRecompStats() const;

    void startXgkick(uint32_t qwordAddress);
    void progressXgkick();
    void finishXgkick();

    void recordEntryPair(uint32_t pc, uint32_t lo, uint32_t up,
                         const uint8_t *vuData, uint32_t dataSize,
                         const int32_t oldVi[16], const uint32_t oldVf[32][4]);
    void snapshotTraceHeaders(const uint8_t *vuData, uint32_t dataSize);
    void buildTraceDetail(const uint8_t *vuCode, uint32_t codeSize,
                          PS2Memory *memory, uint64_t cyclesUsed,
                          std::string &out);
};

#endif
