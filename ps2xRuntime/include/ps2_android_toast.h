#pragma once
// UX1: Android Toast for quick-save/load feedback (DS1 gap §2). The play
// present mode skips all GL draws, so the raylib status line is invisible;
// the runtime posts each noteQuickStatus message here and a short-lived
// worker shows it as a Toast on the UI thread. Post-and-return: the caller
// (GameThread at the vsync save point, or the main thread on chord edges)
// never blocks on the UI. Android-only; no-op declaration elsewhere so
// shared call sites compile everywhere.

#include <string>

namespace ps2x
{

#if defined(__ANDROID__)
void postQuickStatusToast(const std::string &message);
#else
inline void postQuickStatusToast(const std::string &) {}
#endif

} // namespace ps2x
