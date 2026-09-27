// VRB1 (local branch vrb1, never pushed): VU1 run capture + replay format.
//
// Capture (game boot, default off):
//   PS2X_VU1_CAPTURE=<dir>   write <dir>/vu1cap.bin (uncompressed; gzip after)
//   PS2X_VU1_CAP_FROM=2400   first vsync tick recorded (default 2400)
//   PS2X_VU1_CAP_TO=2500     last vsync tick recorded, inclusive (default 2500)
// The hook lives in VU1Interpreter::execute/resume/finishXgkick (ps2_vu1_core.cpp),
// the same boundary as VP1's census and RV11's proposed matched timers. It only
// reads guest state, so a det boot with capture on must stay det-IDENTICAL.
// Runs are recorded in execute() order (true at W=0 and at W=1, which also
// serializes runs). MSCNT resumes are counted, never recorded: format v1 holds
// MSCAL runs only (VP1 saw 0 resumes in t1714-3000).
//
// Replay (vu1bench, ps2xRuntime/tools/vu1bench): streams the jobs back through
// VU1Interpreter::execute on the same code images, verifies data + registers +
// cycles + XGKICK bytes bit-exact, and times the execute() boundary per run.
//
// File layout (all little-endian):
//   header:  magic[8]="VU1CAP01" ver u32=1 flags u32 from u64 to u64 jobs u64
//   jobs:    per run (see Reader::readJob)
//   images:  u64 count + per image { u64 xxh64 + 16384 code bytes }
//   footer:  u64 imageTableOffset + magic[8]="VU1CAPEND"
// A job's dataIn is a delta vs the previous recorded job's OUTPUT (job 0 is
// stored full); its dataOut is a delta vs its own input. Deltas are adaptive:
// u32 count, then count<u16 idx,u32 val> pairs when small, else a 512-byte
// word bitmap + count u32 values in ascending index order.

#ifndef PS2_VU1CAP_H
#define PS2_VU1CAP_H

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "runtime/ps2_vu1.h"

namespace ps2_vu1cap
{

constexpr uint32_t kVersion = 1;
constexpr uint32_t kCodeSize = 16384;
constexpr uint32_t kDataSize = 16384;
constexpr uint32_t kDataWords = 4096;
constexpr uint32_t kPairsThreshold = 254; // below: idx/val pairs; else bitmap+values
constexpr uint32_t kFullMarker = 0xFFFFFFFFu;
constexpr uint32_t kFlagResumeSeen = 1u << 0;

// Packed register image (no padding; bit-compared on replay).
struct Regs
{
    float vf[32][4];
    int32_t vi[16];
    float acc[4];
    float q, p, i;
    uint32_t r, pc, mac, clip, status;
    uint64_t cycles;
    uint32_t top, itop;
    uint32_t branchTarget;
    uint32_t branchDelay;
    uint8_t flags; // bit0 ebit,1 haltAfterDelaySlot,2 dBit,3 tBit,4 stoppedByD,5 stoppedByT,6 branchPending
    uint8_t reserved[3];
};
static_assert(sizeof(Regs) == 652, "Regs must be packed");

inline void packRegs(const VU1State &s, Regs &r)
{
    std::memcpy(r.vf, s.vf, sizeof(r.vf));
    std::memcpy(r.vi, s.vi, sizeof(r.vi));
    std::memcpy(r.acc, s.acc, sizeof(r.acc));
    r.q = s.q;
    r.p = s.p;
    r.i = s.i;
    r.r = s.r;
    r.pc = s.pc;
    r.mac = s.mac;
    r.clip = s.clip;
    r.status = s.status;
    r.cycles = s.cycles;
    r.top = s.top;
    r.itop = s.itop;
    r.branchTarget = s.branchTarget;
    r.branchDelay = s.branchDelay;
    r.flags = static_cast<uint8_t>((s.ebit ? 1u : 0u) | (s.haltAfterDelaySlot ? 2u : 0u) |
                                   (s.dBitEnabled ? 4u : 0u) | (s.tBitEnabled ? 8u : 0u) |
                                   (s.stoppedByD ? 16u : 0u) | (s.stoppedByT ? 32u : 0u) |
                                   (s.branchPending ? 64u : 0u));
    r.reserved[0] = r.reserved[1] = r.reserved[2] = 0;
}

inline void unpackRegs(const Regs &r, VU1State &s)
{
    std::memcpy(s.vf, r.vf, sizeof(s.vf));
    std::memcpy(s.vi, r.vi, sizeof(s.vi));
    std::memcpy(s.acc, r.acc, sizeof(s.acc));
    s.q = r.q;
    s.p = r.p;
    s.i = r.i;
    s.r = r.r;
    s.pc = r.pc;
    s.mac = r.mac;
    s.clip = r.clip;
    s.status = r.status;
    s.cycles = r.cycles;
    s.top = r.top;
    s.itop = r.itop;
    s.branchTarget = r.branchTarget;
    s.branchDelay = r.branchDelay;
    s.ebit = (r.flags & 1u) != 0u;
    s.haltAfterDelaySlot = (r.flags & 2u) != 0u;
    s.dBitEnabled = (r.flags & 4u) != 0u;
    s.tBitEnabled = (r.flags & 8u) != 0u;
    s.stoppedByD = (r.flags & 16u) != 0u;
    s.stoppedByT = (r.flags & 32u) != 0u;
    s.branchPending = (r.flags & 64u) != 0u;
}

// ---- capture side (game boot) ----------------------------------------------

namespace detail
{
    inline bool captureOn()
    {
        static const bool on = std::getenv("PS2X_VU1_CAPTURE") != nullptr;
        return on;
    }
    inline uint64_t envU64(const char *name, uint64_t def)
    {
        const char *v = std::getenv(name);
        return v ? std::strtoull(v, nullptr, 10) : def;
    }
    inline uint64_t fnv1a64(const uint8_t *p, size_t n)
    {
        // Capture-side image key only; the bench re-keys each image with XXH64.
        uint64_t h = 1469598103934665603ull;
        for (size_t i = 0; i < n; ++i)
            h = (h ^ p[i]) * 1099511628211ull;
        return h;
    }
} // namespace detail

// True when the capture hook is compiled in and PS2X_VU1_CAPTURE is set.
// One cached branch per run when off.
inline bool on() { return detail::captureOn(); }

struct Capture
{
    static Capture &instance()
    {
        static Capture c;
        return c;
    }

