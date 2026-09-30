#include "MiniTest.h"
#include "ps2_audio_stretch.h"
#include "SoundTouch.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <vector>
#include <string>

namespace
{
constexpr uint32_t kTarget = ps2_audio_stretch::kTargetFrames;
constexpr double kDt = 0.011; // ~10 ms callbacks, as on the Odin (AU11)

bool near(float a, float b, float tol)
{
    return std::fabs(a - b) <= tol;
}

bool nearD(double a, double b, double tol)
{
    return std::fabs(a - b) <= tol;
}

// AU15: closed-loop ring model. The guest pushes 512-frame sound ticks at
// speed(t) x 93.75 Hz (+/-20 % tick jitter); the host callback every
// 10 +/- 3 ms drains 48 kHz x dt output frames, i.e. tempo x that many
// source frames (bypass: 1x). lockRatio > 0 is passed to the controller
// every step (PS2X_VSYNC_LOCK_AUDIO=tempo, locked). Stats cover t >= 10 s.
struct SimResult
{
    uint64_t engages = 0; // after 10 s
    uint64_t steps = 0, bypassed = 0;
    double tempoSum = 0.0;
    float tempoMin = 9.0f, tempoMax = 0.0f;
    double fillMin = 1e18, fillMax = 0.0;
    uint64_t underrunFrames = 0;
};

template <typename SpeedFn>
SimResult simulate(double seconds, float lockRatio, SpeedFn speed)
{
    ps2_audio_stretch::StretchController c;
    uint32_t rng = 12345u;
    const auto uni = [&rng]() // [0, 1)
    {
        rng = rng * 1664525u + 1013904223u;
        return static_cast<double>(rng >> 8) / 16777216.0;
    };
    SimResult r;
    double fill = kTarget, t = 0.0, nextTick = 0.0, dt = -1.0;
    uint64_t engagesAt10 = 0;
    bool marked = false;
    while (t < seconds)
    {
        const auto s = c.update(static_cast<uint64_t>(fill), dt, lockRatio);
        if (t >= 10.0)
        {
            if (!marked)
            {
                engagesAt10 = c.stats().engages;
                marked = true;
            }
            ++r.steps;
            r.bypassed += s.bypass ? 1u : 0u;
            r.tempoSum += s.tempo;
            r.tempoMin = std::min(r.tempoMin, s.tempo);
            r.tempoMax = std::max(r.tempoMax, s.tempo);
            r.fillMin = std::min(r.fillMin, fill);
            r.fillMax = std::max(r.fillMax, fill);
        }
        dt = 0.010 + (uni() - 0.5) * 0.006;
        const double drain = 48000.0 * dt * (s.bypass ? 1.0 : s.tempo);
        if (drain > fill)
        {
            if (t >= 10.0)
                r.underrunFrames += static_cast<uint64_t>(drain - fill);
            fill = 0.0;
        }
        else
            fill -= drain;
        t += dt;
        while (nextTick <= t)
        {
            fill += 512.0;
            nextTick += 512.0 / 48000.0 / speed(nextTick) * (0.8 + 0.4 * uni());
        }
    }
    r.engages = c.stats().engages - engagesAt10;
    return r;
}
} // namespace

