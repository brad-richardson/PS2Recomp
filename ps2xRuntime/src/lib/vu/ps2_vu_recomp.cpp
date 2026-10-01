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

    void replaceAll(std::string &text, const std::string &from, const std::string &to)
    {
        for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
            text.replace(at, from.size(), to);
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
        // VX1: the VU1-only block/direct counters (hash builds).
        if constexpr (!vu0)
            derived().printRecompStats();
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
        const auto &registry = recompRegistry<D>();
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
            // VX1: VU0 images target VU0Interpreter; "vu0e_" keeps them apart
            // from pre-VX1 vu0_ images (VU1Interpreter-based) in a shared dir.
            std::snprintf(name, sizeof(name), vu0 ? "/vu0e_%016llx.cpp" : "/vu1_%016llx.cpp",
                          static_cast<unsigned long long>(m_recompHash));
            const std::string path = std::string(dumpDir) + name;
            const bool ok = emitRecompSource(vuCode, codeSize, m_recompHash, path);
            std::fprintf(stderr, "[%s] dump %s %s generation=%llu\n", vu0 ? "vu0-recomp" : "vu1-recomp",
                         path.c_str(), ok ? "ok" : "FAILED", static_cast<unsigned long long>(generation));
        }
    }
    return m_recompProgram;
}

