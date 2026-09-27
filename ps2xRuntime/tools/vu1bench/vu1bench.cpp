// VRB1 vu1bench: replay a PS2X_VU1_CAPTURE window through the runtime's VU1
// core + compiled-in generated images (no game, no GS, no EE).
//
//   vu1bench <vu1cap.bin> [--cpu N] [--repeat N] [--skip-verify]
//
// Links the same VU1 TUs as the runner (core/upper/lower/recomp + the vu1gen
// images) with the same Release flags + PS2X_VU1_FMAC_SIMD=1, drives the same
// VU1Interpreter::execute() boundary RV11 M1 asks per-run timers for, and
// verifies every run's data memory, registers, guest cycles and XGKICK bytes
// bit-exact against the capture. Reports per-image/per-entry timing, total ms
// per guest-frame equivalent, and an FNV checksum over replayed outputs.
//
// Env (same names as the runner): PS2X_VU1_BLOCKS=1 to match the play path,
// PS2X_VU_FLOAT=<mode> when VF1 lands (bench just inherits it).
//
// Boundary stubs: PS2Memory/GS ctor+dtor only (bench touches m_vu1Code,
// m_vu1CodeGeneration and binds a GS& that XGKICK capture intercepts); the
// GIF submit paths abort if ever reached.

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <vector>
#if defined(__linux__) || defined(__ANDROID__)
#include <sched.h>
#endif

#include "ps2_vu1cap.h"
#include "ps2_vu1_engine.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_vu1.h"

#define XXH_INLINE_ALL
#include "runtime/third_party/xxhash.h"

// ---- minimal runtime stubs (see file comment) -------------------------------

PS2Memory::PS2Memory()
    : m_rdram(nullptr), m_scratchpad(nullptr), iop_ram(nullptr), m_seenGifCopy(false),
      m_gsVRAM(nullptr)
{
    m_vu1Code = new uint8_t[PS2_VU1_CODE_SIZE]();
}

PS2Memory::~PS2Memory()
{
    delete[] m_vu1Code;
    m_vu1Code = nullptr;
}

GS::GS() = default;
GS::~GS() = default;

// The bench never creates a GsWorker; this satisfies unique_ptr<GsWorker>.
GsWorker::~GsWorker() {}

void PS2Memory::submitGifPacket(GifPathId, const uint8_t *, uint32_t, bool, bool)
{
    std::fprintf(stderr, "[vu1bench] fatal: submitGifPacket reached (capture off?)\n");
    std::abort();
}

void GS::processGIFPacket(const uint8_t *, uint32_t)
{
    std::fprintf(stderr, "[vu1bench] fatal: processGIFPacket reached (capture off?)\n");
    std::abort();
}

// ---- helpers ----------------------------------------------------------------

namespace
{
    uint64_t nowNs()
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
    }

    bool pinCpu(int cpu)
    {
#if defined(__linux__) || defined(__ANDROID__)
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu, &set);
        if (sched_setaffinity(0, sizeof(set), &set) != 0)
        {
            std::perror("[vu1bench] sched_setaffinity");
            return false;
        }
        return true;
#else
        (void)cpu;
        std::fprintf(stderr, "[vu1bench] note: --cpu is a no-op on this OS\n");
        return true;
#endif
    }

    struct EntryAgg
    {
        uint64_t jobs = 0;
        uint64_t ns = 0;
        uint64_t cycles = 0;
    };
} // namespace

