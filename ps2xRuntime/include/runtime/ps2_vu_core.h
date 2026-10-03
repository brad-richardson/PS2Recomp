#ifndef PS2_VU_CORE_H
#define PS2_VU_CORE_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "runtime/ps2_vu_state.h"

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

// VR4 D1: the vector form of the exact FMAC core (ps2_vu_fmac_simd.h) needs
// clang/GCC vector extensions and VuWide == double. PS2X_VU1_FMAC_SIMD (CMake
// option, default on) makes it the form execUpperImpl uses; the scalar form
// stays as the reference either way. Builds outside CMake default to scalar.
#ifndef PS2X_VU1_FMAC_SIMD
#define PS2X_VU1_FMAC_SIMD 0
#endif
#if (defined(__clang__) || defined(__GNUC__)) && !(defined(PS2X_VU_WIDE_QUAD) && PS2X_VU_WIDE_QUAD)
#define PS2X_VU1_FMAC_SIMD_AVAILABLE 1
#else
#define PS2X_VU1_FMAC_SIMD_AVAILABLE 0
#endif
#if PS2X_VU1_FMAC_SIMD && !PS2X_VU1_FMAC_SIMD_AVAILABLE
#error "PS2X_VU1_FMAC_SIMD needs clang/GCC vector extensions and VuWide == double"
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

// VR3: generated VU0 programs (PS2X_VU0_RECOMP_DUMP / PS2X_VU0_RECOMP_DIR).
template <uint64_t kImageHash>
struct VU0RecompImage;

enum class VuUnit : uint8_t
{
    VU0,
    VU1
};

struct VuSavestate;

// VX1: the VU engine shared by VU0Interpreter (ps2_vu0.h) and VU1Interpreter
// (ps2_vu1.h): decode, scoreboard, pipelines, the upper/lower executors, the
// pair step (issuePair), the run loop and generated-image keying. Was the body
// of VU1Interpreter (one class for both units, picked by a Unit member).
//
// CRTP: Derived supplies `static constexpr VuUnit kUnit`. Everything only VU1
// has (XGKICK/PATH1) lives in VU1Interpreter and is reached from the shared
// code only under `if constexpr (isVu1())`, so VU0 compiles none of it.
// VX2: generated images are VU0-only; VU1 always interprets.
// VI1: every PS2X_VU1_ALWAYS_INLINE member carries the attribute on its
// declaration here. On a class template the out-of-class definition's
// attribute never reaches the instantiation, so without it the generated VU0
// pairs called issuePair and the FMAC/flag helpers out of line (EB1).
template <class Derived>
class VuCore
{
    friend struct VuSavestate;

public:
    using Unit = VuUnit;
    static constexpr bool isVu1() { return Derived::kUnit == VuUnit::VU1; }

    void reset();

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
    using RecompPairFn = bool (*)(Derived &, RunContext &);
    struct RecompProgram
    {
        uint64_t hash = 0;
        uint32_t codeSize = 0;
        uint32_t pairCount = 0;
        const RecompPairFn *pairs = nullptr;
        // VBK1: the same pcs, with each block leader running a group of
        // straight-line pairs in one host function (PS2X_VU0_BLOCKS=1). Null
        // in images emitted before VBK1 (they always run pairs).
        const RecompPairFn *blockPairs = nullptr;
        // VBK1 Part 2: the same groups behind an entry guard
        // (PS2X_VU0_BLOCKS=2): a leader whose guard holds runs its group's
        // statically scheduled body (VR2 stage-4 issue: no per-pair scoreboard
        // read where proven ready, constant direct-commit bits, in-place
        // commits, plain pc tails); a failed guard runs the exact group.
        const RecompPairFn *groupPairs = nullptr;
    };
    // One registry per unit (VX1; was one shared registry keyed by hash).
    static void registerRecompProgram(const RecompProgram &program);
    // Writes the generated C++ for one code image (every pair that decodes
    // without a reserved instruction). Returns false on a write error.
    // VR3: VU0 decodes with VU0's reserved ops and emits a 4 KiB image
    // (VU0RecompImage, pair functions only). VX2: VU0Interpreter only.
    static bool emitRecompSource(const uint8_t *vuCode, uint32_t codeSize,
                                 uint64_t hash, const std::string &path);

