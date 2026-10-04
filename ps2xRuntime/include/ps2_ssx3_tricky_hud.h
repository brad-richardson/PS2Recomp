#pragma once
// TK43a phase 1: TRICKY meter HUD reskin on Tricky courses (output-only).
//
// A host-side overlay drawn over the presented frame (never into guest
// memory): Tricky's coil stack, jewel, full-pill and chrome `tricky` arch,
// driven by SSX 3's committed boost words (TK43 memo section 4.1):
//   fill = f32[R+0x2f8] normalized to 16 coils (SSX 3 shows floor(10*fill)),
//   full = i32[R+0x2f4] (uber level) >= 1.
// R = u32[PS2X_TK12_AP_PTR] (the rider pointer the Tricky AP recipes set;
// APH1 default 0x53FF4C when unset). Letters lighting, the song and the
// announcer are later lanes (TK43b/c).
//
// Active only when PS2X_SSX3_TRICKY_HUD=1 AND a non-Stock course-manifest
// mode is in RAM (the TK25c alias scope): SSX 3 courses are byte-identical
// in output with the knob on. Art comes from the staged atlas file named
// by PS2X_SSX3_TRICKY_HUD_ART (built from the Tricky ISO by
// local/research/TK43a/tooling/make_trickyhud_atlas.py; never committed).
// Every failure (no art, unreadable rider, invalid words) draws nothing.
//
// Change class: output-only. The guest det-hash cannot move: this reads
// committed guest words and writes only the host presentation buffer.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ps2_ssx3_tricky_hud
{

constexpr uint32_t kRiderFillOff = 0x2f8u;   // f32 boost target 0..1 (FH10)
constexpr uint32_t kRiderUberOff = 0x2f4u;   // i32 uber level 0..10 (`sw`, 0x10e9e8)
constexpr uint32_t kDefaultRiderPtr = 0x53ff4cu; // APH1 default (Pad.cpp)
constexpr uint32_t kRamMask = 0x01ffffffu;   // 32 MB RDRAM (ps2_memory.h)
constexpr int kCoils = 16;
constexpr int kAtlasW = 256;
constexpr int kAtlasH = 256;

struct Rect
{
    int x, y, w, h;
};

// Atlas rects mirror make_trickyhud_atlas.py ATLAS (256x256 RGBA).
inline Rect ringRect(int band) // band 0..3 = pale, gold, orange, red (bottom..top)
{
    static const Rect kBands[4] = {{144, 0, 48, 14}, {96, 0, 48, 14}, {48, 0, 48, 14}, {0, 0, 48, 14}};
    return kBands[band < 0 ? 0 : (band > 3 ? 3 : band)];
}
inline Rect silverRingRect() { return {192, 0, 48, 14}; }
inline Rect letterRect(int i) // 0..5 = T,R,I,C,K,Y (TK43a Part 2: spaced arch)
{
    static const Rect kLetters[6] = {{0, 16, 19, 50},  {20, 16, 15, 30}, {36, 16, 12, 33},
                                     {49, 16, 12, 33}, {62, 16, 15, 29}, {78, 16, 14, 29}};
    return kLetters[i < 0 ? 0 : (i > 5 ? 5 : i)];
}
inline Rect jewelGreyRect() { return {100, 16, 46, 33}; }
inline Rect jewelRedRect() { return {150, 16, 46, 33}; }
inline Rect pillRect() { return {100, 52, 28, 18}; }
inline Rect pillGreyRect() { return {180, 52, 28, 18}; }
inline Rect snowflakeRect() { return {132, 52, 38, 32}; } // Part 3: re-cut (full flake)

struct Atlas
{
    int w = 0;
    int h = 0;
    std::vector<uint8_t> rgba; // w*h*4, top-left origin
    bool ok = false;
};

// Header: "TKHUD1\0\0" + u32le w,h,reserved, then w*h*4 RGBA bytes.
inline Atlas parseAtlas(const uint8_t *data, size_t size)
{
    Atlas a;
    static const char kMagic[8] = {'T', 'K', 'H', 'U', 'D', '1', '\0', '\0'};
    if (!data || size < 20u || std::memcmp(data, kMagic, 8) != 0)
        return a;
    uint32_t w = 0, h = 0;
    std::memcpy(&w, data + 8, 4);
    std::memcpy(&h, data + 12, 4);
    if (w == 0u || h == 0u || w > 1024u || h > 1024u)
        return a;
    const size_t want = 20u + static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
    if (size < want)
        return a;
    a.w = static_cast<int>(w);
    a.h = static_cast<int>(h);
    a.rgba.assign(data + 20, data + want);
    a.ok = true;
    return a;
}

inline Atlas loadAtlasFile(const char *path)
{
    Atlas a;
    if (!path || !*path)
        return a;
    std::FILE *f = std::fopen(path, "rb");
    if (!f)
        return a;
    std::vector<uint8_t> buf;
    uint8_t chunk[65536];
    size_t n = 0;
    while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0u)
        buf.insert(buf.end(), chunk, chunk + n);
    std::fclose(f);
    return parseAtlas(buf.data(), buf.size());
}