    bool armed = false;      // env set, file open
    bool runOpen = false;    // current run is being recorded
    bool finalized = false;
    FILE *f = nullptr;
    uint64_t from = 2400, to = 2500;
    uint64_t jobs = 0;
    uint64_t resumes = 0;
    uint64_t bytes = 0;
    // current run scratch
    uint32_t curTick = 0, curPC = 0, curTop = 0, curItop = 0;
    uint64_t curCycle = 0, curCodeHash = 0;
    uint32_t curImageIdx = 0;
    Regs curRegsIn{};
    uint8_t curIn[kDataSize]{};
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> curXgk; // size + bytes
    // delta base: previous recorded job's output
    uint8_t prevOut[kDataSize]{};
    bool havePrevOut = false;
    // image table
    std::unordered_map<uint64_t, uint32_t> imageIdx;
    std::vector<std::vector<uint8_t>> imageCode;

    void ensureOpen()
    {
        if (armed || finalized)
            return;
        const char *dir = std::getenv("PS2X_VU1_CAPTURE");
        if (!dir)
            return;
        from = detail::envU64("PS2X_VU1_CAP_FROM", 2400);
        to = detail::envU64("PS2X_VU1_CAP_TO", 2500);
        std::string path = std::string(dir) + "/vu1cap.bin";
        f = std::fopen(path.c_str(), "wb");
        if (!f)
        {
            std::fprintf(stderr, "[vu1cap] cannot open %s; capture off\n", path.c_str());
            finalized = true;
            return;
        }
        // header (job count patched at finalize)
        std::fwrite("VU1CAP01", 1, 8, f);
        writeU32(kVersion);
        writeU32(0); // flags
        writeU64(from);
        writeU64(to);
        writeU64(0); // jobs
        bytes = 40;
        armed = true;
        std::fprintf(stderr, "[vu1cap] recording %s ticks [%llu,%llu]\n", path.c_str(),
                     (unsigned long long)from, (unsigned long long)to);
        std::atexit([] { Capture::instance().finalize(); });
    }

    void writeU32(uint32_t v)
    {
        std::fwrite(&v, 1, 4, f);
        bytes += 4;
    }
    void writeU64(uint64_t v)
    {
        std::fwrite(&v, 1, 8, f);
        bytes += 8;
    }
    void writeBytes(const void *p, size_t n)
    {
        if (n)
            std::fwrite(p, 1, n, f);
        bytes += n;
    }

