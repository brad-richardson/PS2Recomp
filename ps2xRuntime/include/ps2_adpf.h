#pragma once
// AD1: Android ADPF performance hints for the hot threads. Each hot thread
// (GameThread, MTVU, GS worker, GS back) gets one hint session bound to its
// own TID; every guest vsync tick the owning accounting reports that tick's
// busy time, and the governor boosts the thread toward the target. Android
// only: ADPF is API 33+ and minSdk is 29, so the .cpp resolves every symbol
// with dlsym (never called through a null pointer; missing manager or a
// failed createSession degrades to no-hint logging). The header compiles out
// off Android (inline no-ops) so shared call sites build everywhere.
//
// Timing-only, host-side: the guest can't observe it. With PS2X_ADPF unset
// (default) enabled() is false and every call site is one cached branch.

#include <cstdint>
#include <cstdlib>

namespace ps2x::adpf
{

enum class Thread : uint8_t
{
    Game = 0, // GameThread (EeScheduler executor): ee.busy per tick
    Mtvu,     // MTVU worker: mtvu.busy per tick (reported at vblank)
    GsWorker, // GS worker: gs.busy per GuestVsync
    GsBack,   // GE1 back thread: gsback.busy per GuestVsync
    Count
};

constexpr size_t kThreadCount = static_cast<size_t>(Thread::Count);

inline const char *threadName(Thread t)
{
    switch (t)
    {
    case Thread::Game:
        return "game";
    case Thread::Mtvu:
        return "mtvu";
    case Thread::GsWorker:
        return "gsw";
    case Thread::GsBack:
        return "gsb";
    default:
        return "?";
    }
}

inline bool enabledFromEnv(const char *value)
{
    return value != nullptr && value[0] == '1' && value[1] == '\0';
}

constexpr int64_t kDefaultTargetNs = 10'000'000; // PS2X_ADPF_TARGET_MS default

// PB9: reportActualWorkDuration rejects 0 ns (AD1 §5d: exact-40 mtvu errors
// across legs, plus one racy gsb). Zero-busy ticks carry no boost signal, so
// skip them instead of reporting. The suite covers the predicate.
inline bool shouldReport(uint64_t busyNs)
{
    return busyNs != 0;
}

// Parse PS2X_ADPF_TARGET_MS (decimal ms) into ns. Missing, unparseable or
// non-positive input reads the default; the suite covers the edges.
inline int64_t parseTargetNs(const char *value)
{
    if (value == nullptr || value[0] == '\0')
        return kDefaultTargetNs;
    char *end = nullptr;
    const double ms = std::strtod(value, &end);
    if (end == value || *end != '\0' || !(ms > 0.0))
        return kDefaultTargetNs;
    const double ns = ms * 1e6;
    if (!(ns > 0.0) || ns >= 9.0e18)
        return kDefaultTargetNs;
    return static_cast<int64_t>(ns);
}

#if defined(__ANDROID__)
// Cached knob reads (env is fixed before main; parsed once each).
bool enabled();
int64_t targetNs();
// Record the calling thread's TID for session `t` (call once on the owning
// thread at startup; GsBack is discovered via /proc instead, see the .cpp).
void noteThread(Thread t);
// Report this tick's busy time (ns, steady-clock sourced like the PT2 rings).
// No-op unless enabled; safe from any thread (each session is reported from
// exactly one thread: Game+Mtvu from GameThread at vblank, GsWorker+GsBack
// from the GS worker at GuestVsync). Throttled [adpf] line once a second.
void report(Thread t, uint64_t busyNs);
#else
inline bool enabled() { return false; }
inline int64_t targetNs() { return kDefaultTargetNs; }
inline void noteThread(Thread) {}
inline void report(Thread, uint64_t) {}
#endif

} // namespace ps2x::adpf