template <class D>
bool VuCore<D>::emitRecompSource(const uint8_t *vuCode, uint32_t codeSize,
                                      uint64_t hash, const std::string &path)
{
    // The decoder is a pure function of the two instruction words and the
    // unit (VR3: VU0 reserves MFP, XGKICK, the EFU ops and WAITP).
    const auto decoder = std::make_unique<D>();
    constexpr bool vu0 = !isVu1();
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
           "    static const VU1::RecompPairFn kPairs[" << pairCount << "];\n";
    if (!vu0)
        out << "    static const VU1::RecompPairFn kPairsNative[" << pairCount << "];\n";
    out <<
           "    // Chain to the next pair's function (tail call) while run()'s loop\n"
           "    // header would let it issue; otherwise return to run().\n"
        << (vu0 ? "" : "    template <bool kNative>\n")
        << "    static bool next(VU1 &vu, VU1::RunContext &c)\n    {\n"
           "        if (!vu.recompChainReady(c))\n            return false;\n"
        << (vu0 ? "        const VU1::RecompPairFn fn = kPairs[vu.m_state.pc >> 3];\n"
                : "        const VU1::RecompPairFn fn = (kNative ? kPairsNative : kPairs)[vu.m_state.pc >> 3];\n")
        <<
           "        if (fn == nullptr)\n            return false;\n"
           "        PS2X_VU1_MUSTTAIL return fn(vu, c);\n    }\n";
    std::vector<bool> emitted(pairCount, false);
    // VR3: VU0 images get pair functions only (no stage-4 blocks).
    std::vector<RecompBlockPlan> blocks;
    std::vector<uint8_t> directMap;
    if constexpr (!vu0)
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
        if (!vu0)
            out << "    template <bool kNative>\n";
        out << "    PS2X_VU1_NOINLINE static bool f" << label << "(VU1 &vu, VU1::RunContext &c)\n    {\n"
            << "        if (vu.issuePair<true" << (vu0 ? codeSizeArgs : ", -1, false, 0x4000u, false, (kNative ? 1 : 0)")
            << ">(d" << label << ", c))\n            return true;\n"
            // F4-2b: the handoff must be a *guaranteed* tail call (like next()'s
            // table call above). A plain return here nests one frame per pair
            // and overflows small stacks (Odin S1: 512 nested f-frames).
            << "        PS2X_VU1_MUSTTAIL return next" << (vu0 ? "" : "<kNative>") << "(vu, c);\n    }\n";
    }
    // VR2 stage 4: one function per block leader. Guard fails -> the leader's
    // pair function; otherwise the pairs run back to back (a pair that ends
    // the program returns true, a stop request returns to run()).
    std::vector<bool> blockAt(pairCount, false);
    for (const RecompBlockPlan &block : blocks)
        blockAt[block.start] = true;
    uint32_t blockPairs = 0u, noStallPairs = 0u, plainTailPairs = 0u;
    for (const RecompBlockPlan &block : blocks)
    {
        char label[16];
        std::snprintf(label, sizeof(label), "%04x", block.start * 8u);
        // VR2 2D: b<pc> is a frameless trampoline (guard, then a tail call to
        // the leader's pair function or to the out-of-line body B<pc>), so a
        // failed guard (knob off) costs no frame setup.
        out << "    template <bool kNative>\n"
            << "    static bool b" << label << "(VU1 &vu, VU1::RunContext &c)\n    {\n"
            << "        if (!vu.recompBlockReady(c, " << block.maxCycles << "u, " << block.pairs.size() << "u))\n"
            << "            PS2X_VU1_MUSTTAIL return f" << label << "<kNative>(vu, c);\n"
            << "        PS2X_VU1_MUSTTAIL return B" << label << "<kNative>(vu, c);\n    }\n"
            << "    template <bool kNative>\n"
            << "    PS2X_VU1_NOINLINE static bool B" << label << "(VU1 &vu, VU1::RunContext &c)\n    {\n";
        for (size_t k = 0; k < block.pairs.size(); ++k)
        {
            char pairLabel[16];
            std::snprintf(pairLabel, sizeof(pairLabel), "%04x", block.pairs[k] * 8u);
            // GV2: in-place mask. A bit is set only where the snapshot path's
            // own proof already commits directly: the direct-map bit, no
            // shadowed/suppressed same-register interference, and (VI) one
            // written register at latency <= 1. ACC is always direct in a
            // block (kBlock implies direct).
            const DecodedInstructionPair pd = decoder->decodeInstructionPair(vuCode, block.pairs[k] * 8u);
            const uint8_t map = directMap[block.pairs[k]];
            const bool hasUpper = pd.upperUsage.vfWrite.reg != 0u;
            const bool hasLower = pd.lowerUsage.vfWrite.reg != 0u &&
                                  pd.suppressedLowerVf != pd.lowerUsage.vfWrite.reg;
            int inPlace = 0;
            if (hasUpper && (map & kDirectMapUpperVf) != 0u && pd.upperVfShadowReg == 0u &&
                pd.suppressedLowerVf != pd.upperUsage.vfWrite.reg)
                inPlace |= kInPlaceUpperVf;
            if (hasLower && (map & kDirectMapLowerVf) != 0u &&
                (!hasUpper || pd.lowerUsage.vfWrite.reg != pd.upperUsage.vfWrite.reg))
                inPlace |= kInPlaceLowerVf;
            if (pd.upperUsage.accWrite != 0u)
                inPlace |= kInPlaceAcc;
            const uint32_t viWrites = pd.lowerUsage.viWrite & 0xFFFEu;
            const uint32_t viLatency = pd.lowerUsage.viLatency != 0u ? pd.lowerUsage.viLatency
                                                                     : pd.lowerUsage.latency;
            if (viWrites != 0u && (viWrites & (viWrites - 1u)) == 0u && viLatency <= 1u)
                inPlace |= kInPlaceVi;
            out << "        if (vu.issuePair<true, " << unsigned(map) << ", "
                << (block.noStall[k] != 0u);
            if (block.plainTail[k] != 0u)
                out << ", 0x4000u, true, (kNative ? 1 : 0), " << inPlace << ">(d" << pairLabel << ", c, 0x" << std::hex
                    << ((block.pairs[k] * 8u + 8u) & (kRecompCodeSize - 1u)) << std::dec << "u))\n            return true;\n";
            else
                out << ", 0x4000u, false, (kNative ? 1 : 0), " << inPlace << ">(d" << pairLabel << ", c))\n            return true;\n";
            plainTailPairs += block.plainTail[k] != 0u ? 1u : 0u;
            if (k + 1u < block.pairs.size())
                out << "        if (vu.m_stopRequested)\n            return false;\n";
            ++blockPairs;
            noStallPairs += block.noStall[k] != 0u ? 1u : 0u;
        }
        const uint32_t nextIndex = (block.pairs.back() + 1u) & (pairCount - 1u);
        if (block.plainTail.back() != 0u && emitted[nextIndex])
        {
            char nextLabel[16];
            std::snprintf(nextLabel, sizeof(nextLabel), "%04x", nextIndex * 8u);
            out << "        if (!vu.recompChainReady(c))\n            return false;\n"
                << "        PS2X_VU1_MUSTTAIL return " << (blockAt[nextIndex] ? 'b' : 'f')
                << nextLabel << "<kNative>(vu, c);\n    }\n";
        }
        else
            out << "        PS2X_VU1_MUSTTAIL return next<kNative>(vu, c);\n    }\n";
    }
    out << "};\n\n// VR2 stage 4: " << blocks.size() << " blocks, " << blockPairs << " block pairs, "
        << noStallPairs << " without a scoreboard read, " << plainTailPairs << " with a plain tail\n";
    for (unsigned mode = 0; mode < (vu0 ? 1u : 2u); ++mode)
    {
        out << "const VU1::RecompPairFn " << image << "<" << hashText << ">::"
            << (mode == 0u ? "kPairs" : "kPairsNative") << "[" << pairCount << "] = {\n";
        for (uint32_t index = 0; index < pairCount; ++index)
        {
            char label[16];
            std::snprintf(label, sizeof(label), "%04x", index * 8u);
            out << "        "
                << (!emitted[index] ? std::string("nullptr")
                    : std::string("&") + image + "<" + hashText + ">::" + (blockAt[index] ? "b" : "f") + label
                    + (vu0 ? "" : (mode == 0u ? "<false>" : "<true>")))
                << ",\n";
        }
        out << "};\n\n";
    }
    out << "namespace\n{\n    const bool kRegistered = []\n    {\n"
           "        VU1::RecompProgram program;\n"
           "        program.hash = " << hashText << ";\n"
           "        program.codeSize = " << codeSize << "u;\n"
           "        program.pairCount = " << pairCount << "u;\n"
           "        program.pairs = " << image << "<" << hashText << ">::kPairs;\n";
    if (!vu0)
        out << "        program.nativePairs = " << image << "<" << hashText << ">::kPairsNative;\n";
    out <<
           "        VU1::registerRecompProgram(program);\n"
           "        return true;\n    }();\n}\n";

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
        return false;
    std::string text = out.str();
    if constexpr (vu0)
    {
        // VX1: VU0 images target VU0Interpreter (the text above is VU1's
        // layout; VU1 images stay byte-identical to pre-VX1 output).
        replaceAll(text, "#include \"ps2_vu1_recomp_gen.h\"", "#include \"ps2_vu0_recomp_gen.h\"");
        replaceAll(text, "using VU1 = VU1Interpreter;", "using VU0 = VU0Interpreter;");
        replaceAll(text, "VU1::", "VU0::");
        replaceAll(text, "(VU1 &vu,", "(VU0 &vu,");
        replaceAll(text, "PS2X_VU1_MUSTTAIL", "PS2X_VU_MUSTTAIL");
        replaceAll(text, "PS2X_VU1_NOINLINE", "PS2X_VU_NOINLINE");
    }
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(file);
}

