// HUD4 (from HUD3): shared GLES3 composite (iOS EAGL; the Android EGL glue
// is dropped). P0 copies the HUD region to a temp texture with a trivial
// shader (exact copy; a blit would need matching frame/temp formats, and
// iOS rejects GL_BGRA_EXT as a texture internalformat with INVALID_ENUM),
// P1 draws the smears sampling the temp, P2 draws the sprite quads sampling
// the atlas (RGBA8UI, uploaded once), blended in draw order. The shaders
// mirror the CPU model (execHudSceneModel) expression for expression; the
// device diag proves the real GPU output. Any GL failure returns false (CPU
// fallback); after 3 consecutive failures the backend parks itself broken
// (CPU forever).
#if defined(__ANDROID__)
#include <GLES3/gl3.h>
#elif defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IOS
#include <OpenGLES/ES3/gl.h>
#endif
#endif
#if !defined(__ANDROID__) && (!defined(__APPLE__) || !TARGET_OS_IOS)
#error "ps2_tricky_hud_gl.cpp compiles on Android/iOS only"
#endif

#include "ps2_ssx3_tricky_hud.h"
#include "runtime/gs/ps2_tricky_hud_gl.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
// HUD4: the temp is always RGBA8 (see ensureTemp). iOS rejects GL_BGRA_EXT
// (0x80E1) as a glTexImage2D internalformat (INVALID_ENUM, found on the
// first iPad leg); the P0 copy is a shader, so no format match is needed
// and every lane treats the bytes uniformly.

const char *kVsSmear = R"GLSL(
#version 300 es
uniform vec2 uFrameWH;
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec4 aRect;
layout(location = 2) in vec2 aEdges;
layout(location = 3) in vec2 aOrg;
out vec2 vPos;
out vec4 vRect;
out vec2 vEdges;
out vec2 vOrg;
void main()
{
    vPos = aPos;
    vRect = aRect;
    vEdges = aEdges;
    vOrg = aOrg;
    vec2 ndc = vec2(aPos.x / uFrameWH.x * 2.0 - 1.0, aPos.y / uFrameWH.y * 2.0 - 1.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
}
)GLSL";

const char *kVsSprite = R"GLSL(
#version 300 es
uniform vec2 uFrameWH;
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec4 aSrc;
layout(location = 2) in vec4 aDst;
layout(location = 3) in float aDim;
out vec2 vPos;
out vec4 vSrc;
out vec4 vDst;
out float vDim;
void main()
{
    vPos = aPos;
    vSrc = aSrc;
    vDst = aDst;
    vDim = aDim;
    vec2 ndc = vec2(aPos.x / uFrameWH.x * 2.0 - 1.0, aPos.y / uFrameWH.y * 2.0 - 1.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
}
)GLSL";

// The smear fragment mirrors smearCoverS statement for statement: the same
// feather/lerp expressions in the same order (t, fy0/fy1, fx0/fx1, the
// sequential mins), the same edge columns, the same mix. Temp reads recover
// the exact u8 (floor(t*255+0.5) is exact: t*255 lands within 3e-5 of the
// integer). Output (mix, a) blends exactly as the CPU's formula.
const char *kFsSmear = R"GLSL(
#version 300 es
precision highp float;
precision highp sampler2D;
uniform sampler2D uTemp;
in vec2 vPos;
in vec4 vRect;
in vec2 vEdges;
in vec2 vOrg;
layout(location = 0) out vec4 oColor;
void main()
{
    int x = int(floor(vPos.x));
    int y = int(floor(vPos.y));
    float span = vRect.z - vRect.x;
    float t = (float(x) - vRect.x) / span;
    float fy0 = (float(y) - vRect.y) / 3.0;
    float fy1 = (vRect.w - float(y)) / 3.0;
    float a = fy0;
    if (fy1 < a)
        a = fy1;
    float fx0 = (float(x) - vRect.x) / 2.0;
    float fx1 = (vRect.z - float(x)) / 2.0;
    if (fx0 < a)
        a = fx0;
    if (fx1 < a)
        a = fx1;
    if (a <= 0.0)
        discard;
    if (a > 1.0)
        a = 1.0;
    ivec2 Li = ivec2(int(vEdges.x - vOrg.x), y - int(vOrg.y));
    ivec2 Ri = ivec2(int(vEdges.y - vOrg.x), y - int(vOrg.y));
    vec3 L = floor(texelFetch(uTemp, Li, 0).rgb * 255.0 + 0.5);
    vec3 R = floor(texelFetch(uTemp, Ri, 0).rgb * 255.0 + 0.5);
    vec3 mixc = L + (R - L) * t;
#if HUD3_SWAP_RB
    oColor = vec4(mixc.b / 255.0, mixc.g / 255.0, mixc.r / 255.0, a);
#else
    oColor = vec4(mixc.r / 255.0, mixc.g / 255.0, mixc.b / 255.0, a);
#endif
}
)GLSL";

