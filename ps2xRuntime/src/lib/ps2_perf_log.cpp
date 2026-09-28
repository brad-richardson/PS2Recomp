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
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <pthread.h>
#endif

#if defined(__linux__)
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#endif
#if defined(__linux__) || defined(__APPLE__)
#include <dlfcn.h> // PL2: AThermal dlsym (the Mac leg feeds the test seam only)
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

#if defined(__linux__)
// PL1: /proc + sysfs samplers. Main thread, once per wall second, bounded:
// fixed caps on entries, stack buffers, open/read/close (no FILE, no
// allocation in the reads themselves). threadsAvailable=false when
// /proc/self/task won't open; every device channel degrades to "na".
struct LinuxCpuSample
{
    long tid = 0;
    std::string label; // "<comm>#<tid>"
    double cumMs = 0.0; // user+system, cumulative
};

bool perfReadSmallFile(const char *path, char *buf, size_t bufSize, size_t &lenOut)
{
    lenOut = 0;
    if (bufSize < 2)
        return false;
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    size_t total = 0;
    for (;;)
    {
        const ssize_t n = ::read(fd, buf + total, bufSize - 1 - total);
        if (n < 0)
        {
            ::close(fd);
            return false;
        }
        if (n == 0)
            break;
        total += static_cast<size_t>(n);
        if (total >= bufSize - 1)
        {
            // Full without EOF: not a small status file; refuse truncation.
            char probe = 0;
            const ssize_t m = ::read(fd, &probe, 1);
            ::close(fd);
            if (m != 0)
                return false;
            break;
        }
    }
    ::close(fd);
    buf[total] = '\0';
    lenOut = total;
    return true;
}

