// ci-e2e-1 synthetic end-to-end smoke: boot a hand-rolled EE ELF through the
// real recompiler output, one guest "frame" per vsync, hashing EE state.
//
// Build: configured only when PS2X_E2E_CODEGEN_DIR points at a ps2_recomp
// output dir (single_file_output=true) plus e2e_image.h from gen_e2e_elf.py.
// Run headless: PS2X_GS_BACKEND=headless PS2X_UNPACED=1.
// Usage: ps2xE2ESmoke <expected.txt> | ps2xE2ESmoke --mint <expected.txt>
// Exit 0 iff all N per-vsync combined hashes match. Never mints in CI.

#include "e2e_image.h"

#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_memory.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
constexpr int kFrames = 120;
constexpr uint32_t kExitAddr = 0x100008u; // mid-function slot; never dispatched (slot 2)
constexpr uint32_t kStackAddr = 0x001F0000u;
constexpr uint32_t kCycleChunk = 1u << 22u;

void exitStub(uint8_t *, R5900Context *ctx, PS2Runtime *)
{
    ctx->pc = kExitAddr;
}

std::string toHex(uint64_t value)
{
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(16) << value;
    return oss.str();
}

uint32_t readU32(const uint8_t *ram, uint32_t addr)
{
    uint32_t value = 0;
    std::memcpy(&value, ram + addr, sizeof(value));
    return value;
}
} // namespace

// Same-named accessor the scheduler befriends for its unit tests; grants
// this harness the private due-deadline pump and det-hash snapshot. Must be
// at global scope: the friend declaration names ::EeSchedulerTestAccess.
struct EeSchedulerTestAccess
{
    static void due(EeScheduler &ee) { ee.processDueDeadlines(); }
    static EeScheduler::DetHashSnapshot hash(const EeScheduler &ee) { return ee.makeDetHashSnapshot(); }
};

int main(int argc, char **argv)
{
    const bool mint = argc == 3 && std::string(argv[1]) == "--mint";
    const char *expectPath = mint ? argv[2] : (argc == 2 ? argv[1] : nullptr);
    if (expectPath == nullptr)
    {
        std::cerr << "usage: ps2xE2ESmoke <expected.txt>|--mint <out>\n";
        return 2;
    }

    PS2Runtime runtime;
    // Guest RAM must be the runtime-owned RDRAM: the det-hash snapshot
    // hashes m_runtime.memory(), and bails when handed any other base.
    // (Same setup as the scheduler's own det-hash unit test.)
    if (!runtime.memory().initialize())
    {
        std::cerr << "e2e: runtime memory init failed\n";
        return 2;
    }
    uint8_t *ram = runtime.memory().getRDRAM();
    std::memset(ram, 0, PS2_RAM_SIZE);
    std::memcpy(ram + kE2ETextBase, kE2EText, kE2ETextSize);
    std::memcpy(ram + kE2EDataBase, kE2EData, kE2EDataSize);
    runtime.setMissingFunctionPolicy(PS2Runtime::MissingFunctionPolicy::Stop);
    if (runtime.hasFunction(kExitAddr))
    {
        std::cerr << "e2e: exit slot unexpectedly occupied\n";
        return 2;
    }
    if (!runtime.registerFunction(kExitAddr, exitStub))
    {
        std::cerr << "e2e: exit stub registration failed\n";
        return 2;
    }

    R5900Context ctx{};
    ctx.pc = kE2EEntry;
    ctx.r[29] = _mm_set_epi64x(0, static_cast<int64_t>(kStackAddr));
    EeScheduler &sched = runtime.eeScheduler();
    sched.reset(ram, ctx);

    PS2Runtime::RecompiledFunction entry = runtime.lookupFunction(kE2EEntry);
    if (entry == nullptr)
    {
        std::cerr << "e2e: no function at entry\n";
        return 2;
    }

    std::vector<std::string> hashes;
    hashes.reserve(kFrames);
    for (int frame = 0; frame < kFrames; ++frame)
    {
        ctx.pc = kE2EEntry;
        ctx.r[31] = _mm_set_epi64x(0, static_cast<int64_t>(kExitAddr));
        entry(ram, &ctx, &runtime);

        const uint64_t wantTick = sched.currentVSyncTick() + 1u;
        for (int guard = 0; guard < 8 && sched.currentVSyncTick() < wantTick; ++guard)
        {
            sched.accountCycles(kCycleChunk);
            EeSchedulerTestAccess::due(sched);
        }
        if (sched.currentVSyncTick() != wantTick)
        {
            std::cerr << "e2e: vsync did not advance on frame " << frame << "\n";
            return 1;
        }
#if PS2X_ENABLE_DET_HASH_TAP
        auto snap = EeSchedulerTestAccess::hash(sched);
        if (!snap.valid)
        {
            std::cerr << "e2e: invalid det-hash snapshot on frame " << frame << "\n";
            return 1;
        }
        hashes.push_back(toHex(snap.combined));
#else
        std::cerr << "e2e: built without PS2X_ENABLE_DET_HASH_TAP\n";
        return 2;
#endif
    }

    if (mint)
    {
        std::ofstream out(expectPath, std::ios::trunc);
        for (const std::string &h : hashes)
            out << h << "\n";
        std::cout << "E2E-MINTED " << hashes.size() << " frames\n";
        return 0;
    }

    std::ifstream in(expectPath);
    if (!in)
    {
        std::cerr << "e2e: cannot open " << expectPath << "\n";
        return 2;
    }
    std::vector<std::string> expected;
    std::string line;
    while (std::getline(in, line))
    {
        if (!line.empty())
            expected.push_back(line);
    }
    if (expected.size() != hashes.size())
    {
        std::cerr << "e2e: frame count mismatch: got " << hashes.size() << " want " << expected.size() << "\n";
        return 1;
    }
    for (size_t i = 0; i < hashes.size(); ++i)
    {
        if (hashes[i] != expected[i])
        {
            std::cerr << "e2e: hash mismatch frame " << i << ": got " << hashes[i] << " want " << expected[i] << "\n";
            std::cerr << "e2e: counter=" << readU32(ram, kE2EDataBase + 0x00)
                      << " sum=" << readU32(ram, kE2EDataBase + 0x04)
                      << " fpu=0x" << std::hex << readU32(ram, kE2EDataBase + 0x08)
                      << " count=" << std::dec << readU32(ram, kE2EDataBase + 0x0C)
                      << " csum=" << readU32(ram, kE2EDataBase + 0x10) << "\n";
            return 1;
        }
    }
    std::cout << "E2E-OK " << hashes.size() << " frames, final=" << hashes.back() << "\n";
    return 0;
}