struct MeterFrame
{
    bool ok = false;
    float fill = 0.0f;
    int32_t level = 0;
};

inline MeterFrame readMeterFrame(const uint8_t *ram, size_t ramSize, uint32_t ptrAddr)
{
    MeterFrame f;
    if (!ram || ramSize == 0u)
        return f;
    const uint32_t slot = ptrAddr & kRamMask;
    if (slot + 4u > ramSize)
        return f;
    uint32_t r = 0;
    std::memcpy(&r, ram + slot, 4);
    const uint32_t rb = r & kRamMask;
    if (r == 0u || rb + kRiderFillOff + 4u > ramSize)
        return f;
    float fill = 0.0f;
    int32_t level = 0;
    std::memcpy(&fill, ram + rb + kRiderFillOff, 4);
    std::memcpy(&level, ram + rb + kRiderUberOff, 4);
    if (!std::isfinite(fill))
        return f;
    if (level < 0 || level > 10)
        return f;
    // Drain overshoot reads -0.0001 at rest: clamp, never blank (TK43a gap 2).
    if (fill < 0.0f)
        fill = 0.0f;
    if (fill > 1.0f)
        fill = 1.0f;
    f.ok = true;
    f.fill = fill;
    f.level = level;
    return f;
}

// Diag-only display override (PS2X_SSX3_TRICKY_HUD_FORCE="fill,level", e.g.
// "1,1"): forces the OVERLAY's displayed values for screenshots. Host-side
// only; never writes guest memory. Unlisted knob (diag class): it shows in
// the knobs line as an extra. Owner lane: TK43a (leaves with the lane).
struct ForceValue
{
    bool ok = false;
    float fill = 0.0f;
    int32_t level = 0;
};

inline ForceValue parseForce(const char *s)
{
    ForceValue v;
    if (!s || !*s)
        return v;
    float fill = 0.0f;
    long level = 0;
    int end = 0;
    if (std::sscanf(s, "%f,%ld%n", &fill, &level, &end) != 2 || s[end] != '\0')
        return v;
    if (!std::isfinite(fill))
        return v;
    if (fill < 0.0f)
        fill = 0.0f;
    if (fill > 1.0f)
        fill = 1.0f;
    if (level < 0)
        level = 0;
    if (level > 10)
        level = 10;
    v.ok = true;
    v.fill = fill;
    v.level = static_cast<int32_t>(level);
    return v;
}

inline int litCoils(float fill)
{
    int lit = static_cast<int>(fill * static_cast<float>(kCoils) + 1e-6f);
    if (lit < 0)
        lit = 0;
    if (lit > kCoils)
        lit = kCoils;
    return lit;
}

