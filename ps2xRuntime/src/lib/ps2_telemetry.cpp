// TEL1: play telemetry I/O (formats and pure logic in ps2_telemetry.h).
#include "ps2_telemetry.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>

#if !defined(_WIN32)
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#define TEL1_HAS_PIPE 1
#endif

namespace ps2x::telemetry
{
bool g_vuOn = false;

namespace
{
using Clock = std::chrono::steady_clock;

Clock::time_point origin()
{
    static const Clock::time_point t0 = Clock::now();
    return t0;
}

uint64_t nowNs()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - origin()).count());
}

const std::string &stamp()
{
    static const std::string s = []
    {
        const std::time_t now = std::time(nullptr);
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &now);
#else
        localtime_r(&now, &tm);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
        return std::string(buf);
    }();
    return s;
}

std::string utcNow()
{
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

// Size-capped append-only writer; every failure leaves it inert.
struct Sink
{
    std::FILE *f = nullptr;
    uint64_t bytes = 0;
    bool capped = false;

    bool open(const char *knob, const char *prefix)
    {
        const char *dir = std::getenv(knob);
        if (!dir || !*dir)
            return false;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const std::string path = sessionPath(dir, prefix, "tsv");
        f = std::fopen(path.c_str(), "w");
        if (!f)
        {
            std::fprintf(stderr, "[tel1] %s: cannot open %s (%s); telemetry off\n", knob, path.c_str(),
                         ec ? ec.message().c_str() : "fopen failed");
            return false;
        }
        std::fprintf(stderr, "[tel1] %s -> %s\n", knob, path.c_str());
        return true;
    }
    void write(const std::string &text)
    {
        if (!f || capped || text.empty())
            return;
        if (bytes + text.size() > kMaxFileBytes)
        {
            std::fputs("# TEL1 file cap reached; telemetry stopped\n", f);
            std::fflush(f);
            capped = true;
            return;
        }
        bytes += std::fwrite(text.data(), 1, text.size(), f);
        std::fflush(f);
    }
};

// ---- VU1 -------------------------------------------------------------------
Sink s_vu;
Vu1Census s_census;

// ---- GS --------------------------------------------------------------------
#if defined(TEL1_HAS_PIPE)
int s_gsWriteFd = -1;
#endif

void setThreadName(const char *name)
{
#if defined(__APPLE__)
    pthread_setname_np(name);
#elif defined(TEL1_HAS_PIPE)
    pthread_setname_np(pthread_self(), name);
#else
    (void)name;
#endif
}
} // namespace

std::string sessionPath(const std::string &dir, const char *prefix, const char *ext)
{
    std::string path = dir;
    if (!path.empty() && path.back() != '/')
        path += '/';
    path += prefix;
    path += '-';
    path += stamp();
    path += '.';
    path += ext;
    return path;
}

void vuInit()
{
    if (g_vuOn || s_vu.f)
        return; // resetForLoad re-runs configure(): keep the session file
    (void)origin();
    if (!s_vu.open("PS2X_VU_TELEMETRY", "vu1"))
        return;
    s_vu.write("# TEL1 vu1 v1 start=" + utcNow() +
               " window_ms=10000 (format: ps2xRuntime/include/ps2_telemetry.h)\n"
               "#N\tt_ms\ttick\timg\thash\tpc\tfirst_us\truns\tmean_us\tmax_us\n"
               "#W\tt_ms\ttick\truns\tresumes\trun_us\tmax_us\tuploads\tnew_img\tnew_ent\tnew_us\timgs\tents\n");
    s_census.startWindow(nowNs() / 1000000);
    g_vuOn = true;
}

uint32_t vuBegin(const uint8_t *code, size_t codeSize, uint64_t generation, uint32_t startPc, bool resume,
                 uint64_t tick, uint64_t &t0Ns)
{
    const uint64_t t = nowNs();
    const uint32_t entry = s_census.begin(generation, code, codeSize, startPc, resume, tick, t / 1000000);
    // Start the clock after the (rare) upload hash so run time excludes it.
    t0Ns = nowNs();
    return entry;
}

void vuEnd(uint32_t entry, uint64_t t0Ns, uint64_t tick)
{
    const uint64_t t1 = nowNs();
    s_census.end(entry, (t1 - t0Ns) / 1000);
    const uint64_t tMs = t1 / 1000000;
    if (s_census.windowDue(tMs))
    {
        std::string out;
        s_census.takeWindow(tMs, tick, out);
        s_vu.write(out);
    }
}

void vuFlush(uint64_t tick)
{
    if (!g_vuOn)
        return;
    std::string out;
    s_census.takeWindow(nowNs() / 1000000, tick, out);
    s_vu.write(out);
}

