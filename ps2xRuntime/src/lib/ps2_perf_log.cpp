// IP3: PS2X_PERF_LOG=1 file backend. Main-thread only (called from the
// render loop in PS2Runtime::run); with the knob off the call sites never
// reach here. One line per wall second, flushed per line so a crash keeps
// the tail. Failures disable the log with one stderr note.
#include "ps2_perf_log.h"
#if defined(PS2X_IOS)
#include "ps2_ios_runtime.h"
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <pthread.h>
#endif

namespace ps2x::perflog
{
bool enabled()
{
    return enabledFromEnv(std::getenv("PS2X_PERF_LOG"));
}

namespace
{
bool hasPrefixSuffix(const std::string &name)
{
    if (name.size() < 10 || name.compare(0, 5, "perf-") != 0)
        return false;
    return name.compare(name.size() - 4, 4, ".log") == 0;
}

std::string defaultDir()
{
    if (const char *override = std::getenv("PS2X_PERF_LOG_DIR"); override && override[0] != '\0')
        return override;
#if defined(PS2X_IOS)
    // Same Documents derivation as prepareEnvironment (HOME/Documents).
    if (const char *home = std::getenv("HOME"); home && home[0] != '\0')
        return std::string(home) + "/Documents/perf";
    return {};
#elif defined(__ANDROID__)
    // Same files dir the N-lane env shim reads ps2x.env from.
#if defined(PS2X_DEFAULT_BOOT_ELF)
    const std::string bootElf(PS2X_DEFAULT_BOOT_ELF);
    const size_t slash = bootElf.find_last_of('/');
    if (slash != std::string::npos)
        return bootElf.substr(0, slash) + "/perf";
#endif
    return {};
#else
    return "perf";
#endif
}

std::string launchStamp()
{
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "perf-%Y%m%d-%H%M%S", &tm);
    return buf;
}

std::string utcIso(std::time_t when)
{
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &when);
#else
    gmtime_r(&when, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

#if defined(__APPLE__)
// Cumulative user+system CPU ms per thread, in task_threads() order (same
// source as logThreadCpu in ps2_runtime.cpp); names fall back to t#<index>.
std::vector<std::pair<std::string, double>> snapshotThreadCpu()
{
    std::vector<std::pair<std::string, double>> out;
    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS)
        return out;
    for (mach_msg_type_number_t i = 0; i < count; ++i)
    {
        thread_basic_info_data_t info{};
        mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
        if (thread_info(threads[i], THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &n) == KERN_SUCCESS)
        {
            char name[64] = {0};
            if (pthread_t pt = pthread_from_mach_thread_np(threads[i]))
                pthread_getname_np(pt, name, sizeof(name));
            const double ms = (info.user_time.seconds + info.system_time.seconds) * 1000.0 +
                              (info.user_time.microseconds + info.system_time.microseconds) / 1000.0;
            char label[80];
            std::snprintf(label, sizeof(label), "%s#%u", name[0] ? name : "t", i);
            out.emplace_back(label, ms);
        }
        mach_port_deallocate(mach_task_self(), threads[i]);
    }
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads), count * sizeof(thread_act_t));
    return out;
}
#endif

struct Logger
{
    bool tried = false;
    bool active = false;
    std::FILE *file = nullptr;
    std::chrono::steady_clock::time_point t0{};
    std::chrono::steady_clock::time_point windowStart{};
    uint64_t windowTick = 0;
    uint64_t windowPresents = 0;
    double windowMaxGapMs = -1.0;
    bool haveLastPresent = false;
    std::chrono::steady_clock::time_point lastPresent{};
#if defined(__APPLE__)
    std::vector<std::pair<std::string, double>> lastCpu;
    bool haveCpu = false;
#endif

    ~Logger()
    {
        if (file)
            std::fclose(file);
    }

