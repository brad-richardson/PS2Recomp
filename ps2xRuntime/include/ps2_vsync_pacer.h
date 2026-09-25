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
