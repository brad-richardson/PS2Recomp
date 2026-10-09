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
// were counted by a host-side CD tap (TK47: now the game's own 0x2133 post;
// the overlay lights one red letter per post, T->R->I->C->K->Y); the 6th
// opens a 64-tick all-red flash, then the letters reset (real Tricky past
// R is unobserved; reset and say so). The red sprites are new atlas cells
// (map1 red wordmark run, TK43a2 boxes); the atlas magic is TKHUD2.
//
// TK43e: the draw list is composeHudInto (region + origin), drawn by both
// the GL path (composeOverlay: copy out, compose, copy back) and the VK/AHB
// path (queuePendingAhb composites the region into the locked AHB before
// queue); TKL1: both consume the layer's immutable packet, decided per
// VBlank by the EE-owned reducer (ps2_ssx3_tricky_layer.h).
//
// TK44: race-only visibility (the chain resolves in menus, so the meter used
// to draw on the event card and in pause): the overlay draws only while the
// HUD race time [B+0xc] advances (GO..finish; frozen in pause, results,
// menus and on the pre-race card). The uber tap moved to the CD serve path
// (it never fired: noteCdRead's callers are trace-armed-gated).
//
// TK47: letters count the game's own uber post (0x2133 through 0x2B1458,
// wrapped in ps2_runtime.cpp) instead of the deleted CD-LBN tap; a
// post-boundary adopt window (restart or GO from a frozen 0) swallows the
// guest's post-GO meter init writes (no spurious GO burst).

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
// TK44: the race object's frame counters (chain B; ps2_fh1_full120.h System 12).
constexpr uint32_t kRaceTickOff = 0x8u;      // race tick (bumps every rider pass, incl. results fly-by)
constexpr uint32_t kRaceClockOff = 0xcu;     // HUD race time (runs GO..finish; frozen in pause/menus/results)
// TK44: presents after a clock freeze before the meter hides. 2 is 120-safe:
// at full120 the clock bumps 1x per 2 guest ticks, so grace 1 would flicker.
constexpr uint64_t kRaceClockGraceTicks = 2u;
// TK47: post-boundary adopt window (host ticks). The guest (re)initializes
// meter words around race boundaries, and a same-session race can step
// 0->full ~65 ticks after GO (gari peek: R stable, fill 0->0.9999 and
// level 0->1 between 7450-7455 at GO+65). No legit top lands within 120
// ticks of a boundary: the rider is at the gate and the meter starts
// empty (race-1 reference: 0/0 at GO; earliest legit top GO+~2500).
constexpr uint64_t kGoAdoptTicks = 120u;
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

// TKL1 F8: every hard-coded sprite rect (plus the bilinear sampler's reads)
// must fit the atlas. The sampler clamps to s.w-2/s.h-2 and reads the four
// neighbors, so a rect inside the atlas with both dims >= 2 can never read
// out of bounds; anything smaller is refused with the atlas.
inline bool atlasLayoutValid(int w, int h)
{
    if (w <= 0 || h <= 0)
        return false;
    const Rect rects[] = {
        ringRect(0), ringRect(1), ringRect(2), ringRect(3), silverRingRect(), poleRect(),
        letterRect(0), letterRect(1), letterRect(2), letterRect(3), letterRect(4), letterRect(5),
        litLetterRect(0), litLetterRect(1), litLetterRect(2), litLetterRect(3), litLetterRect(4),
        litLetterRect(5), jewelGreyRect(), jewelRedRect(), pillRect(), pillGreyRect(), snowflakeRect(),
    };
    for (const Rect &s : rects)
    {
        if (s.w < 2 || s.h < 2 || s.x < 0 || s.y < 0 || s.x + s.w > w || s.y + s.h > h)
            return false;
    }
    return true;
}

// Header: "TKHUD2\0\0" + u32le w,h,reserved, then w*h*4 RGBA bytes.
// TK43c: magic bumped (TKHUD1 atlases lack the red-letter row and are
// refused; the atlas is staged fresh, never committed).
// TKL1 F8: the layout is exactly 256x256 (the staged atlas from
// make_trickyhud_atlas.py); any other dimensions are refused, since the
// hard-coded rects above assume this layout.
inline Atlas parseAtlas(const uint8_t *data, size_t size)
{
    Atlas a;
    static const char kMagic[8] = {'T', 'K', 'H', 'U', 'D', '2', '\0', '\0'};
    if (!data || size < 20u || std::memcmp(data, kMagic, 8) != 0)
        return a;
    uint32_t w = 0, h = 0;
    std::memcpy(&w, data + 8, 4);
    std::memcpy(&h, data + 12, 4);
    if (w != static_cast<uint32_t>(kAtlasW) || h != static_cast<uint32_t>(kAtlasH))
        return a;
    if (!atlasLayoutValid(static_cast<int>(w), static_cast<int>(h)))
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
    // TKL1 F8: bound the read before allocating (a 256x256 atlas is 256 KiB;
    // anything past 4 MiB is refused without growing the buffer further).
    static constexpr size_t kMaxAtlasFile = 20u + 1024u * 1024u * 4u;
    std::vector<uint8_t> buf;
    uint8_t chunk[65536];
    size_t n = 0;
    bool tooBig = false;
    while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0u)
    {
        if (buf.size() + n > kMaxAtlasFile)
        {
            tooBig = true;
            break;
        }
        buf.insert(buf.end(), chunk, chunk + n);
    }
    std::fclose(f);
    if (tooBig)
        return a;
    return parseAtlas(buf.data(), buf.size());
}