bool perfTrimEquals(const char *text, size_t len, const char *want)
{
    size_t begin = 0;
    while (begin < len && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\n' || text[begin] == '\r'))
        ++begin;
    size_t end = len;
    while (end > begin &&
           (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\n' || text[end - 1] == '\r'))
        --end;
    for (size_t i = 0;; ++i)
    {
        const bool textEnd = begin + i >= end;
        const bool wantEnd = want[i] == '\0';
        if (textEnd || wantEnd)
            return textEnd && wantEnd;
        if (text[begin + i] != want[i])
            return false;
    }
}

// Per-thread cumulative user+system CPU ms from /proc/self/task/*/stat,
// keyed by tid (readdir order is unstable). Labels carry the tid so
// duplicate comms (unnamed threads share the process name) stay distinct.
bool snapshotLinuxThreadCpu(long clkTck, std::vector<LinuxCpuSample> &out)
{
    out.clear();
    DIR *dir = ::opendir("/proc/self/task");
    if (!dir)
        return false;
    int scanned = 0;
    for (;;)
    {
        const dirent *de = ::readdir(dir);
        if (!de)
            break;
        char *end = nullptr;
        const long tid = std::strtol(de->d_name, &end, 10);
        if (end == de->d_name || *end != '\0' || tid <= 0)
            continue;
        if (++scanned > 256)
            break;
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/self/task/%ld/stat", tid);
        char text[1024]; // stat lines are < 512 bytes (comm <= 16)
        size_t len = 0;
        if (!perfReadSmallFile(path, text, sizeof(text), len))
            continue;
        ProcTaskCpu cpu;
        if (!parseProcTaskStat(tid, std::string_view(text, len), cpu))
            continue;
        char label[64];
        std::snprintf(label, sizeof(label), "%s#%ld", cpu.comm.c_str(), tid);
        LinuxCpuSample sample;
        sample.tid = tid;
        sample.label = label;
        sample.cumMs = procTicksToMs(cpu.utime + cpu.stime, clkTck);
        out.push_back(std::move(sample));
    }
    ::closedir(dir);
    return true;
}
#endif

#if defined(__linux__) || defined(__APPLE__)
// PL2: AThermal is API 30+ and minSdk is 29, so resolve at run time. The NDK
// getter takes a manager: acquire once (main thread, lazily, cached) and
// pass it. PL1 called the getter with no argument and SIGABRTed on device.
// Any missing symbol or a null manager reads -1 (na); the getter is never
// called then. Desktop libandroid.so is absent, so desktop always reads -1
// (the host suite asserts that). Namespace scope, so it outlives the Logger
// and the shutdown release below is strictly safe. The Mac leg exists only
// for that test; production sampling (sampleLinuxDevice) stays Linux-only.
struct ThermalState
{
    bool lookedUp = false;
    ThermalFns fns;
    void *libHandle = nullptr; // never dlclosed (process lifetime)
    void *manager = nullptr;   // owned; released at shutdown
    bool acquired = false;
};
ThermalState gThermal;

void lookupThermalFns()
{
    gThermal.lookedUp = true;
    void *handle = ::dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    if (!handle)
        return;
    gThermal.libHandle = handle;
    gThermal.fns.acquireManager = reinterpret_cast<void *(*)()>(::dlsym(handle, "AThermal_acquireManager"));
    gThermal.fns.getStatus =
        reinterpret_cast<int32_t (*)(void *)>(::dlsym(handle, "AThermal_getCurrentThermalStatus"));
    gThermal.fns.releaseManager = reinterpret_cast<void (*)(void *)>(::dlsym(handle, "AThermal_releaseManager"));
}

int32_t perfThermalStatus()
{
    if (!gThermal.lookedUp)
        lookupThermalFns();
    if (!gThermal.fns.acquireManager || !gThermal.fns.getStatus || !gThermal.fns.releaseManager)
        return -1;
    if (!gThermal.acquired)
    {
        gThermal.acquired = true;
        gThermal.manager = gThermal.fns.acquireManager();
    }
    return thermalStatusWith(gThermal.fns, gThermal.manager);
}

void releaseThermalManager()
{
    // Shutdown only (Logger dtor): never triggers a lookup.
    if (gThermal.manager && gThermal.fns.releaseManager)
        gThermal.fns.releaseManager(gThermal.manager);
    gThermal.manager = nullptr;
}
#endif

#if defined(__linux__)
std::string sampleLinuxDevice()
{
    AndroidDevice d;
    const int32_t thermal = perfThermalStatus();
    if (thermal >= 0)
    {
        d.hasThermal = true;
        d.thermal = thermal;
    }
    // Prime-zone temperature: the best-ranked sysfs thermal zone
    // (primeZoneRank; cpu-1-1-1 first, the Odin 3 prime core, same source
    // as odin_run.py's pre-launch check), millidegrees C. thermal_zone*
    // only (cooling_device* entries share the dir and wasted scan slots),
    // 256 cap (the Odin carries 116 entries; 64 never reached zone 28).
    if (DIR *dir = ::opendir("/sys/class/thermal"))
    {
        int zones = 0;
        int bestRank = -1;
        char bestZone[64] = {};
        for (;;)
        {
            const dirent *de = ::readdir(dir);
            if (!de)
                break;
            if (de->d_name[0] == '.')
                continue;
            if (std::strncmp(de->d_name, "thermal_zone", 12) != 0)
                continue;
            if (++zones > 256)
                break;
            char typePath[160];
            if (std::snprintf(typePath, sizeof(typePath), "/sys/class/thermal/%s/type", de->d_name) >=
                static_cast<int>(sizeof(typePath)))
                continue;
            char type[64];
            size_t typeLen = 0;
            if (!perfReadSmallFile(typePath, type, sizeof(type), typeLen))
                continue;
            size_t begin = 0;
            while (begin < typeLen && (type[begin] == ' ' || type[begin] == '\t' || type[begin] == '\n' ||
                                       type[begin] == '\r'))
                ++begin;
            size_t end = typeLen;
            while (end > begin && (type[end - 1] == ' ' || type[end - 1] == '\t' || type[end - 1] == '\n' ||
                                   type[end - 1] == '\r'))
                --end;
            const int rank = primeZoneRank(std::string_view(type + begin, end - begin));
            if (rank < 0 || (bestRank >= 0 && rank >= bestRank))
                continue;
            bestRank = rank;
            std::snprintf(bestZone, sizeof(bestZone), "%s", de->d_name);
            if (bestRank == 0)
                break;
        }
        ::closedir(dir);
        if (bestRank >= 0)
        {
            char tempPath[160];
            if (std::snprintf(tempPath, sizeof(tempPath), "/sys/class/thermal/%s/temp", bestZone) <
                static_cast<int>(sizeof(tempPath)))
            {
                char temp[32];
                size_t tempLen = 0;
                long milli = 0;
                if (perfReadSmallFile(tempPath, temp, sizeof(temp), tempLen) &&
                    parseSysfsLong(std::string_view(temp, tempLen), milli))
                {
                    d.hasPrimeC = true;
                    d.primeC = static_cast<double>(milli) / 1000.0;
                }
            }
        }
    }
    {
        char cap[32];
        size_t capLen = 0;
        long pct = 0;
        if (perfReadSmallFile("/sys/class/power_supply/battery/capacity", cap, sizeof(cap), capLen) &&
            parseSysfsLong(std::string_view(cap, capLen), pct) && pct >= 0 && pct <= 100)
        {
            d.hasBatt = true;
            d.batt = static_cast<int>(pct);
        }
    }
    // AC: any non-battery supply (USB/Mains/Wireless/...) reporting online.
    if (DIR *dir = ::opendir("/sys/class/power_supply"))
    {
        int supplies = 0;
        for (;;)
        {
            const dirent *de = ::readdir(dir);
            if (!de)
                break;
            if (de->d_name[0] == '.')
                continue;
            if (++supplies > 32)
                break;
            char typePath[192];
            if (std::snprintf(typePath, sizeof(typePath), "/sys/class/power_supply/%s/type", de->d_name) >=
                static_cast<int>(sizeof(typePath)))
                continue;
            char type[32];
            size_t typeLen = 0;
            if (!perfReadSmallFile(typePath, type, sizeof(type), typeLen) ||
                perfTrimEquals(type, typeLen, "Battery"))
                continue;
            char onlinePath[192];
            if (std::snprintf(onlinePath, sizeof(onlinePath), "/sys/class/power_supply/%s/online", de->d_name) >=
                static_cast<int>(sizeof(onlinePath)))
                continue;
            char online[32];
            size_t onlineLen = 0;
            long flag = 0;
            if (perfReadSmallFile(onlinePath, online, sizeof(online), onlineLen) &&
                parseSysfsLong(std::string_view(online, onlineLen), flag))
            {
                d.hasAc = true;
                d.ac = flag != 0 ? 1 : 0;
                if (d.ac == 1)
                    break;
            }
        }
        ::closedir(dir);
    }
    return formatAndroidDevice(d);
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
#if defined(__linux__)
    std::vector<LinuxCpuSample> lastCpuLinux;
    bool haveCpuLinux = false;
    long clkTck = 0;
#endif

    ~Logger()
    {
        if (file)
            std::fclose(file);
#if defined(__linux__) || defined(__APPLE__)
        releaseThermalManager();
#endif
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
#if defined(__linux__)
        clkTck = ::sysconf(_SC_CLK_TCK);
        if (clkTck <= 0)
            clkTck = 100; // Linux default USER_HZ
        snapshotLinuxThreadCpu(clkTck, lastCpuLinux);
        haveCpuLinux = true;
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
#elif defined(__linux__)
    {
        std::vector<LinuxCpuSample> snap;
        if (snapshotLinuxThreadCpu(log.clkTck > 0 ? log.clkTck : 100, snap))
        {
            s.threadsAvailable = true;
            for (const LinuxCpuSample &cur : snap)
            {
                double base = -1.0;
                if (log.haveCpuLinux)
                {
                    for (const LinuxCpuSample &prev : log.lastCpuLinux)
                    {
                        if (prev.tid == cur.tid)
                        {
                            base = prev.cumMs;
                            break;
                        }
                    }
                }
                const double delta = base >= 0.0 ? cur.cumMs - base : 0.0;
                s.threads.push_back({cur.label, delta >= 0.0 ? delta : 0.0});
            }
            log.lastCpuLinux = snap;
            log.haveCpuLinux = true;
        }
        else
        {
            s.threadsAvailable = false;
        }
    }
#else
    s.threadsAvailable = false;
#endif
#if defined(PS2X_IOS)
    s.device = ps2x::ios::perfDeviceState();
#elif defined(__linux__)
    s.device = sampleLinuxDevice();
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

#if defined(__linux__) || defined(__APPLE__)
int32_t perfThermalStatusForTest()
{
    return perfThermalStatus();
}
#endif
} // namespace ps2x::perflog
