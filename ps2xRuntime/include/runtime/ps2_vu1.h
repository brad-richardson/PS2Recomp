#ifndef PS2_VU1_H
#define PS2_VU1_H

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "ps2_vu1_trace.h"

// E45: VU1 FMAC exact math runs in `VuWide`. On macOS arm64 `long double`
// is already 64-bit (static_assert below), so the default `double` is a
// no-op there; on Linux/Android arm64 it drops 128-bit quad soft-float
// (__addtf3 et al, ~8-9% of Select-Character time on the Odin per N4).
// Define PS2X_VU_WIDE_QUAD=1 to keep the old quad type for A/B runs.
#if defined(PS2X_VU_WIDE_QUAD) && PS2X_VU_WIDE_QUAD
using VuWide = long double;
#else
using VuWide = double;
#endif

#if defined(__APPLE__)
static_assert(sizeof(long double) == sizeof(double),
              "E45 assumes macOS long double is 64-bit (Mac VU1 math unchanged)");
#endif

class GS;
class PS2Memory;

#if defined(_MSC_VER)
#define PS2X_VU1_ALWAYS_INLINE __forceinline
#else
#define PS2X_VU1_ALWAYS_INLINE __attribute__((always_inline))
#endif

// VR1: generated VU1 programs (one explicit specialization per code image,
// emitted by PS2X_VU1_RECOMP_DUMP and compiled in from PS2X_VU1_RECOMP_DIR).
template <uint64_t kImageHash>
struct VU1RecompImage;

struct VU1State
{
    float vf[32][4];
    int32_t vi[16];
    float acc[4];
    float q;
    float p;
    float i;
    uint32_t r;
    uint32_t pc;
    uint32_t mac;
    uint32_t clip;
    uint32_t status;
    uint64_t cycles;
    bool ebit;
    bool haltAfterDelaySlot;
    bool dBitEnabled;
    bool tBitEnabled;
    bool stoppedByD;
    bool stoppedByT;
    uint32_t top;  // VIF TOP visible to XTOP
    uint32_t itop; // VIF ITOP visible to XITOP

    bool branchPending;
    uint32_t branchTarget;
    uint32_t branchDelay;
};

class VU1Interpreter
{
public:
    enum class Unit : uint8_t
    {
        VU0,
        VU1
    };

    explicit VU1Interpreter(Unit unit = Unit::VU1);

    void reset();

    void execute(uint8_t *vuCode, uint32_t codeSize,
                 uint8_t *vuData, uint32_t dataSize,
                 GS &gs, PS2Memory *memory = nullptr,
                 uint32_t startPC = 0, uint32_t top = 0, uint32_t itop = 0,
                 uint32_t maxCycles = 65536);

    void resume(uint8_t *vuCode, uint32_t codeSize,
                uint8_t *vuData, uint32_t dataSize,
                GS &gs, PS2Memory *memory = nullptr,
                uint32_t top = 0, uint32_t itop = 0, uint32_t maxCycles = 65536);

    // VR1: static recompilation hooks. A registered image is keyed by the
    // XXH64 of the whole code memory; each pair PC has its own function (null =
    // interpreter for that PC), so a run can enter or leave generated code at
    // any pair (execute, resume/MSCNT, budget truncation).
    struct RunContext
    {
        uint8_t *vuCode = nullptr;
        uint32_t codeSize = 0;
        uint8_t *vuData = nullptr;
        uint32_t dataSize = 0;
        GS *gs = nullptr;
        PS2Memory *memory = nullptr;
        uint64_t budgetEnd = 0;
        bool programEnded = false;
    };
    using RecompPairFn = bool (*)(VU1Interpreter &, RunContext &);
    struct RecompProgram
    {
        uint64_t hash = 0;
        uint32_t codeSize = 0;
        uint32_t pairCount = 0;
        const RecompPairFn *pairs = nullptr;
    };
    static void registerRecompProgram(const RecompProgram &program);
    // Writes the generated C++ for one code image (every pair that decodes
    // without a reserved instruction). Returns false on a write error.
    static bool emitRecompSource(const uint8_t *vuCode, uint32_t codeSize,
                                 uint64_t hash, const std::string &path);

    VU1State &state() { return m_state; }
    const VU1State &state() const { return m_state; }
#if PS2X_ENABLE_DET_HASH_TAP
    uint64_t programStartCount() const { return m_programStartCount; }
#endif

private:
    template <uint64_t>
    friend struct VU1RecompImage;

