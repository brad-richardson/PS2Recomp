#pragma once
// I25: iOS-only startup and window helpers (implemented in
// src/lib/ps2_ios_runtime.mm, compiled only when PS2X_IS_IOS).

#include <cstdint>
#include <string>

namespace ps2x::ios
{
// Apply <bundle>/ps2x.env, then <bundle>/full120.env when the Settings.bundle
// "Target 120 fps" switch is on (IQ1), then <Documents>/ps2x.env (later wins;
// keys the launcher already set win over all), expand ${BUNDLE} / ${DOCUMENTS},
// honour the Settings.bundle "Auto-route" switch, create the memory-card
// dirs, and set SDL hints (landscape, no accelerometer joystick). Call first
// thing in main(), before anything reads the environment or starts SDL.
void prepareEnvironment(const char *argv0);

// Attach SDL's UIWindow to the app's window scene (once it has connected)
// and forward UIKit's window size to raylib whenever it changes, so the
// viewport and GetScreenWidth/Height match. Call after InitWindow and once
// per presented frame (main thread); cheap when nothing changed.
void syncWindowSize();
// IQ1: PS2X_DISPLAY_HZ=120 on iOS. Adds a main-run-loop CADisplayLink whose
// preferredFrameRateRange asks for 120 Hz (ProMotion; the Info.plist carries
// CADisableMinimumFrameDurationOnPhone), logs UIScreen.maximumFramesPerSecond.
// Inert on a 60 Hz panel. Call once on the main thread after InitWindow.
void requestDisplayRate(int hz);
// IX1: the panel's size in physical pixels, landscape (width >= height), from
// UIScreen.nativeBounds. Cached by prepareEnvironment (main thread), so it is
// callable from any thread before the window exists. False if never cached.
bool panelSize(int &w, int &h);
// I26: current touches from SDL's finger state, normalised 0..1 to the
// window (x right, y down). Returns how many were written (<= max).
// VT1: IDs are SDL finger IDs (stable per touch until lift-off).
int touchPoints(int64_t *ids, float *xs, float *ys, int max);
// IP3: one-line device state for the perf log ("thermal=<0-3> lpm=<0/1>
// batt=<0-100|-1> chg=<0/1>"; batt=-1 when the level is unknown). Main
// thread only; cheap (no I/O).
std::string perfDeviceState();
// IB3 (AirPods): set AVAudioSessionCategoryPlayback after InitAudioDevice.
// raylib's miniaudio inits its iOS context with the default category, which
// tries PlayAndRecord + DefaultToSpeaker without Bluetooth options, so iOS
// routes game audio to the speaker and AirPods lose A2DP. miniaudio sets the
// category only at context init, so this override sticks. Main thread.
void setAudioSessionPlayback();
} // namespace ps2x::ios
