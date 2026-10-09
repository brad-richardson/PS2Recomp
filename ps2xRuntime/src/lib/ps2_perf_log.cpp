// IP3: PS2X_PERF_LOG=1 file backend. Main-thread only (called from the
// render loop in PS2Runtime::run); with the knob off the call sites never
// reach here. One [perf] line plus one [perf-stage] line per stage (PT2) per
// wall second, flushed per poll so a crash keeps the tail. Failures disable
// the log with one stderr note.
#include "ps2_perf_log.h"
#include "ps2_mtvu.h"
#include "ps2_snd_audio_output.h"
#include "ps2_vsync_lock.h"
#include "ps2_android_pause.h"
#include "ps2_session_telemetry.h" // TEL2
#if defined(PS2X_IOS)
#include "ps2_ios_runtime.h"
#endif
#if defined(__ANDROID__)
#include "runtime/gs/ps2_present_vk.h" // DSP3: SF-latch window drain
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <time.h> // clock_gettime(CLOCK_THREAD_CPUTIME_ID)
#endif

#if defined(__APPLE__)
#include <mach/mach.h>
#include <pthread.h>
#if __has_include(<pthread/qos.h>)
#include <pthread/qos.h> // PT3: utility QoS for the background tail-flush worker
#endif
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
// PL3: cumulative user+system CPU ms per thread, keyed by the STABLE Mach
// thread ID (thread_identifier_info), not the task_threads() enumeration
// index (whose churn reset deltas or attached them to the wrong thread).
// Names fall back to t/main; isMain marks the snapshot thread (poll() and
// tryOpen() run on the main thread only).
std::vector<StableThreadSample> snapshotThreadCpu()
{
    std::vector<StableThreadSample> out;
    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS)
        return out;
    const thread_act_t self = mach_thread_self();
    for (mach_msg_type_number_t i = 0; i < count; ++i)
    {
        thread_identifier_info_data_t ident{};
        mach_msg_type_number_t nic = THREAD_IDENTIFIER_INFO_COUNT;
        if (thread_info(threads[i], THREAD_IDENTIFIER_INFO, reinterpret_cast<thread_info_t>(&ident), &nic) !=
            KERN_SUCCESS)
        {
            // No stable ID: skip rather than key by enumeration position.
            mach_port_deallocate(mach_task_self(), threads[i]);
            continue;
        }
        thread_basic_info_data_t info{};
        mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
        if (thread_info(threads[i], THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &n) != KERN_SUCCESS)
        {
            mach_port_deallocate(mach_task_self(), threads[i]);
            continue;
        }
        char name[64] = {0};
        if (pthread_t pt = pthread_from_mach_thread_np(threads[i]))
            pthread_getname_np(pt, name, sizeof(name));
        StableThreadSample s;
        s.id = ident.thread_id;
        s.name = name;
        s.isMain = (threads[i] == self);
        s.cumMs = (info.user_time.seconds + info.system_time.seconds) * 1000.0 +
                  (info.user_time.microseconds + info.system_time.microseconds) / 1000.0;
        out.push_back(std::move(s));
        mach_port_deallocate(mach_task_self(), threads[i]);
    }
    mach_port_deallocate(mach_task_self(), self);
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
    std::string comm; // thread name, for the PL3 role sum
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
        sample.comm = cpu.comm;
        sample.cumMs = procTicksToMs(cpu.utime + cpu.stime, clkTck);
        out.push_back(std::move(sample));
    }
    ::closedir(dir);
    return true;
}

