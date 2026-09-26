// VR1: VU1 static recompilation, stage A (registry, keying, source emitter).
//
// A code image is the whole VU1 code memory at the time run() starts; its key
// is XXH64 over those bytes, recomputed only when the memory's VU1 code
// generation changes (the same trigger as the interpreter's decode cache).
// Generated images (PS2X_VU1_RECOMP_DIR at build time) register themselves
// from static initializers. Unknown images, VU0, untracked code pointers and
// unaligned PCs run in the interpreter.
//
// Environment:
//   PS2X_VU1_RECOMP=0          disable generated code (interpreter only).
//   PS2X_VU1_RECOMP_DUMP=<dir> write <dir>/vu1_<hash>.cpp for each image that
//                              has no generated code yet (derived from game
//                              data: keep it outside the repo).
//   PS2X_VU1_RECOMP_STATS=1    print cumulative generated/interpreted cycles
//                              (VR3: a [vu0-recomp] line for VU0 too).
//
// VR3: VU0 images (the whole 4 KiB VU0 micro memory, same keying on the VU0
// code generation). Default off:
//   PS2X_VU0_RECOMP=1          use generated VU0 code.
//   PS2X_VU0_RECOMP_DUMP=<dir> write <dir>/vu0_<hash>.cpp for each VU0 image
//                              without generated code (game-derived: keep it
//                              outside the repo).

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
    std::unordered_map<uint64_t, VU1Interpreter::RecompProgram> &recompRegistry()
    {
        static std::unordered_map<uint64_t, VU1Interpreter::RecompProgram> registry;
        return registry;
    }

    bool recompEnabled()
    {
        static const bool enabled = []
        {
            const char *value = std::getenv("PS2X_VU1_RECOMP");
            return value == nullptr || value[0] != '0';
        }();
        return enabled;
    }

    const char *recompDumpDir()
    {
        static const char *dir = []() -> const char *
        {
            const char *value = std::getenv("PS2X_VU1_RECOMP_DUMP");
            return value != nullptr && value[0] != '\0' ? value : nullptr;
        }();
        return dir;
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

bool VU1Interpreter::vu0RecompEnabled()
{
    static const bool enabled = []
    {
        const char *value = std::getenv("PS2X_VU0_RECOMP");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

void VU1Interpreter::registerRecompProgram(const RecompProgram &program)
{
    recompRegistry()[program.hash] = program;
}

const VU1Interpreter::RecompProgram *VU1Interpreter::findRecompProgram(uint64_t hash)
{
    const auto &registry = recompRegistry();
    const auto it = registry.find(hash);
    return it != registry.end() ? &it->second : nullptr;
}

const VU1Interpreter::RecompProgram *VU1Interpreter::lookupRecompProgram(
    const uint8_t *vuCode, uint32_t codeSize, PS2Memory *memory)
{
    // VR2: generated pairs assume the whole 16 KiB micro memory (constant code
    // size, every masked pc in range; see recompChainReady). VR3: VU0 images
    // the whole 4 KiB VU0 micro memory.
    const bool vu0 = m_unit == Unit::VU0;
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
        std::fprintf(stderr, "[vu0-recomp] runs=%llu generated_cycles=%llu interpreted_cycles=%llu generated_share=%.4f\n",
                     static_cast<unsigned long long>(m_recompRuns),
                     static_cast<unsigned long long>(m_recompCycles),
                     static_cast<unsigned long long>(m_interpCycles),
                     total != 0u ? static_cast<double>(m_recompCycles) / static_cast<double>(total) : 0.0);
    }
    if (!vu0 && recompStatsEnabled() && (++m_recompRuns & 0x3FFFu) == 0u)
    {
        const uint64_t total = m_recompCycles + m_interpCycles;
        std::fprintf(stderr, "[vu1-recomp] runs=%llu generated_cycles=%llu interpreted_cycles=%llu generated_share=%.4f\n",
                     static_cast<unsigned long long>(m_recompRuns),
                     static_cast<unsigned long long>(m_recompCycles),
                     static_cast<unsigned long long>(m_interpCycles),
                     total != 0u ? static_cast<double>(m_recompCycles) / static_cast<double>(total) : 0.0);
#if PS2X_ENABLE_DET_HASH_TAP
        // VR2 2D: block counters are kept in hash builds only (no per-entry
        // read-modify-write on the hot path).
        std::fprintf(stderr, "[vu1-blocks] on=%d entries=%llu pairs=%llu nostall_misses=%llu miss_off=%llu miss_branch=%llu miss_end=%llu miss_budget=%llu\n",
                     m_blocksOn ? 1 : 0, static_cast<unsigned long long>(m_blockEntries),
                     static_cast<unsigned long long>(m_blockPairs),
                     static_cast<unsigned long long>(m_blockNoStallMisses),
                     static_cast<unsigned long long>(m_blockMissOff),
                     static_cast<unsigned long long>(m_blockMissBranch),
                     static_cast<unsigned long long>(m_blockMissEnd),
                     static_cast<unsigned long long>(m_blockMissBudget));
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
    if (vu0 ? vu0RecompEnabled() : recompEnabled())
    {
        const auto &registry = recompRegistry();
        const auto it = registry.find(m_recompHash);
        if (it != registry.end() && it->second.codeSize == codeSize)
            m_recompProgram = &it->second;
    }
    const char *dumpDir = vu0 ? vu0RecompDumpDir() : recompDumpDir();
    if (m_recompProgram == nullptr && dumpDir != nullptr)
    {
        static std::unordered_set<uint64_t> dumped;
        if (dumped.insert(m_recompHash ^ (static_cast<uint64_t>(codeSize) << 48)).second)
        {
            char name[64];
            std::snprintf(name, sizeof(name), vu0 ? "/vu0_%016llx.cpp" : "/vu1_%016llx.cpp",
                          static_cast<unsigned long long>(m_recompHash));
            const std::string path = std::string(dumpDir) + name;
            const bool ok = emitRecompSource(vuCode, codeSize, m_recompHash, path, m_unit);
            std::fprintf(stderr, "[%s] dump %s %s generation=%llu\n", vu0 ? "vu0-recomp" : "vu1-recomp",
                         path.c_str(), ok ? "ok" : "FAILED", static_cast<unsigned long long>(generation));
        }
    }
    return m_recompProgram;
}

bool VU1Interpreter::emitRecompSource(const uint8_t *vuCode, uint32_t codeSize,
                                      uint64_t hash, const std::string &path, Unit unit)
{
    // The decoder is a pure function of the two instruction words and the
    // unit (VR3: VU0 reserves MFP, XGKICK, the EFU ops and WAITP).
    const auto decoder = std::make_unique<VU1Interpreter>(unit);
    const bool vu0 = unit == Unit::VU0;
    const char *const image = vu0 ? "VU0RecompImage" : "VU1RecompImage";
    char hashText[32];
    std::snprintf(hashText, sizeof(hashText), "0x%016llxull", static_cast<unsigned long long>(hash));
    const uint32_t pairCount = codeSize / 8u;

    std::ostringstream out;
    out << std::boolalpha;
    out << (vu0 ? "// Generated by PS2X_VU0_RECOMP_DUMP (VR3). Derived from game data:\n"
                : "// Generated by PS2X_VU1_RECOMP_DUMP (VR1 stage A). Derived from game data:\n")
        << "// keep outside the repo, never commit.\n"
           "#include \"ps2_vu1_recomp_gen.h\"\n\n"
           "using VU1 = VU1Interpreter;\n\n"
           "template <>\nstruct " << image << "<" << hashText << ">\n{\n"
           "    using D = VU1::DecodedInstructionPair;\n"
           "    using U = VU1::InstructionUsage;\n"
           "    using P = VU1::Pipeline;\n"
           "    static const VU1::RecompPairFn kPairs[" << pairCount << "];\n"
           "    // Chain to the next pair's function (tail call) while run()'s loop\n"
           "    // header would let it issue; otherwise return to run().\n"
           "    static bool next(VU1 &vu, VU1::RunContext &c)\n    {\n"
           "        if (!vu.recompChainReady(c))\n            return false;\n"
           "        const VU1::RecompPairFn fn = kPairs[vu.m_state.pc >> 3];\n"
           "        if (fn == nullptr)\n            return false;\n"
           "        PS2X_VU1_MUSTTAIL return fn(vu, c);\n    }\n";
    std::vector<bool> emitted(pairCount, false);
    // VR3: VU0 images get pair functions only (no stage-4 blocks).
    std::vector<RecompBlockPlan> blocks;
    std::vector<uint8_t> directMap;
    if (!vu0)
    {
        decoder->planRecompBlocks(vuCode, codeSize, blocks);
        decoder->buildDirectFlagMap(vuCode, codeSize, directMap);
    }
    char codeSizeArgs[48] = "";
    if (vu0)
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
        // VR2 2D: pair functions are reached through the table or a tail call;
        // noinline keeps a leader's from being merged into its block trampoline.
        out << "    PS2X_VU1_NOINLINE static bool f" << label << "(VU1 &vu, VU1::RunContext &c)\n    {\n"
            << "        if (vu.issuePair<true" << codeSizeArgs << ">(d" << label << ", c))\n            return true;\n"
            // F4-2b: the handoff must be a *guaranteed* tail call (like next()'s
            // table call above). A plain return here nests one frame per pair
            // and overflows small stacks (Odin S1: 512 nested f-frames).
            << "        PS2X_VU1_MUSTTAIL return next(vu, c);\n    }\n";
    }
    // VR2 stage 4: one function per block leader. Guard fails -> the leader's
    // pair function; otherwise the pairs run back to back (a pair that ends
    // the program returns true, a stop request returns to run()).
    std::vector<bool> blockAt(pairCount, false);
    uint32_t blockPairs = 0u, noStallPairs = 0u;
    for (const RecompBlockPlan &block : blocks)
    {
        char label[16];
        std::snprintf(label, sizeof(label), "%04x", block.start * 8u);
        blockAt[block.start] = true;
        // VR2 2D: b<pc> is a frameless trampoline (guard, then a tail call to
        // the leader's pair function or to the out-of-line body B<pc>), so a
        // failed guard (knob off) costs no frame setup.
        out << "    static bool b" << label << "(VU1 &vu, VU1::RunContext &c)\n    {\n"
            << "        if (!vu.recompBlockReady(c, " << block.maxCycles << "u, " << block.pairs.size() << "u))\n"
            << "            PS2X_VU1_MUSTTAIL return f" << label << "(vu, c);\n"
            << "        PS2X_VU1_MUSTTAIL return B" << label << "(vu, c);\n    }\n"
            << "    PS2X_VU1_NOINLINE static bool B" << label << "(VU1 &vu, VU1::RunContext &c)\n    {\n";
        for (size_t k = 0; k < block.pairs.size(); ++k)
        {
            char pairLabel[16];
            std::snprintf(pairLabel, sizeof(pairLabel), "%04x", block.pairs[k] * 8u);
            out << "        if (vu.issuePair<true, " << unsigned(directMap[block.pairs[k]]) << ", "
                << (block.noStall[k] != 0u) << ">(d" << pairLabel << ", c))\n            return true;\n";
            if (k + 1u < block.pairs.size())
                out << "        if (vu.m_stopRequested)\n            return false;\n";
            ++blockPairs;
            noStallPairs += block.noStall[k] != 0u ? 1u : 0u;
        }
        out << "        PS2X_VU1_MUSTTAIL return next(vu, c);\n    }\n";
    }
    out << "};\n\n// VR2 stage 4: " << blocks.size() << " blocks, " << blockPairs << " block pairs, "
        << noStallPairs << " without a scoreboard read\n";
    out << "const VU1::RecompPairFn " << image << "<" << hashText << ">::kPairs[" << pairCount << "] = {\n";
    for (uint32_t index = 0; index < pairCount; ++index)
    {
        char label[16];
        std::snprintf(label, sizeof(label), "%04x", index * 8u);
        out << "        "
            << (!emitted[index] ? std::string("nullptr")
                : std::string("&") + image + "<" + hashText + ">::" + (blockAt[index] ? "b" : "f") + label)
            << ",\n";
    }
    out << "};\n\nnamespace\n{\n    const bool kRegistered = []\n    {\n"
           "        VU1::RecompProgram program;\n"
           "        program.hash = " << hashText << ";\n"
           "        program.codeSize = " << codeSize << "u;\n"
           "        program.pairCount = " << pairCount << "u;\n"
           "        program.pairs = " << image << "<" << hashText << ">::kPairs;\n"
           "        VU1::registerRecompProgram(program);\n"
           "        return true;\n    }();\n}\n";

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
        return false;
    const std::string text = out.str();
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(file);
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

// VB1: direct commit (stage B). PS2X_VU1_DIRECT=0 turns it off (queue every
// write, as stage A did).
bool VU1Interpreter::directCommitEnabled()
{
    static const bool enabled = []
    {
        const char *value = std::getenv("PS2X_VU1_DIRECT");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

// VR3: VU0 direct commit (VB1's rules, unchanged). PS2X_VU0_DIRECT=1 turns it
// on; default off.
bool VU1Interpreter::vu0DirectEnabled()
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
void VU1Interpreter::buildDirectFlagMap(const uint8_t *vuCode, uint32_t codeSize,
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
    constexpr uint32_t kMaxBlockPairs = 16u;
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

const uint8_t *VU1Interpreter::directFlagMap(const uint8_t *vuCode, uint32_t codeSize, bool tracked)
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
    static std::unordered_map<uint64_t, std::vector<uint8_t>> maps;
    const uint64_t key = m_recompHash ^ (static_cast<uint64_t>(codeSize) << 48);
    auto it = maps.find(key);
    if (it == maps.end())
    {
        it = maps.emplace(key, std::vector<uint8_t>()).first;
        buildDirectFlagMap(vuCode, codeSize, it->second);
    }
    return it->second.data();
}