// The sprite fragment mirrors sampleAtlas statement for statement (the same
// (rx+0.5)*s/d-0.5 mapping, floor, the same clamp branches) with exact-u8
// atlas reads (RGBA8UI + usampler). Output (sample, sa) blends exactly as
// blendSample; uForceAlpha draws the alpha-fix pass (255 where sa > 0).
const char *kFsSprite = R"GLSL(
#version 300 es
precision highp float;
precision highp usampler2D;
uniform usampler2D uAtlas;
uniform int uForceAlpha;
in vec2 vPos;
in vec4 vSrc;
in vec4 vDst;
in float vDim;
layout(location = 0) out vec4 oColor;
void main()
{
    int rx = int(floor(vPos.x - vDst.x));
    int ry = int(floor(vPos.y - vDst.y));
    float sw = vSrc.z;
    float sh = vSrc.w;
    float dw = vDst.z;
    float dh = vDst.w;
    float v = (float(ry) + 0.5) * sh / dh - 0.5;
    int v0 = int(floor(v));
    float fv = v - float(v0);
    if (v0 < 0)
    {
        v0 = 0;
        fv = 0.0;
    }
    if (v0 > int(sh) - 2)
    {
        v0 = int(sh) - 2;
        fv = 1.0;
    }
    float u = (float(rx) + 0.5) * sw / dw - 0.5;
    int u0 = int(floor(u));
    float fu = u - float(u0);
    if (u0 < 0)
    {
        u0 = 0;
        fu = 0.0;
    }
    if (u0 > int(sw) - 2)
    {
        u0 = int(sw) - 2;
        fu = 1.0;
    }
    ivec2 base = ivec2(int(vSrc.x) + u0, int(vSrc.y) + v0);
    vec4 P00 = vec4(texelFetch(uAtlas, base, 0));
    vec4 P10 = vec4(texelFetch(uAtlas, base + ivec2(1, 0), 0));
    vec4 P01 = vec4(texelFetch(uAtlas, base + ivec2(0, 1), 0));
    vec4 P11 = vec4(texelFetch(uAtlas, base + ivec2(1, 1), 0));
    float w00 = (1.0 - fu) * (1.0 - fv);
    float w10 = fu * (1.0 - fv);
    float w01 = (1.0 - fu) * fv;
    float w11 = fu * fv;
    float sr = (P00.r * w00 + P10.r * w10 + P01.r * w01 + P11.r * w11) * vDim;
    float sg = (P00.g * w00 + P10.g * w10 + P01.g * w01 + P11.g * w11) * vDim;
    float sb = (P00.b * w00 + P10.b * w10 + P01.b * w01 + P11.b * w11) * vDim;
    float sa = (P00.a * w00 + P10.a * w10 + P01.a * w01 + P11.a * w11) / 255.0;
    if (sa <= 0.0)
        discard;
#if HUD3_SWAP_RB
    float tmp = sr;
    sr = sb;
    sb = tmp;
#endif
    float alpha = uForceAlpha != 0 ? 1.0 : sa;
    oColor = vec4(sr / 255.0, sg / 255.0, sb / 255.0, alpha);
}
)GLSL";

// HUD4 P0: exact region copy, frame texture -> temp (fullscreen triangle
// from gl_VertexID; no VBO). texelFetch reads the stored bytes uniformly,
// so the temp holds the same bytes as the frame on every lane.
const char *kVsCopy = R"GLSL(
#version 300 es
void main()
{
    float x = -1.0 + float((gl_VertexID & 1) << 2);
    float y = -1.0 + float((gl_VertexID & 2) << 1);
    gl_Position = vec4(x, y, 0.0, 1.0);
}
)GLSL";

const char *kFsCopy = R"GLSL(
#version 300 es
precision highp float;
uniform sampler2D uSrc;
uniform ivec2 uOrigin;
layout(location = 0) out vec4 oColor;
void main()
{
    oColor = texelFetch(uSrc, ivec2(gl_FragCoord.xy) + uOrigin, 0);
}
)GLSL";