// PL3: Linux snapshots keyed by stable kernel tid (readdir order is
// unstable). The main thread's tid equals the pid.
std::vector<StableThreadSample> stableFromLinux(const std::vector<LinuxCpuSample> &snap)
{
    std::vector<StableThreadSample> out;
    out.reserve(snap.size());
    const long mainTid = ::getpid();
    for (const LinuxCpuSample &c : snap)
    {
        StableThreadSample st;
        st.id = static_cast<uint64_t>(c.tid);
        st.name = c.comm;
        st.cumMs = c.cumMs;
        st.isMain = (c.tid == mainTid);
        out.push_back(std::move(st));
    }
    return out;
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
std::mutex gThermalMu;

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
    std::lock_guard<std::mutex> lock(gThermalMu);
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
    std::lock_guard<std::mutex> lock(gThermalMu);
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
    // PL3: cap on the per-window frame-age samples (presents/window fit).
    static constexpr size_t kFrameAgeCap = 2048;
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
    std::atomic<uint64_t> presentGaps16{0};
    std::atomic<bool> havePresent{false};
    // FH6: present-path detail (always counted while the log is on).
    std::atomic<uint64_t> gsVsyncs{0}, latches{0}, latchNs{0}, latchMaxNs{0};
    // PL3: unique-frame identity. noteFrameAvailable (main thread, each new
    // frame) stages the pending frame; notePresentedFrame (main thread, each
    // present) compares it against the last shown one; noteWorkerPresent (VK
    // worker, each shown buffer) keys off the buffer id instead. Guarded by
    // presentMu (present rate only).
    std::mutex presentMu;
    bool haveAvail = false;
    uint64_t availTick = 0, availBirthNs = 0, availExt = 0, availSeq = 0;
    bool havePresented = false;
    uint64_t presentedSeq = 0;
    uint64_t uniqueFrames = 0, dupFrames = 0;
    uint64_t lastWorkerId = 0;
    bool haveWorkerId = false;
    std::vector<double> frameAges; // ms, capped at kFrameAgeCap
#if defined(__APPLE__) || defined(__linux__)
    // PL3: previous window's stable-ID snapshot (see diffStableCpu).
    std::vector<StableThreadSample> lastCpuStable;
    bool haveCpuStable = false;
#endif
#if defined(__linux__)
    long clkTck = 0;
#endif
    // PT2: per-stage drain cursors (heads consumed by the last poll()).
    uint64_t stageConsumed[kStageCount] = {};
    // PT2 Part 2a: kill-proof tail flush cursors + cadence + current file.
    // PT3: the timer flush runs on a background worker (see tailFlushBusy):
    // tailConsumed/tailCurrent are only touched by the flushing thread (the
    // worker for "timer"; the caller for "pause", after joining the worker).
    uint64_t tailConsumed[kStageCount] = {};
    std::chrono::steady_clock::time_point lastTailFlush{};
    std::string tailCurrent;
    // PT3: at most one timer flush in flight. poll() claims the flag and
    // starts tailFlushThread; a set flag means skip this cadence. The thread
    // handle is only touched on the main thread (poll/dumpTail/pause join
    // it; the destructor reaps it).
    std::atomic<bool> tailFlushBusy{false};
    std::thread tailFlushThread;
    // PT2 Part 2b: in-app kgsl sampling (Android only; main thread).
    bool kgslDeniedNote = false;
    // DSP3 item 4: consecutive windows posting >= 115 but latched < 70 (main
    // thread only); one [present-vk] note per run once it reaches 10 (~10 s).
    uint64_t sfStuckWindows = 0;
    bool sfStuckNoted = false;

    ~Logger()
    {
        if (tailFlushThread.joinable())
            tailFlushThread.join();
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
        lastCpuStable = snapshotThreadCpu();
        haveCpuStable = true;
#endif
#if defined(__linux__)
        clkTck = ::sysconf(_SC_CLK_TCK);
        if (clkTck <= 0)
            clkTck = 100; // Linux default USER_HZ
        {
            std::vector<LinuxCpuSample> snap;
            if (snapshotLinuxThreadCpu(clkTck, snap))
            {
                lastCpuStable = stableFromLinux(snap);
                haveCpuStable = true;
            }
        }
#endif
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
StageStats drainStage(Stage s, uint64_t &consumed, std::FILE *file)
{
    StageRing &r = stageRing(s);
    const uint64_t head = r.head();
    consumed = clampDrainStart(consumed, head, StageRing::kCap);
    std::vector<StageEntry> v;
    v.reserve(static_cast<size_t>(head - consumed));
    for (uint64_t i = consumed; i != head; ++i)
        v.push_back(StageRing::decode(r.slotAt(i)));
    consumed = head;
    const StageStats st = summarizeStage(v);
    const std::string line = formatStageLine(stageName(s), st);
    std::fprintf(file, "%s\n", line.c_str());
    return st;
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
std::mutex healthGpuMu;
std::string healthGpuBusy = "na", healthGpuClk = "na";
} // namespace

DeviceHealth deviceHealth()
{
    DeviceHealth out;
    std::string rss = "na", temp = "na", busy = "na", clk = "na";
#if defined(__linux__)
    if (FILE* f = std::fopen("/proc/self/statm", "r")) {
        unsigned long long pages = 0, resident = 0;
        if (std::fscanf(f, "%llu %llu", &pages, &resident) == 2) {
            const long size = sysconf(_SC_PAGESIZE);
            if (size > 0) { out.rss = resident * size; rss = std::to_string(out.rss); }
        }
        std::fclose(f);
    }
    out.thermal = perfThermalStatus();
    char buf[64] = {};
    long tenth = 0;
    if (readSysfs("/sys/class/power_supply/battery/temp", buf, sizeof(buf)) &&
        parseSysfsLong(buf, tenth)) temp = std::to_string(tenth / 10.0);
    if (!enabled()) {
        uint64_t b=0, total=0, hz=0; double pct=0;
        if (readSysfs("/sys/class/kgsl/kgsl-3d0/gpubusy", buf, sizeof(buf)) &&
            parseKgslBusy(buf,b,total) && kgslSamplePct(b,total,pct)) busy = std::to_string(pct);
        if (readSysfs("/sys/class/kgsl/kgsl-3d0/gpuclk", buf, sizeof(buf)) && parseKgslClk(buf,hz)) clk=std::to_string(hz);
    } else {
        std::lock_guard<std::mutex> lock(healthGpuMu);
        busy = healthGpuBusy; clk = healthGpuClk;
    }
#endif
    out.fields = "rss_bytes=" + rss + " thermal=" + (out.thermal < 0 ? "na" : std::to_string(out.thermal)) +
                 " battery_c=" + temp + " gpu_hz=" + clk + " gpu_busy_pct=" + busy;
    return out;
}

// PT3: file-local timer-flush launcher (defined after flushTailWrite): poll()
// starts the 60 s flush on a background worker instead of blocking the host
// main loop on ~87k fprintf'd entries.
void requestTimerFlush();

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
    // PT3: the flush runs on a background worker (at most one in flight; a
    // busy worker means skip this cadence and retry at the next poll).
    if (std::chrono::duration<double>(now - log.lastTailFlush).count() >= 60.0)
    {
        requestTimerFlush();
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
    const uint64_t gaps16 = log.presentGaps16.exchange(0u, std::memory_order_relaxed);
    s.maxGapMs = s.presents >= 2 ? static_cast<double>(maxGapNs) / 1e6 : -1.0;
    RoleSums cpuSums;
    bool haveCpuSums = false;
#if defined(__APPLE__)
    {
        // PL3: stable-ID deltas (see diffStableCpu) plus the per-role sums.
        const std::vector<StableThreadSample> snap = snapshotThreadCpu();
        s.threadsAvailable = true;
        const std::vector<StableThreadDelta> deltas = diffStableCpu(log.lastCpuStable, log.haveCpuStable, snap);
        for (const StableThreadDelta &d : deltas)
            s.threads.push_back({d.label, d.ms, d.isNew});
        cpuSums = sumRoles(deltas);
        haveCpuSums = true;
        log.lastCpuStable = snap;
        log.haveCpuStable = true;
    }
#elif defined(__linux__)
    {
        std::vector<LinuxCpuSample> snap;
        if (snapshotLinuxThreadCpu(log.clkTck > 0 ? log.clkTck : 100, snap))
        {
            // PL3: same stable-ID matching as Apple (tids are stable; the
            // readdir order is not, so the old index-free code already keyed
            // by tid — now with the new-thread mark and the role sums).
            s.threadsAvailable = true;
            const std::vector<StableThreadSample> stable = stableFromLinux(snap);
            const std::vector<StableThreadDelta> deltas =
                diffStableCpu(log.lastCpuStable, log.haveCpuStable, stable);
            for (const StableThreadDelta &d : deltas)
                s.threads.push_back({d.label, d.ms, d.isNew});
            cpuSums = sumRoles(deltas);
            haveCpuSums = true;
            log.lastCpuStable = stable;
            log.haveCpuStable = true;
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
    if (ps2x::tel::sessionOn()) {
        std::lock_guard<std::mutex> lock(healthGpuMu);
        healthGpuBusy = s.kgslBusy.empty() ? "na" : s.kgslBusy;
        healthGpuClk = s.kgslClk.empty() ? "na" : s.kgslClk;
    }
    const std::string line = formatLine(s);
    std::fprintf(log.file, "%s\n", line.c_str());
    if (haveCpuSums)
        std::fprintf(log.file, "%s\n", formatCpuLine(vsyncTick, cpuSums).c_str());
    {
        // PL3: the [perf-present] line is now unconditional (the old
        // PS2X_PERF_PRESENT_DETAIL knob is retired): presents plus unique
        // displayed frames and frame age.
        const uint64_t gv = log.gsVsyncs.exchange(0u, std::memory_order_relaxed);
        const uint64_t la = log.latches.exchange(0u, std::memory_order_relaxed);
        const uint64_t lns = log.latchNs.exchange(0u, std::memory_order_relaxed);
        const uint64_t lmax = log.latchMaxNs.exchange(0u, std::memory_order_relaxed);
        PresentStats pst;
        pst.vblanks = vsyncTick - log.windowTick;
        pst.gsVsyncs = gv;
        pst.latches = la;
        pst.latchAvgMs = la ? (static_cast<double>(lns) / 1e6) / static_cast<double>(la) : 0.0;
        pst.latchMaxMs = static_cast<double>(lmax) / 1e6;
        pst.presents = s.presents;
        {
            std::lock_guard<std::mutex> lock(log.presentMu);
            pst.uframes = log.uniqueFrames;
            pst.udup = log.dupFrames;
            log.uniqueFrames = 0;
            log.dupFrames = 0;
            if (!log.frameAges.empty())
            {
                pst.ageP50Ms = ageP50(log.frameAges);
                double ageMax = log.frameAges[0];
                for (double a : log.frameAges)
                {
                    if (a > ageMax)
                        ageMax = a;
                }
                pst.ageMaxMs = ageMax;
            }
            log.frameAges.clear();
        }
#if defined(__ANDROID__)
        // DSP3: distinct SF latch times this window (the VK ledger counts them
        // in complete(); the drain also resets the window). Off the VK path
        // (or before the first completion) the drain reads zero.
        {
            const ps2x_present_vk::Ledger::SfLatchWindow sf = ps2x_present_vk::takeSfLatchWindow();
            pst.sfLatched = sf.latched;
            if (sf.intervals > 0)
                pst.sfLatchShortPct =
                    100.0 * static_cast<double>(sf.shortIntervals) / static_cast<double>(sf.intervals);
            // Item 4: >= 10 s of SF latching 60/s while we post 120/s. One
            // stderr line per run (no toast); the bench check reads latched=.
            if (pst.sfLatched < 70 && pst.presents >= 115)
                ++log.sfStuckWindows;
            else
                log.sfStuckWindows = 0;
            if (log.sfStuckWindows >= 10 && !log.sfStuckNoted)
            {
                log.sfStuckNoted = true;
                std::fprintf(stderr,
                             "[present-vk] SF latching 60/s (latched=%llu presents=%llu; sleep/wake the "
                             "screen to reset the SF vsync tracker)\n",
                             static_cast<unsigned long long>(pst.sfLatched),
                             static_cast<unsigned long long>(pst.presents));
            }
        }
#endif
        std::fprintf(log.file, "%s\n", formatPresentLine(vsyncTick, pst).c_str());
    }
    {
        // IP6: host audio rates per wall second (device callback demand,
        // guest PCM pushed/consumed, stretch tempo), plus the iOS session's
        // hardware rate and route: the "audio too fast" observables.
        const ps2_snd_audio_output::WindowCounters a = ps2_snd_audio_output::takeWindowCounters();
        if (a.ready)
        {
            std::string session = "na";
#if defined(PS2X_IOS)
            session = ps2x::ios::audioSessionState();
#endif
            std::fprintf(log.file,
                         "[perf-audio] tick=%llu stream_rate=%u stretch=%d cb_fps=%.0f push_fps=%.0f pop_fps=%.0f "
                         "callbacks=%llu fill=%llu underruns=%llu overflows=%llu bypass_pct=%.1f tempo_mean=%.4f "
                         "tempo_min=%.4f tempo_max=%.4f session=\"%s\"\n",
                         (unsigned long long)vsyncTick, a.streamRate, a.stretch ? 1 : 0, a.cbFrames / windowS,
                         a.pushed / windowS, a.consumed / windowS, (unsigned long long)a.callbacks,
                         (unsigned long long)a.fill, (unsigned long long)a.underruns,
                         (unsigned long long)a.overflows,
                         a.callbacks ? 100.0 * static_cast<double>(a.bypassed) / static_cast<double>(a.callbacks)
                                     : 0.0,
                         a.tempoMean, a.tempoMin, a.tempoMax, session.c_str());
        }
    }
    double telemetryHz = 0.0; // unknown unless the lock reports a valid, fresh grid
    if (ps2_vsync_lock::enabled())
    {
        // IP7: vsync-lock health per window (deltas): grid feeds, pacer
        // slots taken / missed (late) / FP1 fallbacks, and the fitted grid.
        static uint64_t pLatches = 0, pLocked = 0, pLate = 0, pUnlocked = 0;
        uint64_t latches, locked, late, unlocked;
        ps2_vsync_lock::Grid g;
        {
            ps2_vsync_lock::Shared &vl = ps2_vsync_lock::shared();
            std::lock_guard<std::mutex> lock(vl.m);
            latches = vl.latches;
            locked = vl.locked;
            late = vl.late;
            unlocked = vl.unlocked;
            g = vl.tracker.grid();
        }
        if (g.valid && g.periodNs > 0.0)
        {
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
            if (ns >= g.lastLatchNs && ns - g.lastLatchNs <= 250000000)
                telemetryHz = 1e9 / g.periodNs;
        }
        std::fprintf(log.file,
                     "[perf-lock] tick=%llu latches=%llu locked=%llu late=%llu unlocked=%llu valid=%d hz=%.3f\n",
                     (unsigned long long)vsyncTick, (unsigned long long)(latches - pLatches),
                     (unsigned long long)(locked - pLocked), (unsigned long long)(late - pLate),
                     (unsigned long long)(unlocked - pUnlocked), g.valid ? 1 : 0,
                     g.periodNs > 0.0 ? 1e9 / g.periodNs : 0.0);
        pLatches = latches;
        pLocked = locked;
        pLate = late;
        pUnlocked = unlocked;
    }
    {
        // IOSR1: per-reason MTVU sync attribution for this window. The EE
        // published per-tick deltas into the window accumulators; the take
        // exchanges them here (main thread). No line when MTVU is off.
        ps2_mtvu::MtvuWindowSample sample;
        if (ps2_mtvu::takeMtvuWindowSample(sample))
        {
            MtvuReasonCell cells[ps2_mtvu::kMtvuReasonCount];
            for (size_t i = 0; i < ps2_mtvu::kMtvuReasonCount; ++i)
            {
                cells[i].name =
                    ps2_mtvu::reasonName(static_cast<ps2_mtvu::Reason>(i));
                cells[i].n = sample.n[i];
                cells[i].ns = sample.ns[i];
            }
            const std::string line =
                formatMtvuLine(vsyncTick, cells, ps2_mtvu::kMtvuReasonCount,
                               sample.otherN, sample.otherNs, sample.finisheeSkips,
                               sample.vif1statfreeSkips);
            std::fprintf(log.file, "%s\n", line.c_str());
        }
    }
    ps2x::tel::StageMax stageMax[kStageCount];
    for (size_t i = 0; i < kStageCount; ++i)
    {
        const StageStats st = drainStage(static_cast<Stage>(i), log.stageConsumed[i], log.file);
        stageMax[i] = {stageName(static_cast<Stage>(i)), st.n ? st.max : -1.0, st};
    }
    std::fflush(log.file);
    // TEL2: session-log hitch/race-window accounting (no-op when off). Not
    // while the BG1 gate holds the game thread (a paused window is no hitch).
    if (!ps2x::androidPause::pausedFlag().load(std::memory_order_relaxed))
        ps2x::tel::notePerfWindow(log.windowTick, vsyncTick, windowS, telemetryHz,
                                    s.vsyncsPerS, s.maxGapMs, stageMax, kStageCount, gaps16);
    log.windowStart = now;
    log.windowTick = vsyncTick;
}

void noteGsVsync()
{
    Logger &log = logger();
    if (!log.active.load(std::memory_order_relaxed))
        return;
    log.gsVsyncs.fetch_add(1u, std::memory_order_relaxed);
}

void noteLatch(uint64_t ns)
{
    Logger &log = logger();
    if (!log.active.load(std::memory_order_relaxed))
        return;
    log.latches.fetch_add(1u, std::memory_order_relaxed);
    log.latchNs.fetch_add(ns, std::memory_order_relaxed);
    uint64_t prev = log.latchMaxNs.load(std::memory_order_relaxed);
    while (ns > prev && !log.latchMaxNs.compare_exchange_weak(prev, ns, std::memory_order_relaxed))
    {
    }
}

// PL3: stage the pending frame for the unique-frame count. A new internal
// sequence starts whenever the guest tick advances or the mailbox sequence
// does (repeats of the same frame keep the old one, so the present reads a
// duplicate).
void noteFrameAvailable(uint64_t tick, uint64_t birthWallNs, uint64_t extSeq)
{
    Logger &log = logger();
    if (!log.active.load(std::memory_order_relaxed))
        return;
    std::lock_guard<std::mutex> lock(log.presentMu);
    const bool isNew =
        !log.haveAvail || tick != log.availTick || (extSeq != 0u && extSeq != log.availExt);
    log.haveAvail = true;
    log.availTick = tick;
    log.availBirthNs = birthWallNs;
    log.availExt = extSeq;
    if (isNew)
        ++log.availSeq;
}

// PL3: one main-thread present against the pending frame.
void notePresentedFrame()
{
    Logger &log = logger();
    if (!log.active.load(std::memory_order_relaxed))
        return;
    std::lock_guard<std::mutex> lock(log.presentMu);
    if (!log.haveAvail)
        return;
    if (!log.havePresented || log.availSeq != log.presentedSeq)
    {
        log.havePresented = true;
        log.presentedSeq = log.availSeq;
        ++log.uniqueFrames;
        if (log.availBirthNs != 0u && log.frameAges.size() < Logger::kFrameAgeCap)
        {
            const uint64_t now = steadyNs();
            if (now > log.availBirthNs)
                log.frameAges.push_back(static_cast<double>(now - log.availBirthNs) / 1e6);
        }
    }
    else
    {
        ++log.dupFrames;
    }
}

// PL3: VK worker path (no mailbox identity): presents plus a unique count
// keyed off the shown buffer id (a re-shown buffer reads as a duplicate).
void noteWorkerPresent(uint64_t id)
{
    Logger &log = logger();
    if (!log.active.load(std::memory_order_relaxed))
        return;
    notePresent();
    std::lock_guard<std::mutex> lock(log.presentMu);
    if (!log.haveWorkerId || id != log.lastWorkerId)
    {
        log.haveWorkerId = true;
        log.lastWorkerId = id;
        ++log.uniqueFrames;
    }
    else
    {
        ++log.dupFrames;
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
        if (gap >= 16000000u) log.presentGaps16.fetch_add(1, std::memory_order_relaxed);
        uint64_t m = log.presentMaxGapNs.load(std::memory_order_relaxed);
        while (gap > m && !log.presentMaxGapNs.compare_exchange_weak(m, gap, std::memory_order_relaxed))
        {
        }
    }
}

// PT3: the actual tail-file write. Runs on the background worker for
// "timer" (see requestTimerFlush) and synchronously on the caller for
// "pause" (flushTail joins the worker first), so tailConsumed/tailCurrent
// are only ever touched by one thread at a time. The ring reads are
// lock-free, so the GameThread never stalls on this. The [perf-tail-flush]
// marker trails the entries: it carries the worker's wall ms for the whole
// open+write+close+prune (dev-only timing field).
void flushTailWrite(const char *reason)
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
    const auto t0 = std::chrono::steady_clock::now();
    std::FILE *out = std::fopen(path.c_str(), "a");
    if (!out)
    {
        std::fprintf(stderr, "[perf] cannot open %s; tail flush skipped\n", path.c_str());
        return;
    }
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
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    // Marker trails the entries so it can carry the worker ms. Same tag as
    // the PT2 stock marker plus ms (readers grep the tag, not its position).
    out = std::fopen(path.c_str(), "a");
    if (out)
    {
        std::fprintf(out, "[perf-tail-flush] reason=%s tick=%llu entries=%llu ms=%.1f\n",
                     pause ? "pause" : "timer", static_cast<unsigned long long>(maxTick),
                     static_cast<unsigned long long>(entries), ms);
        std::fflush(out);
        std::fclose(out);
    }
    std::fprintf(stderr, "[perf-tail-flush] reason=%s tick=%llu entries=%llu ms=%.1f\n",
                 pause ? "pause" : "timer", static_cast<unsigned long long>(maxTick),
                 static_cast<unsigned long long>(entries), ms);
}

void flushTail(const char *reason)
{
    Logger &log = logger();
    const bool pause = reason && std::strcmp(reason, "pause") == 0;
    if (pause)
    {
        // Synchronous: the app is backgrounding and has to persist. Join an
        // in-flight timer flush first so tailConsumed/tailCurrent stay
        // single-threaded, then write on this thread.
        if (log.tailFlushThread.joinable())
        {
            log.tailFlushThread.join();
            log.tailFlushBusy.store(false, std::memory_order_release);
        }
        flushTailWrite("pause");
        return;
    }
    // Direct "timer" calls (anything but poll's cadence) stay synchronous so
    // the API is total; poll() itself uses requestTimerFlush below.
    if (log.tailFlushThread.joinable())
    {
        log.tailFlushThread.join();
        log.tailFlushBusy.store(false, std::memory_order_release);
    }
    flushTailWrite("timer");
}

// PT3: start the 60 s flush on a background worker (utility QoS on Apple)
// instead of blocking the host main loop. At most one flush is in flight: a
// set busy flag means skip this cadence (poll() retries in 60 s).
void requestTimerFlush()
{
    Logger &log = logger();
    if (!log.active.load(std::memory_order_relaxed) || !log.file)
        return;
    if (log.tailFlushBusy.exchange(true, std::memory_order_acq_rel))
        return;
    // Reap the previous (finished: the flag was clear) worker first.
    if (log.tailFlushThread.joinable())
        log.tailFlushThread.join();
    log.tailFlushThread = std::thread([] {
#if defined(__APPLE__) && defined(QOS_CLASS_UTILITY)
        (void)::pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#endif
        flushTailWrite("timer");
        logger().tailFlushBusy.store(false, std::memory_order_release);
    });
}

void dumpTail()
{
    Logger &log = logger();
    if (!log.active.load(std::memory_order_relaxed) || !log.file)
        return;
    // PT3: wait for an in-flight timer flush so the tail files are complete
    // before the full-ring dump runs (and no worker outlives shutdown).
    if (log.tailFlushThread.joinable())
    {
        log.tailFlushThread.join();
        log.tailFlushBusy.store(false, std::memory_order_release);
    }
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