// Bilinear atlas blit with alpha-over onto an RGBA frame (both top-left).
// tint (optional): desaturate the sample to luminance, then scale by
// tint[3] (Part 3: chrome + red-shadow letters from the gold map4 art).
inline void blit(const Atlas &a, const Rect &s, uint8_t *frame, int fw, int fh, int dx, int dy, int dw,
                 int dh, float dim = 1.0f, const float *tint = nullptr)
{
    if (!a.ok || !frame || fw <= 0 || fh <= 0 || dw <= 0 || dh <= 0)
        return;
    const int x0 = dx < 0 ? 0 : dx;
    const int y0 = dy < 0 ? 0 : dy;
    const int x1 = dx + dw > fw ? fw : dx + dw;
    const int y1 = dy + dh > fh ? fh : dy + dh;
    for (int y = y0; y < y1; ++y)
    {
        const float v = (static_cast<float>(y - dy) + 0.5f) * static_cast<float>(s.h) / static_cast<float>(dh) - 0.5f;
        int v0 = static_cast<int>(std::floor(v));
        float fv = v - static_cast<float>(v0);
        if (v0 < 0)
        {
            v0 = 0;
            fv = 0.0f;
        }
        if (v0 > s.h - 2)
        {
            v0 = s.h - 2;
            fv = 1.0f;
        }
        if (s.h < 2)
        {
            v0 = 0;
            fv = 0.0f;
        }
        for (int x = x0; x < x1; ++x)
        {
            const float u = (static_cast<float>(x - dx) + 0.5f) * static_cast<float>(s.w) / static_cast<float>(dw) - 0.5f;
            int u0 = static_cast<int>(std::floor(u));
            float fu = u - static_cast<float>(u0);
            if (u0 < 0)
            {
                u0 = 0;
                fu = 0.0f;
            }
            if (u0 > s.w - 2)
            {
                u0 = s.w - 2;
                fu = 1.0f;
            }
            if (s.w < 2)
            {
                u0 = 0;
                fu = 0.0f;
            }
            const uint8_t *p00 = &a.rgba[((s.y + v0) * a.w + s.x + u0) * 4];
            const uint8_t *p10 = p00 + 4;
            const uint8_t *p01 = p00 + static_cast<size_t>(a.w) * 4u;
            const uint8_t *p11 = p01 + 4;
            const float w00 = (1.0f - fu) * (1.0f - fv);
            const float w10 = fu * (1.0f - fv);
            const float w01 = (1.0f - fu) * fv;
            const float w11 = fu * fv;
            float sr = (p00[0] * w00 + p10[0] * w10 + p01[0] * w01 + p11[0] * w11) * dim;
            float sg = (p00[1] * w00 + p10[1] * w10 + p01[1] * w01 + p11[1] * w11) * dim;
            float sb = (p00[2] * w00 + p10[2] * w10 + p01[2] * w01 + p11[2] * w11) * dim;
            if (tint)
            {
                const float lum = (sr * 299.0f + sg * 587.0f + sb * 114.0f) / 1000.0f;
                sr = lum * tint[0];
                sg = lum * tint[1];
                sb = lum * tint[2];
            }
            if (sr > 255.0f)
                sr = 255.0f;
            if (sg > 255.0f)
                sg = 255.0f;
            if (sb > 255.0f)
                sb = 255.0f;
            const float sa = (p00[3] * w00 + p10[3] * w10 + p01[3] * w01 + p11[3] * w11) / 255.0f;
            if (sa <= 0.0f)
                continue;
            uint8_t *d = &frame[(static_cast<size_t>(y) * static_cast<size_t>(fw) + static_cast<size_t>(x)) * 4u];
            const float ia = 1.0f - sa;
            d[0] = static_cast<uint8_t>(sr * sa + d[0] * ia + 0.5f);
            d[1] = static_cast<uint8_t>(sg * sa + d[1] * ia + 0.5f);
            d[2] = static_cast<uint8_t>(sb * sa + d[2] * ia + 0.5f);
            d[3] = 255;
        }
    }
}

