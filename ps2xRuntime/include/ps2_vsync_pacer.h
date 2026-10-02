#pragma once

// FP1: wall-clock pacer for guest VBlank events.
//
// The EE scheduler fires VBlank on guest-cycle deadlines. Under
// PS2X_DETERMINISTIC=1 those deadlines ignore host time entirely
// (EeScheduler::processDueDeadlines, cycle-only branch), so a fast host runs
// the guest faster than a PS2 (F1 B2 menus: 1.287x). This pacer adds one
// wall-clock wait per guest vsync: next deadline = prev + 1/59.94 s; when
// ahead the executor sleeps until the deadline, when behind it runs without
// sleeping, and when behind by more than two frames the deadline resyncs to
// now + period instead of carrying a stale anchor.
//
// The pacer only inserts host sleeps: it never advances guest cycles, event
// order, or guest-visible state, so det-hash output is unchanged. Default
// on; PS2X_UNPACED=1 restores today's behaviour exactly (no sleep calls).

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace ps2_vsync_pacer
{

// Nominal guest vsync rate, NTSC 60000/1001 Hz. One vsync every 1001/60000 s
// = 16683333 ns; 1e9/16683333 = 59.94006 Hz.
constexpr int64_t kPeriodNs = 1000000000ll * 1001 / 60000;
constexpr int64_t kResyncBehindPeriods = 2;

class Pacer
{
public:
    explicit Pacer(int64_t periodNs = kPeriodNs)
        : m_period(periodNs)
    {
    }

    // Fake-clock-testable deadline step. nowNs is nanoseconds on any fixed
    // clock. Returns nanoseconds to sleep (0 = run now) and advances the
    // internal deadline. The first call anchors (deadline = now + period,
    // no sleep).
    [[nodiscard]] int64_t onVsync(int64_t nowNs)
    {
        if (m_nextNs < 0)
        {
            m_nextNs = nowNs + m_period;
            return 0;
        }
        if (nowNs < m_nextNs)
        {
            const int64_t sleepNs = m_nextNs - nowNs;
            m_nextNs += m_period;
            return sleepNs;
        }
        if (nowNs - m_nextNs > kResyncBehindPeriods * m_period)
        {
            m_nextNs = nowNs + m_period;
        }
        else
        {
            m_nextNs += m_period;
        }
        return 0;
    }

    [[nodiscard]] int64_t nextDeadlineNs() const
    {
        return m_nextNs;
    }

    [[nodiscard]] int64_t periodNs() const
    {
        return m_period;
    }

private:
    int64_t m_period;
    int64_t m_nextNs = -1;
};

inline bool unpacedFromEnv(const char *value)
{
    return value != nullptr && std::strcmp(value, "1") == 0;
}

// AT1: dev-only forced guest rates for audio experiments (fraction of full
// speed, e.g. 0.8). PS2X_HOST_PACE_RATE applies from boot; PS2X_HOST_PACE_RATE2
// from guest vsync tick PS2X_HOST_PACE_TICK2. Unset or invalid = FP1 behavior
// (cap at 1.0x). A set rate implies pacing on (wins over PS2X_UNPACED).
// Sleeps only; guest state and the det-hash are untouched.
struct HostPaceConfig
{
    bool enabled = false;
    double rate1 = 1.0;
    double rate2 = 1.0;
    uint64_t tick2 = 0;
    bool havePhase2 = false;
};

inline bool parsePaceRate(const char *value, double &rate)
{
    if (value == nullptr || *value == '\0')
        return false;
    char *end = nullptr;
    const double r = std::strtod(value, &end);
    if (end == value || *end != '\0' || !(r > 0.0) || !(r <= 2.0))
        return false;
    rate = r;
    return true;
}

inline HostPaceConfig hostPaceFromEnv(const char *rate1, const char *rate2, const char *tick2)
{
    HostPaceConfig config;
    if (!parsePaceRate(rate1, config.rate1))
        return config;
    config.enabled = true;
    double r2 = 1.0;
    if (parsePaceRate(rate2, r2) && tick2 != nullptr && *tick2 != '\0')
    {
        char *end = nullptr;
        const unsigned long long t2 = std::strtoull(tick2, &end, 10);
        if (end != tick2 && *end == '\0')
        {
            config.rate2 = r2;
            config.tick2 = static_cast<uint64_t>(t2);
            config.havePhase2 = true;
        }
    }
    return config;
}

inline HostPaceConfig hostPaceFromProcessEnv()
{
    return hostPaceFromEnv(
        std::getenv("PS2X_HOST_PACE_RATE"), std::getenv("PS2X_HOST_PACE_RATE2"),
        std::getenv("PS2X_HOST_PACE_TICK2"));
}

constexpr int64_t periodForRate(double rate)
{
    return static_cast<int64_t>(static_cast<double>(kPeriodNs) / rate);
}

inline bool unpacedFromProcessEnv()
{
    return unpacedFromEnv(std::getenv("PS2X_UNPACED"));
}

// Executor-side one-liner: sleep until this vsync's wall deadline when ahead.
// nowNs/sleepNs round-trip through the steady clock's epoch so the wait lands
// on the absolute deadline computed by Pacer::onVsync.
inline void sleepNsUntil(int64_t nowNs, int64_t sleepNs)
{
    if (sleepNs <= 0)
    {
        return;
    }
    using Clock = std::chrono::steady_clock;
    const Clock::time_point deadline =
        Clock::time_point{} + std::chrono::nanoseconds(nowNs + sleepNs);
    std::this_thread::sleep_until(deadline);
}

inline int64_t steadyNowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace ps2_vsync_pacer