    enum Pipeline : uint8_t
    {
        PipelineNone = 0,
        PipelineFmac,
        PipelineLsu,
        PipelineFdiv,
        PipelineEfu,
        PipelineIalu,
        PipelineBranch,
        PipelineXgkick
    };

    struct VfAccess
    {
        uint8_t reg = 0;
        uint8_t lanes = 0;
    };

    struct InstructionUsage
    {
        std::array<VfAccess, 2> vfRead{};
        VfAccess vfWrite{};
        uint8_t vfReadCount = 0;
        uint16_t viRead = 0;
        uint16_t viWrite = 0;
        uint8_t accRead = 0;
        uint8_t accWrite = 0;
        uint8_t latency = 0;
        uint8_t vfLatency = 0;
        uint8_t viLatency = 0;
        Pipeline pipeline = PipelineNone;
        bool waitQ = false;
        bool waitP = false;
        bool readsClip = false;
        bool writesClip = false;
        bool delaysNextBranchRead = false;
        bool reserved = false;
    };

    struct DecodedInstructionPair
    {
        uint32_t lower = 0;
        uint32_t upper = 0;
        InstructionUsage lowerUsage{};
        InstructionUsage upperUsage{};
        bool iBit = false;
        bool eBit = false;
        bool mBit = false;
        bool dBit = false;
        bool tBit = false;
        uint8_t upperVfShadowReg = 0;
        uint8_t suppressedLowerVf = 0;
    };

    struct FlagPipelineEntry
    {
        uint64_t readyCycle = 0;
        uint64_t issueCycle = 0;
        uint32_t mac = 0;
        uint32_t status = 0;
        uint32_t extraSticky = 0;
        uint32_t clip = 0;
        bool valid = false;
        bool writesMac = false;
        bool writesStatus = false;
        bool writesSticky = false;
        bool writesClip = false;
    };

    struct ScalarPipelineEntry
    {
        uint64_t readyCycle = 0;
        float value = 0.0f;
        uint32_t statusDi = 0;
        bool valid = false;
    };

    struct PendingStore
    {
        uint64_t readyCycle = 0;
        uint32_t address = 0;
        std::array<uint32_t, 4> words{};
        uint8_t laneMask = 0;
        bool valid = false;
    };

    struct PendingVfWrite
    {
        uint64_t readyCycle = 0;
        uint64_t sequence = 0;
        std::array<float, 4> value{};
        uint8_t reg = 0;
        uint8_t laneMask = 0;
        bool valid = false;
    };

    struct PendingViWrite
    {
        uint64_t readyCycle = 0;
        uint64_t sequence = 0;
        int32_t value = 0;
        uint8_t reg = 0;
        bool valid = false;
    };

    struct PendingAccWrite
    {
        uint64_t readyCycle = 0;
        uint64_t sequence = 0;
        std::array<float, 4> value{};
        uint8_t laneMask = 0;
        bool valid = false;
    };

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
    };

    static constexpr uint32_t kFmacLatency = 4u;
    static constexpr uint32_t kAccForwardLatency = 1u;
    static constexpr uint32_t kMaxFlagEntries = 8u;
    static constexpr uint32_t kMaxPendingStores = 8u;
    static constexpr uint32_t kMaxPendingVfWrites = 16u;
    static constexpr uint32_t kMaxPendingViWrites = 8u;
    static constexpr uint32_t kMaxPendingAccWrites = 8u;
    static constexpr uint32_t kMaxDecodedPairs = 0x4000u / 8u;

    Unit m_unit;
    VU1State m_state;
#if PS2X_ENABLE_DET_HASH_TAP
    uint64_t m_programStartCount = 0;
