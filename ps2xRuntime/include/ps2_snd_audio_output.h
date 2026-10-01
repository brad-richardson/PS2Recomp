#pragma once

#include <cstdint>

namespace ps2_snd_audio_output
{
bool initialize();
void shutdown();
// BG1: pause/resume the host stream for Android pause-on-background.
// Idempotent; no-ops unless the stream is live.
void pausePlayback();
void resumePlayback();

// IP6: host-side audio counters since the previous call (perf log
// [perf-audio], main thread). cbFrames = frames the device callback asked for
// at the stream rate; pushed/consumed = PCM ring frames in/out (consumed
// excludes overflow drops); tempo = applied stretch tempo per callback.
struct WindowCounters
{
    bool ready = false;
    bool stretch = false;
    uint32_t streamRate = 0;
    uint64_t callbacks = 0;
    uint64_t cbFrames = 0;
    uint64_t pushed = 0;
    uint64_t consumed = 0;
    uint64_t underruns = 0;
    uint64_t overflows = 0;
    uint64_t fill = 0;
    uint64_t bypassed = 0;
    double tempoMean = 1.0;
    double tempoMin = 1.0;
    double tempoMax = 1.0;
};
WindowCounters takeWindowCounters();
}
