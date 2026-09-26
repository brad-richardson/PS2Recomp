#pragma once
// VK1 prototype: present the paraLLEl scanout straight from Vulkan (Android).
//
// Android, paraLLEl backend only; default ON since VK1 Part 2 (PS2X_PRESENT_VULKAN=0
// forces the GL path). The
// backend blits each scanout into one of four AHardwareBuffer-backed VkImages
// (imported into Turnip) and queues the buffer on a child SurfaceControl of
// the NativeActivity window (ASurfaceTransaction_setBuffer, API 29+), which
// SurfaceFlinger/HWC scale to the present rect. No GPU->CPU readback, no GLES
// upload; raylib's GL window stays underneath for input and lifecycle and
// only clears to black. Turnip is loaded as a HAL (no platform loader), so it
// has no Vulkan WSI swapchain; this is the swapchain. Any failure falls back
// to the readback path: the sink marks itself broken(), the backend stops
// queueing, active() goes false and the presenter draws the GL quad again.
// With the virtual pad on, the child sits UNDER the GL window (z = -1), which is
// then RGBA and clears the game rect to transparent. Not included by generated code.
#include <cstdint>

struct AHardwareBuffer;
struct ANativeWindow;
struct ANativeActivity;

namespace ps2x_present_vk
{
bool enabled(); // Android, API >= 29, PS2X_PRESENT_VULKAN != 0; false elsewhere
bool active();  // enabled, not broken, at least one buffer queued (the presenter skips its upload/draw)
bool broken();  // a setup/runtime failure switched this run to the GL path
void fallBack(const char *why); // any thread: give up on the layer for this run
// Main thread, before the first frame: put the child under the GL window (for an overlay
// such as the virtual pad). The GL window must then be RGBA and clear the game rect to 0.
void setUnderlay(bool under);
bool underlay();
// The current window's child has shown a buffer (the GL window is fully covered when
// there is no underlay and the rect fills the window).
bool layerLive();
// Bumped whenever the child is (re)made or detached for a window change. The GL
// swap may only be skipped after GL has put buffers on the current window: a
// window layer without a buffer has no buffer-to-window scaling, and the child
// would show at raw buffer size (VK1 Part 2 D2: 796x448 in a corner).
uint32_t windowGeneration();

// Main thread, once per host frame: the current window (nullptr while it is
// gone), the presenter's aspect (ps2x::present::Aspect as int) and the size of
// the window's own buffers (raylib's EGL surface). A child layer inherits the
// parent's buffer-to-window scaling, so its destination rect is placed in that
// buffer space. Creates the child layer for a new window.
void setHostWindow(ANativeWindow *window, ANativeActivity *activity, int aspect, int bufferW, int bufferH);
// Main thread, from APP_CMD_TERM_WINDOW (before the window is destroyed):
// detach the child; the next setHostWindow makes a new one even if the
// ANativeWindow pointer is reused.
void windowLost();

// GsWorker: RGBA8 buffer for a w x h slot (GPU color output + sampled +
// composer overlay + CPU_READ_RARELY, which keeps Qualcomm gralloc linear).
AHardwareBuffer *allocateBuffer(uint32_t w, uint32_t h);
void releaseBuffer(AHardwareBuffer *buffer);
// GsWorker: block (bounded) until SurfaceFlinger released `buffer` from its
// previous queue. False on timeout (counted; the caller reuses it anyway).
bool waitReusable(AHardwareBuffer *buffer, int timeoutMs);
// GsWorker: queue a finished buffer (w x h valid). False when no layer exists
// (frame dropped, counted).
bool queue(AHardwareBuffer *buffer, uint32_t w, uint32_t h);
// Diagnostic: lock the buffer through gralloc (CPU view) and hash/compare it
// against RGBA pixels at `rgba` (w x h, tight rows). Returns differing pixels
// (RGB only), or -1 if the lock failed. Writes PPMs when dumpDir is set.
long compareBuffer(AHardwareBuffer *buffer, const uint8_t *rgba, uint32_t w, uint32_t h, uint64_t tick,
                   const char *dumpDir);
// One-line counters for the periodic backend stats line.
void appendStats(char *out, unsigned size);
} // namespace ps2x_present_vk
