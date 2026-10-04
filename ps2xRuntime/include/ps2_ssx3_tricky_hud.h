#pragma once
// TK43a phase 1: TRICKY meter HUD reskin on Tricky courses (output-only).
//
// A host-side overlay drawn over the presented frame (never into guest
// memory): Tricky's coil stack, jewel, full-pill and chrome `tricky` arch,
// driven by SSX 3's committed boost words (TK43 memo section 4.1):
//   fill = f32[R+0x2f8] normalized to 16 coils (SSX 3 shows floor(10*fill)),
//   full = i32[R+0x2f4] (uber level) >= 1.
// R reaches the player-1 rider through the game's own globals (TK43d):
// R = [[[0x4a28a8]+0x84]+0xC]+0x28 (PS2X_TK12_AP_PTR stays as an override
// when set). Letters lighting, the song and the announcer are later lanes
// (TK43b/c).
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
//
// TK43c: TRICKY letters + fanfare flash (still output-only). Uber landings
// are counted by a host-side CD tap (noteCdReadForUber, called from
// ps2_e41_trace::noteCdRead): the speech engine opens an Arcade_Uber slot's
// stream with a >=2-sector read at a fixed disc LBN, once per trigger. The
// overlay lights one red letter per tap (T->R->I->C->K->Y); the 6th opens a
// 64-tick all-red flash, then the letters reset (real Tricky past R is
// unobserved; reset and say so). The red sprites are new atlas cells
// (map1 red wordmark run, TK43a2 boxes); the atlas magic is TKHUD2.

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace ps2_ssx3_tricky_hud
{

constexpr uint32_t kRiderFillOff = 0x2f8u;   // f32 boost target 0..1 (FH10)
constexpr uint32_t kRiderUberOff = 0x2f4u;   // i32 uber level 0..10 (`sw`, 0x10e9e8)
// TK43d chain root: [gp-0x848] (gp = 0x4a30f0, the ELF .reginfo ri_gp_value).
constexpr uint32_t kChainRoot = 0x4a28a8u;
constexpr uint32_t kChainAOff = 0x84u;       // game object (0x1298c8, func_28B1C8)
constexpr uint32_t kChainBOff = 0x0cu;       // race object (func_28B1D8; tick at +8)
constexpr uint32_t kChainROff = 0x28u;       // player-1 rider (HUD s5=0 entry, 0x29ab40)
constexpr uint32_t kRamMask = 0x01ffffffu;   // 32 MB RDRAM (ps2_memory.h)
constexpr int kCoils = 16;
constexpr int kAtlasW = 256;
constexpr int kAtlasH = 256;

struct Rect
{
    int x, y, w, h;
};

// Atlas rects mirror make_trickyhud_atlas.py ATLAS (256x256 RGBA). TK43a2:
// cut from the correct (linear-CLUT) decode; Tricky's art is red, orange,
// gold and silver rings, chrome letters pre-rotated for the arc, a silver
// and a red jewel, all drawn at 1 texel = 1 buffer px in the real game.
inline Rect ringRect(int band) // band 0..3 = gold, orange, orange, red (bottom..top)
{
    static const Rect kBands[4] = {{98, 0, 49, 12}, {49, 0, 49, 12}, {49, 0, 49, 12}, {0, 0, 49, 12}};
    return kBands[band < 0 ? 0 : (band > 3 ? 3 : band)];
}
inline Rect silverRingRect() { return {147, 0, 49, 12}; }
inline Rect poleRect() { return {200, 0, 31, 2}; } // cylinder profile, drawn across the pole
inline Rect letterRect(int i) // 0..5 = T,R,I,C,K,Y
{
    static const Rect kLetters[6] = {{0, 16, 35, 25},  {36, 16, 19, 27}, {56, 16, 17, 31},
                                     {74, 16, 19, 23}, {94, 16, 25, 35}, {120, 16, 30, 30}};
    return kLetters[i < 0 ? 0 : (i > 5 ? 5 : i)];
}
inline Rect litLetterRect(int i) // 0..5 = T,R,I,C,K,Y, lit red (TK43c)
{
    static const Rect kLit[6] = {{0, 96, 35, 24},  {36, 96, 19, 27}, {56, 96, 17, 31},
                                 {74, 96, 18, 23}, {94, 96, 25, 34}, {120, 96, 30, 30}};
    return kLit[i < 0 ? 0 : (i > 5 ? 5 : i)];
}
inline Rect jewelGreyRect() { return {152, 16, 35, 32}; }
inline Rect jewelRedRect() { return {188, 16, 35, 32}; }
inline Rect pillRect() { return {0, 56, 22, 18}; }
inline Rect pillGreyRect() { return {24, 56, 22, 18}; }
inline Rect snowflakeRect() { return {48, 56, 38, 32}; }

struct Atlas
{
    int w = 0;
    int h = 0;
    std::vector<uint8_t> rgba; // w*h*4, top-left origin
    bool ok = false;
};

// Header: "TKHUD2\0\0" + u32le w,h,reserved, then w*h*4 RGBA bytes.
// TK43c: magic bumped (TKHUD1 atlases lack the red-letter row and are
// refused; the atlas is staged fresh, never committed).
inline Atlas parseAtlas(const uint8_t *data, size_t size)
{
    Atlas a;
    static const char kMagic[8] = {'T', 'K', 'H', 'U', 'D', '2', '\0', '\0'};
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

inline MeterFrame readMeterFrameAt(const uint8_t *ram, size_t ramSize, uint32_t r)
{
    MeterFrame f;
    if (!ram || ramSize == 0u)
        return f;
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

inline MeterFrame readMeterFrame(const uint8_t *ram, size_t ramSize, uint32_t ptrAddr)
{
    if (!ram || ramSize == 0u)
        return MeterFrame();
    const uint32_t slot = ptrAddr & kRamMask;
    if (slot + 4u > ramSize)
        return MeterFrame();
    uint32_t r = 0;
    std::memcpy(&r, ram + slot, 4);
    return readMeterFrameAt(ram, ramSize, r);
}

// TK43d: the player-1 rider through the game's own globals:
// R = [[[0x4a28a8]+0x84]+0xC]+0x28. The SSX 3 HUD draws its meter from the
// same struct: 0x29ab40 reads s2 = [func_28B1D8()+s5+0x28] (s5 = 0 for
// player 1) and s2 carries the rider words (+0x2F0 at 0x29acd8, +0x790 at
// 0x29abfc as in boost-add 0x10e990, +0x870 at 0x29ac80 as in func_29AB08
// with a1 = R). Each hop is null- and range-checked; any failure returns 0
// (menus / pre-race: the overlay draws nothing, as before).
inline uint32_t resolveChainR(const uint8_t *ram, size_t ramSize)
{
    if (!ram || ramSize == 0u)
        return 0u;
    uint32_t g = 0u, a = 0u, b = 0u, r = 0u;
    const uint32_t gs = kChainRoot & kRamMask;
    if (gs + 4u > ramSize)
        return 0u;
    std::memcpy(&g, ram + gs, 4);
    if (g == 0u)
        return 0u;
    const uint32_t as = (g + kChainAOff) & kRamMask;
    if (as + 4u > ramSize)
        return 0u;
    std::memcpy(&a, ram + as, 4);
    if (a == 0u)
        return 0u;
    const uint32_t bs = (a + kChainBOff) & kRamMask;
    if (bs + 4u > ramSize)
        return 0u;
    std::memcpy(&b, ram + bs, 4);
    if (b == 0u)
        return 0u;
    const uint32_t rs = (b + kChainROff) & kRamMask;
    if (rs + 4u > ramSize)
        return 0u;
    std::memcpy(&r, ram + rs, 4);
    return r;
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

// TK43c: Arcade_Uber stream-open tap. The speech engine opens a slot's
// stream with a 1-sector staging read at the slot's first sector, then
// 15-sector chunks from the next sector (TK43b A7 cdread: slot 2 @t12083
// 0x614d9(1)+0x614da(15)...; slot 1 @t13398 0x6147c(1)+0x6147d(15)...;
// B repeats both LBNs at t12141/t13398/t19424). The tap matches the
// >=2-sector read at slot-start+1: slot 0's first sector (0x61416) is also
// touched by neighboring Silence/Prompts streams, but its second sector
// (0x61417) sits inside Arcade_Uber.dat and is read by no other stream.
// LBNs computed off the pinned SSX 3 ISO (SPEECH.BIG LBN 393999 +
// Arcade_Uber.dat member 8929152 + SCHl slot offsets); identical under the
// TK43b alias composite (same-size, same layout). Slots 1-2 verified once
// per trigger in A7+B; slot 0 is predicted (it has never fired in any
// boot). Called from ps2_e41_trace::noteCdRead on every CD read, whether
// or not the read trace is armed; self-gated on PS2X_SSX3_TRICKY_HUD.
inline constexpr uint32_t kUberTapLbn[3] = {0x61417u, 0x6147du, 0x614dau};

inline int slotForUberRead(uint32_t lbn, uint32_t sectors)
{
    if (sectors < 2u)
        return -1;
    for (int i = 0; i < 3; ++i)
    {
        if (lbn == kUberTapLbn[i])
            return i;
    }
    return -1;
}

struct UberTap
{
    std::atomic<uint64_t> count{0};
    std::atomic<uint64_t> tick{0};
    std::atomic<int> slot{-1};
};

inline UberTap &uberTap()
{
    static UberTap t;
    return t;
}

inline void noteCdReadForUber(uint32_t lbn, uint32_t sectors, uint64_t vsync)
{
    static const bool wanted = [] {
        const char *env = std::getenv("PS2X_SSX3_TRICKY_HUD");
        return env && env[0] == '1';
    }();
    if (!wanted)
        return;
    const int slot = slotForUberRead(lbn, sectors);
    if (slot < 0)
        return;
    UberTap &t = uberTap();
    const uint64_t n = t.count.fetch_add(1u, std::memory_order_relaxed) + 1u;
    t.tick.store(vsync, std::memory_order_relaxed);
    t.slot.store(slot, std::memory_order_relaxed);
    std::fprintf(stderr, "[ssx3-tricky-hud] uber #%llu slot=%d tick=%llu\n",
                 static_cast<unsigned long long>(n), slot,
                 static_cast<unsigned long long>(vsync));
}

// TK43c: letter state machine (pure; the overlay owns the state).
// One lit letter per uber tap, T->R->I->C->K->Y. Triggers arriving during
// the fanfare flash are consumed but light nothing (triggers are minutes
// apart; a mid-flash trigger cannot spell). The 6th letter opens the
// 64-tick all-red flash; when it lapses the letters reset.
constexpr uint64_t kLetterFlashTicks = 64u;

struct LetterState
{
    uint64_t seen = 0;      // tap count consumed
    int lit = 0;            // 0..6
    uint64_t flashUntil = 0; // fanfare window (0 = none)
};

inline void updateLetters(LetterState &st, uint64_t count, uint64_t tick)
{
    while (st.seen < count)
    {
        ++st.seen;
        if (tick < st.flashUntil)
            continue;
        if (st.lit < 6)
        {
            ++st.lit;
            if (st.lit == 6)
                st.flashUntil = tick + kLetterFlashTicks;
        }
    }
    if (st.lit == 6 && st.flashUntil != 0u && tick >= st.flashUntil)
    {
        st.lit = 0;
        st.flashUntil = 0;
    }
}

// Diag-only letter preset (PS2X_SSX3_TRICKY_LETTERS_PRESET=N, 0..5): the
// overlay enters Tricky mode with N letters already lit, so the fanfare
// path is provable in-game on a replay with fewer than 6 ubers. Unlisted
// knob (diag class): shows in the knobs line as an extra. Owner lane:
// TK43c (leaves with the lane). Returns -1 when unset/invalid.
inline int parseLettersPreset(const char *s)
{
    if (!s || !*s)
        return -1;
    char *end = nullptr;
    const long v = std::strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < 0 || v > 5)
        return -1;
    return static_cast<int>(v);
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
inline void blit(const Atlas &a, const Rect &s, uint8_t *frame, int fw, int fh, int dx, int dy, int dw,
                 int dh, float dim = 1.0f)
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
            const float sr = (p00[0] * w00 + p10[0] * w10 + p01[0] * w01 + p11[0] * w11) * dim;
            const float sg = (p00[1] * w00 + p10[1] * w10 + p01[1] * w01 + p11[1] * w11) * dim;
            const float sb = (p00[2] * w00 + p10[2] * w10 + p01[2] * w01 + p11[2] * w11) * dim;
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
                           uint64_t tick, uint64_t splashUntilTick, int litLetters,
                           uint64_t flashUntilTick)
{
    if (!frame || !a.ok || fw <= 0 || fh <= 0)
        return;
    Layout L;
    L.s = static_cast<float>(fh) / 480.0f;
    L.fw = fw;
    // Pole through the stack (covers the SSX 3 red center line, x582-586):
    // Tricky's grey cylinder profile stretched across its width.
    blit(a, poleRect(), frame, fw, fh, L.X(584), L.Y(140), L.W(5), L.H(260));
    // 16 rings, bottom-up: gold, orange, orange, red bands; unlit slots
    // silver. Native 49x12 (the real game draws them 1:1) on Part 2's
    // 16.5 px pitch, which fits the SSX 3 coil footprint (y143-395) with the
    // jewel touching above; the pole shows through the 4.5 px gaps.
    const int lit = litCoils(fill);
    for (int i = 0; i < kCoils; ++i)
    {
        const float cy = 395.0f - static_cast<float>(i) * 16.5f;
        const Rect src = i < lit ? ringRect(i / 4) : silverRingRect();
        blit(a, src, frame, fw, fh, L.X(563), L.Y(cy - 6.0f), L.W(49), L.H(12));
    }
    // Jewel over the ball: grey, or red pulsing at ~3.7 Hz when full.
    const Rect jewel = full ? jewelRedRect() : jewelGreyRect();
    const float dim = (full && ((tick >> 3) & 1u) != 0u) ? 0.82f : 1.0f;
    blit(a, jewel, frame, fw, fh, L.X(568), L.Y(107), L.W(40), L.H(36), dim);
    // Label cover: feathered sky smear, no panel (Part 3). The chrome arch
    // draws over it; the score (above y49) and jewel (below y101) are spared.
    smearCover(frame, fw, fh, L.X(536), L.Y(49), L.X(628), L.Y(101));
    // Chrome arch (TK43a2): the 6 pre-rotated letter sprites at native size,
    // each placed at its offset from the stack center (x587.5) and jewel top
    // (y107) template-matched in Brad's recording (t135, t460).
    // TK43c: the first `litLetters` draw from the red cells (same dest
    // rects; the red art is the same graffiti shapes). During the fanfare
    // window the whole arch blinks red/chrome at the jewel-pulse cadence.
    if (litLetters < 0)
        litLetters = 0;
    if (litLetters > 6)
        litLetters = 6;
    const bool flashing = tick < flashUntilTick;
    const bool flashRed = flashing && (((tick >> 3) & 1u) == 0u);
    static const int kArch[6][2] = {
        {527, 94}, {550, 83}, {565, 72}, {579, 79}, {595, 73}, {607, 94},
    };
    for (int i = 0; i < 6; ++i)
    {
        const bool red = flashing ? flashRed : (i < litLetters);
        const Rect src = red ? litLetterRect(i) : letterRect(i);
        const Rect dst = letterRect(i);
        blit(a, src, frame, fw, fh, L.X(kArch[i][0]), L.Y(kArch[i][1]), L.W(dst.w), L.H(dst.h));
    }
    // Pill slot over the S (y400-427): Tricky's red S hexagon when full,
    // its grey copy otherwise. Real Tricky shows no pill until full; the
    // grey slot keeps the SSX 3 S covered and mirrors the jewel.
    blit(a, full ? pillRect() : pillGreyRect(), frame, fw, fh, L.X(571), L.Y(400), L.W(34), L.H(30));
    // First-full snowflake splash (TK43 section 1.3: cheap one-shot).
    // Part 3: full re-cut flake, recording orange, 156x116 over the jewel
    // bottom and top ~5 rings (tile 1: x346-463 y101-166).
    if (tick < splashUntilTick)
        blit(a, snowflakeRect(), frame, fw, fh, L.X(481), L.Y(114), L.W(156), L.H(116));
}

} // namespace ps2_ssx3_tricky_hud