void register_ps2_audio_stretch_tests()
{
    MiniTest::Case("Ps2AudioStretch", [](TestCase &tc)
    {
        tc.Run("stretch defaults on, =0 disables", [](TestCase &t)
        {
            t.IsTrue(ps2_audio_stretch::stretchEnabledFromEnv(nullptr), "unset means on");
            t.IsTrue(ps2_audio_stretch::stretchEnabledFromEnv("1"), "1 means on");
            t.IsTrue(ps2_audio_stretch::stretchEnabledFromEnv(""), "only the exact 0 disables");
            t.IsFalse(ps2_audio_stretch::stretchEnabledFromEnv("0"), "0 means off");
        });

        tc.Run("hysteresis params default + parse + clamp", [](TestCase &t)
        {
            const ps2_audio_stretch::Params d =
                ps2_audio_stretch::paramsFromEnv(nullptr, nullptr, nullptr, nullptr);
            t.IsFalse(d.legacy, "default is the sustained-deficit controller");
            t.IsTrue(near(d.leave, 0.035f, 1e-6f), "default leave band is 3.5 %");
            t.IsTrue(near(d.rejoin, 0.01f, 1e-6f), "default rejoin band is 1 %");
            t.IsTrue(nearD(d.sustainS, 0.30, 1e-9), "default sustain is 300 ms");
            t.IsTrue(nearD(d.rejoinS, 0.0, 1e-12), "default rejoin is immediate");

            const ps2_audio_stretch::Params leg =
                ps2_audio_stretch::paramsFromEnv("1", nullptr, nullptr, nullptr);
            t.IsTrue(leg.legacy, "STRETCH_LEGACY=1 selects AT1");
            t.IsTrue(near(leg.leave, 0.02f, 1e-6f), "legacy leave band is 2 %");
            t.IsTrue(nearD(leg.sustainS, 0.0, 1e-12), "legacy engages with no dwell");
            t.IsTrue(nearD(leg.rejoinS, 0.0, 1e-12), "legacy rejoins with no dwell");
            t.IsFalse(ps2_audio_stretch::paramsFromEnv("0", nullptr, nullptr, nullptr).legacy,
                      "only the exact 1 enables legacy");

            const ps2_audio_stretch::Params tuned =
                ps2_audio_stretch::paramsFromEnv(nullptr, "0.05", "100", "500");
            t.IsTrue(near(tuned.leave, 0.05f, 1e-6f), "leave override parses");
            t.IsTrue(nearD(tuned.sustainS, 0.10, 1e-9), "sustain override parses (ms)");
            t.IsTrue(nearD(tuned.rejoinS, 0.50, 1e-9), "rejoin override parses (ms)");

            const ps2_audio_stretch::Params bad =
                ps2_audio_stretch::paramsFromEnv(nullptr, "abc", "", "1x");
            t.IsTrue(near(bad.leave, 0.035f, 1e-6f), "bad leave keeps the default");
            t.IsTrue(nearD(bad.sustainS, 0.30, 1e-9), "empty sustain keeps the default");
            t.IsTrue(nearD(bad.rejoinS, 0.0, 1e-12), "trailing junk keeps the default");

            const ps2_audio_stretch::Params clamp =
                ps2_audio_stretch::paramsFromEnv(nullptr, "9", "-5", "99999");
            t.IsTrue(near(clamp.leave, 0.15f, 1e-6f), "leave clamps at 15 %");
            t.IsTrue(nearD(clamp.sustainS, 0.0, 1e-12), "negative sustain clamps at 0");
            t.IsTrue(nearD(clamp.rejoinS, 5.0, 1e-9), "rejoin clamps at 5 s");
        });

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

        tc.Run("steady full speed with jitter never engages", [](TestCase &t)
        {
            // 30 s at target with +/-1.5 % wander plus a 100 ms dip to 0.94
            // every ~5.5 s: the Odin full-speed shape (AU11: smooth strays
            // to +/-3 %, bypass 85-95 % under AT1).
            ps2_audio_stretch::StretchController c;
            c.update(kTarget, 0.0);
            for (int i = 1; i <= 2728; ++i)
            {
                double ratio = 1.0 + 0.015 * std::sin(i * 0.05);
                if (i % 500 >= 491)
                    ratio = 0.94; // 9 steps ~= 100 ms transient dip
                const auto s =
                    c.update(static_cast<uint64_t>(kTarget * ratio), kDt);
                if (!s.bypass)
                {
                    t.IsTrue(false, "jitter must not engage the stretcher");
                    break;
                }
            }
            t.IsTrue(c.bypass(), "still bypassed after 30 s of jitter");
            t.Equals(c.stats().engages, static_cast<uint64_t>(0), "zero engagements");
        });

        tc.Run("empty ring waits out the sustain, then engages at the floor", [](TestCase &t)
        {
            ps2_audio_stretch::StretchController c;
            const auto first = c.update(0, 0.0);
            t.IsTrue(first.bypass, "a dry ring holds bypass until the deficit is sustained");
            ps2_audio_stretch::StepResult s{};
            int engageStep = -1;
            for (int i = 1; i <= 60; ++i)
            {
                s = c.update(0, kDt);
                if (!s.bypass && engageStep < 0)
                    engageStep = i;
            }
            t.IsTrue(engageStep >= 25 && engageStep <= 32,
                     "empty ring engages at ~300 ms (sustain), not at once");
            t.IsTrue(near(s.tempo, 0.5f, 1e-6f), "tempo sits at the floor until production starts");
        });

        tc.Run("sustained 0.8x engages within ~0.5 s near 0.8", [](TestCase &t)
        {
            ps2_audio_stretch::StretchController c;
            c.update(kTarget, 0.0);
            ps2_audio_stretch::StepResult s{};
            int engageStep = -1;
            for (int i = 1; i <= 200; ++i)
            {
                s = c.update(static_cast<uint64_t>(kTarget * 0.8), kDt);
                if (!s.bypass && engageStep < 0)
                    engageStep = i;
            }
            t.IsTrue(engageStep > 0 && engageStep <= 45,
                     "0.8x fill must engage within ~0.5 s");
            t.IsTrue(near(s.tempo, 0.8f, 0.02f), "tempo settles at the fill ratio");
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

        tc.Run("3 % slow holds bypass, 6 % slow engages", [](TestCase &t)
        {
            ps2_audio_stretch::StretchController hold;
            hold.update(kTarget, 0.0);
            for (int i = 0; i < 300; ++i)
                hold.update(static_cast<uint64_t>(kTarget * 0.97), kDt);
            t.IsTrue(hold.bypass(), "3 % slow is inside the 3.5 % band: bypass");
            t.Equals(hold.stats().engages, static_cast<uint64_t>(0), "no engagement");

            ps2_audio_stretch::StretchController engage;
            engage.update(kTarget, 0.0);
            ps2_audio_stretch::StepResult s{};
            for (int i = 0; i < 300; ++i)
                s = engage.update(static_cast<uint64_t>(kTarget * 0.94), kDt);
            t.IsFalse(s.bypass, "6 % slow is outside the band: engages");
        });

        tc.Run("recovery rejoins promptly inside the band", [](TestCase &t)
        {
            ps2_audio_stretch::StretchController c;
            c.update(kTarget, 0.0);
            for (int i = 0; i < 100; ++i)
                c.update(static_cast<uint64_t>(kTarget * 0.6), kDt);
            t.IsFalse(c.bypass(), "precondition: engaged");
            for (int i = 0; i < 50; ++i)
                c.update(kTarget, kDt);
            t.IsFalse(c.bypass(), "smooth still below the band: holds");
            ps2_audio_stretch::StepResult s{};
            for (int i = 0; i < 150; ++i)
                s = c.update(kTarget, kDt);
            t.IsTrue(s.bypass, "once smooth crosses inside 1 % the release is immediate");
            t.IsTrue(near(s.tempo, 1.0f, 1e-6f), "released tempo is exactly 1");
        });

        tc.Run("explicit rejoin dwell holds through brief touches", [](TestCase &t)
        {
            ps2_audio_stretch::Params p;
            p.rejoinS = 1.0;
            ps2_audio_stretch::StretchController c(p);
            c.update(kTarget, 0.0);
            for (int i = 0; i < 100; ++i)
                c.update(static_cast<uint64_t>(kTarget * 0.6), kDt);
            t.IsFalse(c.bypass(), "precondition: engaged");
            for (int i = 0; i < 150; ++i)
                c.update(kTarget, kDt);
            t.IsFalse(c.bypass(), "1 s dwell still holds 1.65 s after recovery starts");
            ps2_audio_stretch::StepResult s{};
            for (int i = 0; i < 200; ++i)
                s = c.update(kTarget, kDt);
            t.IsTrue(s.bypass, "sustained full fill releases even with the dwell");
        });

        tc.Run("legacy mode reproduces the AT1 edges", [](TestCase &t)
        {
            const ps2_audio_stretch::Params legacy =
                ps2_audio_stretch::paramsFromEnv("1", nullptr, nullptr, nullptr);
            ps2_audio_stretch::StretchController c(legacy);
            const auto dry = c.update(0, 0.0);
            t.IsFalse(dry.bypass, "legacy: a dry ring engages immediately");
            t.IsTrue(near(dry.tempo, 0.5f, 1e-6f), "legacy: tempo at the floor");

            ps2_audio_stretch::StretchController c2(legacy);
            c2.update(kTarget, 0.0);
            for (int i = 0; i < 200; ++i)
                c2.update(static_cast<uint64_t>(kTarget * 0.99), kDt);
            t.IsTrue(c2.bypass(), "legacy: 1 % slow stays in bypass");
            for (int i = 0; i < 200; ++i)
                c2.update(static_cast<uint64_t>(kTarget * 0.97), kDt);
            t.IsFalse(c2.bypass(), "legacy: 3 % slow engages");
            for (int i = 0; i < 200; ++i)
                c2.update(static_cast<uint64_t>(kTarget * 0.985), kDt);
            t.IsFalse(c2.bypass(), "legacy: hysteresis holds until back inside 1 %");
        });

        tc.Run("drops react faster than rises", [](TestCase &t)
        {
            ps2_audio_stretch::StretchController down, up;
            down.update(kTarget, 0.0);
            int dropSteps = 0;
            for (int i = 0; i < 500; ++i)
            {
                ++dropSteps;
                down.update(0, kDt);
                if (down.smoothedTempo() < 0.75f)
                    break;
            }
            up.update(0, 0.0);
            int riseSteps = 0;
            for (int i = 0; i < 5000; ++i)
            {
                ++riseSteps;
                up.update(kTarget, kDt);
                if (up.smoothedTempo() > 0.99f)
                    break;
            }
            t.IsTrue(dropSteps < riseSteps, "50 ms down-tau beats 300 ms up-tau");
            t.IsTrue(dropSteps < 30, "a draining ring registers within ~30 callbacks");
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

        tc.Run("AU15: lock-audio knob is exactly tempo", [](TestCase &t)
        {
            using ps2_audio_stretch::LockAudioMode;
            t.IsTrue(ps2_audio_stretch::lockAudioModeFromEnv(nullptr) == LockAudioMode::Stretch, "unset = today");
            t.IsTrue(ps2_audio_stretch::lockAudioModeFromEnv("tempo") == LockAudioMode::Tempo, "tempo");
            t.IsTrue(ps2_audio_stretch::lockAudioModeFromEnv("1") == LockAudioMode::Stretch, "1 = today");
            t.IsTrue(ps2_audio_stretch::lockAudioModeFromEnv("Tempo") == LockAudioMode::Stretch, "case-exact");
        });

        tc.Run("AU15: guest at +1.52 %, band logic cycles; locked tempo holds one engagement", [](TestCase &t)
        {
            const auto locked = [](double) { return 1.0152; };
            const SimResult today = simulate(70.0, 0.0f, locked);
            const SimResult au15 = simulate(70.0, 1.0152f, locked);
            // Control: today's controller toggles under the lock (PX1 saw
            // ~0.7 engages/s on the Odin). The model must reproduce that or
            // the second half proves nothing.
            t.IsTrue(today.engages >= 10, "today cycles: " + std::to_string(today.engages) + " engages in 60 s");
            t.IsTrue(today.bypassed > 0 && today.bypassed < today.steps, "today mixes bypass and stretch");
            t.Equals(au15.engages, static_cast<uint64_t>(0), "locked: no engagement after the first");
            t.Equals(au15.bypassed, static_cast<uint64_t>(0), "locked: never bypassed");
            const double mean = au15.tempoSum / au15.steps;
            t.IsTrue(nearD(mean, 1.0152, 0.002), "locked mean tempo " + std::to_string(mean));
            t.IsTrue(au15.fillMin > 0.5 * kTarget && au15.fillMax < 1.6 * kTarget,
                     "fill stays near the target: " + std::to_string(au15.fillMin) + ".." +
                         std::to_string(au15.fillMax));
            t.Equals(au15.underrunFrames, static_cast<uint64_t>(0), "locked: no underruns");
        });

        tc.Run("AU15: locked tempo follows a real guest dip and comes back", [](TestCase &t)
        {
            // 3 s at 0.8x inside the locked window (a heavy scene the device
            // can't keep up with while the pacer stays locked, sleep 0).
            const SimResult r = simulate(40.0, 1.0152f, [](double tt)
                                         { return tt >= 20.0 && tt < 23.0 ? 0.8 : 1.0152; });
            t.IsTrue(r.tempoMin < 0.85f, "tempo drops with the guest: min " + std::to_string(r.tempoMin));
            t.IsTrue(r.tempoMax > 1.0f, "and returns above 1");
            t.Equals(r.bypassed, static_cast<uint64_t>(0), "never bypassed");
            t.IsTrue(r.underrunFrames < 48000u / 10u, "under 100 ms of pads across the dip: " +
                                                          std::to_string(r.underrunFrames));
        });

        tc.Run("AU15: unlock hands back to the band logic", [](TestCase &t)
        {
            ps2_audio_stretch::StretchController c;
            c.update(kTarget, 0.0, 1.0152f);
            ps2_audio_stretch::StepResult s{};
            for (int i = 0; i < 100; ++i)
                s = c.update(kTarget, kDt, 1.0152f);
            t.IsFalse(s.bypass, "locked: engaged");
            t.IsTrue(near(s.tempo, 1.0152f, 1e-4f), "locked tempo = ratio at the target fill");
            t.IsTrue(near(c.lockedBase(), 1.0152f, 1e-4f), "base tracks the ratio");
            s = c.update(kTarget, kDt, 0.0f);
            t.IsTrue(s.bypass, "unlocked at the target fill: rejoins at once");
            t.IsTrue(c.lockedBase() == 0.0f, "base cleared");
            t.Equals(c.stats().locked, static_cast<uint64_t>(101), "101 locked steps counted");
        });

        tc.Run("AU16: stretch-feed knob is exactly demand", [](TestCase &t)
        {
            t.IsFalse(ps2_audio_stretch::feedDemandFromEnv(nullptr), "unset = today");
            t.IsTrue(ps2_audio_stretch::feedDemandFromEnv("demand"), "demand");
            t.IsFalse(ps2_audio_stretch::feedDemandFromEnv("1"), "1 = today");
            t.IsFalse(ps2_audio_stretch::feedDemandFromEnv("Demand"), "case-exact");
        });

        tc.Run("AU16: demand feed stops once the output is covered", [](TestCase &t)
        {
            // Fake stretcher: holds 100 input frames, then passes 1:1.
            struct Fake
            {
                size_t in = 0, out = 0;
                unsigned numSamples() const { return static_cast<unsigned>(out); }
                void putSamples(const float *, unsigned n)
                {
                    in += n;
                    if (in > 100u)
                    {
                        out += in - 100u;
                        in = 100u;
                    }
                }
            } st;
            size_t ring = 1000;
            float scratch[ps2_audio_stretch::kDemandChunkFrames * 2u];
            const auto pop = [&ring](float *, size_t max)
            {
                const size_t got = std::min(ring, max);
                ring -= got;
                return got;
            };
            size_t fed = ps2_audio_stretch::feedOnDemand(st, 0u, 480u, scratch, pop);
            t.IsTrue(st.out >= 480u && st.out < 480u + ps2_audio_stretch::kDemandChunkFrames,
                     "covers the need within one chunk: " + std::to_string(st.out));
            t.Equals(fed, 1000u - ring, "fed = popped");
            fed = ps2_audio_stretch::feedOnDemand(st, 0u, 480u, scratch, pop);
            t.Equals(fed, static_cast<size_t>(0), "already covered: feeds nothing");
            st.out = 0;
            fed = ps2_audio_stretch::feedOnDemand(st, 470u, 480u, scratch, pop);
            t.IsTrue(fed > 0u && fed <= ps2_audio_stretch::kDemandChunkFrames, "queued frames count toward the need");
            ring = 0;
            st.out = 0;
            t.Equals(ps2_audio_stretch::feedOnDemand(st, 0u, 480u, scratch, pop), static_cast<size_t>(0),
                     "empty ring: stops");
        });

        tc.Run("AU16: locked tempo + SoundTouch: demand feed drops the stretcher backlog", [](TestCase &t)
        {
            // The callback's engaged path (48 kHz, 480-frame callbacks) with
            // the output settings, the guest at +1.52 % from an empty ring.
            const auto run = [](bool demand)
            {
                soundtouch::SoundTouch st;
                st.setSampleRate(48000);
                st.setChannels(2);
                st.setSetting(SETTING_SEQUENCE_MS, 30);
                st.setSetting(SETTING_SEEKWINDOW_MS, 20);
                st.setSetting(SETTING_OVERLAP_MS, 10);
                st.setSetting(SETTING_USE_QUICKSEEK, 0);
                st.setSetting(SETTING_USE_AA_FILTER, 0);
                ps2_audio_stretch::StretchController c;
                std::deque<float> ring, q;
                std::vector<float> in, out(2048u * 2u);
                float scratch[ps2_audio_stretch::kDemandChunkFrames * 2u];
                double produced = 0.0, heldSum = 0.0, pathSum = 0.0;
                uint64_t pushed = 0, lateUnder = 0;
                int n = 0;
                for (int cb = 0; cb < 3000; ++cb)
                {
                    produced += 480.0 * 1.0152;
                    for (; pushed + 384u <= produced; pushed += 384u)
                        for (int i = 0; i < 384; ++i)
                            ring.push_back(0.25f * static_cast<float>(std::sin((pushed + i) * 0.05)));
                    const ps2_audio_stretch::StepResult s =
                        c.update(ring.size(), cb ? 0.01 : -1.0, 1.0152f);
                    st.setTempo(s.tempo);
                    const auto popTo = [&ring](float *o, size_t max)
                    {
                        size_t got = 0;
                        for (; got < max && !ring.empty(); ++got)
                        {
                            o[2u * got] = o[2u * got + 1u] = ring.front();
                            ring.pop_front();
                        }
                        return got;
                    };
                    if (demand)
                        ps2_audio_stretch::feedOnDemand(st, q.size(), 480u, scratch, popTo);
                    else
                    {
                        in.resize(static_cast<size_t>(std::ceil(480.0 * s.tempo)) * 2u);
                        const size_t got = popTo(in.data(), in.size() / 2u);
                        if (got)
                            st.putSamples(in.data(), static_cast<unsigned>(got));
                    }
                    for (int i = 0; i < 480; ++i)
                    {
                        if (q.empty())
                        {
                            const unsigned got = st.receiveSamples(out.data(), 1024u);
                            for (unsigned k = 0; k < got; ++k)
                                q.push_back(out[2u * k]);
                        }
                        if (q.empty())
                        {
                            if (cb >= 1000)
                                ++lateUnder;
                            continue;
                        }
                        q.pop_front();
                    }
                    if (cb >= 1000)
                    {
                        const double held = st.numUnprocessedSamples() + st.numSamples() + q.size();
                        heldSum += held;
                        pathSum += held + ring.size();
                        ++n;
                    }
                }
                struct R { double heldMs, pathMs; uint64_t under; };
                return R{heldSum / n / 48.0, pathSum / n / 48.0, lateUnder};
            };
            const auto today = run(false);
            const auto au16 = run(true);
            t.IsTrue(today.heldMs > 60.0, "control: today's feed keeps a backlog: " + std::to_string(today.heldMs) + " ms");
            t.IsTrue(au16.heldMs < 45.0, "demand: only the WSOLA window: " + std::to_string(au16.heldMs) + " ms");
            t.IsTrue(au16.pathMs < today.pathMs - 30.0,
                     "path " + std::to_string(au16.pathMs) + " vs " + std::to_string(today.pathMs) + " ms");
            t.Equals(au16.under, static_cast<uint64_t>(0), "no underruns after startup");
            t.Equals(today.under, static_cast<uint64_t>(0), "control: none either");
        });

        tc.Run("window stats track applied tempo and engagements", [](TestCase &t)
        {
            ps2_audio_stretch::StretchController c;
            c.update(kTarget, 0.0);
            for (int i = 0; i < 100; ++i)
                c.update(static_cast<uint64_t>(kTarget * 0.6), kDt);
            const auto &st = c.stats();
            t.Equals(st.callbacks, static_cast<uint64_t>(101), "101 steps counted");
            t.Equals(st.engages, static_cast<uint64_t>(1), "one engagement");
            t.IsTrue(st.minTempo <= st.maxTempo, "min <= max");
            t.IsTrue(st.minTempo < 1.0f && st.maxTempo == 1.0f, "window spans bypass and stretch");
            c.resetStats();
            t.Equals(c.stats().callbacks, static_cast<uint64_t>(0), "reset clears the window");
            t.Equals(c.stats().engages, static_cast<uint64_t>(0), "reset clears engagements");
        });
    });
}
