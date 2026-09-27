#pragma once
// IP3: opt-in once-per-second perf log (PS2X_PERF_LOG=1). One line per wall
// second goes to <dir>/perf-<launch-stamp>.log, where <dir> is Documents/perf
// on iOS, <files dir>/perf on Android, ./perf on desktop, or
// $PS2X_PERF_LOG_DIR when set. The directory is ring-capped (5 MB, 5 files).
//
// Line format (v1):
//   [perf] wall=<UTC ISO> t=<s>s tick=<vsync> vsyncs_per_s=<f> presents=<n>
//     maxgap_ms=<f> threads="<name>=<ms> ..." device="<kv ...>"
// threads holds per-window user+system CPU ms deltas (Apple only; "na"
// elsewhere), keyed by thread index like [thread-cpu] (t#N; the runtime does
// not name its threads). device holds iOS UIDevice/ProcessInfo state
// (thermal=<0-3> lpm=<0/1> batt=<0-100> chg=<0/1>); "na" elsewhere.
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
} // namespace ps2x::perflog
