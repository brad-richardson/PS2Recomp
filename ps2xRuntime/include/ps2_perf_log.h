#pragma once
// IP3: opt-in once-per-second perf log (PS2X_PERF_LOG=1). One line per wall
// second goes to <dir>/perf-<launch-stamp>.log, where <dir> is Documents/perf
// on iOS, <files dir>/perf on Android, ./perf on desktop, or
// $PS2X_PERF_LOG_DIR when set. The directory is ring-capped (5 MB, 5 files).
//
// Line format (v1):
//   [perf] wall=<UTC ISO> t=<s>s tick=<vsync> vsyncs_per_s=<f> presents=<n>
//     maxgap_ms=<f> threads="<name>=<ms> ..." device="<kv ...>"
// threads holds per-window user+system CPU ms deltas, keyed by thread index
// like [thread-cpu] on Apple (t#N when unnamed) and by kernel tid on Linux
// (<comm>#<tid>; readdir order is unstable so the tid is the match key).
// "na" where the platform sampler is unavailable. device holds iOS
// UIDevice/ProcessInfo state (thermal=<0-3> lpm=<0/1> batt=<0-100> chg=<0/1>)
// or Android state (thermal=<0-6 AThermal> prime=<C> batt=<0-100> ac=<0/1>,
// each "na" when unreadable); "na" elsewhere. The Android prime zone is the
// best-ranked sysfs thermal zone by primeZoneRank (cpu-1-1-1 first: the Odin
// 3 prime core, same source as odin_run.py's pre-launch check).
//
// This header holds the pure parts (tested by ps2_perf_log_tests.cpp); the
// file I/O and platform snapshots live in src/lib/ps2_perf_log.cpp. poll()
// and dumpTail() are main-thread only; notePresent() is thread-safe (the VK
// path calls it from the GS worker) and all three are no-ops until the first
// poll() with the knob on; with the knob off the call sites compile to one
// bool check each (zero cost).

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ps2x::perflog
{
constexpr uint64_t kMaxDirBytes = 5u * 1024u * 1024u;
constexpr size_t kMaxFiles = 5u;

inline bool enabledFromEnv(const char *value)
{
    return value != nullptr && value[0] == '1' && value[1] == '\0';
}

struct ThreadDelta
{
    std::string name;
    double ms = 0.0;
};

struct Sample
{
    std::string wall; // UTC ISO-8601, e.g. 2026-09-27T16:03:01Z
    double elapsedS = 0.0;
    uint64_t tick = 0;
    double vsyncsPerS = 0.0;
    uint64_t presents = 0;
    double maxGapMs = -1.0; // -1 when fewer than 2 presents in the window
    bool threadsAvailable = false;
    std::vector<ThreadDelta> threads;
    std::string device;    // preformatted kv pairs, or "na"
    std::string kgslBusy;  // "43.2" (pct) or "na" (PT2 Part 2b: in-app kgsl)
    std::string kgslClk;   // raw Hz or "na"
};

inline std::string sanitizeThreadName(const std::string &name)
{
    std::string out = name;
    for (char &c : out)
    {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
                        c == '.' || c == '-' || c == '#';
        if (!ok)
            c = '_';
    }
    return out;
}

inline std::string formatLine(const Sample &s)
{
    std::string line;
    char head[256];
    std::snprintf(head, sizeof(head), "[perf] wall=%s t=%.1fs tick=%llu vsyncs_per_s=%.2f presents=%llu maxgap_ms=%.1f ",
                  s.wall.c_str(), s.elapsedS, static_cast<unsigned long long>(s.tick), s.vsyncsPerS,
                  static_cast<unsigned long long>(s.presents), s.maxGapMs);
    line += head;
    line += "threads=\"";
    if (s.threadsAvailable)
    {
        bool first = true;
        for (const auto &t : s.threads)
        {
            if (!first)
                line += ' ';
            first = false;
            char cell[96];
            std::snprintf(cell, sizeof(cell), "%s=%.1f", sanitizeThreadName(t.name).c_str(), t.ms);
            line += cell;
        }
    }
    else
    {
        line += "na";
    }
    line += "\" device=\"";
    line += s.device.empty() ? "na" : s.device;
    line += "\" gpubusy_pct=";
    line += s.kgslBusy.empty() ? "na" : s.kgslBusy;
    line += " gpuclk=";
    line += s.kgslClk.empty() ? "na" : s.kgslClk;
    return line;
}

// PT2 Part 2b: in-app kgsl GPU sampling (Android only; the .cpp reads
// /sys/class/kgsl/kgsl-3d0/{gpubusy,gpuclk} at 1 Hz on the main thread).
// gpubusy is read-reset in us: each read returns the window since the last
// read by ANYONE, then clears (proven on the Odin: 5 s-spaced adb reads see
// ~1.0 s totals because our 1 Hz reads clear them; idle reads "0 0"). Two
// consecutive reads are therefore INDEPENDENT windows, so pct is ALWAYS the
// instantaneous ratio (a delta divides window noise and spikes to 100x+;
// PT2-grav2). On a hypothetical cumulative kernel this degrades to a bounded
// boot-average, never a spike. Unparseable input and zero totals read na.
inline bool parseKgslBusy(std::string_view text, uint64_t &busy, uint64_t &total)
{
    size_t i = 0;
    const auto skipWs = [&]
    {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r'))
            ++i;
    };
    const auto takeNum = [&](uint64_t &out) -> bool
    {
        skipWs();
        if (i >= text.size() || text[i] < '0' || text[i] > '9')
            return false;
        uint64_t v = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9')
        {
            const uint64_t d = static_cast<uint64_t>(text[i] - '0');
            if (v > (~0ull - d) / 10u)
                return false; // overflow
            v = v * 10u + d;
            ++i;
        }
        out = v;
        return true;
    };
    uint64_t b = 0, t = 0;
    if (!takeNum(b) || !takeNum(t))
        return false;
    skipWs();
    if (i != text.size())
        return false;
    busy = b;
    total = t;
    return true;
}

inline bool parseKgslClk(std::string_view text, uint64_t &hz)
{
    size_t i = 0;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r'))
        ++i;
    if (i >= text.size() || text[i] < '0' || text[i] > '9')
        return false;
    uint64_t v = 0;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9')
    {
        const uint64_t d = static_cast<uint64_t>(text[i] - '0');
        if (v > (~0ull - d) / 10u)
            return false;
        v = v * 10u + d;
        ++i;
    }
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r'))
        ++i;
    if (i != text.size())
        return false;
    hz = v;
    return true;
}

// Pct from one (busy, total) read-reset window. Always the instantaneous
// ratio (see the block comment above); false on a zero total (idle window:
// caller prints na).
inline bool kgslSamplePct(uint64_t busy, uint64_t total, double &pct)
{
    if (total == 0u)
        return false;
    pct = 100.0 * static_cast<double>(busy) / static_cast<double>(total);
    return true;
}

struct FileEntry
{
    std::string name;
    uint64_t size = 0;
};

// PL1: Linux /proc sampler pure parts (the /proc walk itself lives in
// ps2_perf_log.cpp; these parse its inputs so the host suite can test them
// on canned text).
struct ProcTaskCpu
{
    long tid = 0;
    std::string comm; // thread name (comm), sanitized at format time
    uint64_t utime = 0; // field 14, clock ticks
    uint64_t stime = 0; // field 15, clock ticks
};

// Parse one /proc/self/task/<tid>/stat line. comm sits between the first
// '(' and the LAST ')' (names may hold spaces and parens); utime/stime are
// the 12th/13th whitespace fields after the ')'. False on any malformed
// input (out untouched).
inline bool parseProcTaskStat(long tid, std::string_view text, ProcTaskCpu &out)
{
    if (tid <= 0)
        return false;
    const size_t open = text.find('(');
    const size_t close = text.find_last_of(')');
    if (open == std::string_view::npos || close == std::string_view::npos || close <= open + 1)
        return false;
    size_t pos = close + 1;
    auto nextField = [&]() -> std::string_view {
        while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n'))
            ++pos;
        const size_t begin = pos;
        while (pos < text.size() && text[pos] != ' ' && text[pos] != '\t' && text[pos] != '\n')
            ++pos;
        return text.substr(begin, pos - begin);
    };
    auto parseU64 = [](std::string_view field, uint64_t &value) -> bool {
        if (field.empty())
            return false;
        uint64_t v = 0;
        for (char c : field)
        {
            if (c < '0' || c > '9')
                return false;
            v = v * 10u + static_cast<uint64_t>(c - '0');
        }
        value = v;
        return true;
    };
    // Fields after comm start at field 3 (state); utime/stime are 14/15.
    for (int i = 3; i < 14; ++i)
    {
        if (nextField().empty())
            return false;
    }
    uint64_t utime = 0, stime = 0;
    if (!parseU64(nextField(), utime) || !parseU64(nextField(), stime))
        return false;
    out.tid = tid;
    out.comm = std::string(text.substr(open + 1, close - open - 1));
    out.utime = utime;
    out.stime = stime;
    return true;
}

inline double procTicksToMs(uint64_t ticks, long clkTck)
{
    if (clkTck <= 0)
        return 0.0;
    return static_cast<double>(ticks) * 1000.0 / static_cast<double>(clkTck);
}

// Parse a small sysfs integer file ("41800\n" -> 41800). Leading/trailing
// ASCII whitespace tolerated; anything else is malformed (out untouched).
inline bool parseSysfsLong(std::string_view text, long &out)
{
    size_t begin = 0;
    while (begin < text.size() &&
           (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\n' || text[begin] == '\r'))
        ++begin;
    size_t end = text.size();
    while (end > begin &&
           (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\n' || text[end - 1] == '\r'))
        --end;
    if (begin == end)
        return false;
    bool negative = false;
    if (text[begin] == '+' || text[begin] == '-')
    {
        negative = text[begin] == '-';
        ++begin;
    }
    if (begin == end)
        return false;
    long value = 0;
    for (size_t i = begin; i < end; ++i)
    {
        const char c = text[i];
        if (c < '0' || c > '9')
            return false;
        value = value * 10 + (c - '0');
    }
    out = negative ? -value : value;
    return true;
}

// UX1: prime-zone fallback rank over a trimmed sysfs type name. 0 is best
// (cpu-1-1-1, the Odin 3 prime core), higher is worse, -1 is no match. The
// Odin carries 116 /sys/class/thermal entries and cpu-1-1-1 sits at
// readdir position 109, past PL1's 64-entry cap (cooling_device* entries
// included) — the sampler never reached it, so prime read "na" on every
// line. The sampler now scans thermal_zone* only with a 256 cap and takes
// the best rank here; siblings cover a renamed/revved sensor.
inline int primeZoneRank(std::string_view trimmedType)
{
    static constexpr const char *kOrder[] = {
        "cpu-1-1-1", "cpu-1-1-0", "cpu-1-0-1", "cpu-1-0-0", "cpuss-1-0", "cpuss-1-1",
    };
    for (size_t i = 0; i < sizeof(kOrder) / sizeof(kOrder[0]); ++i)
    {
        if (trimmedType == kOrder[i])
            return static_cast<int>(i);
    }
    return -1;
}

struct AndroidDevice
{
    bool hasThermal = false;
    int thermal = 0; // AThermal 0-6 (NONE..SHUTDOWN)
    bool hasPrimeC = false;
    double primeC = 0.0; // best-ranked prime zone (primeZoneRank), degrees C
    bool hasBatt = false;
    int batt = 0; // 0-100 %
    bool hasAc = false;
    int ac = 0; // 0/1
};

// device="..." payload for Android; missing channels read "na" (v1 shape,
// same keys every line).
inline std::string formatAndroidDevice(const AndroidDevice &d)
{
    char thermal[16], prime[32], batt[16], ac[16];
    std::snprintf(thermal, sizeof(thermal), d.hasThermal ? "%d" : "na", d.thermal);
    std::snprintf(prime, sizeof(prime), d.hasPrimeC ? "%.1f" : "na", d.primeC);
    std::snprintf(batt, sizeof(batt), d.hasBatt ? "%d" : "na", d.batt);
    std::snprintf(ac, sizeof(ac), d.hasAc ? "%d" : "na", d.ac);
    char buf[96];
    std::snprintf(buf, sizeof(buf), "thermal=%s prime=%s batt=%s ac=%s", thermal, prime, batt, ac);
    return buf;
}

// PL2: AThermal call-shape seam. The NDK getter takes a manager
// (AThermal_acquireManager / _getCurrentThermalStatus(manager) /
// _releaseManager); PL1 called the getter with no argument and SIGABRTed on
// device. The .cpp resolves all three via dlsym (API 30+, minSdk 29); these
// types mirror <android/thermal.h> without including it.
struct ThermalFns
{
    void *(*acquireManager)() = nullptr;
    int32_t (*getStatus)(void *) = nullptr;
    void (*releaseManager)(void *) = nullptr;
};

// Status through an acquired manager: -1 when any fn or the manager is
// missing, or the status falls outside 0..6 (NONE..SHUTDOWN). Never calls
// through a null pointer. Pure: the host suite covers every null combination
// plus the clamp.
inline int32_t thermalStatusWith(const ThermalFns &fns, void *manager)
{
    if (!fns.acquireManager || !fns.getStatus || !fns.releaseManager || !manager)
        return -1;
    const int32_t status = fns.getStatus(manager);
    return (status >= 0 && status <= 6) ? status : -1;
}

// Ring-cap plan over the perf-*.log files in the log dir (the current file
// included: it sorts newest, so it is never picked). Returns the names to
// delete, oldest first. Names sort chronologically (perf-YYYYMMDD-HHMMSS.log,
// optional -N collision suffix).
inline std::vector<std::string> planPrune(std::vector<FileEntry> entries, uint64_t maxBytes = kMaxDirBytes,
                                          size_t maxFiles = kMaxFiles)
{
    std::sort(entries.begin(), entries.end(),
              [](const FileEntry &a, const FileEntry &b) { return a.name < b.name; });
    std::vector<std::string> dead;
    uint64_t bytes = 0;
    for (const auto &e : entries)
        bytes += e.size;
    size_t i = 0;
    while (i < entries.size() && (entries.size() - i > maxFiles || bytes > maxBytes))
    {
        dead.push_back(entries[i].name);
        bytes -= entries[i].size;
        ++i;
    }
    return dead;
}

// PT2: per-stage per-tick tail rings. Each stage's owning thread pushes one
// (tick, ms) entry per guest vsync tick; the main thread drains the rings in
// poll() into per-second [perf-stage] lines, and dumpTail() writes the full
// rings as [perf-tail] lines on graceful shutdown (a force-stop SIGKILLs, so
// the Odin summary reads the flushed per-second lines, never the dump).
//
//   [perf-stage] tick0=<u> tick1=<u> stage=<name> n=<u> mean=<f> p50=<f>
//     p95=<f> p99=<f> max=<f> hist="<lo>:<count> ..."
//   [perf-tail] tick=<u> stage=<name> ms=<f>
//
// IOSR1: one [perf-mtvu] line per window (main thread, after [perf-lock],
// before the [perf-stage] lines; only when MTVU is threaded):
//   [perf-mtvu] tick=<u> <reason>=<n>/<ms> ... other=<n>/<ms>
//     finishee_skip=<n> vif1statfree_skip=<n>
// <reason> covers every ps2_mtvu::Reason in enum order (vblank first);
// <n> is the window's sync count for that reason, <ms> its waited ms.
// other = unattributed waits (Reason::Count callers); reasons + other sum to
// the window's ee.mtvu. finishee_skip = MQ2 unit PATH3 FINISH writes covered
// by an EE credit; vif1statfree_skip = MQ3 VIF1_STAT writes that skipped the
// Vif1Reg sync.
//
// Stats are nearest-rank over the drained entries (rank ceil(q*n)-1); hist
// buckets are 0.25 ms wide over 0..20 ms plus an "ovf" overflow bucket, so
// odin_run.py can merge whole windows into exact-ish tails. A line with n=0
// prints -1 stats and an empty hist (the stage never fired: e.g. gpu on a
// non-GE1 backend, or mtvu with MTVU off).
enum class Stage : uint8_t
{
    EeBusy = 0, // GameThread guest-work wall per tick (frame wall minus waits)
    EeCpu,      // GameThread thread-CPU per tick (spins included, sleeps not)
    EeWait,     // GameThread measured waits per tick (pace + event + enqueue + mtvu sync)
    GsBusy,     // GS worker handler time between GuestVsyncs (idle excluded)
    MtvuBusy,   // MTVU unit-job time between vblanks
    GpuBusy,    // ge1_gs_gpu_ms() per GuestVsync (Vulkan timestamps; GE1 only)
    GsBackBusy, // ge1_gs_back_ms() per GuestVsync (back thread; GE1 pipelined only)
    MtvuGifBusy, // VPL1: MTVU-GIF thread busy between vblanks (PS2X_MTVU_GIF_STAGE=1 only)
    MtvuVifBusy, // VPL2: MTVU-VIF thread job time between vblanks (PS2X_MTVU_VIF_STAGE=1 only)
    // IP7: ee.wait split by source (each per tick; ee.wait minus their sum = pause gate).
    EePace,   // pacer sleep (FP1 or the PX1/IP6 vsync lock)
    EeEvent,  // waitForEvent waits (guest idle until an event/host deadline)
    EeEnq,    // GS enqueue waits (queue full)
    EeMtvu,   // MTVU sync waits, every reason
    EeMtvuVb, // the VBlank-reason part of ee.mtvu (MTVU_LAG=0: the whole frame's VU1 jobs)
    Count
};

inline const char *stageName(Stage s)
{
    switch (s)
    {
    case Stage::EeBusy:
        return "ee.busy";
    case Stage::EeCpu:
        return "ee.cpu";
    case Stage::EeWait:
        return "ee.wait";
    case Stage::GsBusy:
        return "gs.busy";
    case Stage::MtvuBusy:
        return "mtvu.busy";
    case Stage::GpuBusy:
        return "gpu.busy";
    case Stage::GsBackBusy:
        return "gsback.busy";
    case Stage::MtvuGifBusy:
        return "mtvugif.busy";
    case Stage::MtvuVifBusy:
        return "mtvuvif.busy";
    case Stage::EePace:
        return "ee.pace";
    case Stage::EeEvent:
        return "ee.event";
    case Stage::EeEnq:
        return "ee.enq";
    case Stage::EeMtvu:
        return "ee.mtvu";
    case Stage::EeMtvuVb:
        return "ee.mtvuvb";
    default:
        return "?";
    }
}

constexpr size_t kStageCount = static_cast<size_t>(Stage::Count);
constexpr double kHistBucketMs = 0.25;
constexpr size_t kHistBuckets = 80; // edges 0..20 ms; bucket [80] is overflow

struct StageEntry
{
    uint64_t tick = 0;
    float ms = 0.0f;
};

// Fixed-size ring, one writer thread + the main-thread reader, no locks. A
// slot packs (tick32 << 32) | usec32 in one word: the writer stores the slot
// then release-bumps the head, the reader acquire-loads the head. One writer
// per ring per run by construction (EE: ee.*, MTVU vblank: mtvu.*, GS worker:
// gs.* + gpu.* + gsback.*); a reader that fell more than a lap behind clamps
// to the newest lap (see poll()).
class StageRing
{
public:
    static constexpr size_t kCap = 16384; // ~2.3 min at 120 ticks/s; a full leg fits
    static constexpr size_t kMask = kCap - 1;
    static_assert((kCap & kMask) == 0, "power of two");

    void push(uint32_t tick, float ms)
    {
        uint32_t us = 0;
        if (ms > 0.0f)
        {
            const double scaled = static_cast<double>(ms) * 1000.0 + 0.5;
            us = scaled >= 4294967295.0 ? 0xFFFFFFFFu : static_cast<uint32_t>(scaled);
        }
        const uint64_t slot = (static_cast<uint64_t>(tick) << 32) | us;
        const uint64_t i = m_head.load(std::memory_order_relaxed);
        m_slots[i & kMask] = slot;
        // Publish: the release pairs with the reader's acquire-load, so every
        // slot below an observed head is fully written.
        m_head.store(i + 1u, std::memory_order_release);
    }

    uint64_t head() const { return m_head.load(std::memory_order_acquire); }
    uint64_t slotAt(uint64_t i) const { return m_slots[i & kMask]; }
    static StageEntry decode(uint64_t slot)
    {
        StageEntry e;
        e.tick = slot >> 32;
        e.ms = static_cast<float>(slot & 0xFFFFFFFFu) / 1000.0f;
        return e;
    }

private:
    uint64_t m_slots[kCap] = {};
    std::atomic<uint64_t> m_head{0};
};

// PT2 Part 2a: clamp a drain cursor to the newest lap (shared by poll's
// per-second drain and the kill-proof tail flush): the first index to read.
inline uint64_t clampDrainStart(uint64_t consumed, uint64_t head, uint64_t cap)
{
    return (head - consumed > cap) ? head - cap : consumed;
}

struct StageStats
{
    uint64_t n = 0;
    uint64_t tick0 = 0;
    uint64_t tick1 = 0;
    double mean = -1.0;
    double p50 = -1.0;
    double p95 = -1.0;
    double p99 = -1.0;
    double max = -1.0;
    std::array<uint64_t, kHistBuckets + 1> hist{};
};

inline size_t histBucket(double ms)
{
    if (!(ms >= 0.0))
        return 0;
    const size_t b = static_cast<size_t>(ms / kHistBucketMs);
    return b > kHistBuckets ? kHistBuckets : b;
}

// Nearest-rank stats over a copy of the entries (the input order is kept).
inline StageStats summarizeStage(const std::vector<StageEntry> &entries)
{
    StageStats st;
    st.n = entries.size();
    if (entries.empty())
        return st;
    st.tick0 = entries[0].tick;
    st.tick1 = entries[0].tick;
    double sum = 0.0;
    std::vector<float> ms;
    ms.reserve(entries.size());
    for (const auto &e : entries)
    {
        if (e.tick < st.tick0)
            st.tick0 = e.tick;
        if (e.tick > st.tick1)
            st.tick1 = e.tick;
        sum += e.ms;
        ms.push_back(e.ms);
        ++st.hist[histBucket(e.ms)];
    }
    st.mean = sum / static_cast<double>(entries.size());
    std::sort(ms.begin(), ms.end());
    const auto rank = [&](double q) -> double {
        const double exact = std::ceil(q * static_cast<double>(ms.size()));
        size_t i = exact < 1.0 ? 0 : static_cast<size_t>(exact) - 1; // ceil(q*n)-1
        if (i >= ms.size())
            i = ms.size() - 1;
        return ms[i];
    };
    st.p50 = rank(0.50);
    st.p95 = rank(0.95);
    st.p99 = rank(0.99);
    st.max = ms.back();
    return st;
}

inline std::string formatStageLine(const char *name, const StageStats &st)
{
    char head[256];
    std::snprintf(head, sizeof(head), "[perf-stage] tick0=%llu tick1=%llu stage=%s n=%llu mean=%.3f "
                                      "p50=%.3f p95=%.3f p99=%.3f max=%.3f hist=\"",
                  static_cast<unsigned long long>(st.tick0), static_cast<unsigned long long>(st.tick1), name,
                  static_cast<unsigned long long>(st.n), st.mean, st.p50, st.p95, st.p99, st.max);
    std::string line = head;
    bool first = true;
    for (size_t b = 0; b <= kHistBuckets; ++b)
    {
        if (st.hist[b] == 0u)
            continue;
        if (!first)
            line += ' ';
        first = false;
        char cell[48];
        if (b < kHistBuckets)
            std::snprintf(cell, sizeof(cell), "%.2f:%llu", b * kHistBucketMs,
                          static_cast<unsigned long long>(st.hist[b]));
        else
            std::snprintf(cell, sizeof(cell), "ovf:%llu", static_cast<unsigned long long>(st.hist[b]));
        line += cell;
    }
    line += '"';
    return line;
}

// IOSR1: one [perf-mtvu] cell per ps2_mtvu::Reason (name from
// ps2_mtvu::reasonName, in enum order), plus the unattributed "other" bucket
// and the MQ2/MQ3 skip counts. Pure (host suite covers the exact text).
struct MtvuReasonCell
{
    const char *name = "?";
    uint64_t n = 0;
    uint64_t ns = 0;
};

inline std::string formatMtvuLine(uint64_t tick, const MtvuReasonCell *cells, size_t count,
                                  uint64_t otherN, uint64_t otherNs, uint64_t finisheeSkips,
                                  uint64_t vif1statfreeSkips)
{
    char head[64];
    std::snprintf(head, sizeof(head), "[perf-mtvu] tick=%llu",
                  static_cast<unsigned long long>(tick));
    std::string line = head;
    char cell[96];
    for (size_t i = 0; i < count; ++i)
    {
        std::snprintf(cell, sizeof(cell), " %s=%llu/%.3f", cells[i].name,
                      static_cast<unsigned long long>(cells[i].n), cells[i].ns / 1e6);
        line += cell;
    }
    std::snprintf(cell, sizeof(cell), " other=%llu/%.3f",
                  static_cast<unsigned long long>(otherN), otherNs / 1e6);
    line += cell;
    std::snprintf(cell, sizeof(cell), " finishee_skip=%llu vif1statfree_skip=%llu",
                  static_cast<unsigned long long>(finisheeSkips),
                  static_cast<unsigned long long>(vif1statfreeSkips));
    line += cell;
    return line;
}

// Monotonic wall ns for the stage call sites (two stamps per unit of work).
inline uint64_t steadyNs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

// Runtime API (src/lib/ps2_perf_log.cpp). enabled() reads the knob; call it
// once and cache the result at the call site.
bool enabled();
void poll(uint64_t vsyncTick);
void notePresent();
// FH6 (PS2X_PERF_PRESENT_DETAIL=1): present-path counts per window, printed as
// one [perf-present] line after each [perf] line. noteGsVsync: guest VSyncs
// the GS backend processed (worker); noteLatch: one host-loop latch RPC and
// its duration. Lock-free; no-ops unless the log and the knob are on.
void noteGsVsync();
void noteLatch(uint64_t ns);
// Full-ring [perf-tail] dump (graceful shutdown only; no-op unless active).
void dumpTail();
// PT2 Part 2a: kill-proof ring flush. Drains entries accumulated since the
// last flush into a timestamped tail file (reason "timer": appends the
// current tail-*.log, rotating past the size cap; reason "pause": a fresh
// tail-pause-*.log), with a [perf-tail-flush] marker first. A swipe-kill
// loses at most the unflushed remainder. No-op unless active. Main thread
// only (poll's 60 s cadence + the BG1 pause path); lock-free ring reads,
// so the GameThread never stalls on it.
void flushTail(const char *reason);
// Calling thread's CPU ns, or kCpuUnsupported where no cheap query exists.
constexpr uint64_t kCpuUnsupported = ~0ull;
uint64_t threadCpuNs();
// The ring for a stage (function-static singletons in the .cpp).
StageRing &stageRing(Stage s);
#if defined(__linux__) || defined(__APPLE__)
// PL2 test seam: the cached dlsym sampler behind thermalStatusWith
// (main-thread only, like poll()). Desktop libandroid.so is absent, so this
// reads -1 there; the host suite asserts that.
int32_t perfThermalStatusForTest();
#endif
} // namespace ps2x::perflog
