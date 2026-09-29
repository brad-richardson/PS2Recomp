// AD1: Android ADPF performance hints (API 33+, dlsym'd: minSdk is 29).
// One hint session per hot thread; each tick's PT2 busy time is reported so
// the governor boosts the thread toward PS2X_ADPF_TARGET_MS (default 10).
// Timing-only, host-side: nothing here touches guest state.

#include "ps2_adpf.h"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <dlfcn.h>
#include <mutex>
#include <unistd.h>

namespace ps2x::adpf
{
namespace
{

// Mirrors <android/performance_hint.h> (NDK 28, verified against the sysroot
// header: createSession is APerformanceHint_createSession, and
// getPreferredUpdateRateNanos returns int64_t). Opaque handles only.
struct AdpfFns
{
    void *(*getManager)() = nullptr;
    void *(*createSession)(void *, const int32_t *, size_t, int64_t) = nullptr;
    int64_t (*getPreferredUpdateRateNanos)(void *) = nullptr;
    int (*reportActualWorkDuration)(void *, int64_t) = nullptr;
};

AdpfFns g_fns;
void *g_libHandle = nullptr; // never dlclosed (process lifetime)
void *g_manager = nullptr;   // never closed (process lifetime)
int64_t g_rateNs = -1;
bool g_supported = false;
std::once_flag g_initOnce;

// Per-thread state. Sessions are created under g_mutex; report() takes the
// fast path (one acquire-load) once its session exists. Each session is
// reported from exactly one thread (ADPF sessions are not thread-safe).
std::mutex g_mutex;
std::atomic<int> g_tids[kThreadCount];          // 0 = unknown
std::atomic<void *> g_sessions[kThreadCount];   // nullptr = none
std::atomic<uint64_t> g_lastBusyNs[kThreadCount];
std::atomic<uint64_t> g_reportErr[kThreadCount];
bool g_sessionLogged[kThreadCount] = {}; // under g_mutex
uint64_t g_nextCreateNs[kThreadCount] = {}; // under g_mutex: 1/s create retry
uint64_t g_lastGsBackScanNs = 0;             // under g_mutex: 1/s /proc scan
std::atomic<uint64_t> g_lastLogNs{0};

uint64_t monotonicNs()
{
    struct timespec ts = {};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000u + static_cast<uint64_t>(ts.tv_nsec);
}

// The GE1 back thread is named "GS Back" (PCSX2 GSState::BackThreadLoop via
// Threading::SetNameOfCurrentThread); it lives inside libge1_gs, so our side
// finds its TID with a read-only /proc/self/task scan (comm fits: <15 chars).
int scanGsBackTid()
{
    DIR *dir = ::opendir("/proc/self/task");
    if (!dir)
        return 0;
    int found = 0;
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
        std::snprintf(path, sizeof(path), "/proc/self/task/%ld/comm", tid);
        FILE *f = std::fopen(path, "r");
        if (!f)
            continue;
        char comm[64];
        const size_t n = std::fread(comm, 1, sizeof(comm) - 1, f);
        std::fclose(f);
        if (n == 0)
            continue;
        comm[n] = '\0';
        // comm ends with '\n'; trim trailing whitespace, then exact-match.
        size_t len = n;
        while (len > 0 && (comm[len - 1] == '\n' || comm[len - 1] == ' ' || comm[len - 1] == '\t' ||
                            comm[len - 1] == '\r'))
            comm[--len] = '\0';
        if (std::strcmp(comm, "GS Back") == 0)
        {
            found = static_cast<int>(tid);
            break;
        }
    }
    ::closedir(dir);
    return found;
}

void initOnce()
{
    void *handle = ::dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    const char *missing = "libandroid.so";
    if (handle)
    {
        g_libHandle = handle;
        g_fns.getManager = reinterpret_cast<void *(*)()>(::dlsym(handle, "APerformanceHint_getManager"));
        g_fns.createSession = reinterpret_cast<void *(*)(void *, const int32_t *, size_t, int64_t)>(
            ::dlsym(handle, "APerformanceHint_createSession"));
        g_fns.getPreferredUpdateRateNanos =
            reinterpret_cast<int64_t (*)(void *)>(::dlsym(handle, "APerformanceHint_getPreferredUpdateRateNanos"));
        g_fns.reportActualWorkDuration =
            reinterpret_cast<int (*)(void *, int64_t)>(::dlsym(handle, "APerformanceHint_reportActualWorkDuration"));
        if (!g_fns.getManager || !g_fns.createSession || !g_fns.getPreferredUpdateRateNanos ||
            !g_fns.reportActualWorkDuration)
            missing = "APerformanceHint_* symbol(s)";
        else if ((g_manager = g_fns.getManager()) == nullptr)
            missing = "manager (unsupported ROM)";
        else
        {
            g_rateNs = g_fns.getPreferredUpdateRateNanos(g_manager);
            missing = nullptr;
        }
    }
    g_supported = (missing == nullptr);
    std::fprintf(stderr,
                 "[adpf] init enabled=1 target_ns=%" PRId64 " manager=%s preferred_update_rate_ns=%" PRId64 "%s%s\n",
                 targetNs(), g_manager ? "ok" : "null", g_rateNs, missing ? " missing=" : "",
                 missing ? missing : "");
}

void ensureInit()
{
    std::call_once(g_initOnce, initOnce);
}

// Under g_mutex. Creates the session for `t` when its TID is known (GsBack
// resolves via a throttled /proc scan); creation itself retries at 1 Hz.
void ensureSessionLocked(Thread t, uint64_t now)
{
    const size_t i = static_cast<size_t>(t);
    if (g_sessions[i].load(std::memory_order_acquire) != nullptr)
        return;
    if (!g_supported)
        return;
    if (now < g_nextCreateNs[i])
        return;
    g_nextCreateNs[i] = now + 1000000000u;
    int tid = g_tids[i].load(std::memory_order_relaxed);
    if (tid <= 0)
    {
        if (t == Thread::GsBack)
        {
            if (now < g_lastGsBackScanNs + 1000000000u)
                return;
            g_lastGsBackScanNs = now;
            tid = scanGsBackTid();
            if (tid > 0)
                g_tids[i].store(tid, std::memory_order_relaxed);
        }
        if (tid <= 0)
            return;
    }
    const int32_t id = static_cast<int32_t>(tid);
    void *session = g_fns.createSession(g_manager, &id, 1, targetNs());
    if (session)
        g_sessions[i].store(session, std::memory_order_release);
    if (!g_sessionLogged[i])
    {
        g_sessionLogged[i] = true;
        std::fprintf(stderr, "[adpf] session %s tid=%d target_ns=%" PRId64 " rc=%s\n", threadName(t), tid,
                     targetNs(), session ? "ok" : "FAILED");
    }
}

void maybeLog(uint64_t now)
{
    const uint64_t last = g_lastLogNs.load(std::memory_order_relaxed);
    if (now - last < 1000000000u)
        return;
    uint64_t expect = last;
    if (!g_lastLogNs.compare_exchange_strong(expect, now, std::memory_order_relaxed))
        return; // another reporting thread won this second
    char alive[64] = "";
    char lastUs[128] = "";
    char errs[64] = "";
    for (size_t i = 0; i < kThreadCount; ++i)
    {
        const Thread t = static_cast<Thread>(i);
        const bool up = g_sessions[i].load(std::memory_order_acquire) != nullptr;
        const uint64_t busyUs = g_lastBusyNs[i].load(std::memory_order_relaxed) / 1000u;
        const uint64_t err = g_reportErr[i].load(std::memory_order_relaxed);
        char cell[32];
        std::snprintf(cell, sizeof(cell), "%s%s:%d", i ? "," : "", threadName(t), up ? 1 : 0);
        std::strncat(alive, cell, sizeof(alive) - std::strlen(alive) - 1);
        std::snprintf(cell, sizeof(cell), "%s%s:%llu", i ? "," : "", threadName(t),
                      static_cast<unsigned long long>(busyUs));
        std::strncat(lastUs, cell, sizeof(lastUs) - std::strlen(lastUs) - 1);
        if (err)
        {
            std::snprintf(cell, sizeof(cell), "%s%s:%llu", errs[0] ? "," : "", threadName(t),
                          static_cast<unsigned long long>(err));
            std::strncat(errs, cell, sizeof(errs) - std::strlen(errs) - 1);
        }
    }
    std::fprintf(stderr, "[adpf] alive=%s last_us=%s target_ns=%" PRId64 " rate_ns=%" PRId64 "%s%s\n", alive,
                 lastUs, targetNs(), g_rateNs, errs[0] ? " report_err=" : "", errs[0] ? errs : "");
}

} // namespace

