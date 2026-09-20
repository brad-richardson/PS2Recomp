#ifndef PS2_LOG_H
#define PS2_LOG_H

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#ifndef PS2_RUNTIME_LOGS
#define PS2_RUNTIME_LOGS 0
#endif

#ifndef AGRESSIVE_LOGS
#define AGRESSIVE_LOGS 0
#endif

#define RUNTIME_ERROR(x)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        std::ostringstream _ps2_runtime_error_stream;                                                                  \
        _ps2_runtime_error_stream << x;                                                                                \
        const std::string _ps2_runtime_error_text =                                                                    \
            _ps2_runtime_error_stream.str();                                                                           \
                                                                                                                       \
        std::cerr << _ps2_runtime_error_text;                                                                           \
        ps2_log::append_runtime_log_text(_ps2_runtime_error_text);                                                      \
    } while (0)

namespace ps2_log
{
struct RuntimeLogEntry
{
    uint64_t seq = 0;
    std::string text;
};

inline constexpr size_t kMaxRuntimeLogEntries = 4096;

inline std::mutex &runtime_log_mutex()
{
    static std::mutex m;
    return m;
}

inline std::deque<RuntimeLogEntry> &runtime_log_entries()
{
    static std::deque<RuntimeLogEntry> entries;
    return entries;
}

inline uint64_t &runtime_log_next_seq()
{
    static uint64_t seq = 1;
    return seq;
}

inline bool &runtime_log_paused()
{
    static bool paused = false;
    return paused;
}

inline void set_runtime_log_paused(bool paused)
{
    std::lock_guard<std::mutex> lock(runtime_log_mutex());
    runtime_log_paused() = paused;
}

inline bool is_runtime_log_paused()
{
    std::lock_guard<std::mutex> lock(runtime_log_mutex());
    return runtime_log_paused();
}

inline void append_runtime_log_text(const std::string &text)
{
    if (text.empty())
    {
        return;
    }

    std::lock_guard<std::mutex> lock(runtime_log_mutex());
    if (runtime_log_paused())
    {
        return;
    }

    auto &entries = runtime_log_entries();
    RuntimeLogEntry entry{};
    entry.seq = runtime_log_next_seq()++;
    entry.text = text;
    entries.push_back(std::move(entry));

    while (entries.size() > kMaxRuntimeLogEntries)
    {
        entries.pop_front();
    }
}

inline std::vector<RuntimeLogEntry> snapshot_runtime_log_entries()
{
    std::lock_guard<std::mutex> lock(runtime_log_mutex());
    const auto &entries = runtime_log_entries();
    return std::vector<RuntimeLogEntry>(entries.begin(), entries.end());
}

inline void clear_runtime_log_entries()
{
    std::lock_guard<std::mutex> lock(runtime_log_mutex());
    runtime_log_entries().clear();
}

// P1w no-silent-drops census. Every rejected or unhandled path emits one
// "[drop] <site> <reason> <args>" line on stderr, ON by default in every
// build including the runner; a non-empty PS2X_DROP_SILENCE mutes. The env
// is read fresh per call: drops are exceptional so there is no hot-path
// cost, and the kill-switch stays testable and honors late-set env.
// Deliberately cerr-only (no runtime-log ring append): the census channel
// is the log, and a drop flood must not evict ring entries.
inline bool dropsMuted()
{
    const char *env = std::getenv("PS2X_DROP_SILENCE");
    return env != nullptr && env[0] != '\0';
}

inline std::string formatDropLine(const std::string &site, const std::string &reason, const std::string &args)
{
    std::ostringstream out;
    out << "[drop] " << site << ' ' << reason << ' ' << (args.empty() ? "-" : args);
    return out.str();
}

// T1 in-memory census of PRINTED [drop] lines, keyed by site+reason (the
// P24-2a census shape: args vary per call and stay in the log only). The
// bump sits after the mute check so the census equals the visible lines.
// Drops are exceptional: one map increment, no hot-path cost. Single
// writer (EE executor / analyzer pass); no lock, like the P1c counters.
struct DropCensusRow
{
    std::string site;
    std::string reason;
    uint64_t count = 0;
};

inline std::map<std::pair<std::string, std::string>, uint64_t> &dropCensusCounts()
{
    static std::map<std::pair<std::string, std::string>, uint64_t> counts;
    return counts;
}

inline void recordDropCensus(const std::string &site, const std::string &reason)
{
    ++dropCensusCounts()[{site, reason}];
}

inline std::vector<DropCensusRow> snapshotDropCensus()
{
    std::vector<DropCensusRow> rows;
    for (const auto &[key, count] : dropCensusCounts())
    {
        rows.push_back(DropCensusRow{key.first, key.second, count});
    }
    return rows;
}

inline void resetDropCensusForTesting()
{
    dropCensusCounts().clear();
}

inline void emitDropTo(std::ostream &out,
                       const std::string &site,
                       const std::string &reason,
                       const std::string &args = "")
{
    if (dropsMuted())
    {
        return;
    }
    out << formatDropLine(site, reason, args) << std::endl;
    recordDropCensus(site, reason);
}

inline void emitDrop(const std::string &site, const std::string &reason, const std::string &args = "")
{
    emitDropTo(std::cerr, site, reason, args);
}
}