// TKL1 F7: strict guest-pointer validation. A readable/writable guest
// pointer must be nonzero, name a RAM mirror segment (low/KUSEG, KSEG0
// cached or KSEG1 uncached; the game keeps these structs in low RAM),
// be 4-aligned, and span the whole request inside RDRAM with no u32
// wrap. Stale/non-RAM pointers fail closed instead of wrapping into
// valid host RAM. `offset` is the validated RDRAM offset on success.
inline bool validateGuestPtr(uint32_t addr, size_t size, size_t ramSize, uint32_t &offset)
{
    if (addr == 0u || !ramSize || size == 0u || size > ramSize)
        return false;
    const uint32_t seg = addr >> 28;
    if (seg != 0u && seg != 8u && seg != 9u && seg != 0xAu && seg != 0xBu)
        return false;
    if ((addr & 3u) != 0u)
        return false;
    const uint32_t off = addr & kRamMask;
    if (static_cast<size_t>(off) > ramSize - size)
        return false;
    offset = off;
    return true;
}

// One validated u32/f32 lane: the span (base..base+off+4) is validated
// before any arithmetic on offsets, so a wild base can never wrap into
// range. All offsets below are small constants; base+off+4 cannot wrap
// u32 once the span check passes (offsets are < ramSize).
inline bool readGuestU32(const uint8_t *ram, size_t ramSize, uint32_t base, uint32_t off, uint32_t &out)
{
    if (!ram || ramSize == 0u)
        return false;
    uint32_t bo = 0u;
    if (!validateGuestPtr(base, static_cast<size_t>(off) + 4u, ramSize, bo))
        return false;
    std::memcpy(&out, ram + bo + off, 4);
    return true;
}

inline bool readGuestF32(const uint8_t *ram, size_t ramSize, uint32_t base, uint32_t off, float &out)
{
    uint32_t b = 0u;
    if (!readGuestU32(ram, ramSize, base, off, b))
        return false;
    std::memcpy(&out, &b, 4);
    return true;
}

inline bool writeGuestU32(uint8_t *ram, size_t ramSize, uint32_t base, uint32_t off, uint32_t v)
{
    if (!ram || ramSize == 0u)
        return false;
    uint32_t bo = 0u;
    if (!validateGuestPtr(base, static_cast<size_t>(off) + 4u, ramSize, bo))
        return false;
    std::memcpy(ram + bo + off, &v, 4);
    return true;
}

// One race-boundary predicate (TKL1 stage 3): a new rider struct or a
// rewound race clock. The layer reducer and the gems poll evaluate it at
// their own safe points (per-VBlank vs per-physics-step).
inline bool raceBoundaryCrossed(uint32_t lastR, uint32_t lastClock, uint32_t r, uint32_t clock)
{
    return r != lastR || clock < lastClock;
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
    uint32_t rb = 0u;
    if (!validateGuestPtr(r, static_cast<size_t>(kRiderFillOff) + 4u, ramSize, rb))
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
    uint32_t r = 0u;
    if (!readGuestU32(ram, ramSize, ptrAddr, 0u, r))
        return MeterFrame();
    return readMeterFrameAt(ram, ramSize, r);
}

// TK43d: the player-1 rider through the game's own globals:
// R = [[[0x4a28a8]+0x84]+0xC]+0x28. The SSX 3 HUD draws its meter from the
// same struct: 0x29ab40 reads s2 = [func_28B1D8()+s5+0x28] (s5 = 0 for
// player 1) and s2 carries the rider words (+0x2F0 at 0x29acd8, +0x790 at
// 0x29abfc as in boost-add 0x10e990, +0x870 at 0x29ac80 as in func_29AB08
// with a1 = R). Each hop is F7-validated (segment/alignment/span, no wrap);
// any failure returns 0 (menus / pre-race: the overlay draws nothing).
inline uint32_t resolveChainR(const uint8_t *ram, size_t ramSize)
{
    if (!ram || ramSize == 0u)
        return 0u;
    uint32_t g = 0u, a = 0u, b = 0u, r = 0u;
    if (!readGuestU32(ram, ramSize, kChainRoot, 0u, g) || g == 0u)
        return 0u;
    if (!readGuestU32(ram, ramSize, g, kChainAOff, a) || a == 0u)
        return 0u;
    if (!readGuestU32(ram, ramSize, a, kChainBOff, b) || b == 0u)
        return 0u;
    if (!readGuestU32(ram, ramSize, b, kChainROff, r))
        return 0u;
    return r;
}

// TK44: the chain's race object (B = [[0x4a28a8]+0x84]+0xC), carrying the
// race counters. F7-validated like resolveChainR; 0 on any failure.
inline uint32_t resolveChainB(const uint8_t *ram, size_t ramSize)
{
    if (!ram || ramSize == 0u)
        return 0u;
    uint32_t g = 0u, a = 0u, b = 0u;
    if (!readGuestU32(ram, ramSize, kChainRoot, 0u, g) || g == 0u)
        return 0u;
    if (!readGuestU32(ram, ramSize, g, kChainAOff, a) || a == 0u)
        return 0u;
    if (!readGuestU32(ram, ramSize, a, kChainBOff, b))
        return 0u;
    return b;
}

