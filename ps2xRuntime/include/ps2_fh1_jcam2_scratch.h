// EEP1: lazily filled private RAM image for the FH32 jcam2 camera preview.
//
// jcam2Service runs the stock air-predictor solver on a private copy of guest
// RAM. The original copied all 32 MiB per service: in a race at 120 every
// airborne rider is serviced once per stock interval, which made the copy the
// largest single GameThread item in Ruthless Ridge (EEP1 profile).
//
// Here the image is one persistent mapping that starts PROT_NONE. Pages the
// preview has ever touched (the footprint) are re-copied from live RAM at
// the start of each service; the first touch of any other page faults, and
// the GameThread's handler copies that page from live RAM, opens it and adds
// it to the footprint. Every page the preview reads or writes therefore holds
// the bytes the full copy would have held; untouched pages are never
// observed. The live image is never protected, so no other thread can fault.
//
// PS2X_SSX3_JCAM2_SCRATCH=full keeps the original whole-RAM copy (exact
// reference). Platforms without POSIX signals always use the full copy.
#pragma once

#include "runtime/ps2_memory.h"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
#define PS2X_JCAM2_LAZY_SUPPORTED 1
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#if defined(__APPLE__)
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif
#include <unistd.h>
#else
#define PS2X_JCAM2_LAZY_SUPPORTED 0
#endif

