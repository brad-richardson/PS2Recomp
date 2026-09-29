// IP3: PS2X_PERF_LOG=1 file backend. Main-thread only (called from the
// render loop in PS2Runtime::run); with the knob off the call sites never
// reach here. One [perf] line plus one [perf-stage] line per stage (PT2) per
// wall second, flushed per poll so a crash keeps the tail. Failures disable
// the log with one stderr note.
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

#if defined(__linux__)
#include <time.h> // clock_gettime(CLOCK_THREAD_CPUTIME_ID)
#endif

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

StageRing &stageRing(Stage s)
{
    static StageRing rings[kStageCount];
    return rings[static_cast<size_t>(s)];
}

uint64_t threadCpuNs()
{
#if defined(__linux__)
    struct timespec ts{};
    if (::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0)
        return static_cast<uint64_t>(ts.tv_sec) * 1000000000u + static_cast<uint64_t>(ts.tv_nsec);
    return kCpuUnsupported;
#elif defined(__APPLE__)
    thread_basic_info_data_t info{};
    mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
    if (::thread_info(::mach_thread_self(), THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &n) ==
        KERN_SUCCESS)
        return static_cast<uint64_t>(info.user_time.seconds + info.system_time.seconds) * 1000000000u +
               static_cast<uint64_t>(info.user_time.microseconds + info.system_time.microseconds) * 1000u;
    return kCpuUnsupported;
#else
    return kCpuUnsupported;
#endif
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
    bool tried = false; // main thread only (poll/dumpTail/tryOpen)
    std::atomic<bool> active{false}; // read by notePresent from any thread
    std::FILE *file = nullptr;
    std::chrono::steady_clock::time_point t0{};
    std::chrono::steady_clock::time_point windowStart{};
    uint64_t windowTick = 0;
    // PT2: present counting is lock-free: notePresent() runs on the GS worker
    // on the VK/AHB path (queue()) and on the main thread on the GL path.
    // poll() exchanges the counters out once per second.
    std::atomic<uint64_t> presentCount{0};
    std::atomic<uint64_t> presentLastNs{0};
    std::atomic<uint64_t> presentMaxGapNs{0};
    std::atomic<bool> havePresent{false};
    // FH6: present-path detail (PS2X_PERF_PRESENT_DETAIL=1).
    bool presentDetail = false;
    std::atomic<uint64_t> gsVsyncs{0}, latches{0}, latchNs{0}, latchMaxNs{0};
#if defined(__APPLE__)
    std::vector<std::pair<std::string, double>> lastCpu;
    bool haveCpu = false;
#endif
#if defined(__linux__)
    std::vector<LinuxCpuSample> lastCpuLinux;
    bool haveCpuLinux = false;
    long clkTck = 0;
#endif
    // PT2: per-stage drain cursors (heads consumed by the last poll()).
    uint64_t stageConsumed[kStageCount] = {};
    // PT2 Part 2a: kill-proof tail flush cursors + cadence + current file.
    uint64_t tailConsumed[kStageCount] = {};
    std::chrono::steady_clock::time_point lastTailFlush{};
    std::string tailCurrent;
    // PT2 Part 2b: in-app kgsl sampling (Android only; main thread).
    bool kgslDeniedNote = false;

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
        lastTailFlush = now; // PT2 Part 2a: first rolling flush at t+60 s
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
        {
            const char *d = std::getenv("PS2X_PERF_PRESENT_DETAIL");
            presentDetail = d && std::strcmp(d, "1") == 0; // before active: readers gate on active
        }
        active.store(true, std::memory_order_release);
        std::fprintf(stderr, "[perf] logging to %s\n", path.c_str());
    }
};

Logger &logger()
{
    static Logger s;
    return s;
}

// PT2: drain one stage's new entries into a [perf-stage] line. A cursor more
// than a lap behind (a poll stall longer than the ring) drops the overwritten
// oldest entries and keeps the newest lap.
void drainStage(Stage s, uint64_t &consumed, std::FILE *file)
{
    StageRing &r = stageRing(s);
    const uint64_t head = r.head();
    consumed = clampDrainStart(consumed, head, StageRing::kCap);
    std::vector<StageEntry> v;
    v.reserve(static_cast<size_t>(head - consumed));
    for (uint64_t i = consumed; i != head; ++i)
        v.push_back(StageRing::decode(r.slotAt(i)));
    consumed = head;
    const std::string line = formatStageLine(stageName(s), summarizeStage(v));
    std::fprintf(file, "%s\n", line.c_str());
}

