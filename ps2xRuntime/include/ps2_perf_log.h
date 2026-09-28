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
// and notePresent() are main-thread only and no-ops until the first poll()
// with the knob on; with the knob off the call sites compile to one bool
// check each (zero cost).

#include <algorithm>
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
    std::string device; // preformatted kv pairs, or "na"
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
    line += '"';
    return line;
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

// Runtime API (src/lib/ps2_perf_log.cpp). enabled() reads the knob; call it
// once and cache the result at the call site.
bool enabled();
void poll(uint64_t vsyncTick);
void notePresent();
#if defined(__linux__) || defined(__APPLE__)
// PL2 test seam: the cached dlsym sampler behind thermalStatusWith
// (main-thread only, like poll()). Desktop libandroid.so is absent, so this
// reads -1 there; the host suite asserts that.
int32_t perfThermalStatusForTest();
#endif
} // namespace ps2x::perflog
