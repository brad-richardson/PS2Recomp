// VR1/VR3: VU0 static recompilation (registry, keying, source emitter) and
// the VB1 direct-commit maps both units use. VX2: the generated VU1 path
// (vu1_<hash>.cpp images, PS2X_VU1_RECOMP/_DUMP/_BLOCKS/_DIRECT/_FLAG_ELIDE)
// is gone; VU1 runs interpreted (microVU in play).
//
// A code image is the whole micro memory at the time run() starts; its key
// is XXH64 over those bytes, recomputed only when the memory's code
// generation changes (the same trigger as the interpreter's decode cache).
// Generated VU0 images (PS2X_VU0_RECOMP_DIR at build time) register
// themselves from static initializers. Unknown images, untracked code
// pointers and unaligned PCs run in the interpreter. Tracked VU1 code is
// still keyed: the hash names its direct-commit map (directFlagMap).
//
// Environment (VU0 images, the whole 4 KiB VU0 micro memory). Default off:
//   PS2X_VU0_RECOMP=1          use generated VU0 code.
//   PS2X_VU0_RECOMP_DUMP=<dir> write <dir>/vu0e_<hash>.cpp for each VU0 image
//                              without generated code (game-derived: keep it
//                              outside the repo).
//   PS2X_VU0_BLOCKS            which table of a generated image runs.
//                              2/unset (default, VBK1 Part 3) = the guarded
//                              group table: the groups' statically scheduled
//                              bodies behind an entry guard (a failed guard
//                              runs table 1's group). 1 = the group table
//                              (straight-line pairs per host function, same
//                              per-pair steps). 0 = the pair table (exact
//                              reference path).
//   PS2X_VU1_RECOMP_STATS=1    print cumulative generated/interpreted VU0
//                              cycles, the table mode and the guarded-group
//                              entries/misses (the [vu0-recomp] line).

#include "runtime/ps2_vu0.h"
#include "runtime/ps2_vu1.h"
#include "runtime/ps2_memory.h"
#define XXH_NO_XXH32
#define XXH_NO_XXH3
#define XXH_INLINE_ALL
#include "runtime/third_party/xxhash.h"

#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace
{
    // VX1: one registry per unit (VU0/VU1 images have different pair types).
    template <class D>
    std::unordered_map<uint64_t, typename VuCore<D>::RecompProgram> &recompRegistry()
    {
        static std::unordered_map<uint64_t, typename VuCore<D>::RecompProgram> registry;
        return registry;
    }

    constexpr uint8_t laneBit(uint32_t component)
    {
        return static_cast<uint8_t>(1u << (3u - component));
    }

    const char *vu0RecompDumpDir()
    {
        static const char *dir = []() -> const char *
        {
            const char *value = std::getenv("PS2X_VU0_RECOMP_DUMP");
            return value != nullptr && value[0] != '\0' ? value : nullptr;
        }();
        return dir;
    }

    bool recompStatsEnabled()
    {
        static const bool enabled = []
        {
            const char *value = std::getenv("PS2X_VU1_RECOMP_STATS");
            return value != nullptr && value[0] == '1';
        }();
        return enabled;
    }
}

