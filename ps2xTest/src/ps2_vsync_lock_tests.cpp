#include "MiniTest.h"
#include "ps2_vsync_lock.h"
#include "ps2_vsync_pacer.h"

#include <cmath>
#include <cstdint>
#include <string>

namespace
{
    // The Odin's measured panel period (PX1: SF actual-present deltas).
    constexpr double kPanel = 8215000.0;
    constexpr int64_t kGuest120 = ps2_vsync_pacer::kPeriodNs / 2; // 119.88 Hz

    // Deterministic latch jitter in [0, 400) us (SF commit latency after its wake).
    int64_t jitter(int i)
    {
        return static_cast<int64_t>((static_cast<uint32_t>(i) * 2654435761u >> 8) % 400000u);
    }

    int64_t wake(int i) { return 1000000000ll + static_cast<int64_t>(std::llround(i * kPanel)); }

    // First SF latch at or after a post: the next wake + its jitter.
    int64_t latchFor(int64_t postNs, int &iOut)
    {
        int i = static_cast<int>(std::ceil((postNs - 1000000000.0) / kPanel));
        if (wake(i) < postNs)
            ++i;
        iOut = i;
        return wake(i) + jitter(i);
    }
}

void register_ps2_vsync_lock_tests()
{
    MiniTest::Case("Ps2VsyncLock", [](TestCase &tc)
    {
        tc.Run("knob is exactly 1", [](TestCase &t)
        {
            t.IsTrue(ps2_vsync_lock::enabledFromEnv("1"), "1 enables");
            t.IsTrue(!ps2_vsync_lock::enabledFromEnv(nullptr), "unset is off");
            t.IsTrue(!ps2_vsync_lock::enabledFromEnv("0"), "0 is off");
            t.IsTrue(!ps2_vsync_lock::enabledFromEnv("on"), "other text is off");
        });

        tc.Run("tracker learns a 121.7 Hz grid from a 120 Hz start", [](TestCase &t)
        {
            ps2_vsync_lock::Tracker tr(1e9 / 120.0);
            for (int i = 0; i < 300; ++i)
                tr.onLatch(0, wake(i) + jitter(i));
            const ps2_vsync_lock::Grid g = tr.grid();
            t.IsTrue(g.valid, "valid after the minimum sample count");
            t.IsTrue(std::fabs(g.periodNs - kPanel) < 5000.0,
                     "period within 5 us of 8.215 ms, got " + std::to_string(g.periodNs));
            // The anchor sits on the grid (+ mean jitter ~200 us).
            const double off = std::fmod(static_cast<double>(g.anchorNs - wake(0)), kPanel);
            const double r = off > kPanel / 2 ? off - kPanel : off;
            t.IsTrue(std::fabs(r - 200000.0) < 250000.0, "anchor on the grid, residual " + std::to_string(r));
        });

        tc.Run("same-commit completions and junk latches are ignored", [](TestCase &t)
        {
            ps2_vsync_lock::Tracker tr(1e9 / 120.0);
            tr.onLatch(0, wake(0));
            tr.onLatch(0, wake(0)); // two transactions latched in one SF commit
            tr.onLatch(0, 0);       // no latch time
            tr.onLatch(0, -1);
            t.Equals(tr.accepted(), static_cast<uint64_t>(1), "one distinct latch");
        });

        tc.Run("no grid, stale grid or far rates fall back to FP1", [](TestCase &t)
        {
            ps2_vsync_lock::Grid g;
            int64_t prev = -1, sleep = -7;
            t.IsTrue(!ps2_vsync_lock::pickSlot(g, kGuest120, 0, prev, sleep), "invalid grid");
            g.valid = true;
            g.periodNs = kPanel;
            g.anchorNs = wake(0);
            g.lastLatchNs = wake(100);
            t.IsTrue(!ps2_vsync_lock::pickSlot(g, kGuest120, wake(100) + 600000000ll, prev, sleep),
                     "no latch for 600 ms is stale");
            t.IsTrue(!ps2_vsync_lock::pickSlot(g, 10000000, wake(100), prev, sleep),
                     "10 ms guest on an 8.215 ms panel is outside 5 %");
            t.IsTrue(ps2_vsync_lock::pickSlot(g, kGuest120, wake(100), prev, sleep),
                     "119.88 on 121.7 (1.5 %) locks");
            t.IsTrue(ps2_vsync_lock::pickSlot(g, ps2_vsync_pacer::kPeriodNs, wake(100), prev, sleep),
                     "59.94 on 121.7 locks at every second refresh");
        });

        tc.Run("slots march one refresh apart; a late frame gives up its slot only", [](TestCase &t)
        {
            ps2_vsync_lock::Grid g;
            g.valid = true;
            g.periodNs = kPanel;
            g.anchorNs = wake(0);
            g.phaseNs = 1000000.0;
            g.lastLatchNs = wake(1000);
            int64_t prev = -1, sleep = 0;
            const int64_t now0 = wake(1000) + 3000000;
            t.IsTrue(ps2_vsync_lock::pickSlot(g, kGuest120, now0, prev, sleep), "locked");
            t.Equals(prev, wake(1001) + 1000000, "first slot = next grid slot after now");
            t.Equals(sleep, prev - now0, "sleeps until it");
            const int64_t s1 = prev;
            ps2_vsync_lock::pickSlot(g, kGuest120, s1 + 4000000, prev, sleep); // 4 ms of work
            t.Equals(prev, wake(1002) + 1000000, "next slot one panel period later");
            // Frame runs 2 ms past its slot: no sleep, next slot is the one after now.
            const int64_t late = prev + kPanel + 2000000;
            ps2_vsync_lock::pickSlot(g, kGuest120, late, prev, sleep);
            t.Equals(sleep, static_cast<int64_t>(0), "late frame runs at once");
            t.Equals(prev, wake(1003) + 1000000, "late frame consumes the slot it missed");
            ps2_vsync_lock::pickSlot(g, kGuest120, late + 1000000, prev, sleep);
            t.Equals(prev, wake(1004) + 1000000, "then back on the grid, no burst");
            // Stock 60: every second slot.
            int64_t p60 = -1;
            ps2_vsync_lock::pickSlot(g, ps2_vsync_pacer::kPeriodNs, now0, p60, sleep);
            const int64_t a = p60;
            ps2_vsync_lock::pickSlot(g, ps2_vsync_pacer::kPeriodNs, a + 1000000, p60, sleep);
            t.Equals(p60 - a, static_cast<int64_t>(std::llround(2 * kPanel)), "two refreshes per 59.94 frame");
        });

        tc.Run("closed loop: every frame on its own refresh, posts mid-period", [](TestCase &t)
        {
            // Model: VBlank at the slot; the frame is posted one guest frame
            // later + 3 ms of GS/GPU work (+/- 1 ms); SF latches it at the
            // next wake. Start from FP1's free-running phase.
            ps2_vsync_lock::Tracker tr(1e9 / 120.0);
            int64_t now = wake(0);
            // Warm-up at the FP1 rate (119.88 Hz) to get a grid.
            for (int f = 0; f < 60; ++f)
            {
                int i = 0;
                const int64_t post = now + kGuest120 + 3000000;
                tr.onLatch(post, latchFor(post, i));
                now += kGuest120;
            }
            int64_t prev = -1, sleep = 0;
            int lastWake = -1, repeats = 0, doubles = 0, frames = 0;
            int64_t prevSlot = -1;
            for (int f = 0; f < 2400; ++f)
            {
                const ps2_vsync_lock::Grid g = tr.grid();
                if (!ps2_vsync_lock::pickSlot(g, kGuest120, now, prev, sleep))
                {
                    t.Fail("lock dropped at frame " + std::to_string(f));
                    return;
                }
                const int64_t vblank = now + sleep;
                const int64_t work = 3000000 + (jitter(f) * 5) - 1000000; // 2..4 ms
                const int64_t post = vblank + (prevSlot < 0 ? kGuest120 : static_cast<int64_t>(kPanel)) + work;
                int i = 0;
                const int64_t latch = latchFor(post, i);
                tr.onLatch(post, latch);
                if (f >= 600)
                {
                    ++frames;
                    if (lastWake >= 0 && i - lastWake >= 2)
                        repeats += i - lastWake - 1;
                    if (lastWake >= 0 && i == lastWake)
                        ++doubles;
                }
                lastWake = i;
                prevSlot = vblank;
                now = vblank + 4000000; // EE work before the next VBlank
            }
            t.Equals(frames, 1800, "measured frames");
            t.Equals(repeats, 0, "no repeated refresh after convergence");
            t.Equals(doubles, 0, "no two frames on one refresh");
            const double slack = tr.lastSlackMeanNs();
            t.IsTrue(std::fabs(slack - kPanel / 2) < 1500000.0,
                     "posts land mid-period, slack mean " + std::to_string(slack));
        });

        tc.Run("AU15: audio sees 0 before any pace, when unlocked or stale", [](TestCase &t)
        {
            ps2_vsync_lock::Shared s; // local: the process-wide one is untouched
            t.IsTrue(ps2_vsync_lock::audioRatioFrom(s, wake(0)) == 0.0f, "no pace yet");
            s.ratio.store(1.0152f);
            s.ratioAtNs.store(wake(0));
            t.IsTrue(ps2_vsync_lock::audioRatioFrom(s, wake(0) + 400000000) == 1.0152f, "fresh");
            t.IsTrue(ps2_vsync_lock::audioRatioFrom(s, wake(0) + 600000000) == 0.0f, "stale after 500 ms");
            s.ratio.store(0.0f);
            t.IsTrue(ps2_vsync_lock::audioRatioFrom(s, wake(0) + 1000000) == 0.0f, "unlocked");
        });

        tc.Run("AU15: pace() stores guest period / (n * panel period)", [](TestCase &t)
        {
            // The process-wide instance (only this test touches it).
            ps2_vsync_lock::Shared &s = ps2_vsync_lock::shared();
            for (int i = 0; i < 200; ++i)
                s.tracker.onLatch(0, wake(i) + jitter(i));
            int64_t sleep = 0;
            const int64_t now = wake(199) + 1000000;
            t.IsTrue(ps2_vsync_lock::pace(kGuest120, now, sleep), "locked at 119.88");
            const float r120 = s.ratio.load();
            t.IsTrue(std::fabs(r120 - 1.0152f) < 0.001f, "full-120 ratio " + std::to_string(r120));
            t.IsTrue(ps2_vsync_lock::audioRatioFrom(s, now) == r120, "published with its time");
            t.IsTrue(ps2_vsync_lock::pace(ps2_vsync_pacer::kPeriodNs, now + 20000000, sleep), "locked at 59.94");
            t.IsTrue(std::fabs(s.ratio.load() - r120) < 1e-3f, "stock-60 ratio matches (n = 2)");
            t.IsFalse(ps2_vsync_lock::pace(kGuest120, now + 900000000, sleep), "stale grid unlocks");
            t.IsTrue(s.ratio.load() == 0.0f, "unlock publishes 0");
        });
    });
}