// Label cover (Part 3: replaces the dark panel): each row of [x0,x1) is
// filled with the horizontal lerp between the sky sampled just outside
// both edges, feathered 2 px horizontally / 3 px vertically so no rect
// edge shows. The SSX 3 SUPER UBER label (x545-615 y52-97) sits fully
// inside; the chrome arch draws over the smear.
inline void smearCover(uint8_t *frame, int fw, int fh, int x0, int y0, int x1, int y1)
{
    if (!frame || fw <= 0 || fh <= 0 || x1 <= x0 || y1 <= y0)
        return;
    const int xa = x0 < 0 ? 0 : x0;
    const int xb = x1 > fw ? fw : x1;
    const int ya = y0 < 0 ? 0 : y0;
    const int yb = y1 > fh ? fh : y1;
    const float span = static_cast<float>(x1 - x0);
    for (int y = ya; y < yb; ++y)
    {
        const int lx = x0 - 2 < 0 ? 0 : x0 - 2;
        const int rx = x1 + 2 > fw - 1 ? fw - 1 : x1 + 2;
        const uint8_t *L = &frame[(static_cast<size_t>(y) * static_cast<size_t>(fw) + static_cast<size_t>(lx)) * 4u];
        const uint8_t *R = &frame[(static_cast<size_t>(y) * static_cast<size_t>(fw) + static_cast<size_t>(rx)) * 4u];
        const float lr = L[0], lg = L[1], lb = L[2];
        const float rr = R[0], rg = R[1], rb = R[2];
        const float fy0 = static_cast<float>(y - y0) / 3.0f;
        const float fy1 = static_cast<float>(y1 - y) / 3.0f;
        for (int x = xa; x < xb; ++x)
        {
            const float t = static_cast<float>(x - x0) / span;
            float a = fy0;
            if (fy1 < a)
                a = fy1;
            const float fx0 = static_cast<float>(x - x0) / 2.0f;
            const float fx1 = static_cast<float>(x1 - x) / 2.0f;
            if (fx0 < a)
                a = fx0;
            if (fx1 < a)
                a = fx1;
            if (a <= 0.0f)
                continue;
            if (a > 1.0f)
                a = 1.0f;
            uint8_t *d = &frame[(static_cast<size_t>(y) * static_cast<size_t>(fw) + static_cast<size_t>(x)) * 4u];
            const float ia = 1.0f - a;
            d[0] = static_cast<uint8_t>((lr + (rr - lr) * t) * a + d[0] * ia + 0.5f);
            d[1] = static_cast<uint8_t>((lg + (rg - lg) * t) * a + d[1] * ia + 0.5f);
            d[2] = static_cast<uint8_t>((lb + (rb - lb) * t) * a + d[2] * ia + 0.5f);
        }
    }
}

inline void fillRect(uint8_t *frame, int fw, int fh, int dx, int dy, int dw, int dh, uint8_t r, uint8_t g,
                     uint8_t b)
{
    if (!frame || fw <= 0 || fh <= 0)
        return;
    const int x0 = dx < 0 ? 0 : dx;
    const int y0 = dy < 0 ? 0 : dy;
    const int x1 = dx + dw > fw ? fw : dx + dw;
    const int y1 = dy + dh > fh ? fh : dy + dh;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
        {
            uint8_t *d = &frame[(static_cast<size_t>(y) * static_cast<size_t>(fw) + static_cast<size_t>(x)) * 4u];
            d[0] = r;
            d[1] = g;
            d[2] = b;
            d[3] = 255;
        }
}

// Layout in 640x480 space (measured off the SSX 3 meter footprint), mapped
// onto any export size right-anchored and height-scaled. The guest score
// above y=50 is kept; everything else the meter covers is replaced.
struct Layout
{
    float s = 1.0f; // h/480
    int fw = 640;
    int X(float x) const { return static_cast<int>(std::lround(fw - (640.0f - x) * s)); }
    int Y(float y) const { return static_cast<int>(std::lround(y * s)); }
    int W(float w) const { return static_cast<int>(std::lround(w * s)); }
    int H(float h) const { return static_cast<int>(std::lround(h * s)); }
};

