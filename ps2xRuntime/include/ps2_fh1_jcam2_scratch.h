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
};

inline LazyImage g_lazy;
inline struct sigaction g_oldSegv{}, g_oldBus{};

inline void chainSignal(int sig, siginfo_t *info, void *uc)
{
    const struct sigaction &old = sig == SIGBUS ? g_oldBus : g_oldSegv;
    if ((old.sa_flags & SA_SIGINFO) != 0 && old.sa_sigaction)
    {
        old.sa_sigaction(sig, info, uc);
        return;
    }
    if (old.sa_handler != SIG_DFL && old.sa_handler != SIG_IGN && old.sa_handler)
    {
        old.sa_handler(sig);
        return;
    }
    // Default action: restore it and return; the faulting access re-faults.
    signal(sig, SIG_DFL);
}

inline void faultHandler(int sig, siginfo_t *info, void *uc)
{
    LazyImage &z = g_lazy;
    const uintptr_t a = reinterpret_cast<uintptr_t>(info ? info->si_addr : nullptr);
    const uintptr_t b = reinterpret_cast<uintptr_t>(z.base);
    if (z.active.load(std::memory_order_acquire) && b && a >= b &&
        a < b + PS2_RAM_SIZE && pthread_equal(pthread_self(), z.owner))
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
                return;
            }
        }
    }
    chainSignal(sig, info, uc);
}

inline bool lazyInit()
{
    LazyImage &z = g_lazy;
    if (z.base) return true;
    if (z.failed) return false;
    const long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0 || (PS2_RAM_SIZE % static_cast<size_t>(ps)) != 0u) { z.failed = true; return false; }
    void *m = mmap(nullptr, PS2_RAM_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m == MAP_FAILED) { z.failed = true; return false; }
    z.page = static_cast<size_t>(ps);
    z.pages = PS2_RAM_SIZE / z.page;
    z.resident.assign(z.pages, 0u);
    z.fresh.assign(z.pages, 0u);
    struct sigaction sa{};
    sa.sa_sigaction = &faultHandler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, &g_oldSegv) != 0 || sigaction(SIGBUS, &sa, &g_oldBus) != 0)
    {
        munmap(m, PS2_RAM_SIZE);
        z.failed = true;
        return false;
    }
    z.base = static_cast<uint8_t *>(m);
    std::fprintf(stderr, "[jcam2-scratch] lazy image page=%zu pages=%zu\n", z.page, z.pages);
    return true;
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
#endif
} // namespace ps2_fh1_jcam2