// TR3: the game object's replay (P = [[[0x4a28a8]+0x84]+0x28]; the callers
// of cReplay_stopAutoReplay 0x2706F0 pass [A+0x28]) and its state word at
// P+0: 0 while a live race records, 1 while the results screen's auto
// replay plays behind the menu, where the race clock restarts and runs
// (Garibaldi savestates t12000 / t13500). False on any failed read.
constexpr uint32_t kChainReplayOff = 0x28u;
inline bool readReplayState(const uint8_t *ram, size_t ramSize, uint32_t &state)
{
    if (!ram || ramSize == 0u)
        return false;
    uint32_t g = 0u, a = 0u, p = 0u;
    if (!readGuestU32(ram, ramSize, kChainRoot, 0u, g) || g == 0u)
        return false;
    if (!readGuestU32(ram, ramSize, g, kChainAOff, a) || a == 0u)
        return false;
    if (!readGuestU32(ram, ramSize, a, kChainReplayOff, p) || p == 0u)
        return false;
    return readGuestU32(ram, ramSize, p, 0u, state);
}

// TK44: race-only visibility from the HUD race time [B+0xc] (pure; the
// overlay owns the state). Any change (forward bump, gate restart) proves a
// live race; a freeze older than the grace hides (pause, results, menus,
// pre-race card). Nothing shows until the first advance is seen.
// TK47: a race boundary (backward jump, or an advance from a frozen 0 at
// GO/countdown start) opens the adopt window through `adoptUntil`: while
// `tick <= adoptUntil` the caller adopts the meter words instead of
// edging, so post-GO init writes can't fire a spurious burst.
struct RaceClock
{
    bool init = false;
    bool seen = false;
    uint32_t last = 0u;
    uint64_t lastTick = 0u;
    uint64_t adoptUntil = 0u;
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
        const bool frozen = (tick - st.lastTick) > kRaceClockGraceTicks;
        if (clock < st.last || (st.last == 0u && frozen))
            st.adoptUntil = tick + kGoAdoptTicks;
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

// TK47: uber-post event counter. The game posts the uber speech event
// (a2=0x2133, Arcade_Uber) through the speech-queue post function at
// 0x2B1458, called only from func_2A3DE0 (jal at 0x2a3e68; the 0x2133
// immediate exists nowhere else in the game). func_2A3DE0 itself has
// exactly three callers (jal at 0x29b704/0x29b7b4/0x29b804 in
// sub_0029B430/0029B738/0029B7E0: the uber-level transition and the
// trick-bank handler), so every uber-post source converges on this one
// call (ee-at/ee-xref citation in local/research/TK47/REPORT.md). The
// runtime wraps 0x2B1458 (ps2_runtime.cpp, TK47; armed only when
// PS2X_SSX3_TRICKY_HUD=1) and records the post when the event is 0x2133
// and the post is accepted (v0 != 0, the game's own success test at
// 0x2a3e70); Stock courses return early without recording. This replaces
// the TK43c/TK44 CD-LBN tap (deleted: the game serves 0x61413/14/16 at a
// top, never the table's 0x61417/7d/da, TK44 section 7; the LBN proxy was
// the wrong contract anyway, TKA1 F9). One count per post = one letter
// per uber (Brad: "one per any uber, like SSX 3").
constexpr uint32_t kUberSpeechEvent = 0x2133u; // Arcade_Uber (TK43 section 2.2)
constexpr uint32_t kSpeechPostFunc = 0x2b1458u;

inline bool isUberPost(uint32_t a2, uint32_t v0)
{
    return a2 == kUberSpeechEvent && v0 != 0u;
}

// TKL1: the post counter moved to the layer's tick-tagged PostQueue
// (ps2_ssx3_tricky_layer.h); the EE wrapper records there and the VBlank
// reducer drains it. This header keeps only the filter + constants.

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

// THD1: the BGRA-lane blend for the iOS IOSurface path
// (kCVPixelFormatType_32BGRA stores B,G,R,A). Same math as blendSample,
// swapped lanes; the sprite samples stay RGBA floats either way.
inline void blendSampleSwapped(float sr, float sg, float sb, float sa, uint8_t *d)
{
    if (sa <= 0.0f)
        return;
    const float ia = 1.0f - sa;
    d[2] = static_cast<uint8_t>(sr * sa + d[2] * ia + 0.5f);
    d[1] = static_cast<uint8_t>(sg * sa + d[1] * ia + 0.5f);
    d[0] = static_cast<uint8_t>(sb * sa + d[0] * ia + 0.5f);
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

// TR3: the SSX 3 SUPER UBER coil, stem, ball and S draw under this overlay
// (Odin 1080p export, Brad 10-06: coil x559-603, ball y110-130, S y399-415;
// the full-state gold glow reaches x~557). Tricky's rings (x563-612, 4.5 px
// gaps) never hid them, so the red uber coil and the glow showed through.
// Covered by the label cover's feathered smear before any Tricky draw.
inline Rect coilCoverRect(const Layout &L)
{
    const int x0 = L.X(553);
    const int y0 = L.Y(104);
    return {x0, y0, L.X(608) - x0, L.Y(428) - y0};
}

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
    // TR3: coil cover, first, so every Tricky part lands on it.
    const Rect cc = coilCoverRect(L);
    smearCover(dst, bw, bh, cc.x - ox, cc.y - oy, cc.x + cc.w - ox, cc.y + cc.h - oy);
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
    Rect coilCover;
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
    ss.coilCover = coilCoverRect(L);
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
    const Rect &cc = ss.coilCover;
    smearCover(tmp, bw, bh, cc.x - ox, cc.y - oy, cc.x + cc.w - ox, cc.y + cc.h - oy);
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
template <bool SwapRB = false>
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
            uint8_t *d =
                base + static_cast<size_t>(y) * strideBytes + static_cast<size_t>(x) * 4u;
            if constexpr (SwapRB)
                blendSampleSwapped(o[0], o[1], o[2], o[3], d);
            else
                blendSample(o[0], o[1], o[2], o[3], d);
        }
}