// PT2 Part 2a: tail-file family ("tail-*.log" + "tail-pause-*.log"), pruned
// separately from the per-second ring (which the per-tick volume would wash
// out). ~1.1 MB/min at 60 ticks/s x 7 stages: 3 files x 8 MB hold ~20 min of
// per-tick history; a kill loses only the unflushed remainder (< 60 s).
constexpr uint64_t kTailFileMaxBytes = 8u * 1024u * 1024u;
constexpr uint64_t kTailDirMaxBytes = 3u * kTailFileMaxBytes;
constexpr size_t kTailMaxFiles = 3;

bool isTailFile(const std::string &name)
{
    if (name.size() < 10 || name.compare(0, 5, "tail-") != 0)
        return false;
    return name.compare(name.size() - 4, 4, ".log") == 0;
}

std::string tailStamp(bool pause)
{
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[48];
    std::strftime(buf, sizeof(buf), pause ? "tail-pause-%Y%m%d-%H%M%S" : "tail-%Y%m%d-%H%M%S", &tm);
    return buf;
}

void pruneTailFiles(const std::string &dir)
{
    std::error_code ec;
    std::vector<FileEntry> entries;
    for (const auto &entry : std::filesystem::directory_iterator(dir, ec))
    {
        if (ec)
            break;
        const std::string name = entry.path().filename().string();
        if (!isTailFile(name))
            continue;
        uint64_t size = 0;
        if (entry.is_regular_file(ec) && !ec)
            size = entry.file_size(ec);
        entries.push_back({name, size});
    }
    for (const std::string &dead : planPrune(std::move(entries), kTailDirMaxBytes, kTailMaxFiles))
    {
        std::filesystem::remove(std::filesystem::path(dir) / dead, ec);
        ec.clear();
    }
}

#if defined(__ANDROID__)
// PT2 Part 2b: read a small sysfs value (kgsl). No locks, main thread only.
bool readSysfs(const char *path, char *buf, size_t cap)
{
    std::FILE *f = std::fopen(path, "r");
    if (!f)
        return false;
    const size_t n = std::fread(buf, 1, cap - 1, f);
    std::fclose(f);
    buf[n] = '\0';
    return n > 0;
}
#endif
} // namespace