int main(int argc, char **argv)
{
    const char *path = nullptr;
    int cpu = -1;
    int repeat = 1;
    bool verify = true;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--cpu") == 0 && i + 1 < argc)
            cpu = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--repeat") == 0 && i + 1 < argc)
            repeat = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--skip-verify") == 0)
            verify = false;
        else if (argv[i][0] != '-')
            path = argv[i];
        else
        {
            std::fprintf(stderr, "usage: vu1bench <vu1cap.bin> [--cpu N] [--repeat N] [--skip-verify]\n");
            return 2;
        }
    }
    if (!path || repeat < 1)
    {
        std::fprintf(stderr, "usage: vu1bench <vu1cap.bin> [--cpu N] [--repeat N] [--skip-verify]\n");
        return 2;
    }
    if (cpu >= 0 && !pinCpu(cpu))
        return 2;

    ps2_vu1cap::Reader reader;
    std::string err;
    if (!reader.open(path, err))
    {
        std::fprintf(stderr, "[vu1bench] open: %s\n", err.c_str());
        return 1;
    }
    auto &images = reader.images();
    std::fprintf(stderr, "[vu1bench] %s: jobs=%llu ticks=[%llu,%llu] images=%llu\n", path,
                 (unsigned long long)reader.jobCount(), (unsigned long long)reader.tickFrom(),
                 (unsigned long long)reader.tickTo(), (unsigned long long)images.size());

    PS2Memory mem;
    GS gs;
    VU1Interpreter vu(VU1Interpreter::Unit::VU1);

    // Key each image by XXH64 (the recomp lookup key) and require a compiled-in
    // image: replay must run the same generated code as the game.
    for (size_t i = 0; i < images.size(); ++i)
    {
        images[i].xxh = XXH64(images[i].code.data(), images[i].code.size(), 0);
        if (VU1Interpreter::findRecompProgram(images[i].xxh) == nullptr)
        {
            std::fprintf(stderr,
                         "[vu1bench] image %llu xxh=%016llx has no compiled-in image; rebuild bench "
                         "with this vu1gen set\n",
                         (unsigned long long)i, (unsigned long long)images[i].xxh);
            return 1;
        }
        std::fprintf(stderr, "[vu1bench] image %llu xxh=%016llx registered\n",
                     (unsigned long long)i, (unsigned long long)images[i].xxh);
    }

    std::vector<uint64_t> allNs;
    allNs.reserve(static_cast<size_t>(reader.jobCount()));
    std::map<std::pair<uint64_t, uint32_t>, EntryAgg> byEntry;
    std::map<uint64_t, EntryAgg> byImage;
    uint64_t totalCycles = 0;
    uint64_t minTick = ~0ull, maxTick = 0;
    uint64_t jobsDone = 0;
    uint64_t mismatches = 0;
    uint64_t fnv = 1469598103934665603ull;
    auto fnvMix = [&](const void *p, size_t n)
    {
        const auto *b = static_cast<const uint8_t *>(p);
        for (size_t i = 0; i < n; ++i)
            fnv = (fnv ^ b[i]) * 1099511628211ull;
    };

    std::vector<uint8_t> inBuf(ps2_vu1cap::kDataSize), expBuf(ps2_vu1cap::kDataSize);
    std::vector<uint8_t> prevOut(ps2_vu1cap::kDataSize);
    std::vector<uint8_t> xgkVec;
    xgkVec.reserve(1 << 20);
    ps2_vu1cap::Job job;
    int curImage = -1;

    for (int rep = 0; rep < repeat; ++rep)
    {
        reader.rewindJobs();
        bool first = true;
        uint64_t repJobs = 0;
        uint64_t repNs = 0;
        for (;;)
        {
            err.clear();
            if (!reader.readJob(job, err))
                break;
            if (!err.empty())
            {
                std::fprintf(stderr, "[vu1bench] read: %s\n", err.c_str());
                return 1;
            }
            if (job.imageIdx >= images.size())
            {
                std::fprintf(stderr, "[vu1bench] job %llu: bad imageIdx %u\n",
                             (unsigned long long)jobsDone, job.imageIdx);
                return 1;
            }
            // Reconstruct this job's input: full (job 0 of each pass) or the
            // previous job's captured output + delta.
            if (first)
            {
                if (!job.inFull)
                {
                    std::fprintf(stderr, "[vu1bench] job 0 is not full; corrupt capture?\n");
                    return 1;
                }
                std::memcpy(inBuf.data(), job.inFullBytes.data(), inBuf.size());
                first = false;
            }
            else
            {
                std::memcpy(inBuf.data(), prevOut.data(), inBuf.size());
                auto *w = reinterpret_cast<uint32_t *>(inBuf.data());
                for (size_t k = 0; k < job.inIdx.size(); ++k)
                    w[job.inIdx[k]] = job.inVal[k];
            }
            // Expected output = input + delta (also the next job's base).
            std::memcpy(expBuf.data(), inBuf.data(), expBuf.size());
            {
                auto *w = reinterpret_cast<uint32_t *>(expBuf.data());
                for (size_t k = 0; k < job.outIdx.size(); ++k)
                    w[job.outIdx[k]] = job.outVal[k];
            }
            // Code image (MPG in the game: bytes + generation bump).
            if (static_cast<int>(job.imageIdx) != curImage)
            {
                std::memcpy(mem.getVU1Code(), images[job.imageIdx].code.data(),
                            ps2_vu1cap::kCodeSize);
                mem.markVU1CodeModified();
                curImage = static_cast<int>(job.imageIdx);
            }
            ps2_vu1cap::unpackRegs(job.regsIn, vu.state());
            vu.setCycleCounterForBench(job.cycleStart);
            xgkVec.clear();
            ps2_vu1_engine::detail::t_capture = &xgkVec;
            const uint64_t t0 = nowNs();
            vu.execute(mem.getVU1Code(), ps2_vu1cap::kCodeSize, inBuf.data(),
                       ps2_vu1cap::kDataSize, gs, &mem, job.startPC, job.top, job.itop, 65536);
            const uint64_t t1 = nowNs();
            ps2_vu1_engine::detail::t_capture = nullptr;

            const uint64_t dt = t1 - t0;
            allNs.push_back(dt);
            const uint64_t cyc = vu.cycleCounterForBench() - job.cycleStart;
            totalCycles += cyc;
            auto key = std::make_pair(images[job.imageIdx].xxh, job.startPC);
            byEntry[key].jobs++;
            byEntry[key].ns += dt;
            byEntry[key].cycles += cyc;
            byImage[images[job.imageIdx].xxh].jobs++;
            byImage[images[job.imageIdx].xxh].ns += dt;
            byImage[images[job.imageIdx].xxh].cycles += cyc;
            minTick = std::min<uint64_t>(minTick, job.tick);
            maxTick = std::max<uint64_t>(maxTick, job.tick);

            // Checksum over ACTUAL replay outputs (cross-arch determinism check).
            fnvMix(inBuf.data(), inBuf.size());
            fnvMix(xgkVec.data(), xgkVec.size());
            {
                ps2_vu1cap::Regs actual{};
                ps2_vu1cap::packRegs(vu.state(), actual);
                fnvMix(&actual, sizeof(actual));
                fnvMix(&cyc, sizeof(cyc));
            }

            if (verify)
            {
                const char *what = nullptr;
                uint64_t at = 0;
                if (std::memcmp(inBuf.data(), expBuf.data(), expBuf.size()) != 0)
                {
                    what = "data";
                    const auto *a = reinterpret_cast<const uint32_t *>(inBuf.data());
                    const auto *e = reinterpret_cast<const uint32_t *>(expBuf.data());
                    for (uint32_t i = 0; i < ps2_vu1cap::kDataWords; ++i)
                        if (a[i] != e[i])
                        {
                            at = i;
                            break;
                        }
                }
                else
                {
                    ps2_vu1cap::Regs actual{};
                    ps2_vu1cap::packRegs(vu.state(), actual);
                    if (std::memcmp(&actual, &job.regsOut, sizeof(actual)) != 0)
                    {
                        what = "regs";
                        const auto *a = reinterpret_cast<const uint8_t *>(&actual);
                        const auto *e = reinterpret_cast<const uint8_t *>(&job.regsOut);
                        for (size_t i = 0; i < sizeof(actual); ++i)
                            if (a[i] != e[i])
                            {
                                at = i;
                                break;
                            }
                    }
                    else if (vu.cycleCounterForBench() != job.cycleEnd)
                        what = "cycles";
                    else
                    {
                        // engine capture layout: [u32 size][bytes] per kick, in order.
                        size_t off = 0;
                        bool xok = true;
                        size_t expOff = 0;
                        for (size_t i = 0; i < job.xgkSize.size(); ++i)
                        {
                            if (off + 4 > xgkVec.size())
                            {
                                xok = false;
                                break;
                            }
                            uint32_t n = 0;
                            std::memcpy(&n, xgkVec.data() + off, 4);
                            off += 4;
                            if (n != job.xgkSize[i] || off + n > xgkVec.size() ||
                                std::memcmp(xgkVec.data() + off, job.xgkBytes.data() + expOff, n) != 0)
                            {
                                xok = false;
                                at = i;
                                break;
                            }
                            off += n;
                            expOff += n;
                        }
                        if (xok && off != xgkVec.size())
                            xok = false;
                        if (!xok && !what)
                            what = "xgkick";
                    }
                }
                if (what)
                {
                    if (mismatches < 5)
                        std::fprintf(stderr,
                                     "[vu1bench] MISMATCH job=%llu tick=%u pc=%04x img=%u (%016llx) "
                                     "class=%s at=%llu\n",
                                     (unsigned long long)jobsDone, job.tick, job.startPC,
                                     job.imageIdx, (unsigned long long)images[job.imageIdx].xxh,
                                     what, (unsigned long long)at);
                    ++mismatches;
                }
            }

            std::memcpy(prevOut.data(), expBuf.data(), prevOut.size());
            ++jobsDone;
            ++repJobs;
            repNs += dt;
        }
        if (!err.empty())
        {
            std::fprintf(stderr, "[vu1bench] read: %s\n", err.c_str());
            return 1;
        }
        std::fprintf(stderr, "[vu1bench] pass %d: %llu jobs ms=%.3f\n", rep,
                     (unsigned long long)repJobs, repNs / 1e6);
    }

    std::sort(allNs.begin(), allNs.end());
    uint64_t totalNs = 0;
    for (uint64_t v : allNs)
        totalNs += v;
    const uint64_t tickSpan = maxTick >= minTick ? maxTick - minTick + 1 : 0;
    const double msPerFrame = tickSpan ? (static_cast<double>(totalNs) / 1e6 / repeat / tickSpan) : 0;

    std::printf("jobs=%llu repeat=%d ticks=[%llu,%llu] span=%llu\n", (unsigned long long)jobsDone,
                repeat, (unsigned long long)minTick, (unsigned long long)maxTick,
                (unsigned long long)tickSpan);
    std::printf("total_ms=%.3f ms_per_frame=%.3f cycles=%llu fnv=%016llx mismatches=%llu\n",
                static_cast<double>(totalNs) / 1e6 / repeat, msPerFrame,
                (unsigned long long)(totalCycles / (uint64_t)repeat), (unsigned long long)fnv,
                (unsigned long long)mismatches);
    std::printf("run_us p50=%.2f p99=%.2f max=%.2f\n",
                allNs[allNs.size() / 2] / 1000.0, allNs[allNs.size() * 99 / 100] / 1000.0,
                allNs.back() / 1000.0);
    std::printf("interp_cycles=%llu recomp_cycles=%llu (must be 0 interp)\n",
                (unsigned long long)vu.interpCyclesForTest(),
                (unsigned long long)vu.recompCyclesForTest());
    std::printf("--- per image (ms, %% of VU time) ---\n");
    for (const auto &kv : byImage)
        std::printf("img %016llx jobs=%llu ms=%.2f pct=%.1f us_per_run=%.2f\n",
                    (unsigned long long)kv.first, (unsigned long long)(kv.second.jobs / repeat),
                    kv.second.ns / 1e6 / repeat, 100.0 * kv.second.ns / totalNs,
                    (kv.second.ns / (double)kv.second.jobs) / 1000.0);
    std::vector<std::pair<std::pair<uint64_t, uint32_t>, EntryAgg>> entries(byEntry.begin(),
                                                                            byEntry.end());
    std::sort(entries.begin(), entries.end(),
              [](const auto &a, const auto &b) { return a.second.ns > b.second.ns; });
    std::printf("--- top entries by VU ms ---\n");
    for (size_t i = 0; i < entries.size() && i < 20; ++i)
        std::printf("%06llx:%04x jobs=%llu ms=%.2f pct=%.1f us_per_run=%.2f cycles_per_run=%.0f\n",
                    (unsigned long long)(entries[i].first.first >> 40),
                    entries[i].first.second, (unsigned long long)(entries[i].second.jobs / repeat),
                    entries[i].second.ns / 1e6 / repeat, 100.0 * entries[i].second.ns / totalNs,
                    (entries[i].second.ns / (double)entries[i].second.jobs) / 1000.0,
                    entries[i].second.cycles / (double)entries[i].second.jobs);
    return mismatches == 0 ? 0 : 1;
}