template <bool SwapRB = false>
inline void smearCoverS(uint8_t *base, size_t strideBytes, int bw, int bh, int x0, int y0, int x1, int y1)
{
    if (!base || bw <= 0 || bh <= 0 || x1 <= x0 || y1 <= y0)
        return;
    const int xa = x0 < 0 ? 0 : x0;
    const int xb = x1 > bw ? bw : x1;
    const int ya = y0 < 0 ? 0 : y0;
    const int yb = y1 > bh ? bh : y1;
    const float span = static_cast<float>(x1 - x0);
    // THD1: lane indices for the smear's background reads/writes (BGRA swaps
    // the R/B lanes; G/A identical). SwapRB=false is the RGBA path, unchanged.
    constexpr int kR = SwapRB ? 2 : 0;
    constexpr int kB = SwapRB ? 0 : 2;
    for (int y = ya; y < yb; ++y)
    {
        const int lx = x0 - 2 < 0 ? 0 : x0 - 2;
        const int rx = x1 + 2 > bw - 1 ? bw - 1 : x1 + 2;
        const uint8_t *L = base + static_cast<size_t>(y) * strideBytes + static_cast<size_t>(lx) * 4u;
        const uint8_t *R = base + static_cast<size_t>(y) * strideBytes + static_cast<size_t>(rx) * 4u;
        const float lr = L[kR], lg = L[1], lb = L[kB];
        const float rr = R[kR], rg = R[1], rb = R[kB];
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
            d[kR] = static_cast<uint8_t>((lr + (rr - lr) * t) * a + d[kR] * ia + 0.5f);
            d[1] = static_cast<uint8_t>((lg + (rg - lg) * t) * a + d[1] * ia + 0.5f);
            d[kB] = static_cast<uint8_t>((lb + (rb - lb) * t) * a + d[kB] * ia + 0.5f);
        }
    }
}

// The cached compose, stride-aware: same draws, same order, same values as
// stampHudInto. ahbBase is the buffer base; r is the HUD region in it.
// THD1: SwapRB=true stamps BGRA lanes (the iOS IOSurface); false (default)
// is the RGBA path, unchanged.
template <bool SwapRB = false>
inline void stampHudDirect(uint8_t *ahbBase, size_t strideBytes, const Rect &r, const HudSprites &ss,
                           float fill, bool full, uint64_t tick, uint64_t splashUntilTick, int litLetters,
                           uint64_t flashUntilTick)
{
    if (!ahbBase || !ss.ok || r.w <= 0 || r.h <= 0)
        return;
    uint8_t *base = ahbBase + static_cast<size_t>(r.y) * strideBytes + static_cast<size_t>(r.x) * 4u;
    const int bw = r.w, bh = r.h, ox = r.x, oy = r.y;
    const Rect &cc = ss.coilCover;
    smearCoverS<SwapRB>(base, strideBytes, bw, bh, cc.x - ox, cc.y - oy, cc.x + cc.w - ox,
                        cc.y + cc.h - oy);
    stampSpriteS<SwapRB>(ss.pole, base, strideBytes, bw, bh, ss.poleDst.x - ox, ss.poleDst.y - oy);
    const int lit = litCoils(fill);
    for (int i = 0; i < kCoils; ++i)
    {
        const SpriteImg &img = i < lit ? ss.ringImg[i / 4] : ss.ringImg[4];
        stampSpriteS<SwapRB>(img, base, strideBytes, bw, bh, ss.ringDst[i].x - ox, ss.ringDst[i].y - oy);
    }
    const bool dim = full && (((tick >> 3) & 1u) != 0u);
    stampSpriteS<SwapRB>(full ? (dim ? ss.jewelImg[2] : ss.jewelImg[1]) : ss.jewelImg[0], base,
                         strideBytes, bw, bh, ss.jewelDst.x - ox, ss.jewelDst.y - oy);
    smearCoverS<SwapRB>(base, strideBytes, bw, bh, ss.smearX0 - ox, ss.smearY0 - oy, ss.smearX1 - ox,
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
        stampSpriteS<SwapRB>(red ? ss.archRed[i] : ss.archChrome[i], base, strideBytes, bw, bh,
                             ss.archDst[i].x - ox, ss.archDst[i].y - oy);
    }
    stampSpriteS<SwapRB>(full ? ss.pillImg[0] : ss.pillImg[1], base, strideBytes, bw, bh,
                         ss.pillDst.x - ox, ss.pillDst.y - oy);
    if (tick < splashUntilTick)
        stampSpriteS<SwapRB>(ss.splash, base, strideBytes, bw, bh, ss.splashDst.x - ox,
                             ss.splashDst.y - oy);
}