#if PS2_RUNTIME_LOGS || AGRESSIVE_LOGS
#define RUNTIME_LOG(x)                                                                                                  \
    do                                                                                                                  \
    {                                                                                                                   \
        std::ostringstream _ps2_runtime_log_stream;                                                                     \
        _ps2_runtime_log_stream << x;                                                                                   \
        const std::string _ps2_runtime_log_text = _ps2_runtime_log_stream.str();                                        \
        if (_ps2_runtime_log_text.empty())                                                                            \
        {                                                                                                               \
            std::cout.flush();                                                                                          \
        }                                                                                                               \
        else                                                                                                            \
        {                                                                                                               \
            std::cout << _ps2_runtime_log_text;                                                                         \
            ps2_log::append_runtime_log_text(_ps2_runtime_log_text);                                                    \
        }                                                                                                               \
    } while (0)
#else
#define RUNTIME_LOG(x) do {} while(0)
#endif

#if AGRESSIVE_LOGS

namespace ps2_log
{
inline std::string log_path()
{
    static std::string path;
    if (path.empty())
    {
        path = (std::filesystem::current_path() / "ps2_log.txt").string();
    }
    return path;
}
inline std::ostream &log_stream()
{
    static std::ofstream f(log_path(), std::ios::out);
    return f.is_open() ? f : std::cerr;
}
inline int &depth()
{
    static thread_local int d = 0;
    return d;
}
inline void log_entry(const char *name)
{
    for (int i = 0; i < depth(); ++i)
        log_stream() << '\t';
    log_stream() << ">> " << name << " enter\n";
    log_stream().flush();
    depth()++;
}
inline void log_exit(const char *name)
{
    depth()--;
    for (int i = 0; i < depth(); ++i)
        log_stream() << '\t';
    log_stream() << "<< " << name << " exit\n";
    log_stream().flush();
}
inline void print_saved_location()
{
    std::cout << "[PS2 LOG] Logs saved at " << log_path() << std::endl;
}
}

#define PS_LOG_ENTRY(name) \
    ps2_log::log_entry(name); \
    struct _ps2_log_guard_ { const char *_n; _ps2_log_guard_(const char *n) : _n(n) {} \
        ~_ps2_log_guard_() { ps2_log::log_exit(_n); } } _ps2_log_guard_(name)
#define PS2_IF_AGRESSIVE_LOGS(code) \
    do                              \
    {                               \
        code;                       \
    } while (0)

#else

namespace ps2_log
{
inline std::string log_path()
{
    return (std::filesystem::current_path() / "ps2_log.txt").string();
}
inline void print_saved_location() {}
}
#define PS_LOG_ENTRY(name) ((void)0)
#define PS2_IF_AGRESSIVE_LOGS(code) ((void)0)

#endif

#endif
