#include "MiniTest.h"
#include "ps2_vsync_pacer.h"

#include <cstdint>

namespace
{
    constexpr int64_t kPeriod = ps2_vsync_pacer::kPeriodNs;
}

void register_ps2_vsync_pacer_tests()
{
    MiniTest::Case("Ps2VsyncPacer", [](TestCase &tc)
    {
        tc.Run("period is one NTSC vsync", [](TestCase &t)
        {
            t.Equals(kPeriod, static_cast<int64_t>(16683333), "1001/60000 s in ns");
            // 59.94 Hz nominal: 1e12/16683333 = 59940.00 millihertz.
            const int64_t millihertz = 1000000000ll * 1000 / kPeriod;
            t.IsTrue(59880ll < millihertz && millihertz < 60000ll,
                     "period must be within 0.1% of 59.94 Hz");
        });

        tc.Run("first vsync anchors without sleeping", [](TestCase &t)
        {
            ps2_vsync_pacer::Pacer p;
            t.Equals(p.onVsync(1'000'000), static_cast<int64_t>(0), "anchor call never sleeps");
            t.Equals(p.nextDeadlineNs(), static_cast<int64_t>(1'000'000 + kPeriod),
                     "deadline anchors one period out");
        });

        tc.Run("ahead sleeps until the deadline and marches it", [](TestCase &t)
        {
            ps2_vsync_pacer::Pacer p;
            p.onVsync(0);
            t.Equals(p.onVsync(0), kPeriod, "instant second vsync sleeps a full period");
            t.Equals(p.nextDeadlineNs(), 2 * kPeriod, "deadline advances one period per vsync");
            t.Equals(p.onVsync(2 * kPeriod - 1), static_cast<int64_t>(1), "1 ns early sleeps 1 ns");
            t.Equals(p.nextDeadlineNs(), 3 * kPeriod, "deadline keeps marching");
        });

        tc.Run("on-time and slightly behind never sleep", [](TestCase &t)
        {
            ps2_vsync_pacer::Pacer p;
            p.onVsync(0);
            t.Equals(p.onVsync(kPeriod), static_cast<int64_t>(0), "exactly on the deadline runs free");
            t.Equals(p.nextDeadlineNs(), 2 * kPeriod, "deadline still advances");
            // 1.5 periods behind: no sleep, anchor kept for catch-up.
            t.Equals(p.onVsync(2 * kPeriod + kPeriod / 2), static_cast<int64_t>(0),
                     "behind runs free without bursting");
            t.Equals(p.nextDeadlineNs(), 3 * kPeriod, "anchor kept when behind within 2 frames");
        });

        tc.Run("two frames behind keeps the anchor, past two resyncs", [](TestCase &t)
        {
            ps2_vsync_pacer::Pacer p;
            p.onVsync(0);
            // Behind by exactly 2 periods: not > 2, so no resync.
            t.Equals(p.onVsync(kPeriod + 2 * kPeriod), static_cast<int64_t>(0), "no sleep when behind");
            t.Equals(p.nextDeadlineNs(), 2 * kPeriod, "exactly-2-frames-behind keeps the anchor");
            // One ns further behind: resync to now + period.
            const int64_t late = 2 * kPeriod + 2 * kPeriod + 1;
            t.Equals(p.onVsync(late), static_cast<int64_t>(0), "no sleep on resync either");
            t.Equals(p.nextDeadlineNs(), late + kPeriod, "stale anchor resyncs to now + period");
            // After a resync, an on-time vsync runs free and marches on.
            t.Equals(p.onVsync(late + kPeriod), static_cast<int64_t>(0), "post-resync vsync on time");
            t.Equals(p.nextDeadlineNs(), late + 2 * kPeriod, "post-resync deadline marches");
        });

        tc.Run("burst of instant vsyncs paces each one", [](TestCase &t)
        {
            ps2_vsync_pacer::Pacer p;
            p.onVsync(500);
            // Three vsyncs at the same instant: sleeps stack one period each,
            // so a cycle-due burst still takes 3 periods of wall time.
            t.Equals(p.onVsync(500), kPeriod, "burst vsync 1 sleeps one period");
            t.Equals(p.onVsync(500), 2 * kPeriod, "burst vsync 2 sleeps two periods");
            t.Equals(p.onVsync(500), 3 * kPeriod, "burst vsync 3 sleeps three periods");
            t.Equals(p.nextDeadlineNs(), 500 + 4 * kPeriod, "burst advances 4 deadlines total");
        });

        tc.Run("unpaced knob parsing", [](TestCase &t)
        {
            t.IsTrue(ps2_vsync_pacer::unpacedFromEnv("1"), "PS2X_UNPACED=1 disables pacing");
            t.IsFalse(ps2_vsync_pacer::unpacedFromEnv(nullptr), "unset means paced");
            t.IsFalse(ps2_vsync_pacer::unpacedFromEnv(""), "empty means paced");
            t.IsFalse(ps2_vsync_pacer::unpacedFromEnv("0"), "0 means paced");
            t.IsFalse(ps2_vsync_pacer::unpacedFromEnv("true"), "only exact 1 disables");
        });
    });
}
