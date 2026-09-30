#pragma once

// PX1: lock guest VBlank to the display's refresh (PS2X_VSYNC_LOCK=1,
// default off). The Android analogue of PCSX2's SyncToHostRefreshRate +
// UseVSyncForTiming (VMManager::UpdateTargetSpeed: the target speed becomes
// host refresh / guest rate when that ratio is within 0.95..1.05, and vsync
// paces the frames).
//
// Why: the FP1 pacer runs the guest at 59.94/119.88 Hz on its own clock, but
// the Odin's "120 Hz" panel refreshes every 8.215 ms (121.7 Hz; SF --latency
// actual-present deltas, PX1). The two clocks beat: the phase of our posts
// against SurfaceFlinger's latch sweeps through a full period ~1.8 times a
// second, so every sweep repeats a frame (a 16.4 ms hold on screen), and
// posts near the latch point flip between two refreshes (a repeat plus a
// dropped frame).
//
// How: SurfaceFlinger's latch times (ASurfaceTransactionStats_getLatchTime,
// one per completed present) sample its wake grid. Tracker estimates the
// grid period (long-baseline fit over the last kRing latches) and anchor,
// and steers a phase so our posts land mid-period between two latches
// (circular mean of latch - post driven to P/2). pickSlot then gives the
// pacer a deadline on that grid every n refreshes (n = round(guest period /
// panel period): 1 for full-120, 2 for stock 60). A frame that misses its
// slot runs at once and the next one takes the following slot (slow as
// needed, no catch-up burst). With no fresh grid (no presents yet, paused,
// GL path) or a ratio outside 0.95..1.05 the FP1 pacer runs as before.
//
// Class: guest-affecting timing (wall-clock game speed follows the panel,
// +1.5 % on the Odin). Host sleeps only: guest cycles per VBlank, event order
// and det-hash output are unchanged (the det mode never reads host time).
// Pure logic below is fake-clock testable (ps2_vsync_lock_tests.cpp).

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace ps2_vsync_lock
{

inline bool enabledFromEnv(const char *value)
{
    return value != nullptr && std::strcmp(value, "1") == 0;
}

inline bool enabled()
{
    static const bool on = enabledFromEnv(std::getenv("PS2X_VSYNC_LOCK"));
    return on;
}

struct Grid
{
    bool valid = false;
    int64_t anchorNs = 0; // a latch-grid instant (fit intercept)
    double periodNs = 0.0;
    double phaseNs = 0.0; // VBlank slot offset from the latch grid, [0, period)
    int64_t lastLatchNs = 0;
};

class Tracker
{
public:
    static constexpr int kRing = 128;
    static constexpr int kMinSamples = 32;
    static constexpr int kPhaseBatch = 30;

    explicit Tracker(double nominalPeriodNs) : m_period(nominalPeriodNs), m_phase(nominalPeriodNs * 0.5) {}

    // One completed present: postNs = when we applied the transaction,
    // latchNs = when SurfaceFlinger latched it (same monotonic clock).
    void onLatch(int64_t postNs, int64_t latchNs)
    {
        if (latchNs <= 0)
            return;
        if (m_n > 0 && latchNs <= m_ring[(m_head + kRing - 1) % kRing])
            return; // same SF commit as the previous completion (or reordered)
        if (m_n > 0)
        {
            const double d = static_cast<double>(latchNs - m_ring[(m_head + kRing - 1) % kRing]);
            const double k = std::round(d / m_period);
            if (k >= 1.0 && k <= 8.0 && std::fabs(d / k - m_period) < 0.2 * m_period)
                m_period += (d / k - m_period) / 16.0;
        }
        m_ring[m_head] = latchNs;
        m_head = (m_head + 1) % kRing;
        if (m_n < kRing)
            ++m_n;
        ++m_accepted;
        refit(latchNs);
        if (postNs > 0 && latchNs > postNs)
            notePhase(static_cast<double>(latchNs - postNs));
    }

    Grid grid() const
    {
        Grid g;
        g.valid = m_accepted >= kMinSamples;
        g.anchorNs = m_anchor;
        g.periodNs = m_period;
        g.phaseNs = m_phase;
        g.lastLatchNs = m_n ? m_ring[(m_head + kRing - 1) % kRing] : 0;
        return g;
    }

    double lastSlackMeanNs() const { return m_lastSlackMean; }
    double lastResultant() const { return m_lastR; }
    uint64_t accepted() const { return m_accepted; }

private:
    // Long baseline: oldest vs newest latch in the ring gives the period to
    // ~jitter/span; the intercept is the mean residual over the ring.
    void refit(int64_t newest)
    {
        const int64_t oldest = m_ring[(m_head + kRing - m_n) % kRing];
        const double span = static_cast<double>(newest - oldest);
        const double K = std::round(span / m_period);
        if (K >= 16.0 && std::fabs(span - K * m_period) < 0.25 * m_period)
            m_period = span / K;
        double sum = 0.0;
        for (int i = 0; i < m_n; ++i)
        {
            const int64_t t = m_ring[(m_head + kRing - 1 - i) % kRing];
            const double off = static_cast<double>(t - newest);
            sum += off - std::round(off / m_period) * m_period;
        }
        m_anchor = newest + static_cast<int64_t>(std::llround(sum / m_n));
    }

    // Slack = latch - post. Posts just before a latch (slack ~0) and just
    // after one (slack ~P) are the same phase: average on the circle. Drive
    // the circular mean to P/2 by moving the VBlank slot (later slots make
    // later posts and smaller slack).
    void notePhase(double slackNs)
    {
        const double a = 2.0 * M_PI * std::fmod(slackNs, m_period) / m_period;
        m_sx += std::cos(a);
        m_sy += std::sin(a);
        if (++m_batch < kPhaseBatch)
            return;
        const double r = std::sqrt(m_sx * m_sx + m_sy * m_sy) / m_batch;
        double mean = std::atan2(m_sy, m_sx) / (2.0 * M_PI) * m_period;
        if (mean < 0.0)
            mean += m_period;
        m_lastSlackMean = mean;
        m_lastR = r;
        if (r >= 0.3)
        {
            double err = mean - 0.5 * m_period; // > 0: posts early, move the slot later
            m_phase = std::fmod(m_phase + 0.5 * err + 2.0 * m_period, m_period);
        }
        m_sx = m_sy = 0.0;
        m_batch = 0;
    }

    int64_t m_ring[kRing] = {};
    int m_head = 0;
    int m_n = 0;
    uint64_t m_accepted = 0;
    double m_period;
    double m_phase;
    int64_t m_anchor = 0;
    double m_sx = 0.0, m_sy = 0.0;
    int m_batch = 0;
    double m_lastSlackMean = 0.0;
    double m_lastR = 0.0;
};

// Pacer side. prevNs is the caller's last slot (-1 = none). Returns false
// (FP1 paces) when the grid is missing/stale or the rates are too far apart;
// otherwise sets sleepNs (0 = run now) and advances prevNs.
inline bool pickSlot(const Grid &g, int64_t guestPeriodNs, int64_t nowNs, int64_t &prevNs, int64_t &sleepNs,
                     int64_t staleNs = 500000000ll)
{
    if (!g.valid || g.periodNs <= 0.0 || nowNs - g.lastLatchNs > staleNs)
        return false;
    const double P = g.periodNs;
    const double n = std::round(static_cast<double>(guestPeriodNs) / P);
    if (n < 1.0)
        return false;
    const double ratio = static_cast<double>(guestPeriodNs) / (n * P);
    if (ratio < 0.95 || ratio > 1.05)
        return false;
    const double base = static_cast<double>(g.anchorNs) + g.phaseNs;
    auto slotNear = [&](double x) { return base + std::round((x - base) / P) * P; };
    const double now = static_cast<double>(nowNs);
    double target;
    if (prevNs < 0 || std::fabs(now - static_cast<double>(prevNs)) > 4.0 * n * P)
    {
        target = base + std::ceil((now - base) / P) * P; // next slot
    }
    else
    {
        const double prev = static_cast<double>(prevNs);
        target = slotNear(prev + n * P);
        if (target - prev < 0.5 * P)
            target += P;
    }
    if (target > now)
    {
        sleepNs = static_cast<int64_t>(std::llround(target - now));
        prevNs = static_cast<int64_t>(std::llround(target));
    }
    else
    {
        sleepNs = 0; // missed its slot: run now, the next frame takes the following one
        prevNs = static_cast<int64_t>(std::llround(base + std::floor((now - base) / P) * P));
    }
    return true;
}

// Process-wide instance: completions feed it (present thread), the EE
// scheduler asks it for slots. Only touched when enabled().
struct Shared
{
    std::mutex m;
    Tracker tracker{1e9 / 120.0};
    int64_t prevNs = -1; // EE thread's last slot
    uint64_t locked = 0, late = 0, unlocked = 0;
    uint64_t latches = 0;
};

inline Shared &shared()
{
    static Shared *s = new Shared(); // never destroyed: completions can arrive during exit
    return *s;
}

inline void noteLatch(int64_t postNs, int64_t latchNs)
{
    if (!enabled())
        return;
    Shared &s = shared();
    std::lock_guard<std::mutex> lock(s.m);
    s.tracker.onLatch(postNs, latchNs);
    if (++s.latches % 1200u == 0u)
    {
        const Grid g = s.tracker.grid();
        std::fprintf(stderr,
                     "[vsync-lock] latches=%llu valid=%d period_ms=%.4f hz=%.3f phase_ms=%.3f slack_mean_ms=%.3f "
                     "R=%.2f locked=%llu late=%llu unlocked=%llu\n",
                     static_cast<unsigned long long>(s.latches), g.valid ? 1 : 0, g.periodNs / 1e6, 1e9 / g.periodNs,
                     g.phaseNs / 1e6, s.tracker.lastSlackMeanNs() / 1e6, s.tracker.lastResultant(),
                     static_cast<unsigned long long>(s.locked), static_cast<unsigned long long>(s.late),
                     static_cast<unsigned long long>(s.unlocked));
    }
}

// EE thread, once per VBlankStart. True = locked (sleepNs set).
inline bool pace(int64_t guestPeriodNs, int64_t nowNs, int64_t &sleepNs)
{
    Shared &s = shared();
    std::lock_guard<std::mutex> lock(s.m);
    const bool ok = pickSlot(s.tracker.grid(), guestPeriodNs, nowNs, s.prevNs, sleepNs);
    if (!ok)
    {
        s.prevNs = -1;
        ++s.unlocked;
        return false;
    }
    ++s.locked;
    if (sleepNs == 0)
        ++s.late;
    return true;
}

} // namespace ps2_vsync_lock
