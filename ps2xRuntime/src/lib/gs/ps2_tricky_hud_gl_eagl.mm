// HUD3: iOS platform glue for the GLES3 HUD composite. Own EAGLContext
// (GLES3) + own CVOpenGLESTextureCache (the presenter's context/cache on the
// GL thread is untouched) + per-surface frame textures (cached; the pool
// retains surfaces for the process, so the map is bounded by the slot
// count). Any failure returns null/0/false and the caller falls back to the
// CPU stamp.
#include <TargetConditionals.h>
#if defined(__APPLE__) && TARGET_OS_IOS
#include "runtime/gs/ps2_tricky_hud_gl.h"
#include "runtime/gs/ps2_present_share.h"

#define GLES_SILENCE_DEPRECATION 1
#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurfaceRef.h>
#include <OpenGLES/EAGL.h>
#include <OpenGLES/ES3/gl.h>

#include <cstdio>
#include <unordered_map>

struct HudGlPlatform
{
    EAGLContext *context = nil;
    CVOpenGLESTextureCacheRef cache = nullptr;
    std::unordered_map<void *, unsigned> textures; // surface -> GL_TEXTURE_2D
};

HudGlPlatform *hudGlPlatformCreate()
{
    HudGlPlatform *p = new HudGlPlatform();
    for (;;)
    {
        p->context = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES3];
        if (!p->context)
        {
            std::fprintf(stderr, "[ssx3-tricky-hud] gpu: GLES3 EAGLContext failed\n");
            break;
        }
        if (![EAGLContext setCurrentContext:p->context])
        {
            std::fprintf(stderr, "[ssx3-tricky-hud] gpu: EAGL setCurrent failed\n");
            break;
        }
        const CVReturn rc = CVOpenGLESTextureCacheCreate(
            kCFAllocatorDefault, nullptr, (CVEAGLContext)p->context, nullptr, &p->cache);
        if (rc != kCVReturnSuccess || !p->cache)
        {
            std::fprintf(stderr, "[ssx3-tricky-hud] gpu: texture cache failed rc=%d\n", rc);
            p->cache = nullptr;
            break;
        }
        const char *version = (const char *)glGetString(GL_VERSION);
        std::fprintf(stderr, "[ssx3-tricky-hud] gpu: EAGL ready (%s)\n", version ? version : "?");
        [EAGLContext setCurrentContext:nil];
        return p;
    }
    hudGlPlatformDestroy(p);
    return nullptr;
}

void hudGlPlatformDestroy(HudGlPlatform *p)
{
    if (!p)
        return;
    if (p->context)
        [EAGLContext setCurrentContext:p->context];
    for (auto &kv : p->textures)
    {
        GLuint tex = kv.second;
        if (tex)
            glDeleteTextures(1, &tex);
    }
    p->textures.clear();
    if (p->cache)
    {
        CFRelease(p->cache);
        p->cache = nullptr;
    }
    [EAGLContext setCurrentContext:nil];
    p->context = nil;
    delete p;
}

bool hudGlPlatformMakeCurrent(HudGlPlatform *p)
{
    if (!p || !p->context)
        return false;
    return [EAGLContext setCurrentContext:p->context] == YES;
}

unsigned hudGlPlatformFrameTexture(HudGlPlatform *p, void *platformImage, int w, int h,
                                   bool &rbNative)
{
    rbNative = true; // the pool creates 32BGRA surfaces
    if (!p || !platformImage || w <= 0 || h <= 0 || !p->cache)
        return 0;
    auto it = p->textures.find(platformImage);
    if (it != p->textures.end())
        return it->second;
    IOSurfaceRef surface = (IOSurfaceRef)platformImage;
    if ((int)IOSurfaceGetWidth(surface) != w || (int)IOSurfaceGetHeight(surface) != h)
        return 0;
    CVPixelBufferRef pb = (CVPixelBufferRef)ps2x_present_share::pixelBufferFor(platformImage);
    if (!pb)
        return 0;
    CVOpenGLESTextureRef texture = nullptr;
    // Same arguments as the presenter's acquireTexture (GL_BGRA_EXT 0x80E1;
    // the enum lives in the ES2 glext header, which is not included here).
    const GLenum kBgraExt = (GLenum)0x80E1;
    const CVReturn rc = CVOpenGLESTextureCacheCreateTextureFromImage(
        kCFAllocatorDefault, p->cache, pb, nullptr, GL_TEXTURE_2D, GL_RGBA, w, h, kBgraExt,
        GL_UNSIGNED_BYTE, 0, &texture);
    if (rc != kCVReturnSuccess || !texture)
        return 0;
    const GLuint tex = CVOpenGLESTextureGetName(texture);
    CFRelease(texture);
    if (!tex)
        return 0;
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    const bool clean = glGetError() == GL_NO_ERROR;
    glBindTexture(GL_TEXTURE_2D, 0);
    if (!clean)
        return 0;
    p->textures[platformImage] = tex;
    return tex;
}

void hudGlPlatformReleaseImage(HudGlPlatform *p, void *platformImage)
{
    if (!p || !platformImage)
        return;
    auto it = p->textures.find(platformImage);
    if (it == p->textures.end())
        return;
    if (p->context && [EAGLContext setCurrentContext:p->context])
    {
        GLuint tex = it->second;
        if (tex)
            glDeleteTextures(1, &tex);
        [EAGLContext setCurrentContext:nil];
    }
    p->textures.erase(it);
}

void hudGlPlatformFinish(HudGlPlatform *p)
{
    if (!p)
        return;
    glFinish();
    if (p->cache)
        CVOpenGLESTextureCacheFlush(p->cache, 0);
    // Unbind so any thread (composite or retire) can bind next.
    [EAGLContext setCurrentContext:nil];
}
#endif // __APPLE__ && TARGET_OS_IOS