#endif
    std::array<DecodedInstructionPair, kMaxDecodedPairs> m_decodedCodeCache{};
    DecodedInstructionPair m_decodeScratch{};
    const uint8_t *m_cachedVuCode = nullptr;
    const PS2Memory *m_cachedMemory = nullptr;
    uint32_t m_cachedCodeSize = 0;
    uint64_t m_cachedCodeGeneration = 0;
    bool m_decodedCodeCacheValid = false;

    std::array<FlagPipelineEntry, kMaxFlagEntries> m_flagPipeline{};
    ScalarPipelineEntry m_fdiv{};
    std::array<ScalarPipelineEntry, 2> m_efu{};
    std::array<PendingStore, kMaxPendingStores> m_storePipeline{};
    std::array<PendingVfWrite, kMaxPendingVfWrites> m_vfWritePipeline{};
    std::array<PendingViWrite, kMaxPendingViWrites> m_viWritePipeline{};
    std::array<PendingAccWrite, kMaxPendingAccWrites> m_accWritePipeline{};
    XgkickPipeline m_xgkick{};
    std::array<std::array<uint64_t, 4>, 32> m_vfReady{};
    std::array<uint64_t, 16> m_viReady{};
    std::array<uint64_t, 4> m_accReady{};
    // E57: occupancy masks (bit i set = entry i valid, kept in sync with the
    // entries' valid flags) and a lower bound on the earliest readyCycle of
    // any queued entry. commitReadyPipelines() has no effect before
    // m_nextCommitCycle, so it returns early; when it runs it visits only
    // valid entries, in the same index order as a full scan.
    uint32_t m_flagValidMask = 0;
    uint32_t m_storeValidMask = 0;
    uint32_t m_vfWriteValidMask = 0;
    uint32_t m_viWriteValidMask = 0;
    uint32_t m_accWriteValidMask = 0;
    uint64_t m_nextCommitCycle = ~0ull;
    void noteQueued(uint64_t readyCycle)
    {
        if (readyCycle < m_nextCommitCycle)
            m_nextCommitCycle = readyCycle;
    }
    std::array<std::array<uint64_t, 4>, 32> m_vfLatestWrite{};
    std::array<uint64_t, 16> m_viLatestWrite{};
    std::array<uint64_t, 4> m_accLatestWrite{};

    uint64_t m_cycle = 0;
    uint64_t m_nextWriteSequence = 0;
    uint64_t m_efuResourceReady = 0;
    uint32_t m_workingClip = 0;
    uint32_t m_currentUpperInstruction = 0;
    int32_t m_viBranchBackupValue = 0;
    uint8_t m_viBranchBackupReg = 0;
    bool m_viBranchBackupValid = false;
    uint8_t *m_activeVuData = nullptr;
    uint32_t m_activeVuDataSize = 0;
    GS *m_activeGs = nullptr;
    PS2Memory *m_activeMemory = nullptr;
    bool m_stopRequested = false;
    bool m_pendingHaltD = false;
    bool m_pendingHaltT = false;

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

    void run(uint8_t *vuCode, uint32_t codeSize,
             uint8_t *vuData, uint32_t dataSize,
             GS &gs, PS2Memory *memory, uint32_t maxCycles);
    // VR1: one pair issue (stall, execute, queue writes, advance one cycle);
    // defined in ps2_vu1_step_impl.h. Returns true when run() must stop.
    template <bool kStatic>
    bool issuePair(const DecodedInstructionPair &decoded, RunContext &ctx);
    const RecompProgram *lookupRecompProgram(const uint8_t *vuCode, uint32_t codeSize, PS2Memory *memory);
    const RecompProgram *m_recompProgram = nullptr;
    const uint8_t *m_recompCode = nullptr;
    uint32_t m_recompCodeSize = 0;
    uint64_t m_recompGeneration = 0;
    uint64_t m_recompHash = 0;
    bool m_recompValid = false;
    uint64_t m_recompCycles = 0;
    uint64_t m_interpCycles = 0;
    uint64_t m_recompRuns = 0;
    void recordEntryPair(uint32_t pc, uint32_t lo, uint32_t up,
                         const uint8_t *vuData, uint32_t dataSize,
                         const int32_t oldVi[16], const uint32_t oldVf[32][4]);
    void snapshotTraceHeaders(const uint8_t *vuData, uint32_t dataSize);
    void buildTraceDetail(const uint8_t *vuCode, uint32_t codeSize,
                          PS2Memory *memory, uint64_t cyclesUsed,
                          std::string &out);

    InstructionUsage decodeUpperUsage(uint32_t upper) const;
    InstructionUsage decodeLowerUsage(uint32_t lower) const;
    static void addVfRead(InstructionUsage &usage, uint8_t reg, uint8_t lanes);
    static void addVfWrite(InstructionUsage &usage, uint8_t reg, uint8_t lanes);
    static uint8_t vfReadLanes(const InstructionUsage &usage, uint8_t reg);
    DecodedInstructionPair decodeInstructionPair(const uint8_t *vuCode, uint32_t pc) const;
    // E57: returns a reference into the decode cache (or m_decodeScratch for
    // uncached PCs) instead of a copy. The cache is only rebuilt inside this
    // call, and nothing between two calls in run() re-enters it.
    const DecodedInstructionPair &getDecodedInstructionPairForPc(const uint8_t *vuCode, uint32_t codeSize, PS2Memory *memory, uint32_t pc);
    void rebuildDecodedCodeCache(const uint8_t *vuCode, uint32_t codeSize, const PS2Memory *memory, uint64_t generation);

    void execUpper(uint32_t instr);
    void execLower(uint32_t instr, uint8_t *vuData, uint32_t dataSize, GS &gs, PS2Memory *memory, uint32_t upperInstr);
    // VR1: the executor bodies (ps2_vu1_{upper,lower}_impl.h), always inlined.
    void execUpperImpl(uint32_t instr);
    void execLowerImpl(uint32_t instr, uint8_t *vuData, uint32_t dataSize, GS &gs, PS2Memory *memory, uint32_t upperInstr);

    void applyDest(float *dst, const float *result, uint8_t dest);
    void applyDestAcc(const float *result, uint8_t dest);
    void applyFmacDest(float *dst, float *result, uint8_t dest);
    void applyFmacDestAcc(float *result, uint8_t dest);
    void normalizeFmacResult(float *result, uint8_t dest, uint8_t laneFlags[4]);
    bool calculateFmacExactResult(uint32_t component, VuWide &result) const;
    bool calculateFmacExactResults(uint8_t dest, VuWide results[4]) const;
    uint8_t normalizeFmacExactResult(float &value, VuWide exactResult) const;
    uint32_t calculateFmacProductSticky(uint8_t dest) const;
    void updateFmacFlags(const uint8_t laneFlags[4], uint8_t dest, uint32_t extraSticky);
    void queueFsset(uint16_t immediate);
    void queueClip(uint32_t clip);
    void queueFcset(uint32_t clip);
    void queueQ(float value, uint32_t latency, uint32_t statusDi);
    void queueP(float value, uint32_t latency);
    void queueStore(uint32_t address, const uint32_t words[4], uint8_t laneMask);
    void queueVfWrite(uint8_t reg, uint8_t laneMask, const float value[4], uint32_t latency);
    void queueViWrite(uint8_t reg, int32_t value, uint32_t latency);
    void queueAccWrite(uint8_t laneMask, const float value[4], uint32_t latency);
    void startXgkick(uint32_t qwordAddress);

    void resetScheduler();
    void commitReadyPipelines();
    void advanceOneCycle();
    void advanceTo(uint64_t targetCycle);
    void flushPipelines();
    void progressXgkick();
    void finishXgkick();
    uint64_t calculatePairReadyCycle(const DecodedInstructionPair &decoded) const;
    void markPairWrites(const DecodedInstructionPair &decoded);
    bool pipelinesPending() const;

    // E57: defined inline (were out-of-line in ps2_vu1_core.cpp; the N5 Odin
    // profile shows normalizeOperand as its own 2.4 % symbol). Same bodies.
    static float normalizeOperand(float value)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        const uint32_t exponent = (bits >> 23) & 0xFFu;
        if (exponent == 0u)
        {
            bits &= 0x80000000u;
        }
        else if (exponent == 0xFFu)
        {
            bits = (bits & 0x80000000u) | 0x7F7FFFFFu;
        }
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    static float normalizeResult(float value, uint32_t &laneFlags)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        const uint32_t sign = bits & 0x80000000u;
        const uint32_t magnitude = bits & 0x7FFFFFFFu;
        const uint32_t exponent = (bits >> 23) & 0xFFu;

        laneFlags = sign != 0u ? 0x2u : 0u;
        if (magnitude == 0u)
        {
            laneFlags |= 0x1u;
        }
        else if (exponent == 0u)
        {
            laneFlags |= 0x5u;
            bits = sign;
        }
        else if (exponent == 0xFFu)
        {
            laneFlags |= 0x8u;
            bits = sign | 0x7F7FFFFFu;
        }

        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    uint32_t microAddressMask() const;
    int32_t readBranchVi(uint8_t reg) const;
    void recordViWriteForBranch(uint8_t reg, int32_t oldValue);
    void reportReservedInstruction(bool upper, uint32_t instruction);
    float broadcast(const float *vf, uint8_t bc);
};

#endif
