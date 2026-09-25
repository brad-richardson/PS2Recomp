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
    uint64_t seq = 0;
};

bool enabled(); // PS2X_PRESENT_ZERO_COPY=1 (macOS: GL blit; iOS: GLES texture cache)

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

// Diagnostic (PS2X_PRESENT_SHARE_DUMP_TICKS): read the GL frame texture back
// (what DrawTexturePro samples) and write the w x h top-left region as PPM.
void dumpTexture(unsigned int texId, int texW, int texH, uint32_t w, uint32_t h, const char *path);
} // namespace ps2x_present_share
