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
#include <cstring>

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

constexpr int64_t kDefaultTargetNs = 10'000'000; // ADPF target (the PS2X_ADPF_TARGET_MS override is deleted)

// PB9: reportActualWorkDuration rejects 0 ns (AD1 §5d: exact-40 mtvu errors
// across legs, plus one racy gsb). Zero-busy ticks carry no boost signal, so
// skip them instead of reporting. The suite covers the predicate.
inline bool shouldReport(uint64_t busyNs)
{
    return busyNs != 0;
}

// PW2: what the sessions report. Busy (the only mode; the
// PS2X_ADPF_REPORT=critical override is deleted): each thread's own
// busy time. Critical: every session reports the
// GameThread frame's critical path = ee.busy + MTVU syncs + GS enqueue waits
// (the frame's wall minus pacer, pause-gate and guest-idle event waits), so
// the governor lowers clocks while frames finish early and boosts all four
// threads when a frame nears the target. Busy time alone misses the late
// frames: at full-120 heavy they are busy ~5 ms + ~4 ms waiting on MTVU/GS.
enum class ReportMode : uint8_t
{
    Busy = 0,
    Critical,
};

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
// In Critical mode Game/Mtvu reports are dropped (reportFrame sends them)
// and GsWorker/GsBack report the latest frame critical time instead of busy.
void report(Thread t, uint64_t busyNs);
ReportMode reportMode();
// PW2: this tick's GameThread critical path (GameThread only, at the cut).
// Critical mode: reports it to the Game and Mtvu sessions and publishes it
// for the GS sessions. Busy mode: no-op.
void reportFrame(uint64_t criticalNs);
#else
inline bool enabled() { return false; }
inline int64_t targetNs() { return kDefaultTargetNs; }
inline void noteThread(Thread) {}
inline void report(Thread, uint64_t) {}
inline ReportMode reportMode() { return ReportMode::Busy; }
inline void reportFrame(uint64_t) {}
#endif

} // namespace ps2x::adpf