void gsBeforeOpen()
{
    if (const char *dir = std::getenv("PS2X_GS_TFX_RECORD"); dir && *dir)
    {
        if (const char *cur = std::getenv("GE1_TFX_PREWARM"); cur && *cur)
        {
            std::fprintf(stderr, "[tel1] PS2X_GS_TFX_RECORD: GE1_TFX_PREWARM already %s (it records there)\n", cur);
        }
        else
        {
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            const std::string path = sessionPath(dir, "tfx", "bin");
            setenv("GE1_TFX_PREWARM", path.c_str(), 1);
            std::fprintf(stderr, "[tel1] PS2X_GS_TFX_RECORD -> GE1_TFX_PREWARM=%s (record-only)\n", path.c_str());
        }
    }
#if defined(TEL1_HAS_PIPE)
    const char *dir = std::getenv("PS2X_GS_TELEMETRY");
    if (!dir || !*dir || s_gsWriteFd >= 0)
        return;
    if (const char *csv = std::getenv("GE1_PIPE_STATS_CSV"); csv && *csv)
    {
        std::fprintf(stderr, "[tel1] PS2X_GS_TELEMETRY ignored: GE1_PIPE_STATS_CSV=%s is set\n", csv);
        return;
    }
    (void)origin();
    auto *sink = new Sink();
    if (!sink->open("PS2X_GS_TELEMETRY", "gs"))
    {
        delete sink;
        return;
    }
    int fds[2];
    if (pipe(fds) != 0)
    {
        std::fprintf(stderr, "[tel1] PS2X_GS_TELEMETRY: pipe failed; telemetry off\n");
        std::fclose(sink->f);
        delete sink;
        return;
    }
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
#if defined(F_SETPIPE_SZ)
    // ~20k rows of headroom so a slow storage write never backs up the GS thread.
    fcntl(fds[1], F_SETPIPE_SZ, 1 << 20);
#endif
#if defined(__APPLE__)
    const std::string fdPath = "/dev/fd/" + std::to_string(fds[1]);
#else
    const std::string fdPath = "/proc/self/fd/" + std::to_string(fds[1]);
#endif
    setenv("GE1_PIPE_STATS_CSV", fdPath.c_str(), 1);
    s_gsWriteFd = fds[1];
    sink->write("# TEL1 gs v1 start=" + utcNow() +
                " window_ms=10000 source=GE1_PIPE_STATS_CSV (format: ps2xRuntime/include/ps2_telemetry.h)\n"
                "#C\tt_ms\tvsync\tnew_tfx\ttfx_us\tnew_spv\tspv_us\tflush_us\ttfx_slow\ttfx_max_us\twall_us\n"
                "#W\tt_ms\tvsync_first\tvsync_last\tvsyncs\tnew_tfx\ttfx_us\tnew_spv\tspv_us\tflush_us\ttfx_slow"
                "\ttfx_max_us\tup_kb\tuploads\ttex_new\ttex_new_us\twall_max_us\twall_gt20ms\twall_gt50ms\tc_dropped\n");
    const int readFd = fds[0];
    // Drain thread: reads until every write end closes (ge1_gs_close), then
    // writes the last window. Detached; it owns the sink and the read end.
    std::thread([readFd, sink]
                {
        setThreadName("TelGs");
        GsCsvSummary sum;
        sum.startWindow(nowNs() / 1000000);
        std::string pending, out;
        char buf[8192];
        uint64_t rows = 0;
        for (;;)
        {
            const ssize_t n = read(readFd, buf, sizeof(buf));
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                break;
            pending.append(buf, static_cast<size_t>(n));
            const uint64_t tMs = nowNs() / 1000000;
            size_t start = 0;
            for (size_t nl; (nl = pending.find('\n', start)) != std::string::npos; start = nl + 1)
            {
                sum.feed(std::string_view(pending).substr(start, nl - start), tMs, out);
                ++rows;
            }
            pending.erase(0, start);
            bool took = false;
            if (sum.windowDue(tMs))
            {
                sum.takeWindow(tMs, out);
                took = true;
            }
            if (took || out.size() > 16384)
            {
                sink->write(out);
                out.clear();
            }
        }
        sum.takeWindow(nowNs() / 1000000, out);
        out += "# end rows=" + std::to_string(rows) + "\n";
        sink->write(out);
        std::fclose(sink->f);
        delete sink;
        close(readFd); })
        .detach();
#endif
}

void gsAfterOpen(bool opened)
{
    (void)opened;
#if defined(TEL1_HAS_PIPE)
    // GE1 opened its own descriptor on the pipe (or failed to); drop ours so
    // the drain thread sees EOF when GE1 closes the CSV.
    if (s_gsWriteFd >= 0)
    {
        close(s_gsWriteFd);
        s_gsWriteFd = -1;
    }
#endif
}
} // namespace ps2x::telemetry
