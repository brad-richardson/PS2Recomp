#include "ps2_snd_audio_output.h"

#include "ThreadNaming.h"
#include "ps2_audio_stretch.h"
#include "ps2_snd_spike.h"
#include "ps2_vsync_lock.h"
#include "ps2_vsync_pacer.h"
#include "raylib.h"
#include "SoundTouch.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace
{
    constexpr uint32_t kSourceRate = 48000u; // SNDDRV output rate (AU9: tag-1 upsampled 3->4)
    constexpr size_t kWavLimitBytes = 200000000u;
    // AT1: PCSX2 AudioStreamParameters defaults (AudioStreamTypes.h).
    constexpr int kStSequenceMs = 30;
    constexpr int kStSeekWindowMs = 20;
    constexpr int kStOverlapMs = 10;

    struct WavFile
    {
        std::string path;
        std::fstream file;
        size_t bytes = 0;
    };

    struct Output
    {
        AudioStream stream{};
        bool ready = false;
        uint32_t rate = kSourceRate;
        WavFile wav;     // PS2X_SOUND_WAV: knob off = final output (as before);
                         // knob on = pre-stretch source-rate frames.
        WavFile postWav; // PS2X_SND_WAV_OUT: final output (post-stretch).
        double phase = 0.0;
        uint32_t previous = 0;
        uint32_t next = 0;
        bool havePair = false;
        std::chrono::steady_clock::time_point statsAt{};
        uint64_t lastUnderruns = 0;
        uint64_t lastOverflows = 0;
        // AT1 stretch state (audio-callback thread only, except init).
        bool stretch = false;
        std::unique_ptr<soundtouch::SoundTouch> st;
        ps2_audio_stretch::StretchController controller;
        bool lockTempo = false; // AU15: PS2X_VSYNC_LOCK_AUDIO=tempo with PS2X_VSYNC_LOCK=1
        bool feedDemand = false; // AU16: PS2X_STRETCH_FEED=demand
        // AU16 path depth per 5 s window: ring fill + (engaged) frames the
        // stretcher holds ahead of it = push->callback-output latency.
        uint64_t pathSteps = 0;
        double pathSum = 0.0;
        uint64_t pathMax = 0;
        double heldSum = 0.0;
        std::chrono::steady_clock::time_point lastCallback{};
        std::chrono::steady_clock::time_point stretchStatsAt{};
        std::vector<float> floatBuf;
        std::vector<float> floatOut;
        std::vector<float> drainBuf;
        std::vector<int16_t> srcBuf;
        std::vector<uint32_t> tapped;
        // Active + resampled pull state.
        std::vector<uint32_t> stQueue;
        size_t stQueuePos = 0;
        // AU12 per-callback stretch trace (PS2X_STRETCH_TRACE, dev-only):
        // t_ms,fill,smooth,bypass,tempo. Bounded; transitions also go to
        // stderr while tracing (capped separately).
        std::FILE *traceFile = nullptr;
        uint64_t traceLines = 0;
        bool traceTrCapped = false;
        uint64_t traceTrLines = 0;
        bool traceLastBypass = true;
        std::chrono::steady_clock::time_point traceT0{};
    } g_output;
    // IP6: [perf-audio] window counters. The device callback is the only
    // writer of the cb* / tempo fields; takeWindowCounters (main thread)
    // exchanges them out.
    struct AudioWindow
    {
        std::atomic<uint64_t> callbacks{0};
        std::atomic<uint64_t> cbFrames{0};
        std::atomic<uint64_t> bypassed{0};
        std::atomic<uint64_t> tempoSumMicro{0};
        std::atomic<uint32_t> tempoMinMicro{UINT32_MAX};
        std::atomic<uint32_t> tempoMaxMicro{0};
        // Main-thread cursors.
        uint64_t lastWrite = 0;
        uint64_t lastRead = 0;
        uint64_t lastUnderruns = 0;
        uint64_t lastOverflows = 0;
    } g_window;

    void noteWindowCallback(unsigned int frames, bool bypass, float tempo)
    {
        g_window.callbacks.fetch_add(1u, std::memory_order_relaxed);
        g_window.cbFrames.fetch_add(frames, std::memory_order_relaxed);
        if (bypass)
            g_window.bypassed.fetch_add(1u, std::memory_order_relaxed);
        const uint32_t micro = static_cast<uint32_t>(std::lround(static_cast<double>(tempo) * 1e6));
        g_window.tempoSumMicro.fetch_add(micro, std::memory_order_relaxed);
        if (micro < g_window.tempoMinMicro.load(std::memory_order_relaxed))
            g_window.tempoMinMicro.store(micro, std::memory_order_relaxed);
        if (micro > g_window.tempoMaxMicro.load(std::memory_order_relaxed))
            g_window.tempoMaxMicro.store(micro, std::memory_order_relaxed);
    }

    constexpr uint64_t kTraceLineCap = 131072u; // ~21 min at 100 callbacks/s
    constexpr uint64_t kTraceTrCap = 8192u;

    int16_t unpackLeft(uint32_t frame) { return static_cast<int16_t>(frame & 0xffffu); }
    int16_t unpackRight(uint32_t frame) { return static_cast<int16_t>(frame >> 16); }

    void recordWav(WavFile &wav, const int16_t *samples, size_t count)
    {
        if (!wav.file.is_open())
            return;
        const size_t writable = std::min(count * sizeof(int16_t), kWavLimitBytes - wav.bytes);
        if (!writable)
            return;
        wav.file.seekp(44 + static_cast<std::streamoff>(wav.bytes));
        wav.file.write(reinterpret_cast<const char *>(samples), writable);
        wav.bytes += writable;
        // The wall-capped boot harness terminates the runner with SIGTERM.
        // Keep the on-disk header valid after every callback for that path.
        wav.file.seekp(4);
        const uint32_t riffBytes = 36u + static_cast<uint32_t>(wav.bytes);
        const char riff[4] = {static_cast<char>(riffBytes), static_cast<char>(riffBytes >> 8),
                              static_cast<char>(riffBytes >> 16), static_cast<char>(riffBytes >> 24)};
        wav.file.write(riff, sizeof(riff));
        wav.file.seekp(40);
        const uint32_t dataBytes = static_cast<uint32_t>(wav.bytes);
        const char data[4] = {static_cast<char>(dataBytes), static_cast<char>(dataBytes >> 8),
                              static_cast<char>(dataBytes >> 16), static_cast<char>(dataBytes >> 24)};
        wav.file.write(data, sizeof(data));
        wav.file.flush();
    }

    bool nextInputFrame(uint32_t &frame)
    {
        if (ps2_snd_spike::pcmRing().pop(frame))
            return true;
        ps2_snd_spike::pcmRing().noteUnderrun();
        frame = 0;
        return false;
    }

    void minuteStats(const std::chrono::steady_clock::time_point &now)
    {
        if (g_output.statsAt == std::chrono::steady_clock::time_point{})
            g_output.statsAt = now;
        if (now - g_output.statsAt >= std::chrono::minutes(1))
        {
            const uint64_t underruns = ps2_snd_spike::pcmRing().underruns();
            const uint64_t overflows = ps2_snd_spike::pcmRing().overflows();
            std::cerr << "[snd-output] wall-minute underruns=" << (underruns - g_output.lastUnderruns)
                      << " overflows=" << (overflows - g_output.lastOverflows)
                      << " total-underruns=" << underruns << " total-overflows=" << overflows << '\n';
            g_output.lastUnderruns = underruns;
            g_output.lastOverflows = overflows;
            g_output.statsAt = now;
        }
    }

    void audioCallbackDirect(void *buffer, unsigned int frames)
    {
        auto *output = static_cast<int16_t *>(buffer);
        const size_t samples = static_cast<size_t>(frames) * 2u;
        if (g_output.rate == kSourceRate)
        {
            for (unsigned int i = 0; i < frames; ++i)
            {
                uint32_t frame = 0;
                nextInputFrame(frame);
                output[2u * i] = unpackLeft(frame);
                output[2u * i + 1u] = unpackRight(frame);
            }
        }
        else
        {
            for (unsigned int i = 0; i < frames; ++i)
            {
                if (!g_output.havePair)
                {
                    nextInputFrame(g_output.previous);
                    nextInputFrame(g_output.next);
                    g_output.havePair = true;
                }
                const double t = g_output.phase;
                const auto interpolate = [t](int16_t a, int16_t b)
                {
                    return static_cast<int16_t>(std::clamp(
                        static_cast<int>(a + (b - a) * t), -32768, 32767));
                };
                output[2u * i] = interpolate(unpackLeft(g_output.previous), unpackLeft(g_output.next));
                output[2u * i + 1u] = interpolate(unpackRight(g_output.previous), unpackRight(g_output.next));
                g_output.phase += static_cast<double>(kSourceRate) / g_output.rate;
                while (g_output.phase >= 1.0)
                {
                    g_output.previous = g_output.next;
                    nextInputFrame(g_output.next);
                    g_output.phase -= 1.0;
                }
            }
        }
        noteWindowCallback(frames, true, 1.0f);
        recordWav(g_output.wav, output, samples);
        recordWav(g_output.postWav, output, samples);
        minuteStats(std::chrono::steady_clock::now());
    }

    // ---- AT1 stretch path (default on; PS2X_AUDIO_STRETCH=0 disables) ----

    void s16ToFloat(const int16_t *s16, float *out, size_t frames)
    {
        for (size_t i = 0; i < frames * 2u; ++i)
            out[i] = static_cast<float>(s16[i]) / 32768.0f;
    }

    int16_t floatToS16(float v)
    {
        const long rounded = std::lround(static_cast<double>(v) * 32768.0);
        return static_cast<int16_t>(std::clamp(rounded, -32768l, 32767l));
    }

    // Pop up to want frames from the ring (no zero-fill: the caller decides).
    size_t popRing(uint32_t *frames, size_t want)
    {
        size_t got = 0;
        while (got < want && ps2_snd_spike::pcmRing().pop(frames[got]))
            ++got;
        return got;
    }

    void unpackFrames(const uint32_t *frames, size_t count, int16_t *s16)
    {
        for (size_t i = 0; i < count; ++i)
        {
            s16[2u * i] = unpackLeft(frames[i]);
            s16[2u * i + 1u] = unpackRight(frames[i]);
        }
    }

    // Drain-and-discard stretcher output (bypass mode): keeps SoundTouch's
    // internal latency bounded while the callback plays ring frames direct.
    void drainStretcher()
    {
        if (g_output.drainBuf.size() < 1024u * 2u)
            g_output.drainBuf.resize(1024u * 2u);
        while (g_output.st->receiveSamples(g_output.drainBuf.data(), 1024u) != 0)
        {
        }
    }

    // Pull one source-rate frame from the stretcher's output (active mode).
    // Returns false on shortfall (frame = 0 silence, counted as underrun).
    bool pullStretched(uint32_t &frame)
    {
        if (g_output.stQueuePos >= g_output.stQueue.size())
        {
            g_output.stQueue.clear();
            g_output.stQueuePos = 0;
            if (g_output.floatOut.size() < 1024u * 2u)
                g_output.floatOut.resize(1024u * 2u);
            const uint got =
                g_output.st->receiveSamples(g_output.floatOut.data(), 1024u);
            for (uint i = 0; i < got; ++i)
            {
                const int16_t l = floatToS16(g_output.floatOut[2u * i]);
                const int16_t r = floatToS16(g_output.floatOut[2u * i + 1u]);
                g_output.stQueue.push_back(static_cast<uint32_t>(static_cast<uint16_t>(l)) |
                                           (static_cast<uint32_t>(static_cast<uint16_t>(r)) << 16));
            }
        }
        if (g_output.stQueuePos >= g_output.stQueue.size())
        {
            ps2_snd_spike::pcmRing().noteUnderrun();
            frame = 0;
            return false;
        }
        frame = g_output.stQueue[g_output.stQueuePos++];
        return true;
    }

    void traceStretch(const std::chrono::steady_clock::time_point &now, uint64_t fill,
                      const ps2_audio_stretch::StepResult &step)
    {
        if (g_output.traceT0 == std::chrono::steady_clock::time_point{})
        {
            g_output.traceT0 = now;
            g_output.traceLastBypass = true;
        }
        const double tMs =
            std::chrono::duration<double, std::milli>(now - g_output.traceT0).count();
        if (step.bypass != g_output.traceLastBypass)
        {
            g_output.traceLastBypass = step.bypass;
            if (g_output.traceTrLines < kTraceTrCap)
            {
                std::cerr << "[snd-stretch-tr] t=" << tMs << "ms "
                          << (step.bypass ? "rejoin" : "engage")
                          << " smooth=" << g_output.controller.smoothedTempo() << " fill=" << fill
                          << '\n';
                g_output.traceTrLines += 1u;
            }
            else if (!g_output.traceTrCapped)
            {
                std::cerr << "[snd-stretch-tr] (capped)\n";
                g_output.traceTrCapped = true;
            }
        }
        if (g_output.traceLines < kTraceLineCap)
        {
            std::fprintf(g_output.traceFile, "%.3f,%llu,%.6f,%d,%.6f\n", tMs,
                         static_cast<unsigned long long>(fill),
                         static_cast<double>(g_output.controller.smoothedTempo()),
                         step.bypass ? 1 : 0, static_cast<double>(step.tempo));
            g_output.traceLines += 1u;
            // The wall-capped boot harness terminates the runner with
            // SIGTERM; keep the on-disk trace valid for that path.
            std::fflush(g_output.traceFile);
        }
    }

    void stretchStats(const std::chrono::steady_clock::time_point &now)
    {
        if (g_output.stretchStatsAt == std::chrono::steady_clock::time_point{})
            g_output.stretchStatsAt = now;
        if (now - g_output.stretchStatsAt < std::chrono::seconds(5))
            return;
        const auto &st = g_output.controller.stats();
        const double mean = st.callbacks ? st.sumTempo / st.callbacks : 1.0;
        const double bypassPct = st.callbacks ? 100.0 * st.bypassed / st.callbacks : 100.0;
        std::cerr << "[snd-stretch] underruns=" << ps2_snd_spike::pcmRing().underruns()
                  << " overflows=" << ps2_snd_spike::pcmRing().overflows()
                  << " tempo min=" << st.minTempo << " mean=" << mean << " max=" << st.maxTempo
                  << " bypass=" << bypassPct << "% engages=" << st.engages
                  << " callbacks=" << st.callbacks;
        if (g_output.lockTempo)
            std::cerr << " locked=" << (st.callbacks ? 100.0 * st.locked / st.callbacks : 0.0)
                      << "% base=" << (st.locked ? st.sumBase / st.locked : 0.0);
        if (g_output.pathSteps)
        {
            const double msPerFrame = 1000.0 / kSourceRate;
            std::fprintf(stderr, " path_ms=%.1f/%.1f held_ms=%.1f",
                         g_output.pathSum / g_output.pathSteps * msPerFrame,
                         static_cast<double>(g_output.pathMax) * msPerFrame,
                         g_output.heldSum / g_output.pathSteps * msPerFrame);
        }
        std::cerr << '\n';
        g_output.pathSteps = 0;
        g_output.pathSum = 0.0;
        g_output.pathMax = 0;
        g_output.heldSum = 0.0;
        g_output.controller.resetStats();
        g_output.stretchStatsAt = now;
    }

    void audioCallbackStretch(void *buffer, unsigned int frames)
    {
        auto *output = static_cast<int16_t *>(buffer);
        const auto now = std::chrono::steady_clock::now();
        double dt = -1.0;
        if (g_output.lastCallback != std::chrono::steady_clock::time_point{})
            dt = std::chrono::duration<double>(now - g_output.lastCallback).count();
        g_output.lastCallback = now;

        const uint64_t fill = ps2_snd_spike::pcmRing().size();
        const float lockRatio =
            g_output.lockTempo ? ps2_vsync_lock::audioRatio(ps2_vsync_pacer::steadyNowNs()) : 0.0f;
        const ps2_audio_stretch::StepResult step = g_output.controller.update(fill, dt, lockRatio);
        // Bypass keeps the stretcher primed at the fill tempo (seamless
        // engage); engaged, the applied tempo (== smoothed outside AU15's
        // locked mode, which scales it by the locked ratio).
        g_output.st->setTempo(static_cast<double>(
            step.bypass ? g_output.controller.smoothedTempo() : step.tempo));
        if (g_output.traceFile != nullptr)
            traceStretch(now, fill, step);
        {
            const uint64_t held = step.bypass
                                      ? 0u
                                      : static_cast<uint64_t>(g_output.st->numUnprocessedSamples()) +
                                            g_output.st->numSamples() +
                                            (g_output.stQueue.size() - g_output.stQueuePos);
            g_output.pathSteps += 1u;
            g_output.pathSum += static_cast<double>(fill + held);
            g_output.pathMax = std::max(g_output.pathMax, fill + held);
            g_output.heldSum += static_cast<double>(held);
        }

        // Source-rate frames for this callback (pre-resample).
        g_output.srcBuf.resize(static_cast<size_t>(frames) * 2u);
        std::vector<uint32_t> fed;
        fed.reserve(frames + 2u);

        if (step.bypass)
        {
            if (g_output.rate == kSourceRate)
            {
                for (unsigned int i = 0; i < frames; ++i)
                {
                    uint32_t frame = 0;
                    nextInputFrame(frame);
                    fed.push_back(frame);
                    output[2u * i] = unpackLeft(frame);
                    output[2u * i + 1u] = unpackRight(frame);
                }
            }
            else
            {
                // Byte-identical resample of ring frames (same pull order as
                // the direct path); tapped frames feed the stretcher copy.
                g_output.tapped.clear();
                const auto tapNext = [](uint32_t &frame)
                {
                    const bool ok = nextInputFrame(frame);
                    g_output.tapped.push_back(frame);
                    return ok;
                };
                for (unsigned int i = 0; i < frames; ++i)
                {
                    if (!g_output.havePair)
                    {
                        tapNext(g_output.previous);
                        tapNext(g_output.next);
                        g_output.havePair = true;
                    }
                    const double t = g_output.phase;
                    const auto interpolate = [t](int16_t a, int16_t b)
                    {
                        return static_cast<int16_t>(std::clamp(
                            static_cast<int>(a + (b - a) * t), -32768, 32767));
                    };
                    output[2u * i] =
                        interpolate(unpackLeft(g_output.previous), unpackLeft(g_output.next));
                    output[2u * i + 1u] =
                        interpolate(unpackRight(g_output.previous), unpackRight(g_output.next));
                    g_output.phase += static_cast<double>(kSourceRate) / g_output.rate;
                    while (g_output.phase >= 1.0)
                    {
                        g_output.previous = g_output.next;
                        tapNext(g_output.next);
                        g_output.phase -= 1.0;
                    }
                }
                fed = g_output.tapped;
            }
            // Feed the stretcher a copy so engaging is seamless; discard its
            // output (the callback plays ring frames direct).
            if (!fed.empty())
            {
                g_output.floatBuf.resize(fed.size() * 2u);
                for (size_t i = 0; i < fed.size(); ++i)
                {
                    g_output.floatBuf[2u * i] = static_cast<float>(unpackLeft(fed[i])) / 32768.0f;
                    g_output.floatBuf[2u * i + 1u] =
                        static_cast<float>(unpackRight(fed[i])) / 32768.0f;
                }
                g_output.st->putSamples(g_output.floatBuf.data(),
                                        static_cast<uint>(fed.size()));
            }
            drainStretcher();
            g_output.stQueue.clear();
            g_output.stQueuePos = 0;
        }
        else
        {
            const size_t srcNeed = g_output.rate == kSourceRate
                                       ? frames
                                       : static_cast<size_t>(std::ceil(
                                             frames * static_cast<double>(kSourceRate) /
                                             g_output.rate)) +
                                             2u;
            if (g_output.feedDemand)
            {
                g_output.floatBuf.resize(ps2_audio_stretch::kDemandChunkFrames * 2u);
                std::array<uint32_t, ps2_audio_stretch::kDemandChunkFrames> chunk{};
                ps2_audio_stretch::feedOnDemand(
                    *g_output.st, g_output.stQueue.size() - g_output.stQueuePos, srcNeed,
                    g_output.floatBuf.data(),
                    [&fed, &chunk](float *out, size_t max)
                    {
                        const size_t got = popRing(chunk.data(), std::min(max, chunk.size()));
                        for (size_t i = 0; i < got; ++i)
                        {
                            out[2u * i] = static_cast<float>(unpackLeft(chunk[i])) / 32768.0f;
                            out[2u * i + 1u] = static_cast<float>(unpackRight(chunk[i])) / 32768.0f;
                        }
                        fed.insert(fed.end(), chunk.begin(), chunk.begin() + got);
                        return got;
                    });
            }
            size_t fedCount = 0;
            if (!g_output.feedDemand)
            {
                const size_t feedWant =
                    static_cast<size_t>(std::ceil(srcNeed * static_cast<double>(step.tempo)));
                fed.resize(feedWant);
                fedCount = popRing(fed.data(), feedWant);
                fed.resize(fedCount);
            }
            if (fedCount > 0)
            {
                g_output.floatBuf.resize(fedCount * 2u);
                for (size_t i = 0; i < fedCount; ++i)
                {
                    g_output.floatBuf[2u * i] = static_cast<float>(unpackLeft(fed[i])) / 32768.0f;
                    g_output.floatBuf[2u * i + 1u] =
                        static_cast<float>(unpackRight(fed[i])) / 32768.0f;
                }
                g_output.st->putSamples(g_output.floatBuf.data(),
                                        static_cast<uint>(fedCount));
            }
            if (g_output.rate == kSourceRate)
            {
                for (unsigned int i = 0; i < frames; ++i)
                {
                    uint32_t frame = 0;
                    pullStretched(frame);
                    output[2u * i] = unpackLeft(frame);
                    output[2u * i + 1u] = unpackRight(frame);
                }
            }
            else
            {
                for (unsigned int i = 0; i < frames; ++i)
                {
                    if (!g_output.havePair)
                    {
                        pullStretched(g_output.previous);
                        pullStretched(g_output.next);
                        g_output.havePair = true;
                    }
                    const double t = g_output.phase;
                    const auto interpolate = [t](int16_t a, int16_t b)
                    {
                        return static_cast<int16_t>(std::clamp(
                            static_cast<int>(a + (b - a) * t), -32768, 32767));
                    };
                    output[2u * i] =
                        interpolate(unpackLeft(g_output.previous), unpackLeft(g_output.next));
                    output[2u * i + 1u] =
                        interpolate(unpackRight(g_output.previous), unpackRight(g_output.next));
                    g_output.phase += static_cast<double>(kSourceRate) / g_output.rate;
                    while (g_output.phase >= 1.0)
                    {
                        g_output.previous = g_output.next;
                        pullStretched(g_output.next);
                        g_output.phase -= 1.0;
                    }
                }
            }
        }

        // Source WAV: the consumed ring frames at source rate (pre-stretch).
        if (g_output.wav.file.is_open() && !fed.empty())
        {
            g_output.srcBuf.resize(fed.size() * 2u);
            unpackFrames(fed.data(), fed.size(), g_output.srcBuf.data());
            recordWav(g_output.wav, g_output.srcBuf.data(), fed.size() * 2u);
        }
        recordWav(g_output.postWav, output, static_cast<size_t>(frames) * 2u);
        noteWindowCallback(frames, step.bypass, step.bypass ? 1.0f : step.tempo);
        stretchStats(now);
        minuteStats(now);
    }

    void audioCallback(void *buffer, unsigned int frames)
    {
        // PL1: raylib owns this mixer thread (we only own the callback), so
        // name it from inside, once, for the perf log's per-thread CPU.
        static std::atomic<bool> s_named{false};
        if (!s_named.exchange(true, std::memory_order_relaxed))
            ThreadNaming::SetCurrentThreadName("Audio");
        if (g_output.stretch)
            audioCallbackStretch(buffer, frames);
        else
            audioCallbackDirect(buffer, frames);
    }

    void writeU16(std::ostream &file, uint16_t value)
    {
        const char bytes[2] = {static_cast<char>(value), static_cast<char>(value >> 8)};
        file.write(bytes, sizeof(bytes));
    }

    void writeU32(std::ostream &file, uint32_t value)
    {
        const char bytes[4] = {static_cast<char>(value), static_cast<char>(value >> 8),
                               static_cast<char>(value >> 16), static_cast<char>(value >> 24)};
        file.write(bytes, sizeof(bytes));
    }

    void openWav(WavFile &wav, const char *path, uint32_t rate)
    {
        if (!path || !*path)
            return;
        wav.path = path;
        wav.file.open(wav.path, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
        if (wav.file)
        {
            wav.file.write("RIFF", 4);
            writeU32(wav.file, 36u);
            wav.file.write("WAVEfmt ", 8);
            writeU32(wav.file, 16u);
            writeU16(wav.file, 1u);
            writeU16(wav.file, 2u);
            writeU32(wav.file, rate);
            writeU32(wav.file, rate * 4u);
            writeU16(wav.file, 4u);
            writeU16(wav.file, 16u);
            wav.file.write("data", 4);
            writeU32(wav.file, 0u);
            wav.file.flush();
        }
        else
            std::cerr << "[snd-output] failed opening WAV: " << wav.path << '\n';
    }

    void saveWav(WavFile &wav, uint32_t rate)
    {
        if (wav.path.empty())
            return;
        if (wav.file.is_open())
            wav.file.close();
        if (wav.file.fail() || wav.bytes == 0)
            std::cerr << "[snd-output] failed writing WAV: " << wav.path << '\n';
        else
            std::cerr << "[snd-output] WAV " << wav.path << " bytes=" << wav.bytes
                      << " rate=" << rate << '\n';
    }
}

namespace ps2_snd_audio_output
{
bool initialize()
{
    if (!IsAudioDeviceReady())
        return false;
    g_output.stream = LoadAudioStream(kSourceRate, 16, 2);
    if (!IsAudioStreamValid(g_output.stream))
    {
        g_output.rate = 48000u;
        g_output.stream = LoadAudioStream(g_output.rate, 16, 2);
        if (!IsAudioStreamValid(g_output.stream))
            return false;
        std::cerr << "[snd-output] 48 kHz stream unavailable\n";
    }
    g_output.stretch =
        ps2_audio_stretch::stretchEnabledFromEnv(std::getenv("PS2X_AUDIO_STRETCH"));
    if (!g_output.stretch)
        std::cerr << "[snd-output] stretch=off (PS2X_AUDIO_STRETCH=0)\n";
    if (g_output.stretch)
    {
        const ps2_audio_stretch::Params params = ps2_audio_stretch::paramsFromEnv(
            std::getenv("PS2X_STRETCH_LEAVE"), std::getenv("PS2X_STRETCH_SUSTAIN_MS"),
            std::getenv("PS2X_STRETCH_REJOIN_MS"));
        g_output.controller = ps2_audio_stretch::StretchController(params);
        g_output.st = std::make_unique<soundtouch::SoundTouch>();
        g_output.st->setSampleRate(kSourceRate);
        g_output.st->setChannels(2);
        g_output.st->setSetting(SETTING_SEQUENCE_MS, kStSequenceMs);
        g_output.st->setSetting(SETTING_SEEKWINDOW_MS, kStSeekWindowMs);
        g_output.st->setSetting(SETTING_OVERLAP_MS, kStOverlapMs);
        g_output.st->setSetting(SETTING_USE_QUICKSEEK, 0);
        g_output.st->setSetting(SETTING_USE_AA_FILTER, 0);
        g_output.st->setTempo(1.0);
        std::cerr << "[snd-output] stretch=on (SoundTouch " << soundtouch::SoundTouch::getVersionString()
                  << ", target=" << ps2_audio_stretch::kTargetLatencyMs << "ms tempo=["
                  << ps2_audio_stretch::kTempoMin << "," << ps2_audio_stretch::kTempoMax << "] "
                  << "sustained-deficit leave=+/-"
                  << params.leave * 100.0 << "%/" << params.sustainS * 1000.0 << "ms rejoin=+/-"
                  << params.rejoin * 100.0 << "%/" << params.rejoinS * 1000.0 << "ms)\n";
        g_output.lockTempo = ps2_vsync_lock::enabled() &&
                             ps2_audio_stretch::lockAudioModeFromEnv(std::getenv("PS2X_VSYNC_LOCK_AUDIO")) ==
                                 ps2_audio_stretch::LockAudioMode::Tempo;
        if (g_output.lockTempo)
            std::cerr << "[snd-output] vsync-lock audio=tempo (AU15: constant tempo at the locked ratio)\n";
        g_output.feedDemand = ps2_audio_stretch::feedDemandFromEnv(std::getenv("PS2X_STRETCH_FEED"));
        if (g_output.feedDemand)
            std::cerr << "[snd-output] stretch feed=demand (AU16)\n";
        const char *tracePath = std::getenv("PS2X_STRETCH_TRACE");
        if (tracePath != nullptr && *tracePath != '\0')
        {
            g_output.traceFile = std::fopen(tracePath, "w");
            if (g_output.traceFile != nullptr)
            {
                std::fprintf(g_output.traceFile, "# t_ms,fill,smooth,bypass,tempo\n");
                std::fflush(g_output.traceFile);
                std::cerr << "[snd-output] stretch-trace=" << tracePath << '\n';
            }
            else
                std::cerr << "[snd-output] failed opening stretch trace: " << tracePath << '\n';
        }
    }
    SetAudioStreamCallback(g_output.stream, audioCallback);
    openWav(g_output.wav, std::getenv("PS2X_SOUND_WAV"),
            g_output.stretch ? kSourceRate : g_output.rate);
    openWav(g_output.postWav, std::getenv("PS2X_SND_WAV_OUT"), g_output.rate);
    PlayAudioStream(g_output.stream);
    g_output.ready = true;
    std::cerr << "[snd-output] stream rate=" << g_output.rate << " channels=2 bits=16\n";
    return true;
}

void pausePlayback()
{
    // The ring keeps its pause-time fill (the guest is frozen too), so the
    // resume callback drains real frames while the guest refills: no gap.
    if (g_output.ready && IsAudioStreamPlaying(g_output.stream))
        PauseAudioStream(g_output.stream);
}

void resumePlayback()
{
    if (g_output.ready && !IsAudioStreamPlaying(g_output.stream))
        ResumeAudioStream(g_output.stream);
}

WindowCounters takeWindowCounters()
{
    WindowCounters w;
    w.ready = g_output.ready;
    w.stretch = g_output.stretch;
    w.streamRate = g_output.rate;
    auto &ring = ps2_snd_spike::pcmRing();
    const uint64_t write = ring.writeTotal();
    const uint64_t read = ring.readTotal();
    const uint64_t underruns = ring.underruns();
    const uint64_t overflows = ring.overflows();
    w.pushed = write - g_window.lastWrite;
    w.underruns = underruns - g_window.lastUnderruns;
    w.overflows = overflows - g_window.lastOverflows;
    const uint64_t readDelta = read - g_window.lastRead;
    w.consumed = readDelta >= w.overflows ? readDelta - w.overflows : 0u;
    w.fill = ring.size();
    g_window.lastWrite = write;
    g_window.lastRead = read;
    g_window.lastUnderruns = underruns;
    g_window.lastOverflows = overflows;
    w.callbacks = g_window.callbacks.exchange(0u, std::memory_order_relaxed);
    w.cbFrames = g_window.cbFrames.exchange(0u, std::memory_order_relaxed);
    w.bypassed = g_window.bypassed.exchange(0u, std::memory_order_relaxed);
    const uint64_t sum = g_window.tempoSumMicro.exchange(0u, std::memory_order_relaxed);
    const uint32_t mn = g_window.tempoMinMicro.exchange(UINT32_MAX, std::memory_order_relaxed);
    const uint32_t mx = g_window.tempoMaxMicro.exchange(0u, std::memory_order_relaxed);
    if (w.callbacks)
    {
        w.tempoMean = static_cast<double>(sum) / 1e6 / static_cast<double>(w.callbacks);
        w.tempoMin = mn / 1e6;
        w.tempoMax = mx / 1e6;
    }
    return w;
}

void shutdown()
{
    if (g_output.ready)
    {
        StopAudioStream(g_output.stream);
        UnloadAudioStream(g_output.stream);
        g_output.ready = false;
    }
    g_output.st.reset();
    if (g_output.traceFile != nullptr)
    {
        std::fclose(g_output.traceFile);
        std::cerr << "[snd-output] stretch-trace lines=" << g_output.traceLines << '\n';
        g_output.traceFile = nullptr;
    }
    saveWav(g_output.wav, g_output.stretch ? kSourceRate : g_output.rate);
    saveWav(g_output.postWav, g_output.rate);
}
}