bool enabled()
{
    // Magic statics: thread-safe one-time parse (env is fixed before main).
    static const bool on = enabledFromEnv(std::getenv("PS2X_ADPF"));
    return on;
}

int64_t targetNs()
{
    static const int64_t ns = parseTargetNs(std::getenv("PS2X_ADPF_TARGET_MS"));
    return ns;
}

void noteThread(Thread t)
{
    const size_t i = static_cast<size_t>(t);
    if (i >= kThreadCount || !enabled())
        return;
    ensureInit();
    int expect = 0;
    g_tids[i].compare_exchange_strong(expect, static_cast<int>(::gettid()), std::memory_order_relaxed);
    const std::lock_guard<std::mutex> lock(g_mutex);
    ensureSessionLocked(t, monotonicNs());
}

void report(Thread t, uint64_t busyNs)
{
    const size_t i = static_cast<size_t>(t);
    if (i >= kThreadCount || !enabled())
        return;
    ensureInit();
    g_lastBusyNs[i].store(busyNs, std::memory_order_relaxed);
    void *session = g_sessions[i].load(std::memory_order_acquire);
    if (!session)
    {
        const std::lock_guard<std::mutex> lock(g_mutex);
        ensureSessionLocked(t, monotonicNs());
        session = g_sessions[i].load(std::memory_order_acquire);
    }
    if (session && shouldReport(busyNs))
    {
        const int rc = g_fns.reportActualWorkDuration(session, static_cast<int64_t>(busyNs));
        if (rc != 0)
            g_reportErr[i].fetch_add(1u, std::memory_order_relaxed);
    }
    maybeLog(monotonicNs());
}

} // namespace ps2x::adpf