    VuState &state() { return m_state; }
    const VuState &state() const { return m_state; }
    // VB1: test hook for the direct-commit path: -1 follows the unit default
    // (VU1 on; VU0 PS2X_VU0_DIRECT), 0 queues every write, 1 forces it on.
    void setDirectCommitForTest(int mode) { m_directOverride = mode; }
    // VBK1: test hook for the block table: -1 follows PS2X_VU0_BLOCKS, 0 runs
    // the pair table, 1 the block table, 2 the guarded group table (when the
    // image has one).
    void setBlocksForTest(int mode) { m_blocksOverride = mode; }
    // VBK1 Part 2: write latencies of one decoded pair, for the ISA-wide
    // check of the guarded-group proof: {upper VF, lower VF, lower VI,
    // reserved} (0 = no such write).
    std::array<uint32_t, 4> writeLatenciesForTest(uint32_t lower, uint32_t upper) const
    {
        uint8_t code[8];
        std::memcpy(code, &lower, 4);
        std::memcpy(code + 4, &upper, 4);
        const DecodedInstructionPair d = decodeInstructionPair(code, 0u);
        const auto lat = [](const InstructionUsage &u, uint8_t specific)
        { return static_cast<uint32_t>(specific != 0u ? specific : u.latency); };
        return {d.upperUsage.vfWrite.reg != 0u ? lat(d.upperUsage, d.upperUsage.vfLatency) : 0u,
                d.lowerUsage.vfWrite.reg != 0u ? lat(d.lowerUsage, d.lowerUsage.vfLatency) : 0u,
                (d.lowerUsage.viWrite & 0xFFFEu) != 0u ? lat(d.lowerUsage, d.lowerUsage.viLatency) : 0u,
                (d.upperUsage.reserved || d.lowerUsage.reserved) ? 1u : 0u};
    }
    // VBK1 Part 2: guarded group bodies entered / guards that failed.
    uint64_t groupEntriesForTest() const { return m_groupEntries; }
    uint64_t groupMissesForTest() const { return m_groupMisses; }
    // VR2: test hook for generated code without a tracked PS2Memory: run this
    // registered image whenever the code size matches (null = normal lookup).
    static const RecompProgram *findRecompProgram(uint64_t hash);
    void setRecompProgramForTest(const RecompProgram *program) { m_recompTestProgram = program; }
    // F12-fix: test hook for the tracked flag-map cache: builds (or reuses)
    // the map for this image through the same path run() uses.
    const uint8_t *directFlagMapForTest(const uint8_t *vuCode, uint32_t codeSize, bool tracked)
    {
        return directFlagMap(vuCode, codeSize, tracked);
    }
    uint64_t recompCyclesForTest() const { return m_recompCycles; }
    uint64_t interpCyclesForTest() const { return m_interpCycles; }
    // VR2 2C: a reserved-instruction/error stop (reportReservedInstruction) is pending.
    bool stopRequestedForTest() const { return m_stopRequested; }
    // VR4 D1: run one upper instruction through the scalar (simd=false) or
    // vector (simd=true, needs PS2X_VU1_FMAC_SIMD_AVAILABLE) FMAC core, with
    // flag commits direct (directFlags) or queued, and read back everything
    // an FMAC can change (registers, flags, flag pipeline, pending tails).
    void execUpperForTest(uint32_t instr, bool simd, bool directFlags);
    void setFloatModeForTest(bool pcsx2) { m_pcsx2Float = pcsx2; }
    void queueFssetForTest(uint16_t immediate) { queueFsset(immediate); }
    std::vector<uint64_t> fmacStateForTest() const;
#if PS2X_ENABLE_DET_HASH_TAP
    uint64_t programStartCount() const { return m_programStartCount; }
#endif

protected:
    VuCore();
    ~VuCore() = default;
    VuCore(const VuCore &) = delete;
    VuCore &operator=(const VuCore &) = delete;

