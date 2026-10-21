#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>

// Host producer, guest FE consumer. Course/venue resolution belongs to the
// picker; the FE transition only accepts the stock event configuration.
namespace ps2_ssx3_fe_launch
{
struct Request
{
    uint32_t event = 0, type = 0, rules = 0, context = 0;
    // tk-fe-2: picker-session generation stamped at park. The FE consumer
    // drops requests from a closed session instead of firing them later.
    uint64_t session = 0;
};
struct Mailbox
{
    std::mutex mutex;
    Request args;
    bool pending = false, active = false, loading = false, playing = false;
    uint64_t serial = 0, injectedPresses = 0, injectedSamples = 0, menuBuilds = 0;
    uint16_t previousMask = 0;
    std::chrono::steady_clock::time_point requested;
};
inline Mailbox &mailbox() { static Mailbox m; return m; }

inline bool request(uint32_t event, uint32_t type, uint32_t rules, uint32_t context, uint64_t session)
{
    auto &m = mailbox();
    std::lock_guard<std::mutex> lock(m.mutex);
    // tk-fe-2 (Brad multi-tap): a re-tap while the request is still parked
    // (unconsumed) supersedes it — newest tap wins — so tapping Race then
    // Showoff can never stack two launches. Once the FE boundary consumes
    // the request the native transition owns it and a new tap is refused.
    if (m.active && !m.pending) return false;
    if (m.pending)
    {
        // logLocked is defined below; inline the supersede line here.
        std::fprintf(stderr, "[fe-launch] id=%llu stage=superseded-before-consume event=%u type=%u rules=%u\n",
                     static_cast<unsigned long long>(m.serial), m.args.event, m.args.type, m.args.rules);
    }
    m.args = {event, type, rules, context, session};
    m.active = m.pending = true;
    m.loading = m.playing = false;
    ++m.serial;
    m.injectedPresses = m.injectedSamples = m.menuBuilds = 0;
    m.previousMask = 0;
    m.requested = std::chrono::steady_clock::now();
    return true;
}
// tk-fe-2: the FE boundary consumes only requests stamped with the live
// picker session. A request parked by a closed session (tap, back out,
// re-enter) is dropped here — never fired by a later accept — so stale
// venue/audio words can never leak into the next launch.
inline bool take(Request &out, uint64_t liveSession)
{
    auto &m = mailbox();
    std::lock_guard<std::mutex> lock(m.mutex);
    if (!m.pending) return false;
    if (m.args.session != liveSession)
    {
        std::fprintf(stderr,
                     "[fe-launch] id=%llu stage=stale-session-drop event=%u type=%u rules=%u "
                     "request_session=%llu live_session=%llu\n",
                     static_cast<unsigned long long>(m.serial), m.args.event, m.args.type, m.args.rules,
                     static_cast<unsigned long long>(m.args.session),
                     static_cast<unsigned long long>(liveSession));
        m.active = m.pending = false;
        return false;
    }
    out = m.args;
    m.pending = false;
    return true;
}
inline void injected(uint16_t mask)
{
    auto &m = mailbox();
    std::lock_guard<std::mutex> lock(m.mutex);
    if (!m.active) return;
    uint16_t edges = static_cast<uint16_t>(mask & ~m.previousMask);
    for (; edges; edges &= static_cast<uint16_t>(edges - 1)) ++m.injectedPresses;
    if (mask) ++m.injectedSamples;
    m.previousMask = mask;
}
inline void logLocked(const Mailbox &m, const char *stage, uint64_t tick)
{
    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - m.requested).count();
    std::fprintf(stderr, "[fe-launch] id=%llu tick=%llu stage=%s elapsed_ms=%.3f "
                 "event=%u type=%u rules=%u context=%u injected_presses=%llu "
                 "injected_samples=%llu main_menu_builds=%llu\n",
                 static_cast<unsigned long long>(m.serial), static_cast<unsigned long long>(tick), stage, ms,
                 m.args.event, m.args.type, m.args.rules, m.args.context,
                 static_cast<unsigned long long>(m.injectedPresses),
                 static_cast<unsigned long long>(m.injectedSamples),
                 static_cast<unsigned long long>(m.menuBuilds));
}
inline void note(const char *stage, uint64_t tick)
{
    auto &m = mailbox();
    std::lock_guard<std::mutex> lock(m.mutex);
    if (m.active) logLocked(m, stage, tick);
}
// Cancellation and guest consumption compete under the same lock. Once
// consumed, the native transition owns the request and cannot be withdrawn.
inline bool cancelPending(uint64_t tick)
{
    auto &m = mailbox();
    std::lock_guard<std::mutex> lock(m.mutex);
    if (!m.pending) return false;
    logLocked(m, "cancelled-before-consume", tick);
    m.active = m.pending = false;
    return true;
}
inline void loading(uint64_t tick)
{
    auto &m = mailbox();
    std::lock_guard<std::mutex> lock(m.mutex);
    if (!m.active || m.loading) return;
    m.loading = true;
    logLocked(m, "loading-transition-update", tick);
}
inline void playing(uint64_t tick)
{
    auto &m = mailbox();
    std::lock_guard<std::mutex> lock(m.mutex);
    if (!m.active || m.playing) return;
    m.playing = true;
    logLocked(m, "game-update", tick);
}
inline void menuBuild(uint64_t tick)
{
    auto &m = mailbox();
    std::lock_guard<std::mutex> lock(m.mutex);
    if (!m.active) return;
    ++m.menuBuilds;
    if (m.menuBuilds == 1) logLocked(m, "main-menu-build", tick);
}
// Reuse the existing lane screenshot knob for the tap-to-load interval.
inline bool captureWindow()
{
    auto &m = mailbox();
    std::lock_guard<std::mutex> lock(m.mutex);
    return m.active && std::chrono::steady_clock::now() - m.requested < std::chrono::seconds(3);
}
inline void finish(uint64_t tick)
{
    auto &m = mailbox();
    std::lock_guard<std::mutex> lock(m.mutex);
    if (m.active) logLocked(m, "event-exit", tick);
    m.active = m.pending = false;
}
} // namespace ps2_ssx3_fe_launch
