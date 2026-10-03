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
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>
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

// PSO1: presenter read fences (GL_APPLE_sync on the EAGL GLES2 context).
namespace
{
struct PendingFence
{
    uint64_t seq;
    GLsync sync;
};
std::deque<PendingFence> g_fences;
uint64_t g_fenceSeq = 0u;
uint64_t g_fenceDone = 0u;
FenceStats g_fenceStats;
int g_appleSync = -1; // -1 unknown, 0 no, 1 yes
constexpr size_t kMaxPendingFences = 8u; // bounded: a stuck stream glFinishes

bool haveAppleSync()
{
    if (g_appleSync < 0)
    {
        const char *ext = reinterpret_cast<const char *>(glGetString(GL_EXTENSIONS));
        g_appleSync = (ext && std::strstr(ext, "GL_APPLE_sync")) ? 1 : 0;
        g_fenceStats.appleSync = g_appleSync == 1;
        std::fprintf(stderr, "[present-own] GL_APPLE_sync=%d\n", g_appleSync);
    }
    return g_appleSync == 1;
}

// glFinish completes every earlier command on this stream: all reads done.
void finishAll()
{
    glFinish();
    for (PendingFence &f : g_fences)
        glDeleteSyncAPPLE(f.sync);
    g_fences.clear();
    g_fenceDone = g_fenceSeq;
}
} // namespace

uint64_t submitReadFence()
{
    const uint64_t seq = ++g_fenceSeq;
    ++g_fenceStats.submitted;
    if (!haveAppleSync())
    {
        ++g_fenceStats.finishFallbacks;
        finishAll();
        return seq;
    }
    if (g_fences.size() >= kMaxPendingFences)
    {
        ++g_fenceStats.overflowFinishes;
        finishAll();
        return seq;
    }
    GLsync sync = glFenceSyncAPPLE(GL_SYNC_GPU_COMMANDS_COMPLETE_APPLE, 0);
    if (!sync)
    {
        ++g_fenceStats.failures;
        finishAll();
        return seq;
    }
    g_fences.push_back({seq, sync});
    return seq;
}

uint64_t pollReadFences()
{
    while (!g_fences.empty())
    {
        PendingFence &f = g_fences.front();
        const GLenum r = glClientWaitSyncAPPLE(f.sync, GL_SYNC_FLUSH_COMMANDS_BIT_APPLE, 0);
        if (r == GL_ALREADY_SIGNALED_APPLE || r == GL_CONDITION_SATISFIED_APPLE)
        {
            glDeleteSyncAPPLE(f.sync);
            g_fenceDone = f.seq;
            ++g_fenceStats.completed;
            g_fences.pop_front();
            continue;
        }
        if (r == GL_WAIT_FAILED_APPLE)
        {
            // Never treat a failed query as completion: finish the stream.
            ++g_fenceStats.failures;
            finishAll();
        }
        break; // GL_TIMEOUT_EXPIRED_APPLE: later fences are later still
    }
    g_fenceStats.pending = g_fences.size();
    return g_fenceDone;
}

FenceStats fenceStats()
{
    FenceStats s = g_fenceStats;
    s.pending = g_fences.size();
    return s;
}

void finishGl()
{
    glFinish();
}

bool captureDrawable(int width, int height, const char *path)
{
    if (width <= 0 || height <= 0)
        return false;
    std::vector<uint8_t> px(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    FILE *f = std::fopen(path, "wb");
    if (!f)
        return false;
    std::fprintf(f, "P6\n%d %d\n255\n", width, height);
    std::vector<uint8_t> row(static_cast<size_t>(width) * 3u);
    for (int y = height - 1; y >= 0; --y) // GL rows are bottom-up
    {
        const uint8_t *src = &px[static_cast<size_t>(y) * static_cast<size_t>(width) * 4u];
        for (int x = 0; x < width; ++x)
        {
            row[static_cast<size_t>(x) * 3u + 0u] = src[x * 4 + 0];
            row[static_cast<size_t>(x) * 3u + 1u] = src[x * 4 + 1];
            row[static_cast<size_t>(x) * 3u + 2u] = src[x * 4 + 2];
        }
        std::fwrite(row.data(), 1u, row.size(), f);
    }
    std::fclose(f);
    std::fprintf(stderr, "[present-own] captured %dx%d -> %s\n", width, height, path);
    return true;
}
} // namespace ps2x_present_share