// TKL1: the renderer-owned state (HudState), the per-frame decision
// (HudParams) and the live-RDRAM publication (publishRdram/liveRdram) are
// gone. The EE-owned layer (ps2_ssx3_tricky_layer.h) publishes an immutable
// per-VBlank packet; the GL and AHB compositors consume it and never touch
// RDRAM. This header keeps the pure pieces: readers, reducers, the atlas
// and the pixel composer (the reference pixel math, unchanged).
//
// HUD2: fused cached layer (PS2X_TRICKY_HUD_CACHE=1, default off). PRF1: the
// GsHud composite burns 3.1-3.3 ms per update on Tricky courses (smearCoverS
// 43 %, stampSpriteS 20 %), pure heat on ~27 passes over the AHB. The HUD's
// visual state changes far less often than every frame, but the frame under
// it changes every frame, and the smear reads the LIVE frame (its edge
// samples), so a frozen layer cannot be byte-identical. The layer therefore
// caches everything frame-independent per region pixel -- the sprite stack
// fused into one raw sample (+ an op run where sprites overlap), the
// smear's geometric feather/lerp -- and the per-frame pass resolves the
// smear from the live frame and blends once, in a SINGLE pass that reads
// each frame pixel once and writes it once. The per-pixel statements are
// the same expressions (same order, same rounding points) as smearCoverS /
// blendSample, so the cached stamp is byte-identical to stampHudDirect for
// the same packet and frame (locked by the HudCache suite tests on both
// channel orders). Rebuild only when the visual key changes; any anomaly
// (overlapping smears, bad dims) refuses the build and the call site falls
// back to the direct stamp, so the HUD never drops.

// Per-pixel kind bits (HudLayer::kind).
constexpr uint8_t kLayerEmpty = 0u;
constexpr uint8_t kLayerSmear = 1u;  // smear geometry valid (smA, smT)
constexpr uint8_t kLayerSprite = 2u; // sprite sample valid (sr, sg, sb, sa)
constexpr uint8_t kLayerMulti = 4u;  // sprite stack: ops run at multi (implies kLayerSprite)

// Everything the stamp reads, reduced to what the pixels show. tick enters
// only through the pulse phases and the splash/flash windows, so the key
// changes far less often than every frame.
struct HudVisualKey
{
    const Atlas *atlas = nullptr; // art identity (a new atlas rebuilds)
    int fw = 0;
    int fh = 0;
    int lit = 0;      // litCoils(fill)
    bool full = false;
    bool dim = false; // full && ((tick >> 3) & 1)
    bool splash = false;
    int letters = 0; // clamped 0..6
    bool flashing = false;
    bool flashRed = false;
    bool operator==(const HudVisualKey &o) const
    {
        return atlas == o.atlas && fw == o.fw && fh == o.fh && lit == o.lit && full == o.full &&
               dim == o.dim && splash == o.splash && letters == o.letters &&
               flashing == o.flashing && flashRed == o.flashRed;
    }
    bool operator!=(const HudVisualKey &o) const { return !(*this == o); }
};

// The same derivations stampHudDirect applies (same expressions, copied).
inline HudVisualKey visualKeyFor(const Atlas *atlas, int fw, int fh, float fill, bool full,
                                uint64_t tick, uint64_t splashUntilTick, int litLetters,
                                uint64_t flashUntilTick)
{
    HudVisualKey k;
    k.atlas = atlas;
    k.fw = fw;
    k.fh = fh;
    k.lit = litCoils(fill);
    k.full = full;
    k.dim = full && (((tick >> 3) & 1u) != 0u);
    k.splash = tick < splashUntilTick;
    if (litLetters < 0)
        litLetters = 0;
    if (litLetters > 6)
        litLetters = 6;
    k.letters = litLetters;
    k.flashing = tick < flashUntilTick;
    k.flashRed = k.flashing && (((tick >> 3) & 1u) == 0u);
    return k;
}

struct HudLayerPx
{
    float sr = 0.0f, sg = 0.0f, sb = 0.0f, sa = 0.0f; // first/only sprite sample (raw floats)
    float smA = 0.0f, smT = 0.0f;                     // smear feather + edge-lerp position
    uint32_t multi = 0u;                              // multi-op run start in HudLayer::ops
};

struct HudLayerRow
{
    bool smear = false;
    int lx = 0, rx = 0; // edge columns (region coords), read before the row's writes
};

struct HudLayer
{
    bool ok = false;
    int w = 0;
    int h = 0;
    std::vector<uint8_t> kind;   // w*h kind bits
    std::vector<HudLayerPx> px;  // w*h pixel data
    std::vector<float> ops;      // multi runs: [count, sr,sg,sb,sa x count]
    std::vector<HudLayerRow> rows; // h row smears
};

struct HudCache
{
    HudSprites sprites; // pre-scaled art (built once per run, as today)
    HudLayer layer;     // fused layer (rebuilt on visual-key change)
    HudVisualKey key;
    bool has = false;
    uint64_t rebuilds = 0u;
};

