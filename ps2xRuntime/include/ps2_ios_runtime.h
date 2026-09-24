#pragma once
// I25: iOS-only startup and window helpers (implemented in
// src/lib/ps2_ios_runtime.mm, compiled only when PS2X_IS_IOS).

namespace ps2x::ios
{
// Apply <bundle>/ps2x.env then <Documents>/ps2x.env (later wins; keys the
// launcher already set win over both), expand ${BUNDLE} / ${DOCUMENTS},
// honour the Settings.bundle "Auto-route" switch, create the memory-card
// dirs, and set SDL hints (landscape, no accelerometer joystick). Call first
// thing in main(), before anything reads the environment or starts SDL.
void prepareEnvironment(const char *argv0);

// Attach SDL's UIWindow to the app's window scene (once it has connected)
// and forward UIKit's window size to raylib whenever it changes, so the
// viewport and GetScreenWidth/Height match. Call after InitWindow and once
// per presented frame (main thread); cheap when nothing changed.
void syncWindowSize();
// I26: current touches from SDL's finger state, normalised 0..1 to the
// window (x right, y down). Returns how many were written (<= max).
int touchPoints(float *xs, float *ys, int max);
} // namespace ps2x::ios
