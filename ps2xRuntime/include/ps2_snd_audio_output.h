#pragma once

namespace ps2_snd_audio_output
{
bool initialize();
void shutdown();
// BG1: pause/resume the host stream for Android pause-on-background.
// Idempotent; no-ops unless the stream is live.
void pausePlayback();
void resumePlayback();
}
