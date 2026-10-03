#pragma once
// HR1 prototype: present the paraLLEl scanout without a GPU->CPU readback.
//
// PS2X_PRESENT_ZERO_COPY=1 (macOS desktop, paraLLEl backend only; default
// off): the backend blits each scanout into one of three IOSurface-backed
// Vulkan images (MoltenVK VK_EXT_metal_objects) and publishes the surface
// here instead of filling PresentationFrame::pixels; the presenter binds the
// same IOSurface as a GL rectangle texture (CGLTexImageIOSurface2D) and
// GPU-blits it into the raylib frame texture. No CPU pixel copy anywhere.
// Not included by generated code (keeps incremental builds small).
#include <cstdint>

namespace ps2x_present_share
{
struct SharedFrame
{
    void *surface = nullptr; // IOSurfaceRef, owned by the backend's VkImage
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t seq = 0; // publication count (newest frame wins)
    // PL3: guest tick + export-submit wall (steady ns) for the unique-frame
    // count and frame age. The export runs inside this tick's processing, so
    // the submit wall stands in for the frame's guest-vsync wall time.
    uint64_t tick = 0;
    uint64_t submitWallNs = 0;
    int slot = -1; // PSO1: export pool slot (witness / ownership), -1 unknown
};

bool enabled(); // PS2X_PRESENT_ZERO_COPY=1 (macOS: GL blit; iOS: GLES texture cache)

// PSO1 (PRV1 §3): PS2X_PRESENT_OWNERSHIP=1 replaces publish()/latest() on the
// iOS export with ps2x_present_own::Pool (ownPool()): reserved READY/CURRENT
// frames, reuse only after the presenter's GL read fence completes. 0 = the
// pre-PSO1 mailbox (exact legacy path). Default: kOwnershipDefault.
bool ownershipEnabled();

// Apple: a w x h BGRA8 IOSurface that CoreVideo, GL/GLES and Metal accept
// (CVPixelBufferCreate with IOSurface properties; the pixel buffer stays
// retained for the process). MoltenVK's own exported surfaces carry no pixel
// format, which CoreVideo rejects. Returns the IOSurfaceRef, or nullptr.
void *createSurface(uint32_t width, uint32_t height);
void *pixelBufferFor(void *surface); // CVPixelBufferRef made by createSurface
void publish(const SharedFrame &frame);
bool latest(SharedFrame &out);

// macOS desktop only (ps2_present_share_mac.cpp): GPU-blit the surface into
// the w x h top-left rectangle of GL_TEXTURE_2D `texId` and force its alpha
// to one (texture swizzle). Must run on the thread that owns the GL context.
bool blitToTexture(const SharedFrame &frame, unsigned int texId);

// iOS only (ps2_present_share_ios.mm): GL_TEXTURE_2D name of the surface in
// the current EAGL context (CVOpenGLESTextureCache), cached per surface; the
// presenter draws it directly. 0 on failure.
unsigned int acquireTexture(const SharedFrame &frame);

// iOS only, PSO1: GL read fences on the presenter's EAGL stream
// (GL_APPLE_sync; GLES2). submitReadFence() runs after the shared quad's
// batch flush and returns the read's fence sequence (> 0). Without
// GL_APPLE_sync, or after a fence failure, it falls back to glFinish (counted)
// and the returned sequence is already complete. pollReadFences() polls with
// a zero timeout, oldest first, and returns the highest completed sequence.
// Both run on the GL thread only.
uint64_t submitReadFence();
uint64_t pollReadFences();
struct FenceStats
{
    uint64_t submitted = 0, completed = 0, pending = 0, failures = 0, finishFallbacks = 0, overflowFinishes = 0;
    bool appleSync = false;
};
FenceStats fenceStats();
// iOS only, PSO1 diagnostic (PS2X_PRESENT_CAPTURE_DIR): read the bound
// drawable back (glReadPixels RGBA) and write it as PPM. GL thread only.
bool captureDrawable(int width, int height, const char *path);

// Diagnostic (PS2X_PRESENT_SHARE_DUMP_TICKS): read the GL frame texture back
// (what DrawTexturePro samples) and write the w x h top-left region as PPM.
void dumpTexture(unsigned int texId, int texW, int texH, uint32_t w, uint32_t h, const char *path);
} // namespace ps2x_present_share