GLuint compileShader(GLenum type, const char *prefix, const char *src, const char *name)
{
    GLuint sh = glCreateShader(type);
    // #version must lead the translation unit: skip leading blank lines
    // (the raw literals start with one), then splice the prefix after the
    // #version line.
    const char *s = src ? src : "";
    while (*s == '\n' || *s == '\r' || *s == ' ' || *s == '\t')
        ++s;
    std::string head, body;
    const char *nl = std::strchr(s, '\n');
    if (prefix && *prefix && nl && std::strncmp(s, "#version", 8) == 0)
    {
        head.assign(s, nl + 1);
        head += prefix;
        body = nl + 1;
    }
    else
    {
        body = s;
    }
    const char *p[2] = {head.c_str(), body.c_str()};
    glShaderSource(sh, head.empty() ? 1 : 2, head.empty() ? &p[1] : p, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[2048];
        GLsizei n = 0;
        glGetShaderInfoLog(sh, sizeof(log), &n, log);
        std::fprintf(stderr, "[ssx3-tricky-hud] gpu: %s compile failed: %.*s\n", name, (int)n,
                     log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

GLuint linkProgram(GLuint vs, GLuint fs, const char *name)
{
    GLuint pr = glCreateProgram();
    glAttachShader(pr, vs);
    glAttachShader(pr, fs);
    glLinkProgram(pr);
    GLint ok = 0;
    glGetProgramiv(pr, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        char log[2048];
        GLsizei n = 0;
        glGetProgramInfoLog(pr, sizeof(log), &n, log);
        std::fprintf(stderr, "[ssx3-tricky-hud] gpu: %s link failed: %.*s\n", name, (int)n, log);
        glDeleteProgram(pr);
        return 0;
    }
    return pr;
}

bool checkGl(const char *where)
{
    bool clean = true;
    for (;;)
    {
        const GLenum e = glGetError();
        if (e == GL_NO_ERROR)
            break;
        clean = false;
        std::fprintf(stderr, "[ssx3-tricky-hud] gpu: GL error 0x%x at %s\n", e, where);
    }
    return clean;
}

bool checkFbo(const char *where)
{
    const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE)
    {
        std::fprintf(stderr, "[ssx3-tricky-hud] gpu: FBO incomplete 0x%x at %s\n", st, where);
        return false;
    }
    return true;
}
} // namespace

struct HudGlBackend
{
    HudGlPlatform *plat = nullptr;
    bool broken = false;
    unsigned consecutiveFails = 0;
    bool loggedInit = false;
    // Programs (swap variants are lane-diag only, compiled lazily).
    unsigned progSmear = 0, progSmearSwap = 0, progSprite = 0, progSpriteSwap = 0;
    unsigned progCopy = 0; // HUD4 P0 (no swap variant: the copy is byte-identical)
    int uSmearFrameWH = -1, uSpriteFrameWH = -1, uSpriteAtlas = -1, uSpriteForceAlpha = -1;
    int uTemp = -1;
    int uCopySrc = -1, uCopyOrigin = -1;
    int uSmearSwapFrameWH = -1, uTempSwap = -1;
    int uSpriteSwapFrameWH = -1, uSpriteSwapAtlas = -1, uSpriteSwapForceAlpha = -1;
    // Geometry (VAO holds the VBO binding + format; data re-uploaded).
    unsigned vaoSmear = 0, vboSmear = 0, vaoSprite = 0, vboSprite = 0;
    unsigned vaoEmpty = 0; // HUD4 P0 (attribute-less triangle)
    unsigned vboSmearVerts = 0, vboSpriteVerts = 0;
    // Targets.
    unsigned fboFrame = 0, fboRead = 0, fboDraw = 0;
    unsigned texTemp = 0;
    int tempW = 0, tempH = 0;
    unsigned texAtlas = 0;
    const void *atlasPtr = nullptr;
    // Pass state.
    unsigned frameTex = 0;
    int fw = 0, fh = 0;
    // Scene cache (rebuild ~0.1x/s per HUD2's key rate).
    ps2_ssx3_tricky_hud::HudVisualKey key;
    const void *spritesPtr = nullptr;
    bool hasScene = false;
    // Stats.
    uint64_t composites = 0, compositeNs = 0, sceneBuilds = 0;
};

HudGlBackend *hudGlCreate()
{
    HudGlPlatform *plat = hudGlPlatformCreate();
    if (!plat)
        return nullptr;
    HudGlBackend *b = new HudGlBackend();
    b->plat = plat;
    return b;
}

void hudGlDestroy(HudGlBackend *b)
{
    if (!b)
        return;
    hudGlPlatformDestroy(b->plat);
    delete b;
}

void hudGlReleaseImage(HudGlBackend *b, void *platformImage)
{
    if (!b || !b->plat)
        return;
    hudGlPlatformReleaseImage(b->plat, platformImage);
}

void hudGlStats(const HudGlBackend *b, char *out, unsigned size)
{
    if (!out || size == 0)
        return;
    if (!b)
    {
        std::snprintf(out, size, "hud-gpu=off");
        return;
    }
    const double avg =
        b->composites ? static_cast<double>(b->compositeNs) / 1000.0 / b->composites : 0.0;
    std::snprintf(out, size, "hud-gpu n=%llu avg=%.1fus scenes=%llu%s",
                  (unsigned long long)b->composites, avg, (unsigned long long)b->sceneBuilds,
                  b->broken ? " BROKEN(cpu)" : "");
}

namespace
{
bool ensurePrograms(HudGlBackend *b, bool swap)
{
    if (!b->progCopy)
    {
        GLuint vs = compileShader(GL_VERTEX_SHADER, "", kVsCopy, "copy-vs");
        GLuint fs = compileShader(GL_FRAGMENT_SHADER, "", kFsCopy, "copy-fs");
        if (vs && fs)
            b->progCopy = linkProgram(vs, fs, "copy");
        if (vs)
            glDeleteShader(vs);
        if (fs)
            glDeleteShader(fs);
        if (!b->progCopy)
            return false;
        b->uCopySrc = glGetUniformLocation(b->progCopy, "uSrc");
        b->uCopyOrigin = glGetUniformLocation(b->progCopy, "uOrigin");
    }
    if (!b->progSmear)
    {
        GLuint vs = compileShader(GL_VERTEX_SHADER, "", kVsSmear, "smear-vs");
        GLuint fs = compileShader(GL_FRAGMENT_SHADER, "#define HUD3_SWAP_RB 0\n", kFsSmear,
                                  "smear-fs");
        if (vs && fs)
            b->progSmear = linkProgram(vs, fs, "smear");
        if (vs)
            glDeleteShader(vs);
        if (fs)
            glDeleteShader(fs);
        if (!b->progSmear)
            return false;
        b->uSmearFrameWH = glGetUniformLocation(b->progSmear, "uFrameWH");
        b->uTemp = glGetUniformLocation(b->progSmear, "uTemp");
    }
    if (!b->progSprite)
    {
        GLuint vs = compileShader(GL_VERTEX_SHADER, "", kVsSprite, "sprite-vs");
        GLuint fs = compileShader(GL_FRAGMENT_SHADER, "#define HUD3_SWAP_RB 0\n", kFsSprite,
                                  "sprite-fs");
        if (vs && fs)
            b->progSprite = linkProgram(vs, fs, "sprite");
        if (vs)
            glDeleteShader(vs);
        if (fs)
            glDeleteShader(fs);
        if (!b->progSprite)
            return false;
        b->uSpriteFrameWH = glGetUniformLocation(b->progSprite, "uFrameWH");
        b->uSpriteAtlas = glGetUniformLocation(b->progSprite, "uAtlas");
        b->uSpriteForceAlpha = glGetUniformLocation(b->progSprite, "uForceAlpha");
    }
    if (swap && !b->progSmearSwap)
    {
        GLuint vs = compileShader(GL_VERTEX_SHADER, "", kVsSmear, "smear-vs-swap");
        GLuint fs = compileShader(GL_FRAGMENT_SHADER, "#define HUD3_SWAP_RB 1\n", kFsSmear,
                                  "smear-fs-swap");
        if (vs && fs)
            b->progSmearSwap = linkProgram(vs, fs, "smear-swap");
        if (vs)
            glDeleteShader(vs);
        if (fs)
            glDeleteShader(fs);
        if (!b->progSmearSwap)
            return false;
        b->uSmearSwapFrameWH = glGetUniformLocation(b->progSmearSwap, "uFrameWH");
        b->uTempSwap = glGetUniformLocation(b->progSmearSwap, "uTemp");
    }
    if (swap && !b->progSpriteSwap)
    {
        GLuint vs = compileShader(GL_VERTEX_SHADER, "", kVsSprite, "sprite-vs-swap");
        GLuint fs = compileShader(GL_FRAGMENT_SHADER, "#define HUD3_SWAP_RB 1\n", kFsSprite,
                                  "sprite-fs-swap");
        if (vs && fs)
            b->progSpriteSwap = linkProgram(vs, fs, "sprite-swap");
        if (vs)
            glDeleteShader(vs);
        if (fs)
            glDeleteShader(fs);
        if (!b->progSpriteSwap)
            return false;
        b->uSpriteSwapFrameWH = glGetUniformLocation(b->progSpriteSwap, "uFrameWH");
        b->uSpriteSwapAtlas = glGetUniformLocation(b->progSpriteSwap, "uAtlas");
        b->uSpriteSwapForceAlpha = glGetUniformLocation(b->progSpriteSwap, "uForceAlpha");
    }
    return checkGl("ensurePrograms");
}

bool ensureBuffers(HudGlBackend *b)
{
    if (!b->vboSmear)
    {
        glGenVertexArrays(1, &b->vaoSmear);
        glGenBuffers(1, &b->vboSmear);
        glBindVertexArray(b->vaoSmear);
        glBindBuffer(GL_ARRAY_BUFFER, b->vboSmear);
        glBufferData(GL_ARRAY_BUFFER, 2u * 6u * 10u * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 10 * sizeof(float), (void *)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 10 * sizeof(float),
                              (void *)(2 * sizeof(float)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 10 * sizeof(float),
                              (void *)(6 * sizeof(float)));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, 10 * sizeof(float),
                              (void *)(8 * sizeof(float)));
        glBindVertexArray(0);
    }
    if (!b->vboSprite)
    {
        glGenVertexArrays(1, &b->vaoSprite);
        glGenBuffers(1, &b->vboSprite);
        glBindVertexArray(b->vaoSprite);
        glBindBuffer(GL_ARRAY_BUFFER, b->vboSprite);
        glBufferData(GL_ARRAY_BUFFER, 26u * 6u * 11u * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 11 * sizeof(float), (void *)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 11 * sizeof(float),
                              (void *)(2 * sizeof(float)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 11 * sizeof(float),
                              (void *)(6 * sizeof(float)));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, 11 * sizeof(float),
                              (void *)(10 * sizeof(float)));
        glBindVertexArray(0);
    }
    if (!b->fboFrame)
        glGenFramebuffers(1, &b->fboFrame);
    if (!b->fboRead)
        glGenFramebuffers(1, &b->fboRead);
    if (!b->fboDraw)
        glGenFramebuffers(1, &b->fboDraw);
    if (!b->vaoEmpty)
        glGenVertexArrays(1, &b->vaoEmpty);
    return checkGl("ensureBuffers");
}

bool ensureAtlas(HudGlBackend *b, const ps2_ssx3_tricky_hud::Atlas &atlas)
{
    if (b->texAtlas && b->atlasPtr == atlas.rgba.data())
        return true;
    if (!atlas.ok || atlas.w != 256 || atlas.h != 256 ||
        atlas.rgba.size() != 256u * 256u * 4u)
    {
        std::fprintf(stderr, "[ssx3-tricky-hud] gpu: atlas refused ok=%d %dx%d bytes=%zu\n",
                     (int)atlas.ok, atlas.w, atlas.h, atlas.rgba.size());
        return false;
    }
    if (!b->texAtlas)
        glGenTextures(1, &b->texAtlas);
    glBindTexture(GL_TEXTURE_2D, b->texAtlas);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // RGBA8UI + usampler: texelFetch returns the exact u8 (no /255 trip).
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8UI, 256, 256, 0, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE,
                 atlas.rgba.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    b->atlasPtr = atlas.rgba.data();
    return checkGl("ensureAtlas");
}

bool ensureTemp(HudGlBackend *b, int rw, int rh)
{
    if (b->texTemp && b->tempW == rw && b->tempH == rh)
        return true;
    if (rw <= 0 || rh <= 0 || rw > 4096 || rh > 4096)
    {
        std::fprintf(stderr, "[ssx3-tricky-hud] gpu: temp refused %dx%d\n", rw, rh);
        return false;
    }
    if (!b->texTemp)
        glGenTextures(1, &b->texTemp);
    glBindTexture(GL_TEXTURE_2D, b->texTemp);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // Always RGBA8: iOS rejects GL_BGRA_EXT as an internalformat
    // (INVALID_ENUM), and the P0 copy is a shader, so the temp never needs
    // to match the frame's format. Bytes stay uniform on every lane.
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, rw, rh, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);
    b->tempW = rw;
    b->tempH = rh;
    return checkGl("ensureTemp");
}

// Upload the scene (smear quads + sprite quads, 6 verts each). Called only
// when the visual key (or sprites/size) changed.
bool uploadScene(HudGlBackend *b, const ps2_ssx3_tricky_hud::HudScene &sc)
{
    using namespace ps2_ssx3_tricky_hud;
    {
        std::vector<float> v;
        v.reserve(2u * 6u * 10u);
        for (int i = 0; i < sc.nsmears; ++i)
        {
            const HudSceneSmear &sm = sc.smears[i];
            const float x0 = (float)sm.x0, y0 = (float)sm.y0;
            const float x1 = (float)sm.x1, y1 = (float)sm.y1;
            const float corners[6][2] = {{x0, y0}, {x1, y0}, {x1, y1},
                                         {x0, y0}, {x1, y1}, {x0, y1}};
            for (int k = 0; k < 6; ++k)
            {
                v.push_back(corners[k][0]);
                v.push_back(corners[k][1]);
                v.push_back(x0);
                v.push_back(y0);
                v.push_back(x1);
                v.push_back(y1);
                v.push_back((float)sm.lx);
                v.push_back((float)sm.rx);
                v.push_back((float)sc.region.x);
                v.push_back((float)sc.region.y);
            }
        }
        glBindBuffer(GL_ARRAY_BUFFER, b->vboSmear);
        glBufferSubData(GL_ARRAY_BUFFER, 0, v.size() * sizeof(float), v.data());
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        b->vboSmearVerts = (unsigned)(v.size() / 10u);
    }
    {
        std::vector<float> v;
        v.reserve((size_t)sc.nquads * 6u * 11u);
        for (int i = 0; i < sc.nquads; ++i)
        {
            const HudSceneQuad &q = sc.quads[i];
            const float x0 = (float)q.dx, y0 = (float)q.dy;
            const float x1 = (float)(q.dx + q.dw), y1 = (float)(q.dy + q.dh);
            const float corners[6][2] = {{x0, y0}, {x1, y0}, {x1, y1},
                                         {x0, y0}, {x1, y1}, {x0, y1}};
            for (int k = 0; k < 6; ++k)
            {
                v.push_back(corners[k][0]);
                v.push_back(corners[k][1]);
                v.push_back((float)q.src.x);
                v.push_back((float)q.src.y);
                v.push_back((float)q.src.w);
                v.push_back((float)q.src.h);
                v.push_back((float)q.dx);
                v.push_back((float)q.dy);
                v.push_back((float)q.dw);
                v.push_back((float)q.dh);
                v.push_back(q.dim);
            }
        }
        glBindBuffer(GL_ARRAY_BUFFER, b->vboSprite);
        glBufferSubData(GL_ARRAY_BUFFER, 0, v.size() * sizeof(float), v.data());
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        b->vboSpriteVerts = (unsigned)(v.size() / 11u);
    }
    return checkGl("uploadScene");
}

// Run P0+P1+P2 with frameTex bound as the target (fw x fh). swap selects
// the lane-diag R/B-swapped shader variants. The scene VBOs must be current.
bool runPasses(HudGlBackend *b, unsigned frameTex, int fw, int fh,
               const ps2_ssx3_tricky_hud::Rect &region, bool swap)
{
    const int ox = region.x, oy = region.y, rw = region.w, rh = region.h;
    const unsigned progSmear = swap ? b->progSmearSwap : b->progSmear;
    const unsigned progSprite = swap ? b->progSpriteSwap : b->progSprite;
    if (!progSmear || !progSprite || !b->progCopy || !frameTex || !b->texTemp)
    {
        std::fprintf(stderr,
                     "[ssx3-tricky-hud] gpu: passes refused smear=%u sprite=%u copy=%u frame=%u "
                     "temp=%u\n",
                     progSmear, progSprite, b->progCopy, frameTex, b->texTemp);
        return false;
    }
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    // P0: exact region copy into the temp (trivial shader; no blit, so the
    // temp format never needs to match the frame's).
    glBindFramebuffer(GL_FRAMEBUFFER, b->fboDraw);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, b->texTemp,
                           0);
    if (!checkFbo("copy-target"))
        return false;
    glViewport(0, 0, rw, rh);
    glUseProgram(b->progCopy);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, frameTex);
    glUniform1i(b->uCopySrc, 0);
    glUniform2i(b->uCopyOrigin, ox, oy);
    glBindVertexArray(b->vaoEmpty);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    if (!checkGl("copy-draw"))
        return false;
    // P1+P2 render into the frame (scissored to the region, as the CPU
    // clips to the region buffer).
    glBindFramebuffer(GL_FRAMEBUFFER, b->fboFrame);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, frameTex, 0);
    if (!checkFbo("frame"))
        return false;
    glViewport(0, 0, fw, fh);
    glEnable(GL_SCISSOR_TEST);
    glScissor(ox, oy, rw, rh);
    glEnable(GL_BLEND);
    // P1: smears from the temp. Alpha preserved (the CPU smear never
    // touches alpha).
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    glUseProgram(progSmear);
    glUniform2f(swap ? b->uSmearSwapFrameWH : b->uSmearFrameWH, (float)fw, (float)fh);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, b->texTemp);
    glUniform1i(swap ? b->uTempSwap : b->uTemp, 0);
    glBindVertexArray(b->vaoSmear);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)b->vboSmearVerts);
    // P2: sprites from the atlas, in draw order (one call, blending on).
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
    glUseProgram(progSprite);
    const int uFrameWH = swap ? b->uSpriteSwapFrameWH : b->uSpriteFrameWH;
    const int uAtlas = swap ? b->uSpriteSwapAtlas : b->uSpriteAtlas;
    const int uForceAlpha = swap ? b->uSpriteSwapForceAlpha : b->uSpriteForceAlpha;
    glUniform2f(uFrameWH, (float)fw, (float)fh);
    glBindTexture(GL_TEXTURE_2D, b->texAtlas);
    glUniform1i(uAtlas, 0);
    glUniform1i(uForceAlpha, 0);
    glBindVertexArray(b->vaoSprite);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)b->vboSpriteVerts);
    // P2b: alpha fix (255 where sa > 0, exactly as blendSample).
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
    glUniform1i(uForceAlpha, 1);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)b->vboSpriteVerts);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindVertexArray(0);
    glUseProgram(0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    return checkGl("runPasses");
}
} // namespace