void poll(uint64_t vsyncTick)
{
    Logger &log = logger();
    if (!log.tried)
        log.tryOpen(vsyncTick);
    if (!log.active.load(std::memory_order_relaxed) || !log.file)
        return;
    const auto now = std::chrono::steady_clock::now();
    // PT2 Part 2a: rolling ring flush (kill-proof tail). Ahead of the 1 s
    // gate: poll() runs every main-loop iteration, the flush every 60 s.
    if (std::chrono::duration<double>(now - log.lastTailFlush).count() >= 60.0)
    {
        flushTail("timer");
        log.lastTailFlush = now;
    }
    const double windowS = std::chrono::duration<double>(now - log.windowStart).count();
    if (windowS < 1.0)
        return;
    Sample s;
    s.wall = utcIso(std::time(nullptr));
    s.elapsedS = std::chrono::duration<double>(now - log.t0).count();
    s.tick = vsyncTick;
    s.vsyncsPerS = static_cast<double>(vsyncTick - log.windowTick) / windowS;
    s.presents = log.presentCount.exchange(0u, std::memory_order_relaxed);
    const uint64_t maxGapNs = log.presentMaxGapNs.exchange(0u, std::memory_order_relaxed);
    s.maxGapMs = s.presents >= 2 ? static_cast<double>(maxGapNs) / 1e6 : -1.0;
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
#if defined(__ANDROID__)
    // PT2 Part 2b: in-app kgsl GPU sample (1 Hz, main thread, no locks).
    // Unreadable (SELinux), unparseable, or idle-zero reads na.
    {
        char busyBuf[64] = {};
        char clkBuf[64] = {};
        const bool okBusy = readSysfs("/sys/class/kgsl/kgsl-3d0/gpubusy", busyBuf, sizeof(busyBuf));
        const bool okClk = readSysfs("/sys/class/kgsl/kgsl-3d0/gpuclk", clkBuf, sizeof(clkBuf));
        if ((!okBusy || !okClk) && !log.kgslDeniedNote)
        {
            log.kgslDeniedNote = true;
            std::fprintf(stderr, "[perf] kgsl sysfs unreadable (busy=%d clk=%d); gpubusy reads na\n",
                         okBusy ? 1 : 0, okClk ? 1 : 0);
        }
        uint64_t busy = 0, total = 0, hz = 0;
        double pct = 0.0;
        bool havePct = false;
        if (okBusy && parseKgslBusy(busyBuf, busy, total) && kgslSamplePct(busy, total, pct))
            havePct = true; // read-reset window: always instantaneous, no prev needed
        if (havePct)
        {
            char cell[32];
            std::snprintf(cell, sizeof(cell), "%.1f", pct);
            s.kgslBusy = cell;
        }
        if (okClk && parseKgslClk(clkBuf, hz))
            s.kgslClk = std::to_string(hz);
    }
#endif
    const std::string line = formatLine(s);
    std::fprintf(log.file, "%s\n", line.c_str());
    if (log.presentDetail)
    {
        const uint64_t gv = log.gsVsyncs.exchange(0u, std::memory_order_relaxed);
        const uint64_t la = log.latches.exchange(0u, std::memory_order_relaxed);
        const uint64_t lns = log.latchNs.exchange(0u, std::memory_order_relaxed);
        const uint64_t lmax = log.latchMaxNs.exchange(0u, std::memory_order_relaxed);
        std::fprintf(log.file,
                     "[perf-present] tick=%llu vblanks=%llu gs_vsyncs=%llu latches=%llu latch_ms_avg=%.2f "
                     "latch_ms_max=%.2f presents=%llu\n",
                     (unsigned long long)vsyncTick, (unsigned long long)(vsyncTick - log.windowTick),
                     (unsigned long long)gv, (unsigned long long)la, la ? (static_cast<double>(lns) / 1e6) / la : 0.0,
                     static_cast<double>(lmax) / 1e6, (unsigned long long)s.presents);
    }
    for (size_t i = 0; i < kStageCount; ++i)
        drainStage(static_cast<Stage>(i), log.stageConsumed[i], log.file);
    std::fflush(log.file);
    log.windowStart = now;
    log.windowTick = vsyncTick;
}

void noteGsVsync()
{
    Logger &log = logger();
    if (log.presentDetail && log.active.load(std::memory_order_relaxed))
        log.gsVsyncs.fetch_add(1u, std::memory_order_relaxed);
}

void noteLatch(uint64_t ns)
{
    Logger &log = logger();
    if (!log.presentDetail || !log.active.load(std::memory_order_relaxed))
        return;
    log.latches.fetch_add(1u, std::memory_order_relaxed);
    log.latchNs.fetch_add(ns, std::memory_order_relaxed);
    uint64_t prev = log.latchMaxNs.load(std::memory_order_relaxed);
    while (ns > prev && !log.latchMaxNs.compare_exchange_weak(prev, ns, std::memory_order_relaxed))
    {
    }
}

void notePresent()
{
    Logger &log = logger();
    if (!log.active.load(std::memory_order_relaxed))
        return;
    const uint64_t now = steadyNs();
    log.presentCount.fetch_add(1u, std::memory_order_relaxed);
    const uint64_t prev = log.presentLastNs.exchange(now, std::memory_order_relaxed);
    const bool had = log.havePresent.exchange(true, std::memory_order_relaxed);
    // now > prev guards a cross-thread inversion (two paths racing across the
    // GL/VK transition); the gap reads 0 then instead of underflowing.
    if (had && prev != 0u && now > prev)
    {
        const uint64_t gap = now - prev;
        uint64_t m = log.presentMaxGapNs.load(std::memory_order_relaxed);
        while (gap > m && !log.presentMaxGapNs.compare_exchange_weak(m, gap, std::memory_order_relaxed))
        {
        }
    }
}