namespace hud2detail
{
struct Draw
{
    const SpriteImg *img = nullptr;
    int dx = 0;
    int dy = 0;
};

// The draw list stampHudDirect stamps, in order, with identical variant
// selection and dest rects. Mirrored (not shared) so the default path is
// untouched; the HudCache suite tests fail loudly on any drift.
inline void hudDrawList(const HudSprites &ss, const HudVisualKey &key, Draw *out, int &n)
{
    n = 0;
    out[n++] = {&ss.pole, ss.poleDst.x, ss.poleDst.y};
    for (int i = 0; i < kCoils; ++i)
        out[n++] = {key.lit > i ? &ss.ringImg[i / 4] : &ss.ringImg[4], ss.ringDst[i].x,
                    ss.ringDst[i].y};
    out[n++] = {key.full ? (key.dim ? &ss.jewelImg[2] : &ss.jewelImg[1]) : &ss.jewelImg[0],
                ss.jewelDst.x, ss.jewelDst.y};
    for (int i = 0; i < 6; ++i)
    {
        const bool red = key.flashing ? key.flashRed : (i < key.letters);
        out[n++] = {red ? &ss.archRed[i] : &ss.archChrome[i], ss.archDst[i].x, ss.archDst[i].y};
    }
    out[n++] = {key.full ? &ss.pillImg[0] : &ss.pillImg[1], ss.pillDst.x, ss.pillDst.y};
    if (key.splash)
        out[n++] = {&ss.splash, ss.splashDst.x, ss.splashDst.y};
}

// One smear's geometry rasterized with smearCoverS's exact clip and feather
// (same statements, copied). Refuses when this smear shares a row or pixel
// with an earlier one (the layer holds one smear per row/pixel); the two
// production smears are y-disjoint, so this never fires there.
inline bool rasterSmear(HudLayer &L, int x0, int y0, int x1, int y1)
{
    const int bw = L.w, bh = L.h;
    if (x1 <= x0 || y1 <= y0)
        return true;
    const int xa = x0 < 0 ? 0 : x0;
    const int xb = x1 > bw ? bw : x1;
    const int ya = y0 < 0 ? 0 : y0;
    const int yb = y1 > bh ? bh : y1;
    const float span = static_cast<float>(x1 - x0);
    for (int y = ya; y < yb; ++y)
    {
        const int lx = x0 - 2 < 0 ? 0 : x0 - 2;
        const int rx = x1 + 2 > bw - 1 ? bw - 1 : x1 + 2;
        HudLayerRow &row = L.rows[static_cast<size_t>(y)];
        if (row.smear)
            return false;
        row.smear = true;
        row.lx = lx;
        row.rx = rx;
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
            const size_t i = static_cast<size_t>(y) * static_cast<size_t>(bw) + static_cast<size_t>(x);
            if (L.kind[i] & kLayerSmear)
                return false;
            L.kind[i] |= kLayerSmear;
            L.px[i].smA = a;
            L.px[i].smT = t;
        }
    }
    return true;
}
} // namespace hud2detail