    Derived &derived() { return static_cast<Derived &>(*this); }
    const Derived &derived() const { return static_cast<const Derived &>(*this); }

    template <uint64_t>
    friend struct VU0RecompImage;

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
        // VB1: a newer flag write was applied at issue; this entry only ORs
        // its sticky bits in when it lands (its MAC/status bits 0-3 are
        // superseded, exactly as the newer entry would overwrite them).
        bool writesStickyOr = false;
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

    static constexpr uint32_t kFmacLatency = 4u;
    static constexpr uint32_t kAccForwardLatency = 1u;
    static constexpr uint32_t kMaxFlagEntries = 8u;
    static constexpr uint32_t kMaxPendingStores = 8u;
    static constexpr uint32_t kMaxPendingVfWrites = 16u;
    static constexpr uint32_t kMaxPendingViWrites = 8u;
    static constexpr uint32_t kMaxPendingAccWrites = 8u;
    static constexpr uint32_t kMaxDecodedPairs = 0x4000u / 8u;

    // VF1: runtime choice; fixed at construction for game runs.
    bool m_pcsx2Float = false;
    VuState m_state;
#if PS2X_ENABLE_DET_HASH_TAP
    uint64_t m_programStartCount = 0; // VU1 starts only (VU1Interpreter::execute; the det-hash count)
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

    // VX1: the start of execute() both units share (scheduler reset, pc,
    // end/halt state, TOP/ITOP, branch state, VF0).
    void beginProgram(uint32_t startPC, uint32_t top, uint32_t itop);
    // The start of resume() both units share.
    void beginResume(uint32_t top, uint32_t itop);

