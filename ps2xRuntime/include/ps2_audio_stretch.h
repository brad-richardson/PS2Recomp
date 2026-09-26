#pragma once

// AT1: pitch-preserving time-stretch controller (host-side only).
//
// The guest produces 48 kHz stereo PCM into the PcmRing on guest time; the
// host audio callback drains it on wall time. Below full speed the ring
// starves and the callback pads with silence (stutter). This controller
// drives a SoundTouch instance (see ps2_snd_audio_output.cpp): its tempo
// output stretches the audio (tempo < 1 plays slower at the same pitch)
// so the host demand is met from a slower guest without stutter.
//
// Control law (cf. PCSX2 AudioStream::UpdateStretchTempo, which keys tempo
// off buffer fill with a quiet band near 1.0):
//   raw    = clamp(fill / target, kTempoMin, kTempoMax)
//   smooth = asymmetric EMA of raw over wall time (fast on drops, slow on
//            rises: a draining ring threatens underrun, a filling one is
//            absorbed by ring capacity)
//   bypass while smooth is within +/-2 % of 1.0 (hysteresis: re-enter
//   inside +/-1 %), so full speed sounds exactly as today: the callback
//   then copies ring frames straight through and the stretcher output is
//   discarded. The stretcher still ingests a copy so engaging is seamless.
//   Engagement is fill-agnostic on purpose: gating it on a prime level
//   traps the controller in bypass exactly when the ring runs dry (a guest
//   deficit with an empty ring would stutter until a lucky burst crosses
//   the gate). At startup the ring is empty either way, so engaging there
//   pads identically to bypass and converges faster once production starts.
//
// No sqrt() dampening (PCSX2 has it): with the FP1 wall pacer the guest
// rate is exactly <= 1.0, so linear control settles at fill = rate*target
// with more low-rate margin (0.6*target at 0.6x, not 0.36*target).
// Pure logic: no SoundTouch dependency, fake-clock testable.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace ps2_audio_stretch
{

constexpr uint32_t kSourceRate = 48000u; // SNDDRV output rate (AU9)
constexpr uint32_t kTargetLatencyMs = 80u; // in the brief's 60-100 ms band
constexpr uint32_t kTargetFrames = kSourceRate * kTargetLatencyMs / 1000u;
constexpr float kTempoMin = 0.5f;
constexpr float kTempoMax = 1.05f;
constexpr float kBypassLeave = 0.02f;  // engage outside +/-2 % of 1.0
constexpr float kBypassRejoin = 0.01f; // release inside +/-1 % of 1.0
constexpr double kTauDownS = 0.05;      // EMA time constant on drops
constexpr double kTauUpS = 0.30;        // EMA time constant on rises

// PS2X_AUDIO_STRETCH: default on (Brad 09-26, after the C1 listen);
// the exact value "0" disables (legacy direct path, byte-identical output).
inline bool stretchEnabledFromEnv(const char *value)
{
    return value == nullptr || std::strcmp(value, "0") != 0;
}

struct StepResult
{
    float tempo = 1.0f; // applied tempo (1.0 in bypass)
    bool bypass = true;
};

struct WindowStats
{
    uint64_t callbacks = 0;
    uint64_t bypassed = 0;
    float minTempo = 1.0f;
    float maxTempo = 1.0f;
    double sumTempo = 0.0;
};

class StretchController
{
public:
    StretchController() = default;

    // One host-audio callback step. fillFrames is the ring depth BEFORE this
    // callback drains it; dtWallS is wall seconds since the previous step
    // (<= 0 on the first step: adopt raw immediately).
    StepResult update(uint64_t fillFrames, double dtWallS)
    {
        const float raw = std::clamp(static_cast<float>(fillFrames) / kTargetFrames,
                                     kTempoMin, kTempoMax);
        if (!m_init)
        {
            m_smooth = raw;
            m_init = true;
        }
        else if (dtWallS > 0.0)
        {
            const double tau = raw < m_smooth ? kTauDownS : kTauUpS;
            const float alpha = static_cast<float>(1.0 - std::exp(-dtWallS / tau));
            m_smooth += (raw - m_smooth) * alpha;
        }
        if (m_bypass)
        {
            if (std::fabs(m_smooth - 1.0f) > kBypassLeave)
                m_bypass = false;
        }
        else if (std::fabs(m_smooth - 1.0f) < kBypassRejoin)
        {
            m_bypass = true;
        }
        StepResult out;
        out.tempo = m_bypass ? 1.0f : m_smooth;
        out.bypass = m_bypass;
        m_stats.callbacks += 1u;
        if (m_bypass)
            m_stats.bypassed += 1u;
        if (m_stats.callbacks == 1u)
            m_stats.minTempo = m_stats.maxTempo = out.tempo;
        else
        {
            m_stats.minTempo = std::min(m_stats.minTempo, out.tempo);
            m_stats.maxTempo = std::max(m_stats.maxTempo, out.tempo);
        }
        m_stats.sumTempo += out.tempo;
        return out;
    }

    float smoothedTempo() const { return m_smooth; }
    bool bypass() const { return m_bypass; }
    const WindowStats &stats() const { return m_stats; }
    void resetStats() { m_stats = WindowStats{}; }

private:
    bool m_init = false;
    float m_smooth = 1.0f;
    bool m_bypass = true;
    WindowStats m_stats{};
};

} // namespace ps2_audio_stretch
