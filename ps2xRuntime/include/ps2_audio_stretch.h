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
//   AU12: engage only on a SUSTAINED deficit: smooth must stay outside
//   +/-3.5 % of 1.0 continuously for >= 300 ms (brief: the AT1 immediate
//   +/-2 % engage cycled audibly — WSOLA overlap at ~10-30 ms lags sounds
//   like a short echo — on the Odin's +/-3 % fill wander at full speed).
//   Rejoin is immediate inside +/-1 %: the engage side is the sole flutter
//   guard (a 1 s rejoin dwell never releases under real wander — the mini
//   and Odin traces never sit inside +/-1 % for 1 s — trapping the
//   controller engaged; AU12). A sustained 0.8x slowdown still engages
//   within ~0.5 s (the 80 ms target buffer covers the 300 ms sustain: it
//   drains in 400 ms).
//   Bypass plays ring frames straight through (bit-exact) while the
//   stretcher ingests a copy so engaging is seamless; its output is
//   discarded in bypass.
//   Engagement is fill-agnostic on purpose: gating it on a prime level
//   traps the controller in bypass exactly when the ring runs dry (a guest
//   deficit with an empty ring would stutter until a lucky burst crosses
//   the gate). At startup the ring is empty either way, so engaging there
//   pads identically to bypass and converges faster once production starts.
//
// PS2X_STRETCH_LEGACY=1 restores the AT1 behavior (immediate engage outside
// +/-2 %, immediate rejoin inside +/-1 %): it is the same code path with
// sustain/rejoin dwells of 0. Dev-only escape hatch + the "today" reference
// for A/B legs. PS2X_STRETCH_LEAVE / _SUSTAIN_MS / _REJOIN_MS override the
// AU12 band and dwells (dev-only tuning; invalid values keep defaults).
//
// AU15: PS2X_VSYNC_LOCK_AUDIO=tempo (only with PS2X_VSYNC_LOCK=1). The lock
// runs the guest at display refresh / guest rate (Odin: 121.7 / 119.88 =
// 1.0152), so the ring gains ~1.5 % a second and the band logic above cycles
// engage/rejoin (~0.7 per second, PX1). Locked-tempo mode follows PCSX2 under
// Sync to Host Refresh: SPU2::GetNominalRate() returns the host/guest ratio,
// AudioStream::SetNominalRate() makes it the stretcher's tempo when it would
// otherwise idle ("inactive" = tempo m_nominal_rate) and scales its target
// buffer by it. Here, while the pacer reports a locked ratio R: never bypass;
// tempo = base * smooth, base = R through a slow EMA (tau 2 s: the grid's
// period estimate wanders +/-0.3 %, PX1), smooth = the fill ratio above (so
// fill settles at the target, and a real guest dip still slows the tempo).
// No lock (ratio 0: menus before the first present, pause, knob off): the band
// logic resumes from the engaged state. Unset/other values = today.
//
// No sqrt() dampening (PCSX2 has it): with the FP1 wall pacer the guest
// rate is exactly <= 1.0, so linear control settles at fill = rate*target
// with more low-rate margin (0.6*target at 0.6x, not 0.36*target).
// Pure logic: no SoundTouch dependency, fake-clock testable.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace ps2_audio_stretch
{

constexpr uint32_t kSourceRate = 48000u; // SNDDRV output rate (AU9)
constexpr uint32_t kTargetLatencyMs = 80u; // in the brief's 60-100 ms band
constexpr uint32_t kTargetFrames = kSourceRate * kTargetLatencyMs / 1000u;
constexpr float kTempoMin = 0.5f;
constexpr float kTempoMax = 1.05f;
// AT1 legacy band (PS2X_STRETCH_LEGACY=1): immediate engage/rejoin.
constexpr float kLegacyLeave = 0.02f;  // engage outside +/-2 % of 1.0
constexpr float kLegacyRejoin = 0.01f; // release inside +/-1 % of 1.0
// AU12 sustained-deficit band (default): engage/rejoin need dwells.
constexpr float kLeaveDefault = 0.035f;   // engage outside +/-3.5 %
constexpr float kRejoinDefault = 0.01f;   // rejoin inside +/-1 %
constexpr double kSustainDefaultS = 0.30; // continuous outside-band time
constexpr double kRejoinDefaultS = 0.0;   // rejoin dwell (0 = immediate; a
                                          // dwell traps engaged under wander)
constexpr double kTauDownS = 0.05;        // EMA time constant on drops
constexpr double kTauUpS = 0.30;          // EMA time constant on rises
constexpr double kTauBaseS = 2.0;         // AU15: locked-ratio EMA

// PS2X_AUDIO_STRETCH: default on (Brad 09-26, after the C1 listen);
// the exact value "0" disables (legacy direct path, byte-identical output).
inline bool stretchEnabledFromEnv(const char *value)
{
    return value == nullptr || std::strcmp(value, "0") != 0;
}

// AU15: PS2X_VSYNC_LOCK_AUDIO. "tempo" = locked-tempo mode (see top);
// anything else (unset included) = today's band logic.
enum class LockAudioMode
{
    Stretch,
    Tempo,
};

inline LockAudioMode lockAudioModeFromEnv(const char *value)
{
    return value != nullptr && std::strcmp(value, "tempo") == 0 ? LockAudioMode::Tempo
                                                                : LockAudioMode::Stretch;
}

struct Params
{
    bool legacy = false;
    float leave = kLeaveDefault;
    float rejoin = kRejoinDefault;
    double sustainS = kSustainDefaultS;
    double rejoinS = kRejoinDefaultS;
};

namespace detail
{
inline bool parseDouble(const char *text, double &out)
{
    if (text == nullptr || *text == '\0')
        return false;
    char *end = nullptr;
    const double v = std::strtod(text, &end);
    if (end == text || *end != '\0' || !std::isfinite(v))
        return false;
    out = v;
    return true;
}
} // namespace detail

// Pure env parsing (each arg is the getenv() result for the named knob;
// nullptr = unset). Invalid values keep the compiled default.
inline Params paramsFromEnv(const char *legacy, const char *leave, const char *sustainMs,
                            const char *rejoinMs)
{
    Params p;
    if (legacy != nullptr && std::strcmp(legacy, "1") == 0)
    {
        p.legacy = true;
        p.leave = kLegacyLeave;
        p.rejoin = kLegacyRejoin;
        p.sustainS = 0.0;
        p.rejoinS = 0.0;
        return p;
    }
    double v = 0.0;
    if (detail::parseDouble(leave, v))
        p.leave = std::clamp(static_cast<float>(v), 0.005f, 0.15f);
    if (detail::parseDouble(sustainMs, v))
        p.sustainS = std::clamp(v / 1000.0, 0.0, 2.0);
    if (detail::parseDouble(rejoinMs, v))
        p.rejoinS = std::clamp(v / 1000.0, 0.0, 5.0);
    return p;
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
    uint64_t engages = 0; // bypass->engaged transitions (flutter counter)
    uint64_t locked = 0;  // AU15: steps in locked-tempo mode
    double sumBase = 0.0; // AU15: sum of the locked base over those steps
    float minTempo = 1.0f;
    float maxTempo = 1.0f;
    double sumTempo = 0.0;
};

class StretchController
{
public:
    StretchController() = default;
    explicit StretchController(const Params &params) : m_params(params) {}

    // One host-audio callback step. fillFrames is the ring depth BEFORE this
    // callback drains it; dtWallS is wall seconds since the previous step
    // (<= 0 on the first step: adopt raw immediately, no dwell accrues).
    // lockedRatio > 0 = AU15 locked-tempo mode for this step (the caller
    // passes it only under PS2X_VSYNC_LOCK_AUDIO=tempo while the pacer is
    // locked); 0 = today's band logic.
    StepResult update(uint64_t fillFrames, double dtWallS, float lockedRatio = 0.0f)
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
        const double dtPos = dtWallS > 0.0 ? dtWallS : 0.0;
        const float dev = std::fabs(m_smooth - 1.0f);
        if (lockedRatio > 0.0f)
        {
            if (m_base <= 0.0f)
                m_base = lockedRatio;
            else if (dtPos > 0.0)
                m_base += (lockedRatio - m_base) *
                          static_cast<float>(1.0 - std::exp(-dtPos / kTauBaseS));
            if (m_bypass)
                m_stats.engages += 1u;
            m_bypass = false;
            m_outsideS = 0.0;
            m_insideS = 0.0;
            return record(std::clamp(m_base * m_smooth, kTempoMin, kTempoMax * m_base), false,
                          true);
        }
        m_base = 0.0f;
        if (m_bypass)
        {
            // Legacy is the same path with sustainS == rejoinS == 0: the
            // first step outside the band engages (AT1, bit for bit).
            if (dev > m_params.leave)
            {
                m_outsideS += dtPos;
                if (m_outsideS >= m_params.sustainS)
                {
                    m_bypass = false;
                    m_insideS = 0.0;
                    m_stats.engages += 1u;
                }
            }
            else
            {
                m_outsideS = 0.0;
            }
        }
        else if (dev < m_params.rejoin)
        {
            m_insideS += dtPos;
            if (m_insideS >= m_params.rejoinS)
            {
                m_bypass = true;
                m_outsideS = 0.0;
            }
        }
        else
        {
            m_insideS = 0.0;
        }
        return record(m_bypass ? 1.0f : m_smooth, m_bypass, false);
    }

    float smoothedTempo() const { return m_smooth; }
    float lockedBase() const { return m_base; }
    bool bypass() const { return m_bypass; }
    const Params &params() const { return m_params; }
    const WindowStats &stats() const { return m_stats; }
    void resetStats() { m_stats = WindowStats{}; }

private:
    StepResult record(float tempo, bool bypass, bool locked)
    {
        StepResult out;
        out.tempo = tempo;
        out.bypass = bypass;
        m_stats.callbacks += 1u;
        if (locked)
        {
            m_stats.locked += 1u;
            m_stats.sumBase += m_base;
        }
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

    Params m_params;
    bool m_init = false;
    float m_smooth = 1.0f;
    float m_base = 0.0f; // AU15 locked-ratio EMA (0 = not locked)
    bool m_bypass = true;
    double m_outsideS = 0.0; // continuous wall-s with dev > leave (bypassed)
    double m_insideS = 0.0;  // continuous wall-s with dev < rejoin (engaged)
    WindowStats m_stats{};
};

} // namespace ps2_audio_stretch