void flushTail(const char *reason)
{
    Logger &log = logger();
    if (!log.active.load(std::memory_order_relaxed) || !log.file)
        return;
    const std::string dir = defaultDir();
    if (dir.empty())
        return;
    const bool pause = reason && std::strcmp(reason, "pause") == 0;
    // Drain ranges first (marker needs the max tick + entry count).
    struct Range
    {
        Stage stage;
        uint64_t start;
        uint64_t head;
    };
    std::vector<Range> ranges;
    uint64_t entries = 0;
    uint64_t maxTick = 0;
    for (size_t i = 0; i < kStageCount; ++i)
    {
        const Stage s = static_cast<Stage>(i);
        StageRing &r = stageRing(s);
        const uint64_t head = r.head();
        const uint64_t start = clampDrainStart(log.tailConsumed[i], head, StageRing::kCap);
        ranges.push_back({s, start, head});
        for (uint64_t k = start; k != head; ++k)
        {
            const uint64_t tick = StageRing::decode(r.slotAt(k)).tick;
            if (tick > maxTick)
                maxTick = tick;
        }
        entries += head - start;
    }
    // Timer flushes append the current tail file (rotating past the cap);
    // pause flushes mint a fresh tail-pause file so the evidence is obvious.
    std::string path;
    std::error_code ec;
    if (pause)
    {
        path = dir + "/" + tailStamp(true) + ".log";
    }
    else
    {
        if (!log.tailCurrent.empty())
        {
            const uint64_t size =
                std::filesystem::is_regular_file(log.tailCurrent, ec) && !ec
                    ? std::filesystem::file_size(log.tailCurrent, ec)
                    : 0u;
            if (!ec && size <= kTailFileMaxBytes)
                path = log.tailCurrent;
        }
        if (path.empty())
        {
            path = dir + "/" + tailStamp(false) + ".log";
            log.tailCurrent = path;
        }
    }
    std::FILE *out = std::fopen(path.c_str(), "a");
    if (!out)
    {
        std::fprintf(stderr, "[perf] cannot open %s; tail flush skipped\n", path.c_str());
        return;
    }
    std::fprintf(out, "[perf-tail-flush] reason=%s tick=%llu entries=%llu\n", pause ? "pause" : "timer",
                 static_cast<unsigned long long>(maxTick), static_cast<unsigned long long>(entries));
    for (const Range &rg : ranges)
    {
        StageRing &r = stageRing(rg.stage);
        for (uint64_t k = rg.start; k != rg.head; ++k)
        {
            const StageEntry e = StageRing::decode(r.slotAt(k));
            std::fprintf(out, "[perf-tail] tick=%llu stage=%s ms=%.3f\n",
                         static_cast<unsigned long long>(e.tick), stageName(rg.stage),
                         static_cast<double>(e.ms));
        }
    }
    std::fflush(out);
    std::fclose(out);
    for (size_t i = 0; i < kStageCount; ++i)
        log.tailConsumed[i] = ranges[i].head;
    pruneTailFiles(dir);
}

void dumpTail()
{
    Logger &log = logger();
    if (!log.active.load(std::memory_order_relaxed) || !log.file)
        return;
    for (size_t i = 0; i < kStageCount; ++i)
    {
        const Stage s = static_cast<Stage>(i);
        StageRing &r = stageRing(s);
        const uint64_t head = r.head();
        const uint64_t start = head > StageRing::kCap ? head - StageRing::kCap : 0u;
        for (uint64_t k = start; k != head; ++k)
        {
            const StageEntry e = StageRing::decode(r.slotAt(k));
            std::fprintf(log.file, "[perf-tail] tick=%llu stage=%s ms=%.3f\n",
                         static_cast<unsigned long long>(e.tick), stageName(s), static_cast<double>(e.ms));
        }
    }
    std::fflush(log.file);
}

#if defined(__linux__) || defined(__APPLE__)
int32_t perfThermalStatusForTest()
{
    return perfThermalStatus();
}
#endif
} // namespace ps2x::perflog
