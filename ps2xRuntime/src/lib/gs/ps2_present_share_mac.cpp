// HR1 prototype, GL side of PS2X_PRESENT_ZERO_COPY (see ps2_present_share.h).
// Own TU: the macOS GL/CGL/IOSurface headers stay out of raylib's TUs.
#include "runtime/gs/ps2_present_share.h"

#define GL_SILENCE_DEPRECATION 1
#include <IOSurface/IOSurfaceRef.h>
#include <OpenGL/CGLIOSurface.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>

#include <CoreVideo/CoreVideo.h>
#include <TargetConditionals.h>

#include <cstdio>
#include <unordered_map>
#include <vector>

#include "ps2_present_share_surface.inc"

namespace ps2x_present_share
{
bool blitToTexture(const SharedFrame &frame, unsigned int texId)
{
    CGLContextObj ctx = CGLGetCurrentContext();
    if (!ctx || !frame.surface || texId == 0u)
        return false;
    static GLuint s_fbo[2] = {0u, 0u};
    static std::unordered_map<void *, GLuint> s_rects; // one GL rect texture per surface
    static unsigned int s_swizzledTex = 0u;
    if (s_fbo[0] == 0u)
        glGenFramebuffers(2, s_fbo);

    GLint prevRead = 0, prevDraw = 0, prevRect = 0, prevTex2d = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDraw);
    glGetIntegerv(GL_TEXTURE_BINDING_RECTANGLE, &prevRect);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex2d);

    GLuint &rect = s_rects[frame.surface];
    if (rect == 0u)
    {
        glGenTextures(1, &rect);
        glBindTexture(GL_TEXTURE_RECTANGLE, rect);
        const CGLError err = CGLTexImageIOSurface2D(ctx, GL_TEXTURE_RECTANGLE, GL_RGBA8,
                                                    static_cast<GLsizei>(frame.width),
                                                    static_cast<GLsizei>(frame.height), GL_BGRA,
                                                    GL_UNSIGNED_INT_8_8_8_8_REV,
                                                    static_cast<IOSurfaceRef>(frame.surface), 0);
        glBindTexture(GL_TEXTURE_RECTANGLE, static_cast<GLuint>(prevRect));
        std::fprintf(stderr, "[present-share] bind surface=%p %ux%u rect=%u cgl_err=%d\n", frame.surface,
                     frame.width, frame.height, rect, static_cast<int>(err));
        if (err != kCGLNoError)
        {
            glDeleteTextures(1, &rect);
            s_rects.erase(frame.surface);
            return false;
        }
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, s_fbo[0]);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, rect, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_fbo[1]);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texId, 0);
    const GLint w = static_cast<GLint>(frame.width), h = static_cast<GLint>(frame.height);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prevRead));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(prevDraw));

    if (s_swizzledTex != texId)
    {
        // DK1: presentation alpha must be opaque (PS2 alpha is 0x80 on content).
        glBindTexture(GL_TEXTURE_2D, texId);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_ONE);
        s_swizzledTex = texId;
    }
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTex2d));

    static bool s_checked = false;
    if (!s_checked)
    {
        s_checked = true;
        std::fprintf(stderr, "[present-share] first blit %dx%d gl_err=0x%x\n", w, h, glGetError());
    }
    return true;
}

void dumpTexture(unsigned int texId, int texW, int texH, uint32_t w, uint32_t h, const char *path)
{
    std::vector<uint8_t> px(static_cast<size_t>(texW) * texH * 4u);
    GLint prev = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    glBindTexture(GL_TEXTURE_2D, texId);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev));
    FILE *f = std::fopen(path, "wb");
    if (!f)
        return;
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
            std::fwrite(&px[(static_cast<size_t>(y) * texW + x) * 4u], 1, 3, f);
    std::fclose(f);
    std::fprintf(stderr, "[present-share] dumped %s\n", path);
}
} // namespace ps2x_present_share
