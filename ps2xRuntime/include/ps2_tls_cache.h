#ifndef PS2_TLS_CACHE_H
#define PS2_TLS_CACHE_H

// TLS2: cached hot-thread TLS addresses (PRF2 #4, NRS1 Part 2).
//
// [linker]tlsdesc_resolver_dynamic is ~11% self of MTVU-GIF on the Odin:
// bionic resolves every thread_local access in the dlopen'd runner through
// the TLS descriptor. TRM1 piece 4 tried initial-exec for the 14 hot
// thread-locals; bionic refuses IE TLS in a dlopen'd library, so it was
// reverted. This lane caches instead: with PS2X_TLS_CACHE=1 (default off),
// the GIF stage thread resolves the hot thread-locals' addresses ONCE at
// thread entry, and hot paths use the cached pointers.
//
// Exact: the cache holds pointers TO the thread's own TLS objects (same
// objects, same values); any thread but the noted one, or knob off, uses
// the thread-locals directly as today. Dispatch is one get_id + relaxed
// loads (the TT1 pattern), never TLS, never pthread_getspecific.
//
// Validity: the cache is only dereferenced while the current thread is the
// live noted GIF thread (thread ids are unique among live threads, and the
// g_gifTid note is cleared at GIF thread exit), so a restarted thread always
// re-resolves at its entry before any hot use. Each TU's cache struct is
// therefore effectively single-threaded.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ps2_tls_cache
{
inline std::atomic<bool> &enabledFlag()
{
    static std::atomic<bool> flag{false};
    return flag;
}
inline bool enabled()
{
    return enabledFlag().load(std::memory_order_relaxed);
}
// Parsed per GIF-stage start (the GSW2 precedent), so tests can toggle the
// env between starts. Production starts the stage once per run.
inline void configureFromEnv()
{
    const char *env = std::getenv("PS2X_TLS_CACHE");
    const bool on = env && std::strcmp(env, "1") == 0;
    enabledFlag().store(on, std::memory_order_relaxed);
    if (on)
        std::fprintf(stderr,
                     "[tls-cache] PS2X_TLS_CACHE=1: hot TLS cached on the GIF stage thread\n");
}
// Tests flip the flag directly (the suite shares one process; restore after).
inline void setForTest(bool on)
{
    enabledFlag().store(on, std::memory_order_relaxed);
}

// Per-TU note: resolve this thread's hot TLS addresses into the TU's cache.
// Called once, on the GIF stage thread, at its entry (knob on only).
void noteGsWorkerCache();   // defined in gs_worker.cpp
void noteGsFrontendCache(); // defined in gs_frontend.cpp
} // namespace ps2_tls_cache

#endif
