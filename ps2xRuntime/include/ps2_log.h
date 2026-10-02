#ifndef PS2_LOG_H
#define PS2_LOG_H

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <streambuf>
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

// UX1: the one line-buffered diagnostic writer. Multi-fragment lines
// (std::cerr << a << b << ... chains from several threads) interleave
// mid-line on the way to logcat: 59 % of [rb2] lines spliced on the Odin
// (RB2 Part 2). emitLine takes one complete line (no trailing newline),
// and emits it under a process-wide mutex as a single fwrite + fflush:
// exactly one write() per line on unbuffered stderr (Android), one
// buffer-append + flush on buffered stderr (hosts; complete lines stay
// whole in order). Same stdio channel as every other stderr writer, so
// log order is preserved (a raw write(2) would jump ahead of buffered
// content). Diagnostic-gated call sites only: one small allocation per
// emitted line, zero cost when the knob is off.
inline std::mutex &diagLineMutex()
{
    static std::mutex m;
    return m;
}

// LG1: every line leaves through writeLineAtomic: one fwrite + fflush of a
// complete, newline-terminated buffer under diagLineMutex. emitLine (complete
// lines), the cerr/cout line-atomic buffers below and the raylib TraceLog
// callback (main.cpp) all funnel here, so concurrent writers can no longer
// splice into each other mid-line. Content is unchanged.
inline void writeLineAtomic(std::FILE *dest, const char *data, size_t size)
{
    if (size == 0)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(diagLineMutex());
    std::fwrite(data, 1, size, dest);
    std::fflush(dest);
}

inline void emitLine(const std::string &line)
{
    const std::string out = line + '\n';
    writeLineAtomic(stderr, out.data(), out.size());
}

// LG1: std::streambuf that collects each thread's fragments (the
// `std::cerr << a << b << std::endl` chains; cerr is unitbuf, so every `<<`
// used to be its own write()) and hands complete lines to writeLineAtomic.
// A partial line stays in the writing thread's buffer until its newline
// arrives (or the thread exits / the atexit flush runs), capped at 64 KiB so
// a newline-less writer cannot grow it forever.
class LineAtomicBuf : public std::streambuf
{
public:
    explicit LineAtomicBuf(std::FILE *dest) : m_dest(dest) {}

    // Calling thread's partial line (exit path).
    void flushPartial() { flushThread(true); }

protected:
    int_type overflow(int_type ch) override
    {
        if (traits_type::eq_int_type(ch, traits_type::eof()))
        {
            return traits_type::not_eof(ch);
        }
        const char c = traits_type::to_char_type(ch);
        append(&c, 1);
        return ch;
    }

    std::streamsize xsputn(const char *s, std::streamsize n) override
    {
        append(s, static_cast<size_t>(n));
        return n;
    }

    int sync() override { return 0; } // unitbuf must not cut partial lines

private:
    struct Pending
    {
        std::string text;
        LineAtomicBuf *owner = nullptr;
        ~Pending()
        {
            if (owner && !text.empty())
            {
                writeLineAtomic(owner->m_dest, text.data(), text.size());
            }
        }
    };

    Pending &pending()
    {
        // One Pending per (thread, buffer): cerr and cout each own one.
        thread_local std::map<const LineAtomicBuf *, Pending> tl;
        Pending &p = tl[this];
        p.owner = this;
        return p;
    }

    void append(const char *s, size_t n)
    {
        pending().text.append(s, n);
        flushThread(false);
    }

    void flushThread(bool all)
    {
        Pending &p = pending();
        if (p.text.empty())
        {
            return;
        }
        size_t cut = p.text.rfind('\n');
        if (all || p.text.size() > 65536)
        {
            cut = p.text.size() - 1;
        }
        if (cut == std::string::npos)
        {
            return;
        }
        writeLineAtomic(m_dest, p.text.data(), cut + 1);
        p.text.erase(0, cut + 1);
    }

    std::FILE *m_dest;
};

inline LineAtomicBuf &lineAtomicErrBuf()
{
    static LineAtomicBuf b(stderr);
    return b;
}

inline LineAtomicBuf &lineAtomicOutBuf()
{
    static LineAtomicBuf b(stdout);
    return b;
}

// LG1: route std::cerr / std::clog / std::cout through LineAtomicBuf and make
// stdout line-buffered so stdio writers (printf) also flush whole lines.
// Called once at the top of main(). Idempotent.
inline void installLineAtomicLogging()
{
    static bool installed = false;
    if (installed)
    {
        return;
    }
    installed = true;
    std::cerr.rdbuf(&lineAtomicErrBuf());
    std::clog.rdbuf(&lineAtomicErrBuf());
    std::cout.rdbuf(&lineAtomicOutBuf());
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::atexit([]() {
        lineAtomicErrBuf().flushPartial();
        lineAtomicOutBuf().flushPartial();
    });
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
    if (dropsMuted())
    {
        return;
    }
    emitLine(formatDropLine(site, reason, args));
    recordDropCensus(site, reason);
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