    void writeDelta(const uint8_t *base, const uint8_t *cur)
    {
        const auto *b = reinterpret_cast<const uint32_t *>(base);
        const auto *c = reinterpret_cast<const uint32_t *>(cur);
        uint32_t idx[kDataWords];
        uint32_t n = 0;
        for (uint32_t i = 0; i < kDataWords; ++i)
            if (b[i] != c[i])
                idx[n++] = i;
        writeU32(n);
        if (n < kPairsThreshold)
        {
            for (uint32_t k = 0; k < n; ++k)
            {
                const uint16_t ii = static_cast<uint16_t>(idx[k]);
                std::fwrite(&ii, 1, 2, f);
                std::fwrite(&c[idx[k]], 1, 4, f);
                bytes += 6;
            }
            return;
        }
        uint8_t bits[kDataWords / 8];
        std::memset(bits, 0, sizeof(bits));
        for (uint32_t k = 0; k < n; ++k)
            bits[idx[k] >> 3] |= static_cast<uint8_t>(1u << (idx[k] & 7u));
        writeBytes(bits, sizeof(bits));
        for (uint32_t k = 0; k < n; ++k)
            writeBytes(&c[idx[k]], 4);
    }

    void begin(uint64_t tick, uint32_t startPC, const VU1State &st, uint64_t cycle,
               uint32_t top, uint32_t itop, const uint8_t *vuCode, uint32_t codeSize,
               const uint8_t *vuData, uint32_t dataSize, bool resume)
    {
        ensureOpen();
        runOpen = false;
        if (!armed || finalized || tick < from)
            return;
        if (tick > to)
        {
            finalize();
            return;
        }
        if (resume)
        {
            ++resumes; // format v1 holds MSCAL runs only
            return;
        }
        if (codeSize != kCodeSize || dataSize != kDataSize)
        {
            std::fprintf(stderr, "[vu1cap] odd sizes code=%u data=%u at tick %llu; stopping\n",
                         codeSize, dataSize, (unsigned long long)tick);
            finalize();
            return;
        }
        curTick = static_cast<uint32_t>(tick);
        curPC = startPC;
        curTop = top;
        curItop = itop;
        curCycle = cycle;
        packRegs(st, curRegsIn);
        std::memcpy(curIn, vuData, kDataSize);
        curCodeHash = detail::fnv1a64(vuCode, codeSize);
        auto it = imageIdx.find(curCodeHash);
        if (it == imageIdx.end())
        {
            curImageIdx = static_cast<uint32_t>(imageCode.size());
            imageIdx[curCodeHash] = curImageIdx;
            imageCode.emplace_back(vuCode, vuCode + codeSize);
        }
        else
            curImageIdx = it->second;
        curXgk.clear();
        runOpen = true;
    }

    void xgk(const uint8_t *p, uint32_t n)
    {
        if (!runOpen)
            return;
        curXgk.emplace_back(n, std::vector<uint8_t>(p, p + n));
    }

    void end(const VU1State &st, uint64_t cycleEnd, const uint8_t *vuData)
    {
        if (!runOpen)
            return;
        runOpen = false;
        writeU32(curTick);
        writeU32(curPC);
        writeU32(curImageIdx);
        writeU32(curTop);
        writeU32(curItop);
        writeU64(curCycle);
        writeBytes(&curRegsIn, sizeof(curRegsIn));
        if (!havePrevOut)
        {
            writeU32(kFullMarker);
            writeBytes(curIn, kDataSize);
        }
        else
            writeDelta(prevOut, curIn);
        Regs out{};
        packRegs(st, out);
        writeBytes(&out, sizeof(out));
        writeDelta(curIn, vuData);
        writeU64(cycleEnd);
        writeU32(static_cast<uint32_t>(curXgk.size()));
        uint32_t total = 0;
        for (const auto &k : curXgk)
            total += k.first;
        writeU32(total);
        for (const auto &k : curXgk)
        {
            writeU32(k.first);
            writeBytes(k.second.data(), k.first);
        }
        std::memcpy(prevOut, vuData, kDataSize);
        havePrevOut = true;
        ++jobs;
    }

