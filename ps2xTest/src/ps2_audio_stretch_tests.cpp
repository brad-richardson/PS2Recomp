#include "MiniTest.h"
#include "ps2_audio_stretch.h"

#include <cmath>
#include <cstdint>

namespace
{
constexpr uint32_t kTarget = ps2_audio_stretch::kTargetFrames;
constexpr double kDt = 0.011; // ~512 frames at 48 kHz per callback

bool near(float a, float b, float tol)
{
    return std::fabs(a - b) <= tol;
}
} // namespace

void register_ps2_audio_stretch_tests()
{
    MiniTest::Case("Ps2AudioStretch", [](TestCase &tc)
    {
        tc.Run("target fill stays in bypass at tempo 1", [](TestCase &t)
        {
            ps2_audio_stretch::StretchController c;
            for (int i = 0; i < 50; ++i)
            {
                const auto s = c.update(kTarget, i == 0 ? 0.0 : kDt);
                t.IsTrue(s.bypass, "full-speed fill must bypass");
                t.IsTrue(near(s.tempo, 1.0f, 1e-6f), "bypass tempo is exactly 1");
            }
        });

        tc.Run("empty ring engages at the floor (no prime trap)", [](TestCase &t)
        {
            ps2_audio_stretch::StretchController c;
            const auto s = c.update(0, 0.0);
            t.IsFalse(s.bypass, "a dry ring must engage immediately, not wait for a prime level");
            t.IsTrue(near(s.tempo, 0.5f, 1e-6f), "tempo sits at the floor until production starts");
        });

        tc.Run("sustained 0.6x fill engages near 0.6", [](TestCase &t)
        {
            ps2_audio_stretch::StretchController c;
            c.update(kTarget, 0.0);
            ps2_audio_stretch::StepResult s{};
            for (int i = 0; i < 200; ++i)
                s = c.update(static_cast<uint64_t>(kTarget * 0.6), kDt);
            t.IsFalse(s.bypass, "0.6x fill must engage the stretcher");
            t.IsTrue(near(s.tempo, 0.6f, 0.02f), "tempo settles at the fill ratio");
        });

        tc.Run("recovery rejoins the bypass band", [](TestCase &t)
        {
            ps2_audio_stretch::StretchController c;
            c.update(kTarget, 0.0);
            for (int i = 0; i < 100; ++i)
                c.update(static_cast<uint64_t>(kTarget * 0.6), kDt);
            t.IsFalse(c.bypass(), "precondition: engaged");
            ps2_audio_stretch::StepResult s{};
            for (int i = 0; i < 2000; ++i)
                s = c.update(kTarget, kDt);
            t.IsTrue(s.bypass, "back at target fill the stretcher releases");
            t.IsTrue(near(s.tempo, 1.0f, 1e-6f), "released tempo is exactly 1");
        });

        tc.Run("drops react faster than rises", [](TestCase &t)
        {
            ps2_audio_stretch::StretchController down, up;
            down.update(kTarget, 0.0);
            int dropSteps = 0;
            for (int i = 0; i < 500; ++i)
            {
                ++dropSteps;
                if (down.update(0, kDt).tempo < 0.75f)
                    break;
            }
            up.update(0, 0.0);
            int riseSteps = 0;
            for (int i = 0; i < 5000; ++i)
            {
                ++riseSteps;
                if (up.update(kTarget, kDt).tempo > 0.99f || up.bypass())
                    break;
            }
            t.IsTrue(dropSteps < riseSteps, "50 ms down-tau beats 300 ms up-tau");
            t.IsTrue(dropSteps < 30, "a draining ring engages within ~30 callbacks");
        });

        tc.Run("overfull ring clamps at the tempo ceiling", [](TestCase &t)
        {
            ps2_audio_stretch::StretchController c;
            c.update(kTarget, 0.0);
            ps2_audio_stretch::StepResult s{};
            for (int i = 0; i < 200; ++i)
                s = c.update(kTarget * 4u, kDt);
            t.IsFalse(s.bypass, "overfull must engage to drain");
            t.IsTrue(near(s.tempo, 1.05f, 0.02f), "tempo clamps at the ceiling");
        });

        tc.Run("hysteresis holds the bypass near the edge", [](TestCase &t)
        {
            ps2_audio_stretch::StretchController c;
            c.update(kTarget, 0.0);
            // 0.99x: inside the leave band, never engages.
            for (int i = 0; i < 200; ++i)
                c.update(static_cast<uint64_t>(kTarget * 0.99), kDt);
            t.IsTrue(c.bypass(), "1 % slow stays in bypass");
            // 0.97x: engages, and stays engaged (no flap at the edge).
            for (int i = 0; i < 200; ++i)
                c.update(static_cast<uint64_t>(kTarget * 0.97), kDt);
            t.IsFalse(c.bypass(), "3 % slow engages");
            for (int i = 0; i < 200; ++i)
                c.update(static_cast<uint64_t>(kTarget * 0.985), kDt);
            t.IsFalse(c.bypass(), "hysteresis holds until back inside 1 %");
        });

        tc.Run("window stats track applied tempo", [](TestCase &t)
        {
            ps2_audio_stretch::StretchController c;
            c.update(kTarget, 0.0);
            for (int i = 0; i < 10; ++i)
                c.update(static_cast<uint64_t>(kTarget * 0.6), kDt);
            const auto &st = c.stats();
            t.Equals(st.callbacks, static_cast<uint64_t>(11), "11 steps counted");
            t.IsTrue(st.minTempo <= st.maxTempo, "min <= max");
            t.IsTrue(st.minTempo < 1.0f && st.maxTempo == 1.0f, "window spans bypass and stretch");
            c.resetStats();
            t.Equals(c.stats().callbacks, static_cast<uint64_t>(0), "reset clears the window");
        });
    });
}
