// HR1 spike, GLES side of PS2X_PRESENT_ZERO_COPY on iOS (see ps2_present_share.h).
// The backend's IOSurface is wrapped in a CVPixelBuffer and exposed to the
// current EAGL context through CVOpenGLESTextureCache as a GL_TEXTURE_2D; the
// presenter draws that texture directly (no blit, no CPU copy).
#include "runtime/gs/ps2_present_share.h"

#define GLES_SILENCE_DEPRECATION 1
#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurfaceRef.h>
#include <OpenGLES/EAGL.h>
#include <OpenGLES/ES2/gl.h>
#include <OpenGLES/ES2/glext.h>

#include <TargetConditionals.h>

#include <cstdio>
#include <unordered_map>

#include "ps2_present_share_surface.inc"

namespace ps2x_present_share
{
namespace
{
struct CvEntry
{
    CVPixelBufferRef buffer = nullptr;
    CVOpenGLESTextureRef texture = nullptr;
};
} // namespace

unsigned int acquireTexture(const SharedFrame &frame)
{
    static CVOpenGLESTextureCacheRef s_cache = nullptr;
    static std::unordered_map<void *, CvEntry> s_entries;
    if (!frame.surface)
        return 0u;
    if (!s_cache)
    {
        EAGLContext *ctx = [EAGLContext currentContext];
        const CVReturn rc = CVOpenGLESTextureCacheCreate(kCFAllocatorDefault, nullptr, (CVEAGLContext)ctx, nullptr,
                                                         &s_cache);
        std::fprintf(stderr, "[present-share] ios texture cache ctx=%p rc=%d api=%d\n", (void *)ctx, rc,
                     ctx ? (int)ctx.API : -1);
        if (rc != kCVReturnSuccess)
        {
            s_cache = nullptr;
            return 0u;
        }
    }
    CvEntry &e = s_entries[frame.surface];
    if (!e.texture)
    {
        CVReturn rc = kCVReturnSuccess;
        e.buffer = static_cast<CVPixelBufferRef>(pixelBufferFor(frame.surface));
        if (!e.buffer)
            rc = CVPixelBufferCreateWithIOSurface(kCFAllocatorDefault, static_cast<IOSurfaceRef>(frame.surface),
                                                  nullptr, &e.buffer);
        if (rc == kCVReturnSuccess)
            rc = CVOpenGLESTextureCacheCreateTextureFromImage(kCFAllocatorDefault, s_cache, e.buffer, nullptr,
                                                              GL_TEXTURE_2D, GL_RGBA,
                                                              static_cast<GLsizei>(frame.width),
                                                              static_cast<GLsizei>(frame.height), GL_BGRA_EXT,
                                                              GL_UNSIGNED_BYTE, 0, &e.texture);
        const GLuint name = e.texture ? CVOpenGLESTextureGetName(e.texture) : 0u;
        std::fprintf(stderr, "[present-share] ios bind surface=%p %ux%u fmt=0x%x rc=%d tex=%u\n", frame.surface,
                     frame.width, frame.height,
                     (unsigned)IOSurfaceGetPixelFormat(static_cast<IOSurfaceRef>(frame.surface)), rc, name);
        if (rc != kCVReturnSuccess || !e.texture)
        {
            s_entries.erase(frame.surface);
            return 0u;
        }
        glBindTexture(GL_TEXTURE_2D, name);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    return CVOpenGLESTextureGetName(e.texture);
}

bool blitToTexture(const SharedFrame &, unsigned int)
{
    return false; // iOS draws the shared texture directly (acquireTexture)
}

void dumpTexture(unsigned int, int, int, uint32_t, uint32_t, const char *) {}
} // namespace ps2x_present_share