inline void composeOverlay(uint8_t *frame, int fw, int fh, const Atlas &a, float fill, bool full,
                           uint64_t tick, uint64_t splashUntilTick)
{
    if (!frame || !a.ok || fw <= 0 || fh <= 0)
        return;
    Layout L;
    L.s = static_cast<float>(fh) / 480.0f;
    L.fw = fw;
    // Pole through the stack (covers the SSX 3 red center line, x582-586).
    fillRect(frame, fw, fh, L.X(584), L.Y(140), L.W(5), L.H(260), 26, 22, 30);
    // 16 rings, bottom-up: pale, gold, orange, red bands; unlit slots silver.
    // Part 2 geometry off Brad's recording: flat rings (44x9) on a 16.5 px
    // pitch, pole visible through the 7.5 px gaps. The pitch fits the SSX 3
    // coil footprint (y143-395) with the jewel touching above, not over it.
    const int lit = litCoils(fill);
    for (int i = 0; i < kCoils; ++i)
    {
        const float cy = 395.0f - static_cast<float>(i) * 16.5f;
        const Rect src = i < lit ? ringRect(i / 4) : silverRingRect();
        blit(a, src, frame, fw, fh, L.X(566), L.Y(cy - 4.5f), L.W(44), L.H(9));
    }
    // Jewel over the ball: grey, or red pulsing at ~3.7 Hz when full.
    const Rect jewel = full ? jewelRedRect() : jewelGreyRect();
    const float dim = (full && ((tick >> 3) & 1u) != 0u) ? 0.82f : 1.0f;
    blit(a, jewel, frame, fw, fh, L.X(568), L.Y(107), L.W(40), L.H(36), dim);
    // Label cover: feathered sky smear, no panel (Part 3). The chrome arch
    // draws over it; the score (above y49) and jewel (below y101) are spared.
    smearCover(frame, fw, fh, L.X(536), L.Y(49), L.X(628), L.Y(101));
    // Chrome arch (Part 3, off Brad's tiles): 6 letters at ~1.1x native
    // (aspect preserved), x527-639 = 2.5 ring widths, bottoms arched
    // (middle 97, ends 103) just above the jewel. Red drop shadow first
    // (T fakes the recording's wider left swoosh), chrome over it.
    static const float kChrome[3] = {1.6f, 1.6f, 1.65f};
    static const float kShadow[3] = {1.6f, 0.3f, 0.25f};
    static const int kArch[6][4] = {
        {527, 67, 19, 36}, {549, 65, 17, 35}, {569, 59, 14, 38},
        {586, 59, 14, 38}, {603, 67, 17, 33}, {623, 70, 16, 33},
    };
    for (int i = 0; i < 6; ++i)
    {
        const int sox = i == 0 ? -5 : -1; // T swoosh (tile 3: red tail to x1342)
        blit(a, letterRect(i), frame, fw, fh, L.X(kArch[i][0] + sox), L.Y(kArch[i][1] + 4),
             L.W(kArch[i][2]), L.H(kArch[i][3]), 1.0f, kShadow);
    }
    for (int i = 0; i < 6; ++i)
        blit(a, letterRect(i), frame, fw, fh, L.X(kArch[i][0]), L.Y(kArch[i][1]), L.W(kArch[i][2]),
             L.H(kArch[i][3]), 1.0f, kChrome);
    // Pill slot over the S (y400-427): grey when not full, red (two
    // overlapping stamps cover the S fully) when full. Real Tricky shows no
    // pill until full; the grey slot keeps the S covered and mirrors the jewel.
    if (full)
    {
        blit(a, pillRect(), frame, fw, fh, L.X(571), L.Y(398), L.W(34), L.H(24));
        blit(a, pillRect(), frame, fw, fh, L.X(571), L.Y(418), L.W(34), L.H(24));
    }
    else
    {
        blit(a, pillGreyRect(), frame, fw, fh, L.X(571), L.Y(400), L.W(34), L.H(30));
    }
    // First-full snowflake splash (TK43 section 1.3: cheap one-shot).
    // Part 3: full re-cut flake, recording orange, 156x116 over the jewel
    // bottom and top ~5 rings (tile 1: x346-463 y101-166).
    if (tick < splashUntilTick)
        blit(a, snowflakeRect(), frame, fw, fh, L.X(481), L.Y(114), L.W(156), L.H(116));
}

} // namespace ps2_ssx3_tricky_hud