    void finalize()
    {
        if (!armed || finalized)
            return;
        finalized = true;
        runOpen = false;
        const uint64_t tableOff = bytes;
        writeU64(static_cast<uint64_t>(imageCode.size()));
        for (size_t i = 0; i < imageCode.size(); ++i)
        {
            uint64_t h = 0;
            for (const auto &kv : imageIdx)
                if (kv.second == i)
                    h = kv.first;
            writeU64(h);
            writeBytes(imageCode[i].data(), kCodeSize);
        }
        writeU64(tableOff);
        std::fwrite("VU1CAPEND", 1, 8, f);
        bytes += 8;
        // patch header counts
        std::fseek(f, 8 + 4, SEEK_SET);
        uint32_t flags = resumes != 0 ? kFlagResumeSeen : 0u;
        std::fwrite(&flags, 1, 4, f);
        std::fseek(f, 8 + 4 + 4 + 8 + 8, SEEK_SET);
        std::fwrite(&jobs, 1, 8, f);
        std::fclose(f);
        f = nullptr;
        std::fprintf(stderr, "[vu1cap] done: jobs=%llu images=%llu resumes=%llu bytes=%llu\n",
                     (unsigned long long)jobs, (unsigned long long)imageCode.size(),
                     (unsigned long long)resumes, (unsigned long long)bytes);
    }
};

inline bool recording() { return Capture::instance().runOpen; }

// ---- replay side (vu1bench) ------------------------------------------------

struct Image
{
    uint64_t key = 0; // capture-side FNV key
    uint64_t xxh = 0; // XXH64 of the code (recomp lookup key)
    std::vector<uint8_t> code;
};

struct Job
{
    uint32_t tick = 0, startPC = 0, imageIdx = 0, top = 0, itop = 0;
    uint64_t cycleStart = 0, cycleEnd = 0;
    Regs regsIn{}, regsOut{};
    // dataIn encoding (vs previous job's output, or full for job 0)
    bool inFull = false;
    std::vector<uint8_t> inFullBytes;
    std::vector<uint32_t> inIdx; // ascending word indices that differ
    std::vector<uint32_t> inVal; // their values
    // dataOut encoding (vs this job's input)
    std::vector<uint32_t> outIdx;
    std::vector<uint32_t> outVal;
    // expected XGKICK packets
    std::vector<uint32_t> xgkSize;
    std::vector<uint8_t> xgkBytes; // concatenated
};

class Reader
{
  public:
    bool open(const char *path, std::string &err)
    {
        f_ = std::fopen(path, "rb");
        if (!f_)
        {
            err = "cannot open ";
            err += path;
            return false;
        }
        char magic[8];
        if (std::fread(magic, 1, 8, f_) != 8 || std::memcmp(magic, "VU1CAP01", 8) != 0)
        {
            err = "bad magic";
            return false;
        }
        const uint32_t ver = readU32();
        flags_ = readU32();
        from_ = readU64();
        to_ = readU64();
        jobCount_ = readU64();
        if (ver != kVersion)
        {
            err = "bad version";
            return false;
        }
        if ((flags_ & kFlagResumeSeen) != 0)
        {
            err = "capture holds MSCNT resumes (format v1 is MSCAL-only)";
            return false;
        }
        if (std::fseek(f_, -16, SEEK_END) != 0)
        {
            err = "cannot seek to footer";
            return false;
        }
        const uint64_t tableOff = readU64();
        char magic2[8];
        if (std::fread(magic2, 1, 8, f_) != 8 || std::memcmp(magic2, "VU1CAPEND", 8) != 0)
        {
            err = "bad footer magic (truncated capture?)";
            return false;
        }
        jobsEnd_ = tableOff;
        if (std::fseek(f_, static_cast<long>(tableOff), SEEK_SET) != 0)
        {
            err = "cannot seek to image table";
            return false;
        }
        const uint64_t nImg = readU64();
        if (nImg == 0 || nImg > 1024)
        {
            err = "bad image count";
            return false;
        }
        images_.resize(static_cast<size_t>(nImg));
        for (uint64_t i = 0; i < nImg; ++i)
        {
            images_[static_cast<size_t>(i)].key = readU64();
            images_[static_cast<size_t>(i)].code.resize(kCodeSize);
            if (std::fread(images_[static_cast<size_t>(i)].code.data(), 1, kCodeSize, f_) != kCodeSize)
            {
                err = "truncated image blob";
                return false;
            }
        }
        if (std::fseek(f_, kHeaderSize, SEEK_SET) != 0)
        {
            err = "cannot seek to jobs";
            return false;
        }
        return true;
    }

    uint64_t jobCount() const { return jobCount_; }
    uint64_t tickFrom() const { return from_; }
    uint64_t tickTo() const { return to_; }
    std::vector<Image> &images() { return images_; }

