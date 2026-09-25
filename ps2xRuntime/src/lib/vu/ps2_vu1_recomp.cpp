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
//   PS2X_VU1_RECOMP_STATS=1    print cumulative generated/interpreted cycles.

#include "runtime/ps2_vu1.h"
#include "runtime/ps2_memory.h"
#define XXH_NO_XXH32
#define XXH_NO_XXH3
#define XXH_INLINE_ALL
#include "runtime/third_party/xxhash.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
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

void VU1Interpreter::registerRecompProgram(const RecompProgram &program)
{
    recompRegistry()[program.hash] = program;
}

const VU1Interpreter::RecompProgram *VU1Interpreter::lookupRecompProgram(
    const uint8_t *vuCode, uint32_t codeSize, PS2Memory *memory)
{
    if (m_unit != Unit::VU1 || memory == nullptr || vuCode != memory->getVU1Code())
        return nullptr;
    if (recompStatsEnabled() && (++m_recompRuns & 0x3FFFu) == 0u)
    {
        const uint64_t total = m_recompCycles + m_interpCycles;
        std::fprintf(stderr, "[vu1-recomp] runs=%llu generated_cycles=%llu interpreted_cycles=%llu generated_share=%.4f\n",
                     static_cast<unsigned long long>(m_recompRuns),
                     static_cast<unsigned long long>(m_recompCycles),
                     static_cast<unsigned long long>(m_interpCycles),
                     total != 0u ? static_cast<double>(m_recompCycles) / static_cast<double>(total) : 0.0);
    }
    const uint64_t generation = memory->getVU1CodeGeneration();
    if (m_recompValid && m_recompCode == vuCode && m_recompCodeSize == codeSize &&
        m_recompGeneration == generation)
        return m_recompProgram;

    m_recompValid = true;
    m_recompCode = vuCode;
    m_recompCodeSize = codeSize;
    m_recompGeneration = generation;
    m_recompHash = XXH64(vuCode, codeSize, 0);
    m_recompProgram = nullptr;
    if (recompEnabled())
    {
        const auto &registry = recompRegistry();
        const auto it = registry.find(m_recompHash);
        if (it != registry.end() && it->second.codeSize == codeSize)
            m_recompProgram = &it->second;
    }
    if (m_recompProgram == nullptr && recompDumpDir() != nullptr)
    {
        static std::unordered_set<uint64_t> dumped;
        if (dumped.insert(m_recompHash).second)
        {
            char name[64];
            std::snprintf(name, sizeof(name), "/vu1_%016llx.cpp",
                          static_cast<unsigned long long>(m_recompHash));
            const std::string path = std::string(recompDumpDir()) + name;
            const bool ok = emitRecompSource(vuCode, codeSize, m_recompHash, path);
            std::fprintf(stderr, "[vu1-recomp] dump %s %s generation=%llu\n", path.c_str(),
                         ok ? "ok" : "FAILED", static_cast<unsigned long long>(generation));
        }
    }
    return m_recompProgram;
}

bool VU1Interpreter::emitRecompSource(const uint8_t *vuCode, uint32_t codeSize,
                                      uint64_t hash, const std::string &path)
{
    // The decoder is a pure function of the two instruction words (VU1 unit).
    const auto decoder = std::make_unique<VU1Interpreter>(Unit::VU1);
    char hashText[32];
    std::snprintf(hashText, sizeof(hashText), "0x%016llxull", static_cast<unsigned long long>(hash));
    const uint32_t pairCount = codeSize / 8u;

    std::ostringstream out;
    out << std::boolalpha;
    out << "// Generated by PS2X_VU1_RECOMP_DUMP (VR1 stage A). Derived from game data:\n"
           "// keep outside the repo, never commit.\n"
           "#include \"ps2_vu1_recomp_gen.h\"\n\n"
           "using VU1 = VU1Interpreter;\n\n"
           "template <>\nstruct VU1RecompImage<" << hashText << ">\n{\n"
           "    using D = VU1::DecodedInstructionPair;\n"
           "    using U = VU1::InstructionUsage;\n"
           "    using P = VU1::Pipeline;\n";
    std::vector<bool> emitted(pairCount, false);
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
        out << "    static bool f" << label << "(VU1 &vu, VU1::RunContext &c) { return vu.issuePair<true>(d"
            << label << ", c); }\n";
    }
    out << "};\n\nnamespace\n{\n    constexpr VU1::RecompPairFn kPairs[" << pairCount << "] = {\n";
    for (uint32_t index = 0; index < pairCount; ++index)
    {
        char label[16];
        std::snprintf(label, sizeof(label), "%04x", index * 8u);
        out << "        " << (emitted[index] ? std::string("&VU1RecompImage<") + hashText + ">::f" + label : std::string("nullptr"))
            << ",\n";
    }
    out << "    };\n    const bool kRegistered = []\n    {\n"
           "        VU1::RecompProgram program;\n"
           "        program.hash = " << hashText << ";\n"
           "        program.codeSize = " << codeSize << "u;\n"
           "        program.pairCount = " << pairCount << "u;\n"
           "        program.pairs = kPairs;\n"
           "        VU1::registerRecompProgram(program);\n"
           "        return true;\n    }();\n}\n";

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
        return false;
    const std::string text = out.str();
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(file);
}
