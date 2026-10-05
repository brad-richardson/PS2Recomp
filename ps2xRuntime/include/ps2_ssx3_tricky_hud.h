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
//
// TK43e: the draw list is composeHudInto (region + origin), drawn by both
// the GL path (composeOverlay: copy out, compose, copy back) and the VK/AHB
// path (queuePendingAhb composites the region into the locked AHB before
// queue); the state both call sites share is HudState, decided per frame by
// updateHudStateLocked (ps2_ssx3_tricky_hud_state.h).
//
// TK44: race-only visibility (the chain resolves in menus, so the meter used
// to draw on the event card and in pause): the overlay draws only while the
// HUD race time [B+0xc] advances (GO..finish; frozen in pause, results,
// menus and on the pre-race card). The uber tap moved to the CD serve path
// (it never fired: noteCdRead's callers are trace-armed-gated).

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace ps2_ssx3_tricky_hud
{

constexpr uint32_t kRiderFillOff = 0x2f8u;   // f32 boost target 0..1 (FH10)
constexpr uint32_t kRiderUberOff = 0x2f4u;   // i32 uber level 0..10 (`sw`, 0x10e9e8)
// TK44: the race object's frame counters (chain B; ps2_fh1_full120.h System 12).
constexpr uint32_t kRaceTickOff = 0x8u;      // race tick (bumps every rider pass, incl. results fly-by)
constexpr uint32_t kRaceClockOff = 0xcu;     // HUD race time (runs GO..finish; frozen in pause/menus/results)
// TK44: presents after a clock freeze before the meter hides. 2 is 120-safe:
// at full120 the clock bumps 1x per 2 guest ticks, so grace 1 would flicker.
constexpr uint64_t kRaceClockGraceTicks = 2u;
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

// TK44: the chain's race object (B = [[0x4a28a8]+0x84]+0xC), carrying the
// race counters. Null/range-checked like resolveChainR; 0 on any failure.
inline uint32_t resolveChainB(const uint8_t *ram, size_t ramSize)
{
    if (!ram || ramSize == 0u)
        return 0u;
    uint32_t g = 0u, a = 0u, b = 0u;
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
    return b;
}

// TK44: race-only visibility from the HUD race time [B+0xc] (pure; the
// overlay owns the state). Any change (forward bump, gate restart) proves a
// live race; a freeze older than the grace hides (pause, results, menus,
// pre-race card). Nothing shows until the first advance is seen.
struct RaceClock
{
    bool init = false;
    bool seen = false;
    uint32_t last = 0u;
    uint64_t lastTick = 0u;
};

inline bool updateRaceClock(RaceClock &st, uint32_t clock, uint64_t tick)
{
    if (!st.init)
    {
        st.init = true;
        st.last = clock;
        st.lastTick = tick;
        return false;
    }
    if (clock != st.last)
    {
        st.last = clock;
        st.lastTick = tick;
        st.seen = true;
    }
    return st.seen && (tick - st.lastTick) <= kRaceClockGraceTicks;
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
// boot). Called from the CD serve path (Kernel/Stubs/CD.cpp) on every
// served read; self-gated on PS2X_SSX3_TRICKY_HUD. TK44: it used to be
// called from ps2_e41_trace::noteCdRead, whose call sites are gated on the
// trace being armed, so no tap ever fired on a play build.
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

// One bilinear atlas sample (straight-alpha float source), shared by blit
// and the TK43e sprite pre-render so the two are bit-identical by
// construction. (rx, ry) is the dest-px position relative to the draw.
inline void sampleAtlas(const Atlas &a, const Rect &s, int rx, int ry, int dw, int dh, float dim,
                        float &sr, float &sg, float &sb, float &sa)
{
    const float v = (static_cast<float>(ry) + 0.5f) * static_cast<float>(s.h) / static_cast<float>(dh) - 0.5f;
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
    const float u = (static_cast<float>(rx) + 0.5f) * static_cast<float>(s.w) / static_cast<float>(dw) - 0.5f;
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
    sr = (p00[0] * w00 + p10[0] * w10 + p01[0] * w01 + p11[0] * w11) * dim;
    sg = (p00[1] * w00 + p10[1] * w10 + p01[1] * w01 + p11[1] * w11) * dim;
    sb = (p00[2] * w00 + p10[2] * w10 + p01[2] * w01 + p11[2] * w11) * dim;
    sa = (p00[3] * w00 + p10[3] * w10 + p01[3] * w01 + p11[3] * w11) / 255.0f;
}

// Alpha-over blend of one float sample onto a frame px (blit's formula).
inline void blendSample(float sr, float sg, float sb, float sa, uint8_t *d)
{
    if (sa <= 0.0f)
        return;
    const float ia = 1.0f - sa;
    d[0] = static_cast<uint8_t>(sr * sa + d[0] * ia + 0.5f);
    d[1] = static_cast<uint8_t>(sg * sa + d[1] * ia + 0.5f);
    d[2] = static_cast<uint8_t>(sb * sa + d[2] * ia + 0.5f);
    d[3] = 255;
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
        for (int x = x0; x < x1; ++x)
        {
            float sr = 0.0f, sg = 0.0f, sb = 0.0f, sa = 0.0f;
            sampleAtlas(a, s, x - dx, y - dy, dw, dh, dim, sr, sg, sb, sa);
            if (sa <= 0.0f)
                continue;
            blendSample(sr, sg, sb, sa,
                        &frame[(static_cast<size_t>(y) * static_cast<size_t>(fw) + static_cast<size_t>(x)) * 4u]);
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
// onto any export size right-anchored and per-axis scaled. The guest score
// above y=50 is kept; everything else the meter covers is replaced.
// TK43e: the VK/AHB export (1920x1080) is the snapshot content stretched
// per axis (3.0 x 2.25, the anamorphic 16:9), not uniformly scaled: a
// height-only scale misplaces the smear by ~78 px and peeks the guest ball
// (TK43e gari leg sc04). sx == sy on 640x480 and 1280x960, so the GL path
// and the tuned geometry are unchanged there.
struct Layout
{
    float sx = 1.0f; // fw/640
    float sy = 1.0f; // fh/480
    int fw = 640;
    int X(float x) const { return static_cast<int>(std::lround(fw - (640.0f - x) * sx)); }
    int Y(float y) const { return static_cast<int>(std::lround(y * sy)); }
    int W(float w) const { return static_cast<int>(std::lround(w * sx)); }
    int H(float h) const { return static_cast<int>(std::lround(h * sy)); }
};

// TK43e: the HUD's screen region (buffer px, clipped to the frame): the
// union of every draw below in 640x480 space is x481-640 (splash left to
// the right edge) by y49-430 (smear top to pill bottom), plus a 3 px margin
// for Layout rounding (each mapped edge rounds at most 1 px) and the
// smear's 2 px edge samples. Every write and every background read (smear
// edge samples, alpha-over dst) lands strictly inside, so composing into a
// temp pre-filled with the region's background and copying back is exactly
// composeOverlay. The VK/AHB path (no host pixels) composites this region
// straight into the locked AHB; the GL path draws the same region.
inline Rect hudRegionRect(int fw, int fh)
{
    if (fw <= 0 || fh <= 0)
        return {0, 0, 0, 0};
    Layout L;
    L.sx = static_cast<float>(fw) / 640.0f;
    L.sy = static_cast<float>(fh) / 480.0f;
    L.fw = fw;
    int x0 = L.X(481) - 3;
    int y0 = L.Y(49) - 3;
    int x1 = fw;
    int y1 = L.Y(430) + 3;
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > fw)
        x1 = fw;
    if (y1 > fh)
        y1 = fh;
    if (x1 < x0)
        x1 = x0;
    if (y1 < y0)
        y1 = y0;
    return {x0, y0, x1 - x0, y1 - y0};
}

// TK43e: the draw list, once, into a tight-rows buffer holding the HUD
// region's background at frame origin (ox, oy) of an fw x fh frame. The GL
// path and the VK/AHB path both draw this; composeOverlay is the GL
// wrapper (copy out, compose, copy back).
inline void composeHudInto(uint8_t *dst, int bw, int bh, int ox, int oy, int fw, int fh, const Atlas &a,
                           float fill, bool full, uint64_t tick, uint64_t splashUntilTick, int litLetters,
                           uint64_t flashUntilTick)
{
    if (!dst || !a.ok || bw <= 0 || bh <= 0 || fw <= 0 || fh <= 0)
        return;
    Layout L;
    L.sx = static_cast<float>(fw) / 640.0f;
    L.sy = static_cast<float>(fh) / 480.0f;
    L.fw = fw;
    // Pole through the stack (covers the SSX 3 red center line, x582-586):
    // Tricky's grey cylinder profile stretched across its width.
    blit(a, poleRect(), dst, bw, bh, L.X(584) - ox, L.Y(140) - oy, L.W(5), L.H(260));
    // 16 rings, bottom-up: gold, orange, orange, red bands; unlit slots
    // silver. Native 49x12 (the real game draws them 1:1) on Part 2's
    // 16.5 px pitch, which fits the SSX 3 coil footprint (y143-395) with the
    // jewel touching above; the pole shows through the 4.5 px gaps.
    const int lit = litCoils(fill);
    for (int i = 0; i < kCoils; ++i)
    {
        const float cy = 395.0f - static_cast<float>(i) * 16.5f;
        const Rect src = i < lit ? ringRect(i / 4) : silverRingRect();
        blit(a, src, dst, bw, bh, L.X(563) - ox, L.Y(cy - 6.0f) - oy, L.W(49), L.H(12));
    }
    // Jewel over the ball: grey, or red pulsing at ~3.7 Hz when full.
    const Rect jewel = full ? jewelRedRect() : jewelGreyRect();
    const float dim = (full && ((tick >> 3) & 1u) != 0u) ? 0.82f : 1.0f;
    blit(a, jewel, dst, bw, bh, L.X(568) - ox, L.Y(107) - oy, L.W(40), L.H(36), dim);
    // Label cover: feathered sky smear, no panel (Part 3). The chrome arch
    // draws over it; the score (above y49) and jewel (below y101) are spared.
    smearCover(dst, bw, bh, L.X(536) - ox, L.Y(49) - oy, L.X(628) - ox, L.Y(101) - oy);
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
        const Rect dstR = letterRect(i);
        blit(a, src, dst, bw, bh, L.X(kArch[i][0]) - ox, L.Y(kArch[i][1]) - oy, L.W(dstR.w), L.H(dstR.h));
    }
    // Pill slot over the S (y400-427): Tricky's red S hexagon when full,
    // its grey copy otherwise. Real Tricky shows no pill until full; the
    // grey slot keeps the SSX 3 S covered and mirrors the jewel.
    blit(a, full ? pillRect() : pillGreyRect(), dst, bw, bh, L.X(571) - ox, L.Y(400) - oy, L.W(34),
         L.H(30));
    // First-full snowflake splash (TK43 section 1.3: cheap one-shot).
    // Part 3: full re-cut flake, recording orange, 156x116 over the jewel
    // bottom and top ~5 rings (tile 1: x346-463 y101-166).
    if (tick < splashUntilTick)
        blit(a, snowflakeRect(), dst, bw, bh, L.X(481) - ox, L.Y(114) - oy, L.W(156), L.H(116));
}

inline void composeOverlay(uint8_t *frame, int fw, int fh, const Atlas &a, float fill, bool full,
                           uint64_t tick, uint64_t splashUntilTick, int litLetters,
                           uint64_t flashUntilTick)
{
    if (!frame || !a.ok || fw <= 0 || fh <= 0)
        return;
    const Rect r = hudRegionRect(fw, fh);
    if (r.w <= 0 || r.h <= 0)
        return;
    std::vector<uint8_t> tmp(static_cast<size_t>(r.w) * static_cast<size_t>(r.h) * 4u);
    const size_t rowBytes = static_cast<size_t>(r.w) * 4u;
    for (int y = 0; y < r.h; ++y)
        std::memcpy(&tmp[static_cast<size_t>(y) * rowBytes],
                    &frame[(static_cast<size_t>(r.y + y) * static_cast<size_t>(fw) + static_cast<size_t>(r.x)) * 4u],
                    rowBytes);
    composeHudInto(tmp.data(), r.w, r.h, r.x, r.y, fw, fh, a, fill, full, tick, splashUntilTick,
                   litLetters, flashUntilTick);
    for (int y = 0; y < r.h; ++y)
        std::memcpy(&frame[(static_cast<size_t>(r.y + y) * static_cast<size_t>(fw) + static_cast<size_t>(r.x)) * 4u],
                    &tmp[static_cast<size_t>(y) * rowBytes], rowBytes);
}

// TK43e: pre-scaled art sprites for the VK/AHB path. The export size is
// fixed per run, so every draw's bilinear samples are too: renderSprite
// pre-computes them once (float, via the same sampleAtlas) and stampSprite
// blends them per frame with the same blendSample. Cached == direct
// bit-exact (locked by the Region test); per-frame bilinear drops to zero.
// Only the background-dependent smear still runs per frame.
struct SpriteImg
{
    int w = 0;
    int h = 0;
    std::vector<float> px; // w*h*4 straight-alpha float samples
};

inline SpriteImg renderSprite(const Atlas &a, const Rect &s, int dw, int dh, float dim)
{
    SpriteImg img;
    if (!a.ok || dw <= 0 || dh <= 0)
        return img;
    img.w = dw;
    img.h = dh;
    img.px.resize(static_cast<size_t>(dw) * static_cast<size_t>(dh) * 4u);
    for (int y = 0; y < dh; ++y)
        for (int x = 0; x < dw; ++x)
        {
            float sr = 0.0f, sg = 0.0f, sb = 0.0f, sa = 0.0f;
            sampleAtlas(a, s, x, y, dw, dh, dim, sr, sg, sb, sa);
            float *o = &img.px[(static_cast<size_t>(y) * static_cast<size_t>(dw) + static_cast<size_t>(x)) * 4u];
            o[0] = sr;
            o[1] = sg;
            o[2] = sb;
            o[3] = sa;
        }
    return img;
}

inline void stampSprite(const SpriteImg &img, uint8_t *dst, int bw, int bh, int dx, int dy)
{
    if (!dst || bw <= 0 || bh <= 0 || img.w <= 0 || img.h <= 0 || img.px.empty())
        return;
    const int x0 = dx < 0 ? 0 : dx;
    const int y0 = dy < 0 ? 0 : dy;
    const int x1 = dx + img.w > bw ? bw : dx + img.w;
    const int y1 = dy + img.h > bh ? bh : dy + img.h;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
        {
            const float *o =
                &img.px[(static_cast<size_t>(y - dy) * static_cast<size_t>(img.w) + static_cast<size_t>(x - dx)) *
                        4u];
            if (o[3] <= 0.0f)
                continue;
            blendSample(o[0], o[1], o[2], o[3],
                        &dst[(static_cast<size_t>(y) * static_cast<size_t>(bw) + static_cast<size_t>(x)) * 4u]);
        }
}

// Every draw's dest rect (frame coords) + pre-rendered variant images.
// Dest rects come from the same Layout calls as composeHudInto.
struct HudSprites
{
    bool ok = false;
    const Atlas *atlas = nullptr;
    int fw = 0;
    int fh = 0;
    Rect region;
    Rect poleDst;
    SpriteImg pole;
    Rect ringDst[kCoils];
    SpriteImg ringImg[5]; // 0..3 bands, 4 silver
    Rect jewelDst;
    SpriteImg jewelImg[3]; // 0 grey, 1 red bright, 2 red dim
    int smearX0 = 0, smearY0 = 0, smearX1 = 0, smearY1 = 0;
    Rect archDst[6];
    SpriteImg archChrome[6], archRed[6];
    Rect pillDst;
    SpriteImg pillImg[2]; // 0 red (full), 1 grey
    Rect splashDst;
    SpriteImg splash;
};

inline bool buildHudSprites(HudSprites &ss, const Atlas &a, int fw, int fh)
{
    ss = HudSprites{};
    if (!a.ok || fw <= 0 || fh <= 0)
        return false;
    Layout L;
    L.sx = static_cast<float>(fw) / 640.0f;
    L.sy = static_cast<float>(fh) / 480.0f;
    L.fw = fw;
    ss.fw = fw;
    ss.fh = fh;
    ss.atlas = &a;
    ss.region = hudRegionRect(fw, fh);
    if (ss.region.w <= 0 || ss.region.h <= 0)
        return false;
    ss.poleDst = {L.X(584), L.Y(140), L.W(5), L.H(260)};
    ss.pole = renderSprite(a, poleRect(), ss.poleDst.w, ss.poleDst.h, 1.0f);
    for (int i = 0; i < kCoils; ++i)
    {
        const float cy = 395.0f - static_cast<float>(i) * 16.5f;
        ss.ringDst[i] = {L.X(563), L.Y(cy - 6.0f), L.W(49), L.H(12)};
    }
    for (int b = 0; b < 4; ++b)
        ss.ringImg[b] = renderSprite(a, ringRect(b), ss.ringDst[0].w, ss.ringDst[0].h, 1.0f);
    ss.ringImg[4] = renderSprite(a, silverRingRect(), ss.ringDst[0].w, ss.ringDst[0].h, 1.0f);
    ss.jewelDst = {L.X(568), L.Y(107), L.W(40), L.H(36)};
    ss.jewelImg[0] = renderSprite(a, jewelGreyRect(), ss.jewelDst.w, ss.jewelDst.h, 1.0f);
    ss.jewelImg[1] = renderSprite(a, jewelRedRect(), ss.jewelDst.w, ss.jewelDst.h, 1.0f);
    ss.jewelImg[2] = renderSprite(a, jewelRedRect(), ss.jewelDst.w, ss.jewelDst.h, 0.82f);
    ss.smearX0 = L.X(536);
    ss.smearY0 = L.Y(49);
    ss.smearX1 = L.X(628);
    ss.smearY1 = L.Y(101);
    static const int kArch[6][2] = {
        {527, 94}, {550, 83}, {565, 72}, {579, 79}, {595, 73}, {607, 94},
    };
    for (int i = 0; i < 6; ++i)
    {
        const Rect dstR = letterRect(i);
        ss.archDst[i] = {L.X(kArch[i][0]), L.Y(kArch[i][1]), L.W(dstR.w), L.H(dstR.h)};
        ss.archChrome[i] = renderSprite(a, letterRect(i), ss.archDst[i].w, ss.archDst[i].h, 1.0f);
        ss.archRed[i] = renderSprite(a, litLetterRect(i), ss.archDst[i].w, ss.archDst[i].h, 1.0f);
    }
    ss.pillDst = {L.X(571), L.Y(400), L.W(34), L.H(30)};
    ss.pillImg[0] = renderSprite(a, pillRect(), ss.pillDst.w, ss.pillDst.h, 1.0f);
    ss.pillImg[1] = renderSprite(a, pillGreyRect(), ss.pillDst.w, ss.pillDst.h, 1.0f);
    ss.splashDst = {L.X(481), L.Y(114), L.W(156), L.H(116)};
    ss.splash = renderSprite(a, snowflakeRect(), ss.splashDst.w, ss.splashDst.h, 1.0f);
    ss.ok = true;
    return true;
}

// The cached compose: same draws, same order, same values as
// composeHudInto; per-frame state only picks variant images.
inline void stampHudInto(uint8_t *tmp, int bw, int bh, int ox, int oy, const HudSprites &ss, float fill,
                         bool full, uint64_t tick, uint64_t splashUntilTick, int litLetters,
                         uint64_t flashUntilTick)
{
    if (!tmp || !ss.ok || bw <= 0 || bh <= 0)
        return;
    stampSprite(ss.pole, tmp, bw, bh, ss.poleDst.x - ox, ss.poleDst.y - oy);
    const int lit = litCoils(fill);
    for (int i = 0; i < kCoils; ++i)
    {
        const SpriteImg &img = i < lit ? ss.ringImg[i / 4] : ss.ringImg[4];
        stampSprite(img, tmp, bw, bh, ss.ringDst[i].x - ox, ss.ringDst[i].y - oy);
    }
    const bool dim = full && (((tick >> 3) & 1u) != 0u);
    stampSprite(full ? (dim ? ss.jewelImg[2] : ss.jewelImg[1]) : ss.jewelImg[0], tmp, bw, bh,
                ss.jewelDst.x - ox, ss.jewelDst.y - oy);
    smearCover(tmp, bw, bh, ss.smearX0 - ox, ss.smearY0 - oy, ss.smearX1 - ox, ss.smearY1 - oy);
    if (litLetters < 0)
        litLetters = 0;
    if (litLetters > 6)
        litLetters = 6;
    const bool flashing = tick < flashUntilTick;
    const bool flashRed = flashing && (((tick >> 3) & 1u) == 0u);
    for (int i = 0; i < 6; ++i)
    {
        const bool red = flashing ? flashRed : (i < litLetters);
        stampSprite(red ? ss.archRed[i] : ss.archChrome[i], tmp, bw, bh, ss.archDst[i].x - ox,
                    ss.archDst[i].y - oy);
    }
    stampSprite(full ? ss.pillImg[0] : ss.pillImg[1], tmp, bw, bh, ss.pillDst.x - ox, ss.pillDst.y - oy);
    if (tick < splashUntilTick)
        stampSprite(ss.splash, tmp, bw, bh, ss.splashDst.x - ox, ss.splashDst.y - oy);
}

// TK43e Part 2: stride-aware direct composite. Stamps straight into the
// locked AHB (no region temp round-trip): same draws, same order, same
// sample/blend math as the temp path, so bit-identical values. base points
// at the region origin; rows advance by strideBytes; (bw, bh) clip.
inline void stampSpriteS(const SpriteImg &img, uint8_t *base, size_t strideBytes, int bw, int bh, int dx,
                         int dy)
{
    if (!base || bw <= 0 || bh <= 0 || img.w <= 0 || img.h <= 0 || img.px.empty())
        return;
    const int x0 = dx < 0 ? 0 : dx;
    const int y0 = dy < 0 ? 0 : dy;
    const int x1 = dx + img.w > bw ? bw : dx + img.w;
    const int y1 = dy + img.h > bh ? bh : dy + img.h;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
        {
            const float *o =
                &img.px[(static_cast<size_t>(y - dy) * static_cast<size_t>(img.w) + static_cast<size_t>(x - dx)) *
                        4u];
            if (o[3] <= 0.0f)
                continue;
            blendSample(o[0], o[1], o[2], o[3], base + static_cast<size_t>(y) * strideBytes +
                                                     static_cast<size_t>(x) * 4u);
        }
}

inline void smearCoverS(uint8_t *base, size_t strideBytes, int bw, int bh, int x0, int y0, int x1, int y1)
{
    if (!base || bw <= 0 || bh <= 0 || x1 <= x0 || y1 <= y0)
        return;
    const int xa = x0 < 0 ? 0 : x0;
    const int xb = x1 > bw ? bw : x1;
    const int ya = y0 < 0 ? 0 : y0;
    const int yb = y1 > bh ? bh : y1;
    const float span = static_cast<float>(x1 - x0);
    for (int y = ya; y < yb; ++y)
    {
        const int lx = x0 - 2 < 0 ? 0 : x0 - 2;
        const int rx = x1 + 2 > bw - 1 ? bw - 1 : x1 + 2;
        const uint8_t *L = base + static_cast<size_t>(y) * strideBytes + static_cast<size_t>(lx) * 4u;
        const uint8_t *R = base + static_cast<size_t>(y) * strideBytes + static_cast<size_t>(rx) * 4u;
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
            uint8_t *d = base + static_cast<size_t>(y) * strideBytes + static_cast<size_t>(x) * 4u;
            const float ia = 1.0f - a;
            d[0] = static_cast<uint8_t>((lr + (rr - lr) * t) * a + d[0] * ia + 0.5f);
            d[1] = static_cast<uint8_t>((lg + (rg - lg) * t) * a + d[1] * ia + 0.5f);
            d[2] = static_cast<uint8_t>((lb + (rb - lb) * t) * a + d[2] * ia + 0.5f);
        }
    }
}

// The cached compose, stride-aware: same draws, same order, same values as
// stampHudInto. ahbBase is the buffer base; r is the HUD region in it.
inline void stampHudDirect(uint8_t *ahbBase, size_t strideBytes, const Rect &r, const HudSprites &ss,
                           float fill, bool full, uint64_t tick, uint64_t splashUntilTick, int litLetters,
                           uint64_t flashUntilTick)
{
    if (!ahbBase || !ss.ok || r.w <= 0 || r.h <= 0)
        return;
    uint8_t *base = ahbBase + static_cast<size_t>(r.y) * strideBytes + static_cast<size_t>(r.x) * 4u;
    const int bw = r.w, bh = r.h, ox = r.x, oy = r.y;
    stampSpriteS(ss.pole, base, strideBytes, bw, bh, ss.poleDst.x - ox, ss.poleDst.y - oy);
    const int lit = litCoils(fill);
    for (int i = 0; i < kCoils; ++i)
    {
        const SpriteImg &img = i < lit ? ss.ringImg[i / 4] : ss.ringImg[4];
        stampSpriteS(img, base, strideBytes, bw, bh, ss.ringDst[i].x - ox, ss.ringDst[i].y - oy);
    }
    const bool dim = full && (((tick >> 3) & 1u) != 0u);
    stampSpriteS(full ? (dim ? ss.jewelImg[2] : ss.jewelImg[1]) : ss.jewelImg[0], base, strideBytes, bw, bh,
                 ss.jewelDst.x - ox, ss.jewelDst.y - oy);
    smearCoverS(base, strideBytes, bw, bh, ss.smearX0 - ox, ss.smearY0 - oy, ss.smearX1 - ox,
                ss.smearY1 - oy);
    if (litLetters < 0)
        litLetters = 0;
    if (litLetters > 6)
        litLetters = 6;
    const bool flashing = tick < flashUntilTick;
    const bool flashRed = flashing && (((tick >> 3) & 1u) == 0u);
    for (int i = 0; i < 6; ++i)
    {
        const bool red = flashing ? flashRed : (i < litLetters);
        stampSpriteS(red ? ss.archRed[i] : ss.archChrome[i], base, strideBytes, bw, bh,
                     ss.archDst[i].x - ox, ss.archDst[i].y - oy);
    }
    stampSpriteS(full ? ss.pillImg[0] : ss.pillImg[1], base, strideBytes, bw, bh, ss.pillDst.x - ox,
                 ss.pillDst.y - oy);
    if (tick < splashUntilTick)
        stampSpriteS(ss.splash, base, strideBytes, bw, bh, ss.splashDst.x - ox, ss.splashDst.y - oy);
}

// TK43e: the overlay's process-wide state, shared by the GL call site (the
// main thread, trickyHudOverlay in ps2_runtime.cpp) and the VK/AHB call
// site (the GS worker, queuePendingAhb in ps2_gs_external_backend.cpp).
// Only one path runs at a time, but a mid-run VK->GL fallback can overlap
// them for a frame; the mutex keeps the letters/splash/atlas continuous.
struct HudState
{
    std::mutex mu;
    bool wantedInit = false;
    bool wanted = false;
    Atlas atlas;
    bool atlasTried = false;
    bool lastFull = false;
    uint64_t splashUntil = 0u;
    LetterState letters;
    bool lettersInTricky = false;
    bool lettersPresetInit = false;
    int lettersPreset = -1;
    bool forcedInit = false;
    ForceValue forced;
    RaceClock raceClock;     // TK44: race-only visibility from [B+0xc]
    bool lastRacing = false; // TK44: last racing value (transition log)
};

inline HudState &hudState()
{
    static HudState s;
    return s;
}

// The per-frame compose decision (pure values out; the atlas is immutable
// once ok, so the pointer stays valid after the mutex is released).
struct HudParams
{
    bool draw = false;
    const Atlas *atlas = nullptr;
    float fill = 0.0f;
    bool full = false;
    uint64_t splashUntil = 0u;
    int litLetters = 0;
    uint64_t flashUntil = 0u;
};

inline bool hudWanted()
{
    HudState &st = hudState();
    std::lock_guard<std::mutex> lock(st.mu);
    if (!st.wantedInit)
    {
        st.wantedInit = true;
        const char *env = std::getenv("PS2X_SSX3_TRICKY_HUD");
        st.wanted = env && env[0] == '1';
    }
    return st.wanted;
}

// TK43e: the GS worker has no PS2Runtime*, so the main thread publishes the
// live RDRAM base every host iteration (UploadFrame entry), and the VK/AHB
// call site reads it here. Never written, only read, like the GL path.
inline std::atomic<const uint8_t *> &liveRdramPtr()
{
    static std::atomic<const uint8_t *> p(nullptr);
    return p;
}

inline std::atomic<size_t> &liveRdramSize()
{
    static std::atomic<size_t> n(0u);
    return n;
}

inline void publishRdram(const uint8_t *rdram, size_t size)
{
    liveRdramPtr().store(rdram, std::memory_order_release);
    liveRdramSize().store(size, std::memory_order_release);
}

inline const uint8_t *liveRdram(size_t &size)
{
    const uint8_t *p = liveRdramPtr().load(std::memory_order_acquire);
    size = liveRdramSize().load(std::memory_order_acquire);
    return p;
}

} // namespace ps2_ssx3_tricky_hud