// VB1: direct commit (stage B). PS2X_VU1_DIRECT=0 turns it off (queue every
// write, as stage A did).
template <class D>
bool VuCore<D>::directCommitEnabled()
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
    template void VuCore<U>::registerRecompProgram(const RecompProgram &);                         \
    template const VuCore<U>::RecompProgram *VuCore<U>::findRecompProgram(uint64_t);               \
    template const VuCore<U>::RecompProgram *VuCore<U>::lookupRecompProgram(const uint8_t *, uint32_t, \
                                                                            PS2Memory *);          \
    template bool VuCore<U>::emitRecompSource(const uint8_t *, uint32_t, uint64_t, const std::string &); \
    template bool VuCore<U>::directCommitEnabled();                                                \
    template bool VuCore<U>::vu0DirectEnabled();                                                   \
    template void VuCore<U>::buildDirectFlagMap(const uint8_t *, uint32_t, std::vector<uint8_t> &) const; \
    template const uint8_t *VuCore<U>::directFlagMap(const uint8_t *, uint32_t, bool);
PS2X_VU_RECOMP_INSTANTIATE(VU0Interpreter)
PS2X_VU_RECOMP_INSTANTIATE(VU1Interpreter)
#undef PS2X_VU_RECOMP_INSTANTIATE