    void run(uint8_t *vuCode, uint32_t codeSize,
             uint8_t *vuData, uint32_t dataSize,
             GS &gs, PS2Memory *memory, uint32_t maxCycles);
    // VR1: one pair issue (stall, execute, queue writes, advance one cycle);
    // defined in ps2_vu_step_impl.h. Returns true when run() must stop.
    // VR2 stage 4: kBlockMap >= 0 = the pair runs inside a guarded block
    // function (recompBlockReady): direct commit on with this constant
    // direct-map byte and no budget checks. kNoStall = the emitter proved the
    // pair never stalls there (no scoreboard read). VR3: kCodeSize = the
    // generated image's code size (kRecompCodeSize for VU1,
    // kRecompVu0CodeSize for VU0); only kStatic pairs use it. VR4 D2:
    // kPlainTail = a block pair the emitter proved has a plain tail (no
    // branch/E-bit pair or its delay slot, no D/T bit; the block guard clears
    // branch/E-bit/halt state at entry), so the tail is just pc = plainNextPc
    // (a constant argument, folded when inlined; a template argument would
    // instantiate issuePair once per block pair). GV2: kInPlace = an emitter
    // bitmask (kInPlace* below) of this block pair's writes that commit
    // in place: the executors' m_state results are already the committed
    // values, so the snapshot/revert/recopy around them is skipped and only
    // the write sequence + noteDirect bookkeeping runs. Same end state as
    // the snapshot path; the emitter sets a bit only where its proof holds
    // (direct-map bit, no shadow/suppressed same-reg interference, single
    // VI at latency <= 1).
    // VX1: blocks (kBlockMap >= 0, kNoStall, kPlainTail, kInPlace) were VU1-only.
    // VX2: no emitter writes them any more (generated images are VU0 pair
    // functions, which pass only the leading parameters); kept so the issue
    // step and existing VU0 images stay unchanged.
    static constexpr int kInPlaceUpperVf = 1;
    static constexpr int kInPlaceLowerVf = 2;
    static constexpr int kInPlaceAcc = 4;
    static constexpr int kInPlaceVi = 8;
    template <bool kStatic, int kBlockMap = -1, bool kNoStall = false, uint32_t kCodeSize = 0x4000u,
              bool kPlainTail = false, int kFloatMode = -1, int kInPlace = 0>
    PS2X_VU1_ALWAYS_INLINE bool issuePair(const DecodedInstructionPair &decoded, RunContext &ctx, uint32_t plainNextPc = 0u);
    // VR1: the run() loop header between two generated pairs (VR2: the stop
    // request; see step_impl). True when the next pair may issue from
    // generated code.
    PS2X_VU1_ALWAYS_INLINE bool recompChainReady(RunContext &ctx);
    // Hash builds: generated pairs and their cycles.
    uint64_t m_genIssuedPairs = 0, m_genIssuedCycles = 0;
    // VR2: whole-memory VU1 code is keyed for its direct-commit map (VX2: no
    // generated VU1 images).
    static constexpr uint32_t kRecompCodeSize = 0x4000u;
    // VR3: ... and whole-memory VU0 code (PS2X_VU0_RECOMP=1; default off).
    static constexpr uint32_t kRecompVu0CodeSize = 0x1000u;
    static bool vu0RecompEnabled();
    // VBK1: PS2X_VU0_BLOCKS=1 runs an image's block table, 2 its guarded
    // group table (default 0, the pair table).
    static int vu0BlocksMode();
    int m_blocksOverride = -1;
    // VBK1 Part 2: the guard of a guarded group (ps2_vu_step_impl.h).
    PS2X_VU1_ALWAYS_INLINE bool recompGroupReady(const RunContext &ctx, uint32_t maxCycles);
    uint64_t m_groupEntries = 0, m_groupMisses = 0;
    const RecompProgram *lookupRecompProgram(const uint8_t *vuCode, uint32_t codeSize, PS2Memory *memory);
    const RecompProgram *m_recompProgram = nullptr;
    const RecompProgram *m_recompTestProgram = nullptr;
    const uint8_t *m_recompCode = nullptr;
    uint32_t m_recompCodeSize = 0;
    uint64_t m_recompGeneration = 0;
    uint64_t m_recompHash = 0;
    bool m_recompValid = false;
    uint64_t m_recompCycles = 0;
    uint64_t m_interpCycles = 0;
    uint64_t m_recompRuns = 0;
    // VB1 (stage B, direct commit): VU1 writes that the scoreboard already
    // orders (VF, VI, ACC, LSU stores) are applied at issue instead of being
    // queued and committed at readyCycle; FMAC/CLIP flag writes too where
    // m_directFlagMap says no flag reader can issue before they would land.
    // Guards per pair: VU1 (VR3: or VU0 with PS2X_VU0_DIRECT=1), m_cycle + kDirectMaxLatency <=
    // budgetEnd (every direct write lands before a budget cut), VF writes
    // only where the static map shows no newer write can retire them before
    // they land (the queue drops a superseded write, which a cut would show),
    // VI writes only at latency 1, and (flags) no queued FSSET; older queued
    // flag entries are demoted to their sticky OR (demoteQueuedFlags).
    // m_directPendingUntil keeps flushPipelines() cycle-exact.
    static constexpr uint32_t kDirectMaxLatency = 4u;
    static constexpr uint32_t kDirectFlagWindow = 5u;
    bool m_directRunOk = false;
    int m_directOverride = -1;
    bool m_directStores = false;
    bool m_directFlags = false; // VR2: the pair's map bit; the queue check is at the flag write
    uint64_t m_directPendingUntil = 0;
    const uint8_t *m_directFlagSafe = nullptr;
    std::vector<uint8_t> m_directFlagMap; // untracked code (tests): rebuilt per run
    void noteDirect(uint64_t readyCycle)
    {
        // VR2: every direct write issues at m_cycle and lands within
        // kDirectMaxLatency, so one landing at m_cycle + kDirectMaxLatency is at
        // or past every earlier one: stored without the compare (the test
        // folds to a constant where the latency is one, as in generated pairs).
        if (readyCycle == m_cycle + kDirectMaxLatency || readyCycle > m_directPendingUntil)
            m_directPendingUntil = readyCycle;
    }
    static bool vu0DirectEnabled();
    const uint8_t *directFlagMap(const uint8_t *vuCode, uint32_t codeSize, bool tracked);
    // Per-pair bits: kDirectMapFlags (flag writes may commit at issue),
    // kDirectMapUpperVf / kDirectMapLowerVf (that VF write cannot be
    // superseded before it lands; see buildDirectFlagMap).
    static constexpr uint8_t kDirectMapFlags = 1u;
    static constexpr uint8_t kDirectMapUpperVf = 2u;
    static constexpr uint8_t kDirectMapLowerVf = 4u;
    void buildDirectFlagMap(const uint8_t *vuCode, uint32_t codeSize, std::vector<uint8_t> &map) const;
    PS2X_VU1_ALWAYS_INLINE void directVfWrite(uint8_t reg, uint8_t laneMask, const float value[4], uint32_t latency);
    PS2X_VU1_ALWAYS_INLINE void directViWrite(uint8_t reg, int32_t value, uint32_t latency);
    PS2X_VU1_ALWAYS_INLINE void directAccWrite(uint8_t laneMask, const float value[4], uint32_t latency);
    // GV2: in-place forms (kInPlace): the value is already in m_state, so only
    // the latest-write sequence and noteDirect run. Same end state as above.
    PS2X_VU1_ALWAYS_INLINE void directVfWriteInPlace(uint8_t reg, uint8_t laneMask, uint32_t latency);
    PS2X_VU1_ALWAYS_INLINE void directViWriteInPlace(uint8_t reg, uint32_t latency);
    PS2X_VU1_ALWAYS_INLINE void directAccWriteInPlace(uint8_t laneMask, uint32_t latency);
    PS2X_VU1_ALWAYS_INLINE bool flagQueueAllowsDirect() const;
    PS2X_VU1_ALWAYS_INLINE bool directFlagsNow() const;
    PS2X_VU1_ALWAYS_INLINE void demoteQueuedFlags(bool macStatus, bool clip);

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
    // VR1: the executor bodies (ps2_vu_{upper,lower}_impl.h), always inlined.
    // (The always-inline attribute sits on the member template declarations:
    // on the out-of-class definition alone it does not reach instantiations.)
    // kFloatMode: -1 = runtime choice (interpreter/old images), 0 = exact,
    // 1 = PCSX2 native math selected once when a generated VU1 image binds.
    template <bool kSimd = (PS2X_VU1_FMAC_SIMD != 0), int kFloatMode = -1>
    PS2X_VU1_ALWAYS_INLINE void execUpperImpl(uint32_t instr);
#if PS2X_VU1_FMAC_SIMD_AVAILABLE
    // VR4 D1: vector FMAC core (ps2_vu_fmac_simd.h). fmacSimdDispatch runs
    // the upper ops that end in applyFmacDest/applyFmacDestAcc and returns
    // false for every other op (the scalar code handles those).
    template <int kFloatMode = -1>
    PS2X_VU1_ALWAYS_INLINE bool fmacSimdDispatch(uint32_t instr);
    template <int kArith, int kSrc, bool kAcc, int kFloatMode = -1>
    PS2X_VU1_ALWAYS_INLINE void fmacSimd(uint32_t instr);
    template <int kArith, int kSrc, bool kAcc>
    PS2X_VU1_ALWAYS_INLINE void fmacPcsx2(uint32_t instr);
#endif
    PS2X_VU1_ALWAYS_INLINE void execLowerImpl(uint32_t instr, uint8_t *vuData, uint32_t dataSize, GS &gs, PS2Memory *memory, uint32_t upperInstr);