// Fuse the sprite stack + smear geometry for one visual key. False on any
// anomaly (the call site falls back to the direct stamp).
inline bool buildHudLayer(HudLayer &L, const HudSprites &ss, const HudVisualKey &key)
{
    L = HudLayer{};
    if (!ss.ok || ss.region.w <= 0 || ss.region.h <= 0)
        return false;
    const int bw = ss.region.w, bh = ss.region.h;
    const int ox = ss.region.x, oy = ss.region.y;
    const size_t npx = static_cast<size_t>(bw) * static_cast<size_t>(bh);
    if (npx == 0u || npx > static_cast<size_t>(16) * 1024u * 1024u)
        return false;
    L.w = bw;
    L.h = bh;
    L.kind.assign(npx, kLayerEmpty);
    L.px.resize(npx);
    L.rows.resize(static_cast<size_t>(bh));
    const Rect &cc = ss.coilCover;
    if (!hud2detail::rasterSmear(L, cc.x - ox, cc.y - oy, cc.x + cc.w - ox, cc.y + cc.h - oy))
        return false;
    if (!hud2detail::rasterSmear(L, ss.smearX0 - ox, ss.smearY0 - oy, ss.smearX1 - ox,
                                 ss.smearY1 - oy))
        return false;
    hud2detail::Draw draws[1 + kCoils + 1 + 6 + 1 + 1];
    int ndraws = 0;
    hud2detail::hudDrawList(ss, key, draws, ndraws);
    // Pass 1: per-pixel covering-draw counts (draws <= 26, no saturation).
    std::vector<uint8_t> cnt(npx, 0);
    for (int d = 0; d < ndraws; ++d)
    {
        const SpriteImg &img = *draws[d].img;
        if (img.w <= 0 || img.h <= 0 || img.px.empty())
            continue;
        const int dx = draws[d].dx - ox, dy = draws[d].dy - oy;
        const int x0 = dx < 0 ? 0 : dx;
        const int y0 = dy < 0 ? 0 : dy;
        const int x1 = dx + img.w > bw ? bw : dx + img.w;
        const int y1 = dy + img.h > bh ? bh : dy + img.h;
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x)
            {
                const float *o =
                    &img.px[(static_cast<size_t>(y - dy) * static_cast<size_t>(img.w) +
                             static_cast<size_t>(x - dx)) *
                            4u];
                if (o[3] <= 0.0f)
                    continue;
                const size_t i =
                    static_cast<size_t>(y) * static_cast<size_t>(bw) + static_cast<size_t>(x);
                if (cnt[i] < 255u)
                    ++cnt[i];
            }
    }
    // Multi-op pool layout: one run per multi pixel (count + ops).
    std::vector<uint32_t> run(npx, 0u);
    size_t poolFloats = 0u;
    for (size_t i = 0; i < npx; ++i)
    {
        if (cnt[i] >= 2u)
        {
            run[i] = static_cast<uint32_t>(poolFloats);
            poolFloats += 1u + 4u * static_cast<size_t>(cnt[i]);
        }
    }
    if (poolFloats > static_cast<size_t>(64) * 1024u * 1024u)
        return false;
    L.ops.assign(poolFloats, 0.0f);
    std::vector<uint32_t> cursor = run;
    std::vector<uint8_t> seen(npx, 0);
    // Pass 2: fill singles + runs, in draw order.
    for (int d = 0; d < ndraws; ++d)
    {
        const SpriteImg &img = *draws[d].img;
        if (img.w <= 0 || img.h <= 0 || img.px.empty())
            continue;
        const int dx = draws[d].dx - ox, dy = draws[d].dy - oy;
        const int x0 = dx < 0 ? 0 : dx;
        const int y0 = dy < 0 ? 0 : dy;
        const int x1 = dx + img.w > bw ? bw : dx + img.w;
        const int y1 = dy + img.h > bh ? bh : dy + img.h;
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x)
            {
                const float *o =
                    &img.px[(static_cast<size_t>(y - dy) * static_cast<size_t>(img.w) +
                             static_cast<size_t>(x - dx)) *
                            4u];
                if (o[3] <= 0.0f)
                    continue;
                const size_t i =
                    static_cast<size_t>(y) * static_cast<size_t>(bw) + static_cast<size_t>(x);
                const uint8_t c = cnt[i];
                if (c < 2u)
                {
                    L.kind[i] |= kLayerSprite;
                    HudLayerPx &p = L.px[i];
                    p.sr = o[0];
                    p.sg = o[1];
                    p.sb = o[2];
                    p.sa = o[3];
                    continue;
                }
                if (seen[i] == 0u)
                {
                    L.kind[i] |= static_cast<uint8_t>(kLayerSprite | kLayerMulti);
                    L.px[i].multi = run[i];
                    L.ops[cursor[i]++] = static_cast<float>(c);
                }
                seen[i] = 1u;
                float *w = &L.ops[cursor[i]];
                w[0] = o[0];
                w[1] = o[1];
                w[2] = o[2];
                w[3] = o[3];
                cursor[i] += 4u;
            }
    }
    L.ok = true;
    return true;
}

// Ensure sprites + layer for one composite; rebuilds the layer only when the
// visual key changed. False (sprites or layer refused) means fall back to
// the direct stamp.
inline bool ensureHudLayer(HudCache &c, const Atlas &a, int fw, int fh, float fill, bool full,
                           uint64_t tick, uint64_t splashUntilTick, int litLetters,
                           uint64_t flashUntilTick)
{
    if (!a.ok || fw <= 0 || fh <= 0)
        return false;
    if (!c.sprites.ok || c.sprites.atlas != &a || c.sprites.fw != fw || c.sprites.fh != fh)
    {
        if (!buildHudSprites(c.sprites, a, fw, fh))
        {
            c.has = false;
            return false;
        }
    }
    const HudVisualKey key = visualKeyFor(&a, fw, fh, fill, full, tick, splashUntilTick,
                                          litLetters, flashUntilTick);
    if (c.has && c.layer.ok && c.key == key)
        return true;
    HudLayer L;
    if (!buildHudLayer(L, c.sprites, key))
    {
        c.has = false;
        return false;
    }
    c.layer = std::move(L);
    c.key = key;
    c.has = true;
    ++c.rebuilds;
    return true;
}

