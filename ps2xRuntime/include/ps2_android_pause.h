#pragma once

// BG1: Android pause-on-background gate (host-only; the guest cannot observe
// it). The EE (game) thread calls gate() at the top of VBlankStart processing
// — a proven safe point between guest vsyncs, before the pacer, the MTVU
// sync, the tick increment and the guest VSync callbacks. While paused the
// thread sleeps on a condition variable (no spinning); everything downstream
// idles on its own: MTVU and GsWorker drain then wait on their CVs, the
// audio stream is paused by the shell, and raylib's PollInputEvents blocks in
// the looper once the app loses focus. Guest time is frozen (no VBlank fires),
// and on resume the FP1 pacer resyncs (behind by more than two periods), so
// there is no catch-up burst; the engine's own drain bound would resync
// anyway (docs/research/ssx3-engine-timing.md S1 rules 2-3).
//
// Only the Android shell ever sets the flag (APP_CMD_PAUSE/RESUME in
// vk1OnAppCmd); elsewhere gate() is one relaxed load. Det mode is unaffected.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

namespace ps2x::androidPause
{

// PS2X_ANDROID_PAUSE_BG: default on; the exact value "0" keeps the pre-BG1
// behaviour (the guest keeps running while backgrounded). Read fresh per
// transition (transitions are rare; the value cannot change at runtime).
inline bool pauseOnBackgroundEnabled()
{
    const char *value = std::getenv("PS2X_ANDROID_PAUSE_BG");
    return value == nullptr || std::strcmp(value, "0") != 0;
}

inline std::atomic<bool> &pausedFlag()
{
    static std::atomic<bool> paused{false};
    return paused;
}

inline std::atomic<bool> &gateAckFlag()
{
    // Set by the game thread while it sleeps in the gate, so the pausing
    // thread can tell "guest execution stopped" from "still mid-frame".
    static std::atomic<bool> ack{false};
    return ack;
}

inline std::mutex &gateMutex()
{
    static std::mutex mutex;
    return mutex;
}

inline std::condition_variable &gateCv()
{
    static std::condition_variable cv;
    return cv;
}

inline bool paused()
{
    return pausedFlag().load(std::memory_order_acquire);
}

// Game thread (VBlankStart top). stop is the scheduler's stop flag: a stop
// requested while paused must still wake the thread so the join completes.
inline void gate(const std::atomic<bool> &stop)
{
    if (!paused())
        return;
    std::unique_lock<std::mutex> lock(gateMutex());
    // Re-arm the ack every pass: a pause/resume/pause faster than the wakeup
    // must not leave a stale ack=false while already inside the gate.
    while (paused() && !stop.load(std::memory_order_acquire))
    {
        gateAckFlag().store(true, std::memory_order_release);
        gateCv().wait(lock);
    }
    gateAckFlag().store(false, std::memory_order_release);
}

// Main thread. Returns true when this call engaged the pause (first
// transition); false when disabled or already paused.
inline bool requestPause()
{
    if (!pauseOnBackgroundEnabled())
        return false;
    if (pausedFlag().exchange(true, std::memory_order_acq_rel))
        return false;
    gateAckFlag().store(false, std::memory_order_release); // drop a stale ack from a fast pause/resume/pause
    return true;
}

// Main thread. Returns true when this call released the pause.
inline bool requestResume()
{
    if (!pausedFlag().exchange(false, std::memory_order_acq_rel))
        return false;
    {
        std::lock_guard<std::mutex> lock(gateMutex());
    }
    gateCv().notify_all();
    return true;
}

// Bounded wait for the game thread to reach the gate. False on timeout (the
// caller proceeds anyway: the flush RPC below is stream-ordered on the GS
// thread, and the gate engages whenever the game thread gets there).
inline bool waitGateAck(int timeoutMs)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!gateAckFlag().load(std::memory_order_acquire))
    {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// Scheduler stop path: wake a game thread sleeping in the gate.
inline void notifyStop()
{
    gateCv().notify_all();
}

} // namespace ps2x::androidPause