namespace ps2_fh1_jcam2
{
inline bool lazyRequested() noexcept
{
    static const bool on = []
    {
        const char *v = std::getenv("PS2X_SSX3_JCAM2_SCRATCH");
        return !(v && std::strcmp(v, "full") == 0);
    }();
    return on;
}

#if PS2X_JCAM2_LAZY_SUPPORTED
struct LazyImage
{
    uint8_t *base = nullptr;
    size_t page = 0u, pages = 0u;
    std::vector<uint8_t> resident;     // 1 = open and in the footprint
    std::vector<uint32_t> fresh;       // pages opened by the handler this service
    std::atomic<uint32_t> freshCount{0u};
    std::vector<uint32_t> runs;        // footprint as (first, count) pairs
    const uint8_t *live = nullptr;
    pthread_t owner{};
    std::atomic<bool> active{false};
    uint64_t services = 0u, faults = 0u;
    bool failed = false;
    bool installedSegv = false, installedBus = false;
    void *altMemory = nullptr;
    size_t altSize = 0u;
    pthread_t altOwner{};
};

inline LazyImage g_lazy;
inline struct sigaction g_oldSegv{}, g_oldBus{};
inline void faultHandler(int sig, siginfo_t *info, void *uc);
static_assert(std::atomic<bool>::is_always_lock_free && std::atomic<uint32_t>::is_always_lock_free,
              "jcam2 fault-handler atomics must be lock-free");
inline std::atomic<bool> g_resetSegv{false}, g_resetBus{false};

inline bool isOurAction(const struct sigaction &sa)
{
    return (sa.sa_flags & SA_SIGINFO) && sa.sa_sigaction == &faultHandler;
}

inline struct sigaction defaultAction()
{
    struct sigaction sa{};
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    return sa;
}

inline struct sigaction ourAction()
{
    struct sigaction sa{};
    sa.sa_sigaction = &faultHandler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    return sa;
}

inline void chainSignal(int sig, siginfo_t *info, void *uc)
{
    const struct sigaction &old = sig == SIGBUS ? g_oldBus : g_oldSegv;
    // The sentinels share storage with sa_sigaction on POSIX: inspect them
    // before deciding whether this is a three-argument action.
    if (old.sa_handler == SIG_IGN) return;
    const bool reset = (old.sa_flags & SA_RESETHAND) &&
        (sig == SIGBUS ? g_resetBus : g_resetSegv).exchange(true, std::memory_order_relaxed);
    if (old.sa_handler == SIG_DFL || reset)
    {
        const struct sigaction dfl = defaultAction();
        // A synchronous fault must terminate with the original signal. The
        // current handler normally masks it, so unblock it before re-raising.
        sigaction(sig, &dfl, nullptr);
        sigset_t one{};
        sigemptyset(&one);
        sigaddset(&one, sig);
        sigprocmask(SIG_UNBLOCK, &one, nullptr);
        raise(sig);
        _exit(128 + sig); // only reachable if a platform refuses the signal
    }
    // SA_RESETHAND resets the kernel disposition before entering the saved
    // action. Keep our wrapper only after that action returns, so it can
    // continue servicing image pages while later unrelated faults see DFL.
    const bool oneShot = (old.sa_flags & SA_RESETHAND) != 0;
    if (oneShot)
    {
        struct sigaction now{};
        if (sigaction(sig, nullptr, &now) == 0 && isOurAction(now))
        {
            const struct sigaction dfl = defaultAction();
            sigaction(sig, &dfl, nullptr);
        }
    }
    sigset_t current{}, during = old.sa_mask;
    sigprocmask(SIG_SETMASK, nullptr, &current);
    // The ucontext contains the mask before our handler's automatic signal
    // blocking. Apply the saved action to that mask, including SA_NODEFER.
    const sigset_t &before = uc ? static_cast<ucontext_t *>(uc)->uc_sigmask : current;
    for (int n = 1; n < NSIG; ++n)
        if (sigismember(&before, n) == 1) sigaddset(&during, n);
    if (old.sa_flags & SA_NODEFER) sigdelset(&during, sig);
    else sigaddset(&during, sig);
    sigprocmask(SIG_SETMASK, &during, nullptr);
    if (old.sa_flags & SA_SIGINFO)
        old.sa_sigaction(sig, info, uc);
    else if (old.sa_handler)
        old.sa_handler(sig);
    sigprocmask(SIG_SETMASK, &current, nullptr);
    if (oneShot)
    {
        struct sigaction now{};
        if (sigaction(sig, nullptr, &now) == 0 && now.sa_handler == SIG_DFL)
        {
            const struct sigaction ours = ourAction();
            sigaction(sig, &ours, nullptr);
        }
    }
}

inline bool isExecuteFault(void *uc, uintptr_t b)
{
    if (!uc) return true; // do not open an unknown access type
    const auto *ctx = static_cast<ucontext_t *>(uc);
#if defined(__APPLE__) && defined(__aarch64__)
    const uintptr_t pc = static_cast<uintptr_t>(ctx->uc_mcontext->__ss.__pc);
#elif defined(__linux__) && defined(__aarch64__)
    const uintptr_t pc = static_cast<uintptr_t>(ctx->uc_mcontext.pc);
#elif defined(__APPLE__) && defined(__x86_64__)
    const uintptr_t pc = static_cast<uintptr_t>(ctx->uc_mcontext->__ss.__rip);
#elif defined(__linux__) && defined(__x86_64__)
    const uintptr_t pc = static_cast<uintptr_t>(ctx->uc_mcontext.gregs[REG_RIP]);
#else
    return true;
#endif
    return pc >= b && pc < b + PS2_RAM_SIZE;
}

inline bool isProtectionFault(int sig, const siginfo_t *info)
{
    if (!info) return false;
#if defined(__APPLE__)
    // EEP2 child probe on Darwin arm64: a PROT_NONE read reports SIGBUS,
    // si_code=1 (KERN_PROTECTION_FAILURE), despite signal.h naming 1
    // BUS_ADRALN. The image range/page/owner checks below still apply.
    return sig == SIGBUS && info->si_code == 1;
#else
    return sig == SIGSEGV && info->si_code == SEGV_ACCERR;
#endif
}

inline void faultHandler(int sig, siginfo_t *info, void *uc)
{
    const int savedErrno = errno;
    LazyImage &z = g_lazy;
    const uintptr_t a = reinterpret_cast<uintptr_t>(info ? info->si_addr : nullptr);
    const uintptr_t b = reinterpret_cast<uintptr_t>(z.base);
    if (isProtectionFault(sig, info) &&
        z.active.load(std::memory_order_acquire) && b && a >= b &&
        a < b + PS2_RAM_SIZE && !isExecuteFault(uc, b) &&
        pthread_equal(pthread_self(), z.owner))
    {
        const size_t p = (a - b) / z.page;
        if (!z.resident[p])
        {
            uint8_t *dst = z.base + p * z.page;
            if (mprotect(dst, z.page, PROT_READ | PROT_WRITE) == 0)
            {
                std::memcpy(dst, z.live + p * z.page, z.page);
                z.resident[p] = 1u;
                const uint32_t n = z.freshCount.load(std::memory_order_relaxed);
                z.fresh[n] = static_cast<uint32_t>(p);
                z.freshCount.store(n + 1u, std::memory_order_relaxed);
                errno = savedErrno;
                return;
            }
        }
    }
    chainSignal(sig, info, uc);
    errno = savedErrno;
}

using SigactionFn = int (*)(int, const struct sigaction *, struct sigaction *);
inline void restoreOwned(int sig, const struct sigaction &old)
{
    struct sigaction now{};
    if (sigaction(sig, nullptr, &now) == 0 && isOurAction(now))
        sigaction(sig, &old, nullptr);
}

inline bool prepareAltStack(LazyImage &z)
{
    stack_t current{};
    if (sigaltstack(nullptr, &current) != 0) return false;
    const size_t need = static_cast<size_t>(SIGSTKSZ) > 65536u ? static_cast<size_t>(SIGSTKSZ) : 65536u;
    if (!(current.ss_flags & SS_DISABLE))
        return current.ss_sp && current.ss_size >= need;
    void *memory = mmap(nullptr, need, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (memory == MAP_FAILED) return false;
    stack_t ours{};
    ours.ss_sp = memory;
    ours.ss_size = need;
    if (sigaltstack(&ours, nullptr) != 0)
    {
        munmap(memory, need);
        return false;
    }
    z.altMemory = memory;
    z.altSize = need;
    z.altOwner = pthread_self();
    return true;
}

inline void releaseAltStack(LazyImage &z)
{
    if (!z.altMemory) return;
    if (!pthread_equal(pthread_self(), z.altOwner)) return; // retain live stack on its owner thread
    stack_t current{};
    if (sigaltstack(nullptr, &current) == 0 && current.ss_sp == z.altMemory)
    {
        stack_t disabled{};
        disabled.ss_flags = SS_DISABLE;
        if (sigaltstack(&disabled, nullptr) == 0)
        {
            munmap(z.altMemory, z.altSize);
            z.altMemory = nullptr;
            z.altSize = 0u;
        }
    }
}

inline bool lazyInit(SigactionFn installAction = ::sigaction)
{
    LazyImage &z = g_lazy;
    if (z.failed) return false;
    if (z.base) return pthread_equal(pthread_self(), z.owner);
    const long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0 || (PS2_RAM_SIZE % static_cast<size_t>(ps)) != 0u) { z.failed = true; return false; }
    void *m = mmap(nullptr, PS2_RAM_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m == MAP_FAILED) { z.failed = true; return false; }
    if (!prepareAltStack(z))
    {
        munmap(m, PS2_RAM_SIZE);
        z.failed = true;
        return false;
    }
    z.page = static_cast<size_t>(ps);
    z.pages = PS2_RAM_SIZE / z.page;
    z.resident.assign(z.pages, 0u);
    z.fresh.assign(z.pages, 0u);
    const struct sigaction sa = ourAction();
    g_resetSegv.store(false, std::memory_order_relaxed);
    g_resetBus.store(false, std::memory_order_relaxed);
    if (installAction(SIGSEGV, &sa, &g_oldSegv) != 0)
    {
        munmap(m, PS2_RAM_SIZE);
        releaseAltStack(z);
        z.failed = true;
        return false;
    }
    z.installedSegv = true;
    if (installAction(SIGBUS, &sa, &g_oldBus) != 0)
    {
        restoreOwned(SIGSEGV, g_oldSegv);
        struct sigaction now{};
        z.installedSegv = sigaction(SIGSEGV, nullptr, &now) == 0 && isOurAction(now);
        munmap(m, PS2_RAM_SIZE);
        releaseAltStack(z);
        z.failed = true;
        return false;
    }
    z.installedBus = true;
    z.owner = pthread_self();
    z.base = static_cast<uint8_t *>(m);
    std::fprintf(stderr, "[jcam2-scratch] lazy image page=%zu pages=%zu\n", z.page, z.pages);
    return true;
}

inline void lazyShutdown()
{
    LazyImage &z = g_lazy;
    z.active.store(false, std::memory_order_release);
    const struct sigaction dfl = defaultAction();
    if (z.installedBus) restoreOwned(SIGBUS, g_resetBus.load() ? dfl : g_oldBus);
    if (z.installedSegv) restoreOwned(SIGSEGV, g_resetSegv.load() ? dfl : g_oldSegv);
    z.installedBus = z.installedSegv = false;
    if (z.base) munmap(z.base, PS2_RAM_SIZE);
    z.base = nullptr;
    z.live = nullptr;
    z.resident.clear();
    z.fresh.clear();
    z.runs.clear();
    z.freshCount.store(0u, std::memory_order_relaxed);
    z.services = z.faults = 0u;
    z.failed = false;
    releaseAltStack(z);
}

// Opens the service: re-copies the footprint from live and arms the handler.
inline uint8_t *lazyBegin(const uint8_t *live)
{
    if (!lazyInit()) return nullptr;
    LazyImage &z = g_lazy;
    for (size_t i = 0; i + 1u < z.runs.size(); i += 2u)
    {
        const size_t off = static_cast<size_t>(z.runs[i]) * z.page;
        std::memcpy(z.base + off, live + off, static_cast<size_t>(z.runs[i + 1u]) * z.page);
    }
    z.live = live;
    z.owner = pthread_self();
    z.freshCount.store(0u, std::memory_order_relaxed);
    z.active.store(true, std::memory_order_release);
    return z.base;
}

// Closes the service: disarms the handler and folds newly opened pages into
// the footprint runs.
inline void lazyEnd()
{
    LazyImage &z = g_lazy;
    z.active.store(false, std::memory_order_release);
    ++z.services;
    const uint32_t n = z.freshCount.load(std::memory_order_relaxed);
    if (n)
    {
        z.faults += n;
        z.runs.clear();
        for (size_t p = 0; p < z.pages;)
        {
            if (!z.resident[p]) { ++p; continue; }
            size_t q = p;
            while (q < z.pages && z.resident[q]) ++q;
            z.runs.push_back(static_cast<uint32_t>(p));
            z.runs.push_back(static_cast<uint32_t>(q - p));
            p = q;
        }
    }
    // Bounded log: when the footprint grows by a quarter, and at power-of-two
    // service counts (at most 96 lines per process).
    static uint32_t lines = 0u;
    static size_t logged = 0u;
    size_t open = 0u;
    for (size_t i = 1; i < z.runs.size(); i += 2u) open += z.runs[i];
    const bool grew = open * 4u >= logged * 5u && open != logged;
    const bool pow2 = (z.services & (z.services - 1u)) == 0u;
    if (lines < 96u && (grew || pow2))
    {
        ++lines;
        logged = open;
        std::fprintf(stderr, "[jcam2-scratch] service=%llu faults=%llu footprint_pages=%zu runs=%zu bytes=%zu\n",
                     static_cast<unsigned long long>(z.services), static_cast<unsigned long long>(z.faults),
                     open, z.runs.size() / 2u, open * z.page);
    }
}
#else
inline uint8_t *lazyBegin(const uint8_t *) { return nullptr; }
inline void lazyEnd() {}
inline void lazyShutdown() {}
#endif
} // namespace ps2_fh1_jcam2