template <class D>
bool VuCore<D>::vu0RecompEnabled()
{
    static const bool enabled = []
    {
        const char *value = std::getenv("PS2X_VU0_RECOMP");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

// VBK1 Part 3: the guarded group table is the default (an exact refactor:
// det IDENTICAL on every key with it on). PS2X_VU0_BLOCKS=0 restores the
// pair table (the exact reference path), 1 the unguarded group table; any
// other value or unset = 2. Images without the tables fall back in run().
template <class D>
int VuCore<D>::vu0BlocksMode()
{
    static const int mode = []
    {
        const char *value = std::getenv("PS2X_VU0_BLOCKS");
        return value != nullptr && value[0] == '0' ? 0 : value != nullptr && value[0] == '1' ? 1 : 2;
    }();
    return mode;
}

template <class D>
void VuCore<D>::registerRecompProgram(const RecompProgram &program)
{
    recompRegistry<D>()[program.hash] = program;
}

template <class D>
const typename VuCore<D>::RecompProgram *VuCore<D>::findRecompProgram(uint64_t hash)
{
    const auto &registry = recompRegistry<D>();
    const auto it = registry.find(hash);
    return it != registry.end() ? &it->second : nullptr;
}

template <class D>
const typename VuCore<D>::RecompProgram *VuCore<D>::lookupRecompProgram(
    const uint8_t *vuCode, uint32_t codeSize, PS2Memory *memory)
{
    // VR2: generated pairs assume the whole 16 KiB micro memory (constant code
    // size, every masked pc in range; see recompChainReady). VR3: VU0 images
    // the whole 4 KiB VU0 micro memory.
    constexpr bool vu0 = !isVu1();
    if (codeSize != (vu0 ? kRecompVu0CodeSize : kRecompCodeSize))
        return nullptr;
    if (m_recompTestProgram != nullptr)
        return codeSize == m_recompTestProgram->codeSize ? m_recompTestProgram : nullptr;
    // VU0: keyed only while generated code, its dump or VU0 direct commit
    // (whose tracked flag map is keyed by m_recompHash) is on (default off).
    if (vu0 && !vu0RecompEnabled() && !vu0DirectEnabled() && vu0RecompDumpDir() == nullptr)
        return nullptr;
    if (memory == nullptr || vuCode != (vu0 ? memory->getVU0Code() : memory->getVU1Code()))
        return nullptr;
    if (vu0 && recompStatsEnabled() && (++m_recompRuns & 0x3FFFu) == 0u)
    {
        const uint64_t total = m_recompCycles + m_interpCycles;
        std::fprintf(stderr, "[vu0-recomp] runs=%llu generated_cycles=%llu interpreted_cycles=%llu generated_share=%.4f "
                             "blocks_mode=%d group_entries=%llu group_misses=%llu\n",
                     static_cast<unsigned long long>(m_recompRuns),
                     static_cast<unsigned long long>(m_recompCycles),
                     static_cast<unsigned long long>(m_interpCycles),
                     total != 0u ? static_cast<double>(m_recompCycles) / static_cast<double>(total) : 0.0,
                     m_blocksOverride >= 0 ? m_blocksOverride : vu0BlocksMode(),
                     static_cast<unsigned long long>(m_groupEntries), static_cast<unsigned long long>(m_groupMisses));
    }
    const uint64_t generation = vu0 ? memory->getVU0CodeGeneration() : memory->getVU1CodeGeneration();
    if (m_recompValid && m_recompCode == vuCode && m_recompCodeSize == codeSize &&
        m_recompGeneration == generation)
        return m_recompProgram;

    m_recompValid = true;
    m_recompCode = vuCode;
    m_recompCodeSize = codeSize;
    m_recompGeneration = generation;
    m_recompHash = XXH64(vuCode, codeSize, 0);
    m_recompProgram = nullptr;
    if (vu0 && vu0RecompEnabled())
    {
        const auto &registry = recompRegistry<D>();
        const auto it = registry.find(m_recompHash);
        if (it != registry.end() && it->second.codeSize == codeSize)
            m_recompProgram = &it->second;
    }
    if constexpr (vu0)
    {
        const char *dumpDir = vu0RecompDumpDir();
        if (m_recompProgram == nullptr && dumpDir != nullptr)
        {
            static std::unordered_set<uint64_t> dumped;
            if (dumped.insert(m_recompHash ^ (static_cast<uint64_t>(codeSize) << 48)).second)
            {
                char name[64];
                // VX1: VU0 images target VU0Interpreter; "vu0e_" keeps them apart
                // from pre-VX1 vu0_ images (VU1Interpreter-based) in a shared dir.
                std::snprintf(name, sizeof(name), "/vu0e_%016llx.cpp", static_cast<unsigned long long>(m_recompHash));
                const std::string path = std::string(dumpDir) + name;
                const bool ok = emitRecompSource(vuCode, codeSize, m_recompHash, path);
                std::fprintf(stderr, "[vu0-recomp] dump %s %s generation=%llu\n", path.c_str(), ok ? "ok" : "FAILED",
                             static_cast<unsigned long long>(generation));
            }
        }
    }
    return m_recompProgram;
}

template <class D>
bool VuCore<D>::emitRecompSource(const uint8_t *vuCode, uint32_t codeSize,
                                      uint64_t hash, const std::string &path)
{
    // VX2: VU0 images only (the generated VU1 path is gone). The text is
    // byte-identical to the VX1 emitter's VU0 output.
    static_assert(!isVu1(), "generated images are VU0-only");
    // The decoder is a pure function of the two instruction words and the
    // unit (VR3: VU0 reserves MFP, XGKICK, the EFU ops and WAITP).
    const auto decoder = std::make_unique<D>();
    char hashText[32];
    std::snprintf(hashText, sizeof(hashText), "0x%016llxull", static_cast<unsigned long long>(hash));
    const std::string image = std::string("VU0RecompImage<") + hashText + ">";
    const uint32_t pairCount = codeSize / 8u;

    std::ostringstream out;
    out << std::boolalpha;
    out << "// Generated by PS2X_VU0_RECOMP_DUMP (VR3). Derived from game data:\n"
           "// keep outside the repo, never commit.\n"
           "#include \"ps2_vu0_recomp_gen.h\"\n\n"
           "using VU0 = VU0Interpreter;\n\n"
           "template <>\nstruct " << image << "\n{\n"
           "    using D = VU0::DecodedInstructionPair;\n"
           "    using U = VU0::InstructionUsage;\n"
           "    using P = VU0::Pipeline;\n"
           "    static const VU0::RecompPairFn kPairs[" << pairCount << "];\n"
           "    static const VU0::RecompPairFn kBlockPairs[" << pairCount << "];\n"
           "    static const VU0::RecompPairFn kGroupPairs[" << pairCount << "];\n"
           "    // Chain to the next pair's function (tail call) while run()'s loop\n"
           "    // header would let it issue; otherwise return to run().\n"
           "    static bool next(VU0 &vu, VU0::RunContext &c)\n    {\n"
           "        if (!vu.recompChainReady(c))\n            return false;\n"
           "        const VU0::RecompPairFn fn = kPairs[vu.m_state.pc >> 3];\n"
           "        if (fn == nullptr)\n            return false;\n"
           "        PS2X_VU_MUSTTAIL return fn(vu, c);\n    }\n"
           "    // VBK1 Part 2: next() over the guarded group table (PS2X_VU0_BLOCKS=2).\n"
           "    static bool nextGroup(VU0 &vu, VU0::RunContext &c)\n    {\n"
           "        if (!vu.recompChainReady(c))\n            return false;\n"
           "        const VU0::RecompPairFn fn = kGroupPairs[vu.m_state.pc >> 3];\n"
           "        if (fn == nullptr)\n            return false;\n"
           "        PS2X_VU_MUSTTAIL return fn(vu, c);\n    }\n"
           "    // VBK1: next() over the block table (PS2X_VU0_BLOCKS=1).\n"
           "    static bool nextBlock(VU0 &vu, VU0::RunContext &c)\n    {\n"
           "        if (!vu.recompChainReady(c))\n            return false;\n"
           "        const VU0::RecompPairFn fn = kBlockPairs[vu.m_state.pc >> 3];\n"
           "        if (fn == nullptr)\n            return false;\n"
           "        PS2X_VU_MUSTTAIL return fn(vu, c);\n    }\n";
    std::vector<bool> emitted(pairCount, false);
    char codeSizeArgs[48] = "";
    std::snprintf(codeSizeArgs, sizeof(codeSizeArgs), ", -1, false, 0x%xu", codeSize);
    for (uint32_t index = 0; index < pairCount; ++index)
    {
        const uint32_t pc = index * 8u;
        const DecodedInstructionPair d = decoder->decodeInstructionPair(vuCode, pc);
        if (d.upperUsage.reserved || d.lowerUsage.reserved)
            continue;
        emitted[index] = true;
        auto usage = [&out](const InstructionUsage &u)
        {
            out << "U{{{{" << unsigned(u.vfRead[0].reg) << "," << unsigned(u.vfRead[0].lanes) << "},{"
                << unsigned(u.vfRead[1].reg) << "," << unsigned(u.vfRead[1].lanes) << "}}},{"
                << unsigned(u.vfWrite.reg) << "," << unsigned(u.vfWrite.lanes) << "},"
                << unsigned(u.vfReadCount) << "," << unsigned(u.viRead) << "," << unsigned(u.viWrite) << ","
                << unsigned(u.accRead) << "," << unsigned(u.accWrite) << ","
                << unsigned(u.latency) << "," << unsigned(u.vfLatency) << "," << unsigned(u.viLatency) << ","
                << "P(" << unsigned(u.pipeline) << "),"
                << u.waitQ << "," << u.waitP << "," << u.readsClip << "," << u.writesClip << ","
                << u.delaysNextBranchRead << "," << u.reserved << "}";
        };
        char words[64];
        std::snprintf(words, sizeof(words), "0x%08xu, 0x%08xu, ", d.lower, d.upper);
        char label[16];
        std::snprintf(label, sizeof(label), "%04x", pc);
        out << "    static constexpr D d" << label << "{" << words;
        usage(d.lowerUsage);
        out << ", ";
        usage(d.upperUsage);
        out << ", " << d.iBit << "," << d.eBit << "," << d.mBit << "," << d.dBit << "," << d.tBit << ","
            << unsigned(d.upperVfShadowReg) << "," << unsigned(d.suppressedLowerVf) << "};\n";
        // VR2 2D: pair functions are reached through the table or a tail call.
        out << "    PS2X_VU_NOINLINE static bool f" << label << "(VU0 &vu, VU0::RunContext &c)\n    {\n"
            << "        if (vu.issuePair<true" << codeSizeArgs
            << ">(d" << label << ", c))\n            return true;\n"
            // F4-2b: the handoff must be a *guaranteed* tail call (like next()'s
            // table call above). A plain return here nests one frame per pair
            // and overflows small stacks (Odin S1: 512 nested f-frames).
            << "        PS2X_VU_MUSTTAIL return next(vu, c);\n    }\n";
    }
    // VBK1: block functions. A block is a run of up to kVu0BlockMaxPairs
    // emitted pairs that starts at a leader: pc 0, a static branch target,
    // the pair after a branch's or E-bit pair's delay slot, the pair after a
    // D/T-bit pair, a pair after a reserved one, or the pair after a full
    // block. Each pair keeps its pair function's exact step (the same
    // issuePair call); between pairs the block re-checks what next() checks
    // (the stop request) plus that the pc is the next pair's, and otherwise
    // leaves through the block table, so the leader choice only affects
    // speed. Non-leaders in the block table are the pair functions, which
    // chain over the pair table until run() is re-entered.
    std::vector<bool> leader(pairCount, false);
    {
        const auto mark = [&](uint32_t index)
        {
            if (index < pairCount)
                leader[index] = true;
        };
        mark(0u);
        for (uint32_t index = 0; index < pairCount; ++index)
        {
            const DecodedInstructionPair d = decoder->decodeInstructionPair(vuCode, index * 8u);
            if (!emitted[index])
            {
                mark(index + 1u);
                continue;
            }
            if (d.eBit)
                mark(index + 2u);
            if (d.dBit || d.tBit)
            {
                mark(index + 1u);
                mark(index + 2u);
            }
            if (d.iBit)
                continue;
            const uint8_t opHi = static_cast<uint8_t>((d.lower >> 25) & 0x7Fu);
            const bool jump = opHi == 0x24u || opHi == 0x25u;
            const bool branch = opHi == 0x20u || opHi == 0x21u || opHi == 0x28u || opHi == 0x29u ||
                                (opHi >= 0x2Cu && opHi <= 0x2Fu);
            if (branch)
            {
                const int32_t imm = static_cast<int32_t>(d.lower << 21) >> 21;
                mark(((index * 8u + 8u + static_cast<uint32_t>(imm * 8)) & (codeSize - 1u)) / 8u);
            }
            if (branch || jump)
                mark(index + 2u);
        }
    }
    constexpr uint32_t kVu0BlockMaxPairs = 16u;
    std::vector<uint32_t> blockStarts;
    std::vector<uint32_t> blockLengths;
    for (uint32_t index = 0; index < pairCount;)
    {
        if (!emitted[index])
        {
            ++index;
            continue;
        }
        uint32_t end = index + 1u;
        while (end < pairCount && emitted[end] && !leader[end] && end - index < kVu0BlockMaxPairs)
            ++end;
        blockStarts.push_back(index);
        blockLengths.push_back(end - index);
        index = end;
    }
    std::vector<bool> blockLeader(pairCount, false);
    uint32_t histogram[kVu0BlockMaxPairs + 1u] = {};
    for (size_t block = 0; block < blockStarts.size(); ++block)
    {
        const uint32_t start = blockStarts[block];
        const uint32_t length = blockLengths[block];
        blockLeader[start] = true;
        ++histogram[length];
        char label[16];
        std::snprintf(label, sizeof(label), "%04x", start * 8u);
        out << "    PS2X_VU_NOINLINE static bool b" << label << "(VU0 &vu, VU0::RunContext &c)\n    {\n";
        for (uint32_t k = 0; k < length; ++k)
        {
            char pairLabel[16];
            std::snprintf(pairLabel, sizeof(pairLabel), "%04x", (start + k) * 8u);
            out << "        if (vu.issuePair<true" << codeSizeArgs << ">(d" << pairLabel << ", c))\n"
                << "            return true;\n";
            if (k + 1u < length)
            {
                char nextPc[16];
                std::snprintf(nextPc, sizeof(nextPc), "0x%xu", (start + k + 1u) * 8u);
                out << "        if (!vu.recompChainReady(c) || vu.m_state.pc != " << nextPc << ")\n"
                    << "            PS2X_VU_MUSTTAIL return nextBlock(vu, c);\n";
            }
        }
        out << "        PS2X_VU_MUSTTAIL return nextBlock(vu, c);\n    }\n";
    }
    // VBK1 Part 2: guarded group bodies (the VR2 stage-4 VU1 plan, applied to
    // the groups above). Per group: a frameless guard trampoline gNNNN
    // (recompGroupReady, else the exact group bNNNN) and the out-of-line
    // body BNNNN. Per pair of the body:
    //  - kNoStall: no FDIV/EFU pipeline use, no WAITQ/WAITP, and every VF/VI/
    //    ACC lane it reads is ready by issue order alone: written in the
    //    group at least its latency pairs earlier, or not written in the
    //    group and the pair is >= 3 pairs in (a write issued before the
    //    group lands within kDirectMaxLatency = 4 cycles of its issue; every
    //    pair takes >= 1 cycle, stalls only delay issue further).
    //  - plain tail: not a branch/jump/E-bit pair, not their delay slot, no
    //    D/T bit (the guard rules out pending branch/E/halt state), so the
    //    pc/branch/halt tail reduces to pc + 8.
    //  - in-place mask (GV2's rule): the direct-map bit, no shadowed or
    //    suppressed same-register interference, single VI at latency <= 1;
    //    ACC always (the guard makes every write direct).
    //  - the direct-map byte is buildDirectFlagMap of these code bytes, the
    //    same map run() uses for tracked VU0 code.
    // maxCycles = kDirectMaxLatency + sum(1 + worst stall): FDIV/WAITQ 13,
    // EFU/WAITP 54, any other non-noStall pair 4 (its reads wait at most for
    // a 4-cycle VF/VI write). Between pairs only the stop request is checked.
    std::vector<uint8_t> directMap;
    decoder->buildDirectFlagMap(vuCode, codeSize, directMap);
    uint32_t guardedPairs = 0u, noStallPairs = 0u, plainTailPairs = 0u, inPlacePairs = 0u;
    for (size_t block = 0; block < blockStarts.size(); ++block)
    {
        const uint32_t start = blockStarts[block];
        const uint32_t length = blockLengths[block];
        std::vector<DecodedInstructionPair> p(length);
        std::vector<uint8_t> isBranch(length, 0u);
        for (uint32_t k = 0; k < length; ++k)
        {
            p[k] = decoder->decodeInstructionPair(vuCode, (start + k) * 8u);
            if (!p[k].iBit)
            {
                const uint8_t opHi = static_cast<uint8_t>((p[k].lower >> 25) & 0x7Fu);
                isBranch[k] = opHi == 0x20u || opHi == 0x21u || opHi == 0x24u || opHi == 0x25u || opHi == 0x28u ||
                              opHi == 0x29u || (opHi >= 0x2Cu && opHi <= 0x2Fu);
            }
        }
        std::array<std::array<std::pair<uint32_t, uint32_t>, 4>, 32> vfWriter{};
        std::array<std::pair<uint32_t, uint32_t>, 16> viWriter{};
        std::array<std::pair<uint32_t, uint32_t>, 4> accWriter{};
        uint32_t maxCycles = kDirectMaxLatency;
        std::vector<uint8_t> noStall(length), plainTail(length);
        std::vector<int> inPlace(length, 0);
        for (uint32_t k = 0; k < length; ++k)
        {
            const DecodedInstructionPair &q = p[k];
            const auto ready = [&](const std::pair<uint32_t, uint32_t> &w)
            { return w.first != 0u ? k - (w.first - 1u) >= w.second : k >= 3u; };
            bool quiet = q.lowerUsage.pipeline != PipelineFdiv && q.lowerUsage.pipeline != PipelineEfu &&
                         !q.lowerUsage.waitQ && !q.lowerUsage.waitP;
            for (const InstructionUsage *usage : {&q.upperUsage, &q.lowerUsage})
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
            noStall[k] = quiet ? 1u : 0u;
            const bool ender = isBranch[k] != 0u || q.eBit;
            const bool slot = k > 0u && (isBranch[k - 1u] != 0u || p[k - 1u].eBit);
            plainTail[k] = !ender && !slot && !q.dBit && !q.tBit ? 1u : 0u;
            const uint32_t worst = quiet ? 0u
                                   : (q.lowerUsage.pipeline == PipelineEfu || q.lowerUsage.waitP)    ? 54u
                                   : (q.lowerUsage.pipeline == PipelineFdiv || q.lowerUsage.waitQ) ? 13u
                                                                                                    : 4u;
            maxCycles += 1u + worst;

            const uint8_t map = directMap[start + k];
            const bool hasUpper = q.upperUsage.vfWrite.reg != 0u;
            const bool hasLower = q.lowerUsage.vfWrite.reg != 0u && q.suppressedLowerVf != q.lowerUsage.vfWrite.reg;
            if (hasUpper && (map & kDirectMapUpperVf) != 0u && q.upperVfShadowReg == 0u &&
                q.suppressedLowerVf != q.upperUsage.vfWrite.reg)
                inPlace[k] |= kInPlaceUpperVf;
            if (hasLower && (map & kDirectMapLowerVf) != 0u &&
                (!hasUpper || q.lowerUsage.vfWrite.reg != q.upperUsage.vfWrite.reg))
                inPlace[k] |= kInPlaceLowerVf;
            if (q.upperUsage.accWrite != 0u)
                inPlace[k] |= kInPlaceAcc;
            const uint32_t viWrites = q.lowerUsage.viWrite & 0xFFFEu;
            const uint32_t viLatency = q.lowerUsage.viLatency != 0u ? q.lowerUsage.viLatency : q.lowerUsage.latency;
            if (viWrites != 0u && (viWrites & (viWrites - 1u)) == 0u && viLatency <= 1u)
                inPlace[k] |= kInPlaceVi;

            const auto markWrite = [&](const VfAccess &w, uint32_t latency)
            {
                for (uint32_t c = 0; c < 4u; ++c)
                    if ((w.lanes & laneBit(c)) != 0u)
                        vfWriter[w.reg][c] = {k + 1u, latency};
            };
            const VfAccess lw = q.lowerUsage.vfWrite;
            if (lw.reg != 0u && q.suppressedLowerVf != lw.reg)
                markWrite(lw, q.lowerUsage.vfLatency != 0u ? q.lowerUsage.vfLatency : q.lowerUsage.latency);
            if (q.upperUsage.vfWrite.reg != 0u)
                markWrite(q.upperUsage.vfWrite,
                          q.upperUsage.vfLatency != 0u ? q.upperUsage.vfLatency : q.upperUsage.latency);
            for (uint32_t v = viWrites; v != 0u; v &= v - 1u)
                viWriter[std::countr_zero(v)] = {k + 1u, viLatency};
            for (uint32_t c = 0; c < 4u; ++c)
                if ((q.upperUsage.accWrite & laneBit(c)) != 0u)
                    accWriter[c] = {k + 1u, kAccForwardLatency};
        }

        char label[16];
        std::snprintf(label, sizeof(label), "%04x", start * 8u);
        out << "    static bool g" << label << "(VU0 &vu, VU0::RunContext &c)\n    {\n"
            << "        if (!vu.recompGroupReady(c, " << maxCycles << "u))\n"
            << "            PS2X_VU_MUSTTAIL return b" << label << "(vu, c);\n"
            << "        PS2X_VU_MUSTTAIL return B" << label << "(vu, c);\n    }\n"
            << "    PS2X_VU_NOINLINE static bool B" << label << "(VU0 &vu, VU0::RunContext &c)\n    {\n";
        for (uint32_t k = 0; k < length; ++k)
        {
            char pairLabel[16];
            std::snprintf(pairLabel, sizeof(pairLabel), "%04x", (start + k) * 8u);
            out << "        if (vu.issuePair<true, " << unsigned(directMap[start + k]) << ", "
                << (noStall[k] != 0u) << ", 0x" << std::hex << codeSize << std::dec << "u, "
                << (plainTail[k] != 0u) << ", -1, " << inPlace[k] << ">(d" << pairLabel << ", c";
            if (plainTail[k] != 0u)
                out << ", 0x" << std::hex << (((start + k) * 8u + 8u) & (codeSize - 1u)) << std::dec << "u";
            out << "))\n            return true;\n";
            if (k + 1u < length)
                out << "        if (!vu.recompChainReady(c))\n            return false;\n";
            ++guardedPairs;
            noStallPairs += noStall[k];
            plainTailPairs += plainTail[k];
            inPlacePairs += inPlace[k] != 0 ? 1u : 0u;
        }
        const uint32_t nextIndex = start + length;
        if (plainTail[length - 1u] != 0u && nextIndex < pairCount && emitted[nextIndex])
        {
            // The next pair starts the next group (groups are maximal runs).
            char nextLabel[16];
            std::snprintf(nextLabel, sizeof(nextLabel), "%04x", nextIndex * 8u);
            out << "        if (!vu.recompChainReady(c))\n            return false;\n"
                << "        PS2X_VU_MUSTTAIL return g" << nextLabel << "(vu, c);\n    }\n";
        }
        else
            out << "        PS2X_VU_MUSTTAIL return nextGroup(vu, c);\n    }\n";
    }
    // The block summary line VU0 images always carried (no blocks).
    out << "};\n\n// VR2 stage 4: 0 blocks, 0 block pairs, 0 without a scoreboard read, 0 with a plain tail\n";
    out << "// VBK1: " << blockStarts.size() << " pair groups (sizes";
    for (uint32_t length = 1; length <= kVu0BlockMaxPairs; ++length)
        if (histogram[length] != 0u)
            out << " " << length << "x" << histogram[length];
    out << ")\n";
    out << "// VBK1 Part 2: " << blockStarts.size() << " guarded groups, " << guardedPairs << " pairs, " << noStallPairs
        << " without a scoreboard read, " << plainTailPairs << " with a plain tail, " << inPlacePairs
        << " with an in-place commit\n";
    out << "const VU0::RecompPairFn " << image << "::kPairs[" << pairCount << "] = {\n";
    for (uint32_t index = 0; index < pairCount; ++index)
    {
        char label[16];
        std::snprintf(label, sizeof(label), "%04x", index * 8u);
        out << "        " << (!emitted[index] ? std::string("nullptr") : "&" + image + "::f" + label) << ",\n";
    }
    out << "};\n\n";
    out << "const VU0::RecompPairFn " << image << "::kBlockPairs[" << pairCount << "] = {\n";
    for (uint32_t index = 0; index < pairCount; ++index)
    {
        char label[16];
        std::snprintf(label, sizeof(label), "%04x", index * 8u);
        out << "        "
            << (!emitted[index] ? std::string("nullptr")
                                : "&" + image + (blockLeader[index] ? "::b" : "::f") + label)
            << ",\n";
    }
    out << "};\n\n";
    out << "const VU0::RecompPairFn " << image << "::kGroupPairs[" << pairCount << "] = {\n";
    for (uint32_t index = 0; index < pairCount; ++index)
    {
        char label[16];
        std::snprintf(label, sizeof(label), "%04x", index * 8u);
        out << "        "
            << (!emitted[index] ? std::string("nullptr")
                                : "&" + image + (blockLeader[index] ? "::g" : "::f") + label)
            << ",\n";
    }
    out << "};\n\n";
    out << "namespace\n{\n    const bool kRegistered = []\n    {\n"
           "        VU0::RecompProgram program;\n"
           "        program.hash = " << hashText << ";\n"
           "        program.codeSize = " << codeSize << "u;\n"
           "        program.pairCount = " << pairCount << "u;\n"
           "        program.pairs = " << image << "::kPairs;\n"
           "        program.blockPairs = " << image << "::kBlockPairs;\n"
           "        program.groupPairs = " << image << "::kGroupPairs;\n"
           "        VU0::registerRecompProgram(program);\n"
           "        return true;\n    }();\n}\n";

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
        return false;
    const std::string text = out.str();
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(file);
}

// VR3: VU0 direct commit (VB1's rules, unchanged). PS2X_VU0_DIRECT=1 turns it
// on; default off.
template <class D>
bool VuCore<D>::vu0DirectEnabled()
{
    static const bool enabled = []
    {
        const char *value = std::getenv("PS2X_VU0_DIRECT");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

// VB1: per-pair direct-commit map, built from the code bytes.
//
// VR3: branch targets use the unit's pc mask (VU1 0x3FFF, VU0 0xFFF), as
// execLower does.
//
// kDirectMapFlags: pair i may commit its FMAC/CLIP flag writes at issue: no
// flag-reading or flag-setting lower op (0x10..0x1C: FCEQ..FCGET, FSSET,
// FCSET) in pair i itself or in any pair that can issue in the next
// kDirectFlagWindow - 1 pairs. A flag entry lands kFmacLatency (4) cycles
// after issue and every pair takes at least one cycle, so only those pairs
// could have seen the old flags.
//
// kDirectMapUpperVf / kDirectMapLowerVf: that VF write of pair i cannot be
// superseded before it lands. The queue retires an older write when a newer
// write to the same lanes issues first (m_vfLatestWrite), so the old value
// stays in the register until the newer one lands; inside a run nobody can
// read it (reads stall to m_vfReady), but a budget cut or a following
// execute() would. A newer write can only issue before this one lands
// (latency <= 4) if it is in the next three pairs and no pair on the way,
// itself included, reads an overlapping lane (a read stalls until landing).
//
// Control flow for both: branch delay slots, static targets (B/BAL/IBxx),
// not-taken paths and pc wrap are followed; a dynamic target (JR/JALR), an
// out-of-range target, a branch in a delay slot or a reserved pair counts
// as a hit (queue). E/D/T bits are ignored (following past them only adds
// hits). Pair i may also run as the delay slot of a branch at i - 1.
template <class D>
void VuCore<D>::buildDirectFlagMap(const uint8_t *vuCode, uint32_t codeSize,
                                        std::vector<uint8_t> &map) const
{
    enum : uint8_t { kNone, kUncond, kCond, kDynamic };
    constexpr int64_t kNoPending = -1;
    constexpr int64_t kUnknown = -2;
    const uint32_t pairs = codeSize / 8u;
    std::vector<uint8_t> flagOp(pairs, 0u), kind(pairs, kNone), reserved(pairs, 0u);
    std::vector<int64_t> target(pairs, kUnknown);
    std::vector<std::array<uint8_t, 32>> reads(pairs), writes(pairs);
    std::vector<VfAccess> upperWrite(pairs), lowerWrite(pairs);
    for (uint32_t i = 0; i < pairs; ++i)
    {
        const DecodedInstructionPair d = decodeInstructionPair(vuCode, i * 8u);
        reads[i].fill(0u);
        writes[i].fill(0u);
        reserved[i] = d.upperUsage.reserved || d.lowerUsage.reserved;
        for (const InstructionUsage *usage : {&d.upperUsage, &d.lowerUsage})
            for (uint32_t k = 0; k < usage->vfReadCount; ++k)
                reads[i][usage->vfRead[k].reg] |= usage->vfRead[k].lanes;
        upperWrite[i] = d.upperUsage.vfWrite;
        const VfAccess lw = d.lowerUsage.vfWrite;
        if (lw.reg != 0u && d.suppressedLowerVf != lw.reg &&
            (d.upperUsage.vfWrite.reg == 0u || lw.reg != d.upperUsage.vfWrite.reg))
            lowerWrite[i] = lw;
        if (upperWrite[i].reg != 0u)
            writes[i][upperWrite[i].reg] |= upperWrite[i].lanes;
        if (lowerWrite[i].reg != 0u)
            writes[i][lowerWrite[i].reg] |= lowerWrite[i].lanes;

        uint32_t lower = 0, upper = 0;
        std::memcpy(&lower, vuCode + i * 8u, sizeof(lower));
        std::memcpy(&upper, vuCode + i * 8u + 4u, sizeof(upper));
        if ((upper & 0x80000000u) != 0u)
            continue; // I bit: the lower word is an immediate
        const uint8_t opHi = static_cast<uint8_t>((lower >> 25) & 0x7Fu);
        flagOp[i] = opHi >= 0x10u && opHi <= 0x1Cu;
        const bool uncond = opHi == 0x20u || opHi == 0x21u;
        const bool cond = opHi == 0x28u || opHi == 0x29u || (opHi >= 0x2Cu && opHi <= 0x2Fu);
        if (opHi == 0x24u || opHi == 0x25u)
            kind[i] = kDynamic;
        else if (uncond || cond)
        {
            kind[i] = uncond ? kUncond : kCond;
            const int32_t imm = static_cast<int32_t>(lower << 21) >> 21;
            const uint32_t pc = (i * 8u + 8u + static_cast<uint32_t>(imm * 8)) & microAddressMask();
            target[i] = pc + 8u <= codeSize ? static_cast<int64_t>(pc / 8u) : kUnknown;
        }
    }

    // True when visit() hits on some path of n pairs starting at pair i
    // (depth 0 = i). visit returns 0 (go on), 1 (hit) or 2 (this path is done).
    // pending: the pc index after i when i is a delay slot.
    const auto walk = [&](const auto &self, const auto &visit, uint32_t i, uint32_t n,
                          uint32_t depth, int64_t pending) -> bool
    {
        if (n == 0u)
            return false;
        const int verdict = visit(i, depth);
        if (verdict == 1)
            return true;
        if (verdict == 2)
            return false;
        const uint32_t next = (i + 1u) % pairs;
        if (pending != kNoPending)
        {
            if (kind[i] != kNone)
                return true;
            if (pending == kUnknown)
                return n > 1u;
            return self(self, visit, static_cast<uint32_t>(pending), n - 1u, depth + 1u, kNoPending);
        }
        switch (kind[i])
        {
        case kUncond:
            return self(self, visit, next, n - 1u, depth + 1u, target[i]);
        case kCond:
            return self(self, visit, next, n - 1u, depth + 1u, target[i]) ||
                   self(self, visit, next, n - 1u, depth + 1u, static_cast<int64_t>((i + 2u) % pairs));
        case kDynamic:
            return self(self, visit, next, n - 1u, depth + 1u, kUnknown);
        default:
            return self(self, visit, next, n - 1u, depth + 1u, kNoPending);
        }
    };
    // Hit on any entry path of pair i (normal, or as the delay slot of i - 1).
    const auto anyPath = [&](const auto &visit, uint32_t i, uint32_t n) -> bool
    {
        if (walk(walk, visit, i, n, 0u, kNoPending))
            return true;
        const uint32_t prev = (i + pairs - 1u) % pairs;
        return kind[prev] != kNone &&
               walk(walk, visit, i, n, 0u, kind[prev] == kDynamic ? kUnknown : target[prev]);
    };

    map.assign(pairs, 0u);
    for (uint32_t i = 0; i < pairs; ++i)
    {
        const auto flagVisit = [&](uint32_t q, uint32_t) { return flagOp[q] != 0u ? 1 : 0; };
        uint8_t bits = anyPath(flagVisit, i, kDirectFlagWindow) ? 0u : kDirectMapFlags;
        for (const bool isUpper : {true, false})
        {
            const VfAccess w = isUpper ? upperWrite[i] : lowerWrite[i];
            if (w.reg == 0u)
                continue;
            const auto supersedeVisit = [&](uint32_t q, uint32_t depth) -> int
            {
                if (depth == 0u)
                    return 0;
                if (reserved[q] != 0u)
                    return 1;
                if ((reads[q][w.reg] & w.lanes) != 0u)
                    return 2;
                return (writes[q][w.reg] & w.lanes) != 0u ? 1 : 0;
            };
            if (!anyPath(supersedeVisit, i, kDirectMaxLatency))
                bits |= isUpper ? kDirectMapUpperVf : kDirectMapLowerVf;
        }
        map[i] = bits;
    }
}

template <class D>
const uint8_t *VuCore<D>::directFlagMap(const uint8_t *vuCode, uint32_t codeSize, bool tracked)
{
    if (codeSize < 8u || codeSize > 0x4000u || (codeSize & 7u) != 0u)
        return nullptr;
    if (!tracked)
    {
        buildDirectFlagMap(vuCode, codeSize, m_directFlagMap);
        return m_directFlagMap.data();
    }
    // Tracked VU1 code: m_recompHash is the XXH64 of this image (updated by
    // lookupRecompProgram on every code generation change). One map per image.
    // F12-fix: per-thread cache. The map is a pure function of the code
    // image, but the shared static was emplaced concurrently by the MTVU
    // thread (VU1 runs) and GameThread (VU0 microprograms) with
    // PS2X_MTVU=1 (F12 B2 SIGSEGV in directFlagMap): find racing a rehash
    // is UB. Each thread builds its own identical copy; the pointer stays
    // valid for the thread's synchronous use within run().
    static thread_local std::unordered_map<uint64_t, std::vector<uint8_t>> maps;
    const uint64_t key = m_recompHash ^ (static_cast<uint64_t>(codeSize) << 48);
    auto it = maps.find(key);
    if (it == maps.end())
    {
        it = maps.emplace(key, std::vector<uint8_t>()).first;
        buildDirectFlagMap(vuCode, codeSize, it->second);
    }
    return it->second.data();
}

#define PS2X_VU_RECOMP_INSTANTIATE(U)                                                              \
    template bool VuCore<U>::vu0RecompEnabled();                                                   \
    template int VuCore<U>::vu0BlocksMode();                                                       \
    template void VuCore<U>::registerRecompProgram(const RecompProgram &);                         \
    template const VuCore<U>::RecompProgram *VuCore<U>::findRecompProgram(uint64_t);               \
    template const VuCore<U>::RecompProgram *VuCore<U>::lookupRecompProgram(const uint8_t *, uint32_t, \
                                                                            PS2Memory *);          \
    template bool VuCore<U>::vu0DirectEnabled();                                                   \
    template void VuCore<U>::buildDirectFlagMap(const uint8_t *, uint32_t, std::vector<uint8_t> &) const; \
    template const uint8_t *VuCore<U>::directFlagMap(const uint8_t *, uint32_t, bool);
PS2X_VU_RECOMP_INSTANTIATE(VU0Interpreter)
PS2X_VU_RECOMP_INSTANTIATE(VU1Interpreter)
#undef PS2X_VU_RECOMP_INSTANTIATE
template bool VuCore<VU0Interpreter>::emitRecompSource(const uint8_t *, uint32_t, uint64_t, const std::string &);