    // Reads the next job; false at end of the job section or on error (err set).
    bool readJob(Job &job, std::string &err)
    {
        if (std::ftell(f_) < 0 || static_cast<uint64_t>(std::ftell(f_)) >= jobsEnd_)
            return false;
        job.tick = readU32();
        job.startPC = readU32();
        job.imageIdx = readU32();
        job.top = readU32();
        job.itop = readU32();
        job.cycleStart = readU64();
        if (std::fread(&job.regsIn, 1, sizeof(job.regsIn), f_) != sizeof(job.regsIn))
        {
            err = "truncated regsIn";
            return false;
        }
        if (!readDelta(job.inFull, job.inFullBytes, job.inIdx, job.inVal, true, err))
            return false;
        if (std::fread(&job.regsOut, 1, sizeof(job.regsOut), f_) != sizeof(job.regsOut))
        {
            err = "truncated regsOut";
            return false;
        }
        bool dummyFull = false;
        std::vector<uint8_t> dummyBytes;
        if (!readDelta(dummyFull, dummyBytes, job.outIdx, job.outVal, false, err))
            return false;
        job.cycleEnd = readU64();
        const uint32_t nPk = readU32();
        const uint32_t total = readU32();
        if (nPk > 4096 || total > 4 * 1024 * 1024)
        {
            err = "bad xgkick counts";
            return false;
        }
        job.xgkSize.resize(nPk);
        job.xgkBytes.clear();
        job.xgkBytes.reserve(total);
        for (uint32_t i = 0; i < nPk; ++i)
        {
            const uint32_t n = readU32();
            job.xgkSize[i] = n;
            const size_t at = job.xgkBytes.size();
            job.xgkBytes.resize(at + n);
            if (n && std::fread(job.xgkBytes.data() + at, 1, n, f_) != n)
            {
                err = "truncated xgkick bytes";
                return false;
            }
        }
        if (std::ferror(f_))
        {
            err = "file read error";
            return false;
        }
        return true;
    }

    void rewindJobs() { std::fseek(f_, kHeaderSize, SEEK_SET); }

  private:
    static constexpr long kHeaderSize = 40;
    FILE *f_ = nullptr;
    uint32_t flags_ = 0;
    uint64_t from_ = 0, to_ = 0, jobCount_ = 0, jobsEnd_ = 0;
    std::vector<Image> images_;

    uint32_t readU32()
    {
        uint32_t v = 0;
        std::fread(&v, 1, 4, f_);
        return v;
    }
    uint64_t readU64()
    {
        uint64_t v = 0;
        std::fread(&v, 1, 8, f_);
        return v;
    }
    bool readDelta(bool &full, std::vector<uint8_t> &fullBytes, std::vector<uint32_t> &idx,
                   std::vector<uint32_t> &val, bool allowFull, std::string &err)
    {
        const uint32_t n = readU32();
        full = false;
        if (n == kFullMarker)
        {
            if (!allowFull)
            {
                err = "unexpected full delta";
                return false;
            }
            full = true;
            fullBytes.resize(kDataSize);
            if (std::fread(fullBytes.data(), 1, kDataSize, f_) != kDataSize)
            {
                err = "truncated full delta";
                return false;
            }
            return true;
        }
        if (n > kDataWords)
        {
            err = "bad delta count";
            return false;
        }
        idx.resize(n);
        val.resize(n);
        if (n < kPairsThreshold)
        {
            for (uint32_t k = 0; k < n; ++k)
            {
                uint16_t ii = 0;
                if (std::fread(&ii, 1, 2, f_) != 2 || std::fread(&val[k], 1, 4, f_) != 4)
                {
                    err = "truncated pair delta";
                    return false;
                }
                idx[k] = ii;
            }
            return true;
        }
        uint8_t bits[kDataWords / 8];
        if (std::fread(bits, 1, sizeof(bits), f_) != sizeof(bits))
        {
            err = "truncated bitmap delta";
            return false;
        }
        uint32_t k = 0;
        for (uint32_t i = 0; i < kDataWords && k < n; ++i)
            if ((bits[i >> 3] >> (i & 7u)) & 1u)
                idx[k++] = i;
        if (k != n)
        {
            err = "bitmap popcount mismatch";
            return false;
        }
        for (uint32_t j = 0; j < n; ++j)
            if (std::fread(&val[j], 1, 4, f_) != 4)
            {
                err = "truncated bitmap values";
                return false;
            }
        return true;
    }
};

} // namespace ps2_vu1cap

#endif