    void tryOpen(uint64_t tick)
    {
        tried = true;
        const std::string dir = defaultDir();
        if (dir.empty())
        {
            std::fprintf(stderr, "[perf] no log dir (HOME/PS2X_PERF_LOG_DIR); disabled\n");
            return;
        }
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec)
        {
            std::fprintf(stderr, "[perf] cannot create %s: %s; disabled\n", dir.c_str(), ec.message().c_str());
            return;
        }
        std::string path;
        for (int attempt = 0; attempt < 100; ++attempt)
        {
            std::string candidate = dir + "/" + launchStamp();
            if (attempt > 0)
            {
                candidate += "-" + std::to_string(attempt + 1);
            }
            candidate += ".log";
            if (!std::filesystem::exists(candidate, ec))
            {
                path = candidate;
                break;
            }
        }
        if (path.empty())
        {
            std::fprintf(stderr, "[perf] no free file name in %s; disabled\n", dir.c_str());
            return;
        }
        file = std::fopen(path.c_str(), "w");
        if (!file)
        {
            std::fprintf(stderr, "[perf] cannot open %s; disabled\n", path.c_str());
            return;
        }
        // Ring-cap with the new file included (it sorts newest, never pruned).
        std::vector<FileEntry> entries;
        for (const auto &entry : std::filesystem::directory_iterator(dir, ec))
        {
            if (ec)
                break;
            const std::string name = entry.path().filename().string();
            if (!hasPrefixSuffix(name))
                continue;
            uint64_t size = 0;
            if (entry.is_regular_file(ec) && !ec)
                size = entry.file_size(ec);
            entries.push_back({name, size});
        }
        for (const std::string &dead : planPrune(std::move(entries)))
        {
            std::filesystem::remove(std::filesystem::path(dir) / dead, ec);
            ec.clear();
        }
        const auto now = std::chrono::steady_clock::now();
        t0 = now;
        windowStart = now;
        windowTick = tick;
#if defined(__APPLE__)
        lastCpu = snapshotThreadCpu();
        haveCpu = true;
#endif
        active = true;
        std::fprintf(stderr, "[perf] logging to %s\n", path.c_str());
    }
};

Logger &logger()
{
    static Logger s;
    return s;
}
} // namespace

void poll(uint64_t vsyncTick)
{
    Logger &log = logger();
    if (!log.tried)
        log.tryOpen(vsyncTick);
    if (!log.active || !log.file)
        return;
    const auto now = std::chrono::steady_clock::now();
    const double windowS = std::chrono::duration<double>(now - log.windowStart).count();
    if (windowS < 1.0)
        return;
    Sample s;
    s.wall = utcIso(std::time(nullptr));
    s.elapsedS = std::chrono::duration<double>(now - log.t0).count();
    s.tick = vsyncTick;
    s.vsyncsPerS = static_cast<double>(vsyncTick - log.windowTick) / windowS;
    s.presents = log.windowPresents;
    s.maxGapMs = log.windowPresents >= 2 ? log.windowMaxGapMs : -1.0;
#if defined(__APPLE__)
    {
        const auto snap = snapshotThreadCpu();
        s.threadsAvailable = true;
        for (size_t i = 0; i < snap.size(); ++i)
        {
            double base = -1.0;
            if (log.haveCpu && i < log.lastCpu.size() && log.lastCpu[i].first == snap[i].first)
                base = log.lastCpu[i].second;
            s.threads.push_back({snap[i].first, base >= 0.0 ? snap[i].second - base : 0.0});
        }
        log.lastCpu = snap;
        log.haveCpu = true;
    }
#else
    s.threadsAvailable = false;
#endif
#if defined(PS2X_IOS)
    s.device = ps2x::ios::perfDeviceState();
#else
    s.device = "na";
#endif
    const std::string line = formatLine(s);
    std::fprintf(log.file, "%s\n", line.c_str());
    std::fflush(log.file);
    log.windowStart = now;
    log.windowTick = vsyncTick;
    log.windowPresents = 0;
    log.windowMaxGapMs = -1.0;
}

void notePresent()
{
    Logger &log = logger();
    if (!log.active)
        return;
    const auto now = std::chrono::steady_clock::now();
    if (log.haveLastPresent)
    {
        const double gapMs = std::chrono::duration<double, std::milli>(now - log.lastPresent).count();
        if (gapMs > log.windowMaxGapMs)
            log.windowMaxGapMs = gapMs;
    }
    log.haveLastPresent = true;
    log.lastPresent = now;
    ++log.windowPresents;
}
} // namespace ps2x::perflog