    PS2X_VU1_ALWAYS_INLINE void applyDest(float *dst, const float *result, uint8_t dest);
    PS2X_VU1_ALWAYS_INLINE void applyDestAcc(const float *result, uint8_t dest);
    PS2X_VU1_ALWAYS_INLINE void applyFmacDest(float *dst, float *result, uint8_t dest);
    PS2X_VU1_ALWAYS_INLINE void applyFmacDestAcc(float *result, uint8_t dest);
    PS2X_VU1_ALWAYS_INLINE void normalizeFmacResult(float *result, uint8_t dest, uint8_t laneFlags[4]);
    bool calculateFmacExactResult(uint32_t component, VuWide &result) const;
    PS2X_VU1_ALWAYS_INLINE bool calculateFmacExactResults(uint8_t dest, VuWide results[4]) const;
    PS2X_VU1_ALWAYS_INLINE uint8_t normalizeFmacExactResult(float &value, VuWide exactResult) const;
    PS2X_VU1_ALWAYS_INLINE uint32_t calculateFmacProductSticky(uint8_t dest) const;
    PS2X_VU1_ALWAYS_INLINE void updateFmacFlags(const uint8_t laneFlags[4], uint8_t dest, uint32_t extraSticky);
    // VR4 D1: the flag commit half of updateFmacFlags (direct or queued), shared
    // by the scalar and vector FMAC cores.
    PS2X_VU1_ALWAYS_INLINE void commitFmacFlags(uint32_t mac, uint32_t status, uint32_t extraSticky);
    void queueFsset(uint16_t immediate);
    void queueClip(uint32_t clip);
    void queueFcset(uint32_t clip);
    void queueQ(float value, uint32_t latency, uint32_t statusDi);
    void queueP(float value, uint32_t latency);
    void queueStore(uint32_t address, const uint32_t words[4], uint8_t laneMask);
    // VB1: store step used by the lower executors (always-inline, see
    // ps2_vu_fmac_impl.h). The queued path passes m_storeScratch, never a
    // pointer to the caller's local, so a generated pair function keeps no
    // escaping stack array (a stack protector there would turn the chained
    // musttail hand-off into a real call).
    PS2X_VU1_ALWAYS_INLINE void issueStore(uint32_t address, const uint32_t words[4], uint8_t laneMask);
    uint32_t m_storeScratch[4]{};
    void queueVfWrite(uint8_t reg, uint8_t laneMask, const float value[4], uint32_t latency);
    void queueViWrite(uint8_t reg, int32_t value, uint32_t latency);
    void queueAccWrite(uint8_t laneMask, const float value[4], uint32_t latency);

    void resetScheduler();
    void commitReadyPipelines();
    PS2X_VU1_ALWAYS_INLINE void advanceOneCycle();
    void advanceTo(uint64_t targetCycle);
    void flushPipelines();
    PS2X_VU1_ALWAYS_INLINE uint64_t calculatePairReadyCycle(const DecodedInstructionPair &decoded) const;
    PS2X_VU1_ALWAYS_INLINE void markPairWrites(const DecodedInstructionPair &decoded);
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
    static constexpr uint32_t microAddressMask()
    {
        return isVu1() ? 0x3FFFu : 0x0FFFu;
    }
    int32_t readBranchVi(uint8_t reg) const;
    void recordViWriteForBranch(uint8_t reg, int32_t oldValue);
    void reportReservedInstruction(bool upper, uint32_t instruction);
    PS2X_VU1_ALWAYS_INLINE float broadcast(const float *vf, uint8_t bc);
};

#endif