// The cached composite, stride-aware: one pass over the region. base points
// at the region origin; rows advance by strideBytes. Each row's smear edge
// samples are read before any write to that row, so they see the pristine
// frame exactly as the direct stamp's per-smear reads do (the direct stamp
// reads both smears' edges before any draw touches those columns; the two
// smears never share a row, refused at build). Per-pixel statements match
// smearCoverS / blendSample (same order, same rounding points), and sprite
// stacks replay through the real blend functions in draw order.
template <bool SwapRB = false>
inline void stampHudCachedS(uint8_t *base, size_t strideBytes, const HudLayer &L)
{
    if (!base || !L.ok || L.w <= 0 || L.h <= 0)
        return;
    constexpr int kR = SwapRB ? 2 : 0;
    constexpr int kB = SwapRB ? 0 : 2;
    const int bw = L.w, bh = L.h;
    for (int y = 0; y < bh; ++y)
    {
        uint8_t *rowBase = base + static_cast<size_t>(y) * strideBytes;
        const HudLayerRow &rs = L.rows[static_cast<size_t>(y)];
        float lr = 0.0f, lg = 0.0f, lb = 0.0f, rr = 0.0f, rg = 0.0f, rb = 0.0f;
        if (rs.smear)
        {
            const uint8_t *Lp = rowBase + static_cast<size_t>(rs.lx) * 4u;
            const uint8_t *Rp = rowBase + static_cast<size_t>(rs.rx) * 4u;
            lr = Lp[kR];
            lg = Lp[1];
            lb = Lp[kB];
            rr = Rp[kR];
            rg = Rp[1];
            rb = Rp[kB];
        }
        for (int x = 0; x < bw; ++x)
        {
            const size_t i = static_cast<size_t>(y) * static_cast<size_t>(bw) + static_cast<size_t>(x);
            const uint8_t k = L.kind[i];
            if (k == kLayerEmpty)
                continue;
            const HudLayerPx &p = L.px[i];
            uint8_t *d = rowBase + static_cast<size_t>(x) * 4u;
            if ((k & kLayerSmear) == 0u)
            {
                if ((k & kLayerMulti) == 0u)
                {
                    if constexpr (SwapRB)
                        blendSampleSwapped(p.sr, p.sg, p.sb, p.sa, d);
                    else
                        blendSample(p.sr, p.sg, p.sb, p.sa, d);
                    continue;
                }
                uint8_t tmp[4] = {d[0], d[1], d[2], d[3]};
                const float *run = &L.ops[p.multi];
                const int n = static_cast<int>(run[0]);
                for (int j = 0; j < n; ++j)
                {
                    const float *o = run + 1u + static_cast<size_t>(j) * 4u;
                    if constexpr (SwapRB)
                        blendSampleSwapped(o[0], o[1], o[2], o[3], tmp);
                    else
                        blendSample(o[0], o[1], o[2], o[3], tmp);
                }
                d[0] = tmp[0];
                d[1] = tmp[1];
                d[2] = tmp[2];
                d[3] = tmp[3];
                continue;
            }
            const float a = p.smA;
            const float t = p.smT;
            const float ia = 1.0f - a;
            const uint8_t mR =
                static_cast<uint8_t>((lr + (rr - lr) * t) * a + d[kR] * ia + 0.5f);
            const uint8_t mG =
                static_cast<uint8_t>((lg + (rg - lg) * t) * a + d[1] * ia + 0.5f);
            const uint8_t mB =
                static_cast<uint8_t>((lb + (rb - lb) * t) * a + d[kB] * ia + 0.5f);
            if ((k & kLayerSprite) == 0u)
            {
                d[kR] = mR;
                d[1] = mG;
                d[kB] = mB;
                continue;
            }
            uint8_t tmp[4] = {0, 0, 0, 255};
            tmp[kR] = mR;
            tmp[1] = mG;
            tmp[kB] = mB;
            if ((k & kLayerMulti) == 0u)
            {
                if constexpr (SwapRB)
                    blendSampleSwapped(p.sr, p.sg, p.sb, p.sa, tmp);
                else
                    blendSample(p.sr, p.sg, p.sb, p.sa, tmp);
            }
            else
            {
                const float *run = &L.ops[p.multi];
                const int n = static_cast<int>(run[0]);
                for (int j = 0; j < n; ++j)
                {
                    const float *o = run + 1u + static_cast<size_t>(j) * 4u;
                    if constexpr (SwapRB)
                        blendSampleSwapped(o[0], o[1], o[2], o[3], tmp);
                    else
                        blendSample(o[0], o[1], o[2], o[3], tmp);
                }
            }
            d[kR] = tmp[kR];
            d[1] = tmp[1];
            d[kB] = tmp[kB];
            d[3] = tmp[3];
        }
    }
}

// stampHudDirect's calling convention (buffer base + region) over a layer.
template <bool SwapRB = false>
inline void stampHudCached(uint8_t *bufBase, size_t strideBytes, const Rect &r, const HudLayer &L)
{
    if (!bufBase || !L.ok || r.w <= 0 || r.h <= 0 || L.w != r.w || L.h != r.h)
        return;
    stampHudCachedS<SwapRB>(bufBase + static_cast<size_t>(r.y) * strideBytes +
                                static_cast<size_t>(r.x) * 4u,
                            strideBytes, L);
}

// The GL call site's calling convention (whole frame + layer): region temp
// round-trip, as composeOverlay.
inline void composeOverlayCached(uint8_t *frame, int fw, int fh, const HudLayer &L)
{
    if (!frame || !L.ok || fw <= 0 || fh <= 0)
        return;
    const Rect r = hudRegionRect(fw, fh);
    if (r.w <= 0 || r.h <= 0 || L.w != r.w || L.h != r.h)
        return;
    std::vector<uint8_t> tmp(static_cast<size_t>(r.w) * static_cast<size_t>(r.h) * 4u);
    const size_t rowBytes = static_cast<size_t>(r.w) * 4u;
    for (int y = 0; y < r.h; ++y)
        std::memcpy(&tmp[static_cast<size_t>(y) * rowBytes],
                    &frame[(static_cast<size_t>(r.y + y) * static_cast<size_t>(fw) + static_cast<size_t>(r.x)) * 4u],
                    rowBytes);
    stampHudCachedS<false>(tmp.data(), rowBytes, L);
    for (int y = 0; y < r.h; ++y)
        std::memcpy(&frame[(static_cast<size_t>(r.y + y) * static_cast<size_t>(fw) + static_cast<size_t>(r.x)) * 4u],
                    &tmp[static_cast<size_t>(y) * rowBytes], rowBytes);
}

} // namespace ps2_ssx3_tricky_hud