bool hudGlComposite(HudGlBackend *b, void *platformImage, int fw, int fh,
                    const ps2_ssx3_tricky_hud::Atlas &atlas,
                    const ps2_ssx3_tricky_hud::HudSprites &sprites,
                    const ps2_ssx3_tricky_hud::HudVisualKey &key)
{
    using namespace ps2_ssx3_tricky_hud;
    if (!b || b->broken || !platformImage || fw <= 0 || fh <= 0)
        return false;
    if (!hudGlPlatformMakeCurrent(b->plat))
    {
        std::fprintf(stderr, "[ssx3-tricky-hud] gpu: makeCurrent failed\n");
        return false;
    }
    const auto t0 = std::chrono::steady_clock::now();
    bool ok = false;
    for (;;)
    {
        if (!ensurePrograms(b, false) || !ensureBuffers(b))
            break;
        bool rbNative = false; // informational only (the temp is always RGBA8)
        const unsigned frameTex = hudGlPlatformFrameTexture(b->plat, platformImage, fw, fh,
                                                            rbNative);
        (void)rbNative;
        if (!frameTex)
        {
            std::fprintf(stderr, "[ssx3-tricky-hud] gpu: frame texture failed\n");
            break;
        }
        if (!ensureAtlas(b, atlas))
            break;
        const Rect region = hudRegionRect(fw, fh);
        if (region.w <= 0 || region.h <= 0)
            break;
        if (!ensureTemp(b, region.w, region.h))
        {
            std::fprintf(stderr, "[ssx3-tricky-hud] gpu: temp failed\n");
            break;
        }
        const bool sameScene = b->hasScene && b->spritesPtr == &sprites && b->fw == fw &&
                               b->fh == fh && b->key == key;
        if (!sameScene)
        {
            HudScene sc;
            if (!buildHudScene(sc, sprites, key))
            {
                std::fprintf(stderr, "[ssx3-tricky-hud] gpu: scene failed\n");
                break;
            }
            if (!uploadScene(b, sc))
                break;
            b->key = key;
            b->spritesPtr = &sprites;
            b->fw = fw;
            b->fh = fh;
            b->hasScene = true;
            ++b->sceneBuilds;
        }
        if (!runPasses(b, frameTex, fw, fh, region, false))
            break;
        ok = true;
        break;
    }
    hudGlPlatformFinish(b->plat);
    const auto t1 = std::chrono::steady_clock::now();
    if (!ok)
    {
        b->hasScene = false; // re-upload after any failure (cheap, safe)
        if (++b->consecutiveFails == 1u)
            std::fprintf(stderr, "[ssx3-tricky-hud] gpu: composite failed, CPU fallback\n");
        if (b->consecutiveFails >= 3u && !b->broken)
        {
            b->broken = true;
            std::fprintf(stderr, "[ssx3-tricky-hud] gpu: 3 consecutive failures, parked on CPU\n");
        }
        return false;
    }
    b->consecutiveFails = 0;
    b->compositeNs +=
        (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    if (++b->composites == 1u || b->composites % 600u == 0u)
    {
        char stats[128];
        hudGlStats(b, stats, sizeof(stats));
        std::fprintf(stderr, "[ssx3-tricky-hud] gpu: %s\n", stats);
    }
    if (!b->loggedInit)
    {
        b->loggedInit = true;
        std::fprintf(stderr, "[ssx3-tricky-hud] gpu: composite live %dx%d gl=%s\n", fw, fh,
                     (const char *)glGetString(GL_VERSION));
    }
    return true;
}

// --- lane diag (HUD3, removed before push) ---

HudGlCompare hudGlCompareRegion(const uint8_t *exp, const uint8_t *got, int w, int h)
{
    HudGlCompare c;
    if (!exp || !got || w <= 0 || h <= 0)
        return c;
    const size_t n = (size_t)w * (size_t)h;
    for (size_t i = 0; i < n; ++i)
    {
        const uint8_t *e = exp + i * 4u;
        const uint8_t *g = got + i * 4u;
        bool diff = false;
        for (int ch = 0; ch < 4; ++ch)
        {
            const unsigned d = e[ch] > g[ch] ? (unsigned)(e[ch] - g[ch]) : (unsigned)(g[ch] - e[ch]);
            if (d > 0)
                diff = true;
            if (d > c.maxErr)
                c.maxErr = d;
        }
        if (diff)
        {
            if (c.diffPx == 0)
            {
                c.firstOff = i;
                std::memcpy(c.expFirst, e, 4);
                std::memcpy(c.gotFirst, g, 4);
            }
            ++c.diffPx;
        }
    }
    return c;
}

bool hudGlDiagScratch(HudGlBackend *b, const uint8_t *fullFrame, int fw, int fh, bool swapRB,
                      const ps2_ssx3_tricky_hud::Atlas &atlas,
                      const ps2_ssx3_tricky_hud::HudSprites &sprites,
                      const ps2_ssx3_tricky_hud::HudVisualKey &key, uint8_t *outRegion)
{
    using namespace ps2_ssx3_tricky_hud;
    if (!b || b->broken || !fullFrame || !outRegion || fw <= 0 || fh <= 0)
        return false;
    if (!hudGlPlatformMakeCurrent(b->plat))
        return false;
    bool ok = false;
    GLuint scratch = 0;
    for (;;)
    {
        if (!ensurePrograms(b, swapRB) || !ensureBuffers(b))
            break;
        if (!ensureAtlas(b, atlas))
            break;
        const Rect region = hudRegionRect(fw, fh);
        if (region.w <= 0 || region.h <= 0)
            break;
        if (!ensureTemp(b, region.w, region.h))
            break;
        HudScene sc;
        if (!buildHudScene(sc, sprites, key))
            break;
        if (!uploadScene(b, sc))
            break;
        b->hasScene = false; // the production scene re-uploads next composite
        glGenTextures(1, &scratch);
        glBindTexture(GL_TEXTURE_2D, scratch);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, fw, fh, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     fullFrame);
        glBindTexture(GL_TEXTURE_2D, 0);
        if (!checkGl("diag-upload"))
            break;
        if (!runPasses(b, scratch, fw, fh, region, swapRB))
            break;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, b->fboRead);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               scratch, 0);
        if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            break;
        for (int y = 0; y < region.h; ++y)
        {
            glReadPixels(region.x, region.y + y, region.w, 1, GL_RGBA, GL_UNSIGNED_BYTE,
                         outRegion + (size_t)y * (size_t)region.w * 4u);
        }
        ok = checkGl("diag-readback");
        break;
    }
    if (scratch)
        glDeleteTextures(1, &scratch);
    hudGlPlatformFinish(b->plat);
    return ok;
}
