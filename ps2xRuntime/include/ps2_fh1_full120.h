// FH1 full-120 mode (local branch full120; PS2X_SSX3_FULL120=1, default off).
//
// The whole SSX 3 update runs 120 times per guest second with dt 1/120
// (RV13 option A) and every update renders, so the output is native 120 fps:
//   - guest VBlank runs at 2 x 59.94 Hz (EeScheduler period / 2, pacer / 2):
//     each VBlank is one wake -> one producer record -> one app update -> one
//     render -> one present, i.e. two updates and two presents per stock
//     VBlank period. The render gate, flip and present stay the game's own.
//   - manager A=[0x4a5b64]: rate A+0x10 60 -> 120 and dt A+0x14 1/60 -> 1/120,
//     patched once at the manager-init hook (TM3's site). Multiplier and
//     accumulator stay stock (one record per wake).
//   - EE clock x PS2X_SSX3_FULL120_EE_X (1|2|4, default 2): checkpoints charge
//     1/EE_X cycles, so a 120 Hz frame has a stock frame's EE budget. EE
//     timers, COP0 Count, alarms and the sound tick all run off the charged
//     m_eeCycle, so they keep their stock rate in guest seconds (PCSX2's EE
//     cycle-rate setting does the same).
//   - host-side vsync-time consumers keep stock time: the pad-script vsync
//     clock and the CD field clock count ticks / 2; the [vsync-rate] log
//     reports stock-equivalent vs/s.
// Fix groups (PS2X_SSX3_FULL120_FIX=comma list | all; default none) patch
// single-reader per-step constants at the same hook (TS1's list, RV13 §2).
// Knob off: every entry point is one cached-bool test; no guest effect.
#pragma once

#include "ps2_runtime.h"
#include "runtime/ps2_memory.h"
#include "runtime/gs/gs_frontend.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace ps2_fh1
{
inline bool enabled() noexcept
{
    static const bool on = [] {
        const char *v = std::getenv("PS2X_SSX3_FULL120");
        return v && v[0] == '1' && v[1] == '\0';
    }();
    return on;
}

// Guest VBlanks per stock VBlank period.
inline uint32_t vblankDivisor() noexcept
{
    return enabled() ? 2u : 1u;
}

// log2 of the EE clock multiplier (charged cycles = cycles >> shift).
inline uint32_t eeClockShift() noexcept
{
    static const uint32_t shift = [] {
        if (!enabled())
            return 0u;
        const char *v = std::getenv("PS2X_SSX3_FULL120_EE_X");
        if (!v || !*v)
            return 1u;
        if (std::strcmp(v, "1") == 0)
            return 0u;
        if (std::strcmp(v, "2") == 0)
            return 1u;
        if (std::strcmp(v, "4") == 0)
            return 2u;
        std::fprintf(stderr, "fh1-full120-refused PS2X_SSX3_FULL120_EE_X=%s (want 1|2|4)\n", v);
        std::abort();
    }();
    return shift;
}

enum Fix : uint32_t
{
    kFixRider = 1u << 0,     // K1-K4: rider integrators' private dt (class h)
    kFixCountdown = 1u << 1, // C1-C4 + solver quantum/epsilon/residual/gap (class c)
    kFixDrag = 1u << 2,      // 1139a0 per-call drag (a) and z increments (b)
    kFixEvent = 1u << 3,     // 10496c event-node step (e/h)
    kFixSlew = 1u << 4,      // 114124 mode-0 return slew (a/j)
    kFixClock = 1u << 5,     // system 1: wake ordinal in the stock domain + update-count periods
    kFixRaceClock = 1u << 6, // system 12: race tick [race+8] counts stock ticks
    kFixSession = 1u << 7,   // system 12/8: race-session machine 0x26f4a8 at stock cadence (class d)
    kFixTimers = 1u << 8,    // 1/60 float clocks live in race (FH2 scanf): HUD 0x49d998, ramp 0x49b59c, timers 0x49b15c
    kFixCamera = 1u << 9,    // system 7: CM2 §4 pool words (sqrt retentions, 1/120 clocks, halved steps)
};

inline uint32_t fixMask() noexcept
{
    static const uint32_t mask = [] {
        const char *v = std::getenv("PS2X_SSX3_FULL120_FIX");
        if (!enabled() || !v || !*v)
            return 0u;
        const std::string s(v);
        if (s == "all")
            return kFixRider | kFixCountdown | kFixDrag | kFixEvent | kFixSlew | kFixClock | kFixRaceClock | kFixSession |
                   kFixTimers | kFixCamera;
        uint32_t m = 0u;
        size_t at = 0u;
        while (at <= s.size())
        {
            const size_t comma = s.find(',', at);
            const std::string item = s.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
            if (item == "rider") m |= kFixRider;
            else if (item == "countdown") m |= kFixCountdown;
            else if (item == "drag") m |= kFixDrag;
            else if (item == "event") m |= kFixEvent;
            else if (item == "slew") m |= kFixSlew;
            else if (item == "clock") m |= kFixClock;
            else if (item == "raceclock") m |= kFixRaceClock;
            else if (item == "session") m |= kFixSession;
            else if (item == "timers") m |= kFixTimers;
            else if (item == "camera") m |= kFixCamera;
            else if (!item.empty())
            {
                std::fprintf(stderr, "fh1-full120-refused PS2X_SSX3_FULL120_FIX item=%s\n", item.c_str());
                std::abort();
            }
            if (comma == std::string::npos)
                break;
            at = comma + 1u;
        }
        return m;
    }();
    return mask;
}

inline constexpr uint32_t kMgrPtr = 0x4a5b64u;   // gp+0x2a74 -> A
inline constexpr uint32_t kHookSite = 0x316da8u; // jalr after 0x316d88/0x316d98 store dt/rate
inline constexpr uint32_t kUpdateTarget = 0x2306b8u;
inline constexpr uint32_t kRenderTarget = 0x22b008u;
inline constexpr uint32_t kSixtieth = 0x3c888889u;   // float(1/60)
inline constexpr uint32_t kHundredTwentieth = 0x3c088889u; // float(1/120)

inline bool rd32(const uint8_t *ram, uint32_t addr, uint32_t &out) noexcept
{
    const uint32_t phys = addr & 0x1FFFFFFFu;
    if (!ram || phys + 4u > PS2_RAM_SIZE)
        return false;
    std::memcpy(&out, ram + phys, 4);
    return true;
}

inline bool wr32(uint8_t *ram, uint32_t addr, uint32_t val) noexcept
{
    const uint32_t phys = addr & 0x1FFFFFFFu;
    if (!ram || phys + 4u > PS2_RAM_SIZE)
        return false;
    std::memcpy(ram + phys, &val, 4);
    return true;
}

struct Word
{
    uint32_t fix; // 0 = always (manager)
    uint32_t address;
    uint32_t expected;
    uint32_t replacement;
    const char *label;
};

// Manager rate/dt plus TS1's single-coherent word table (TS1 REPORT G0,
// ps2_ts1_mode.h @ ts1 420c34a), minus TS1's multiplier: here the doubled
// VBlank supplies the second update. Every word is verified before any write.
inline void patchAtManagerInit(uint8_t *ram)
{
    const char *sim = std::getenv("PS2X_SSX3_SIM_MODE");
    if (sim && std::strncmp(sim, "split", 5) == 0)
    {
        std::fprintf(stderr, "fh1-full120-refused PS2X_SSX3_SIM_MODE=%s (full120 replaces split120)\n", sim);
        std::abort();
    }
    uint32_t a = 0u;
    if (!rd32(ram, kMgrPtr, a) || !a || (a & 3u) || (a & 0x1FFFFFFFu) + 0x60u > PS2_RAM_SIZE)
    {
        std::fprintf(stderr, "fh1-full120-refused manager ptr=%08x\n", a);
        std::abort();
    }
    const std::array<Word, 61> words = {{
        {0u, a + 0x10u, 60u, 120u, "rate"},
        {0u, a + 0x14u, kSixtieth, kHundredTwentieth, "dt"},
        {0u, a + 0x24u, 0x3f800000u, 0x3f800000u, "mult(stock)"},
        {kFixRider, 0x49be9cu, kSixtieth, kHundredTwentieth, "K1"},
        {kFixRider, 0x49c08cu, kSixtieth, kHundredTwentieth, "K2"},
        {kFixRider, 0x49c12cu, kSixtieth, kHundredTwentieth, "K3"},
        {kFixRider, 0x49b828u, kSixtieth, kHundredTwentieth, "K4"},
        {kFixCountdown, 0x49b480u, kSixtieth, kHundredTwentieth, "C1"},
        {kFixCountdown, 0x49b48cu, kSixtieth, kHundredTwentieth, "C2"},
        {kFixCountdown, 0x49b4a0u, kSixtieth, kHundredTwentieth, "C3"},
        {kFixCountdown, 0x49bf1cu, kSixtieth, kHundredTwentieth, "C4"},
        {kFixCountdown, 0x49b494u, kSixtieth, kHundredTwentieth, "quantum"},
        {kFixCountdown, 0x49b498u, 0x3c23d70au, 0x3ba3d70au, "epsilon"},
        {kFixCountdown, 0x49b49cu, 0x426fffffu, 0x42efffffu, "residual_recip"},
        {kFixCountdown, 0x49bf20u, kSixtieth, kHundredTwentieth, "gap_floor"},
        {kFixCountdown, 0x49bf24u, 0x426fffffu, 0x42efffffu, "gap_recip"},
        {kFixDrag, 0x49b4a4u, 0xbb5a740fu, 0xbadaa2bdu, "damping"},
        {kFixDrag, 0x49b4a8u, 0xc1fd5556u, 0xc17d5556u, "z_up"},
        {kFixDrag, 0x49b4acu, 0xc162aaabu, 0xc0e2aaabu, "z_down"},
        {kFixEvent, 0x49b1b0u, kSixtieth, kHundredTwentieth, "event_step_10496c"},
        // 0x114124 applies this fraction of the remaining R+0x214 gap once per
        // update: alpha_half = 1-sqrt(1-alpha_stock).
        {kFixSlew, 0x49b4f0u, 0x3d2aa635u, 0x3cac76f5u, "mode0_slew_114124"},
        // FH2 live 1/60 clocks (PS2X_FH1_SCANF, stock t1900..2000; writers by DIAG_WATCH):
        // HUD object +0x190 += [gp-0x5758] at 0x1eb034/44 (0x1e9a30 race HUD),
        {kFixTimers, 0x49d998u, kSixtieth, kHundredTwentieth, "hud_clock_1eb02c"},
        // rider object +0x35c += [gp-0x7b54] up to 1.0 at 0x115d98-db4,
        {kFixTimers, 0x49b59cu, kSixtieth, kHundredTwentieth, "rider_ramp_115d98"},
        // per-rider timer list [e+4] -= [gp-0x7f94] at 0x101538 (0x1013a8, RV13 row).
        {kFixTimers, 0x49b15cu, kSixtieth, kHundredTwentieth, "rider_timers_101538"},
        // CM2 §4 / convert.txt (GameCamera 0x1580e10 chase chain, pool words
        // reloaded every tick): class a retentions r -> sqrt(r) ...
        {kFixCamera, 0x49cfe4u, 0x3f59999au, 0x3f6c0535u, "cam_C1_kA0"},
        {kFixCamera, 0x49cfe8u, 0x3f7851ecu, 0x3f7c217au, "cam_C1_kA1"},
        {kFixCamera, 0x49cfd4u, 0x3e8aefe0u, 0x3f055b3fu, "cam_C1_kB0"},
        {kFixCamera, 0x49cfd8u, 0x3f666666u, 0x3f72dce8u, "cam_C1_kB1"},
        {kFixCamera, 0x49d044u, 0x3f6c97c5u, 0x3f761aeeu, "cam_height"},
        {kFixCamera, 0x49cff4u, 0x3f6c89d3u, 0x3f7613aeu, "cam_dist_grow"},
        {kFixCamera, 0x49cff8u, 0x3f684c59u, 0x3f73dc80u, "cam_dist_shrink"},
        {kFixCamera, 0x49d010u, 0x3f7a77e2u, 0x3f7d3813u, "cam_height2"},
        {kFixCamera, 0x49d028u, 0x3f733333u, 0x3f798497u, "cam_fov0"},
        {kFixCamera, 0x49c644u, 0x3f7fbe77u, 0x3f7fdf39u, "cam_fov"},
        {kFixCamera, 0x49d02cu, 0x3f75c28fu, 0x3f7ad3e7u, "cam_lookat_kick"},
        {kFixCamera, 0x49cff0u, 0x3f7ae148u, 0x3f7d6d55u, "cam_side_kick5"},
        {kFixCamera, 0x49d070u, 0x3f787fadu, 0x3f7c38b3u, "cam_lateral"},
        {kFixCamera, 0x49c628u, 0x3f749d64u, 0x3f7a3e1fu, "cam_heading_base"},
        {kFixCamera, 0x49c624u, 0x3d21a510u, 0x3ca3b0d2u, "cam_heading_span"}, // sqrt(base+span)-sqrt(base)
        {kFixCamera, 0x49c49cu, 0x3f7851ecu, 0x3f7c217au, "cam_lift_decay"},
        {kFixCamera, 0x49c7d8u, 0x3f4ccccdu, 0x3f64f92eu, "cam_air_r"},
        {kFixCamera, 0x49c7dcu, 0x3e4ccccdu, 0x3dd8368fu, "cam_air_1mr"},
        // ... class b per-tick decrements halved ...
        {kFixCamera, 0x49d00cu, 0x3fe227ceu, 0x3f6227ceu, "cam_dist_impulse_decay"},
        {kFixCamera, 0x49c5fcu, 0x3abb3ee7u, 0x3a3b3ee7u, "cam_C2_ramp"},
        {kFixCamera, 0x49c5dcu, 0x3bda740fu, 0x3b5a740fu, "cam_ctl_blend2"},
        {kFixCamera, 0x49c5e0u, 0x3c899268u, 0x3c099268u, "cam_ctl_blend3"},
        // ... class e/h 1/60 clocks -> 1/120 (cap 4-1/60 -> 4-1/120).
        {kFixCamera, 0x49c64cu, kSixtieth, kHundredTwentieth, "cam_lookat_timer"},
        {kFixCamera, 0x49c654u, kSixtieth, kHundredTwentieth, "cam_jump_init"},
        {kFixCamera, 0x49c7a0u, kSixtieth, kHundredTwentieth, "cam_jump_clk_a"},
        {kFixCamera, 0x49c7a4u, kSixtieth, kHundredTwentieth, "cam_jump_clk_b"},
        {kFixCamera, 0x49c79cu, 0x407eeeefu, 0x407f7777u, "cam_jump_cap"},
        {kFixCamera, 0x49c7d0u, kSixtieth, kHundredTwentieth, "cam_air_clk"},
        {kFixCamera, 0x49c808u, kSixtieth, kHundredTwentieth, "cam_air_clk2"},
        {kFixCamera, 0x49c810u, kSixtieth, kHundredTwentieth, "cam_air_clk3"},
        {kFixCamera, 0x49c814u, kSixtieth, kHundredTwentieth, "cam_air_clk4"},
        {kFixCamera, 0x49c7c8u, kSixtieth, kHundredTwentieth, "cam_ramp_a"},
        {kFixCamera, 0x49c7ccu, kSixtieth, kHundredTwentieth, "cam_ramp_b"},
        {kFixCamera, 0x49c5d8u, kSixtieth, kHundredTwentieth, "cam_ctl_blend"},
        {kFixCamera, 0x49c5ccu, kSixtieth, kHundredTwentieth, "cam_mode_timer"},
        {kFixCamera, 0x49c824u, kSixtieth, kHundredTwentieth, "cam_shake_clk"},
        {kFixCamera, 0x49c820u, kSixtieth, kHundredTwentieth, "cam_shake_dur"},
    }};
    const uint32_t mask = fixMask();
    size_t written = 0u;
    for (const Word &w : words)
    {
        if (w.fix != 0u && (mask & w.fix) == 0u)
            continue;
        uint32_t got = 0u;
        if (!rd32(ram, w.address, got) || got != w.expected)
        {
            std::fprintf(stderr, "fh1-full120-refused word=%s addr=%08x got=%08x expected=%08x\n",
                         w.label, w.address, got, w.expected);
            std::abort();
        }
    }
    for (const Word &w : words)
    {
        if (w.fix != 0u && (mask & w.fix) == 0u)
            continue;
        wr32(ram, w.address, w.replacement);
        ++written;
    }
    // Lab words (PS2X_FH1_WORDS=addr:expected:replacement,...; hex): try a
    // pool-word conversion without a rebuild. Same verify-then-write rule.
    if (const char *lw = std::getenv("PS2X_FH1_WORDS"))
    {
        std::vector<Word> lab;
        const char *p = lw;
        while (*p)
        {
            char *end = nullptr;
            Word w{0u, 0u, 0u, 0u, "lab"};
            w.address = static_cast<uint32_t>(std::strtoul(p, &end, 16));
            if (*end != ':') break;
            w.expected = static_cast<uint32_t>(std::strtoul(end + 1, &end, 16));
            if (*end != ':') break;
            w.replacement = static_cast<uint32_t>(std::strtoul(end + 1, &end, 16));
            lab.push_back(w);
            if (*end != ',') break;
            p = end + 1;
        }
        for (const Word &w : lab)
        {
            uint32_t got = 0u;
            if (!rd32(ram, w.address, got) || got != w.expected)
            {
                std::fprintf(stderr, "fh1-full120-refused lab word addr=%08x got=%08x expected=%08x\n",
                             w.address, got, w.expected);
                std::abort();
            }
        }
        for (const Word &w : lab)
            wr32(ram, w.address, w.replacement);
        std::fprintf(stderr, "fh1-full120 lab words=%zu\n", lab.size());
    }
    std::fprintf(stderr, "fh1-full120-armed manager=%08x rate=120 dt=%08x vblank_div=%u ee_x=%u fix=0x%x words=%zu\n",
                 a, kHundredTwentieth, vblankDivisor(), 1u << eeClockShift(), mask, written);
}

// ---- Observation tap (PS2X_FH1_TAP=1; env-only, any build) ----------------
// One line per PS2X_FH1_TAP_EVERY guest VBlanks (default 60): update and
// render dispatch counts, manager ordinals, the rider snapshot position
// 0x5409c0 (TM2) and optional words (PS2X_FH1_PEEK=addr[:f|:u|:x][N], with
// '@ptr+off' for one dereference). PS2X_FH1_COUNT=pc,... adds dispatch
// counts for those targets.
struct Tap
{
    bool on = false;
    uint32_t every = 60u;
    uint32_t updates = 0u;
    uint32_t renders = 0u;
    std::array<uint32_t, 16> countPcs{};
    std::array<uint32_t, 16> counts{};
    std::array<std::array<uint32_t, 4>, 16> args{};
    bool logArgs = false;
    size_t nCount = 0u;
    struct Peek
    {
        bool deref = false;
        uint32_t base = 0u;
        uint32_t off = 0u;
        char fmt = 'x';
        uint32_t n = 1u;
    };
    std::array<Peek, 16> peeks{};
    size_t nPeek = 0u;
};

inline Tap &tap()
{
    static Tap t = [] {
        Tap r;
        const char *on = std::getenv("PS2X_FH1_TAP");
        r.on = on && on[0] == '1';
        if (!r.on)
            return r;
        if (const char *e = std::getenv("PS2X_FH1_TAP_EVERY"))
            r.every = std::max<uint32_t>(1u, static_cast<uint32_t>(std::strtoul(e, nullptr, 0)));
        if (const char *la = std::getenv("PS2X_FH1_COUNT_ARGS"))
            r.logArgs = la[0] == '1';
        if (const char *c = std::getenv("PS2X_FH1_COUNT"))
        {
            const char *p = c;
            while (*p && r.nCount < r.countPcs.size())
            {
                char *end = nullptr;
                r.countPcs[r.nCount++] = static_cast<uint32_t>(std::strtoul(p, &end, 0));
                if (end == p || *end != ',')
                    break;
                p = end + 1;
            }
        }
        if (const char *s = std::getenv("PS2X_FH1_PEEK"))
        {
            const char *p = s;
            while (*p && r.nPeek < r.peeks.size())
            {
                Tap::Peek k;
                if (*p == '@')
                {
                    k.deref = true;
                    ++p;
                }
                char *end = nullptr;
                k.base = static_cast<uint32_t>(std::strtoul(p, &end, 0));
                p = end;
                if (*p == '+')
                {
                    k.off = static_cast<uint32_t>(std::strtoul(p + 1, &end, 0));
                    p = end;
                }
                if (*p == ':')
                {
                    k.fmt = p[1];
                    p += 2;
                    if (*p >= '1' && *p <= '9')
                    {
                        k.n = static_cast<uint32_t>(std::strtoul(p, &end, 10));
                        p = end;
                    }
                }
                r.peeks[r.nPeek++] = k;
                if (*p != ',')
                    break;
                ++p;
            }
        }
        return r;
    }();
    return t;
}

inline bool tapOn() noexcept
{
    return tap().on;
}

// Set once the manager words are converted (EE thread only).
inline bool g_patched = false;

inline bool clockFix() noexcept
{
    static const bool on = enabled() && (fixMask() & kFixClock) != 0u;
    return on;
}

// ---- System 1 (clock domains), PS2X_SSX3_FULL120_FIX=clock ----------------
// Wake ordinal A+0x18: the producer 0x317348 increments it once per wake
// (0x317374-80). Full120 wakes 120/s; RV13 §2 lists nine external readers
// that count it in stock wakes (once-per-60 throttles 0x1a3ff8/0x1aabac, the
// /60 seconds at 0x1aa9dc, mod-N blinks 0x1b2340/0x1e79e0, stamps 0x1fae98,
// 0x2f5490, 0x3a9da4) plus the 0x3a7098 interval countdown. Pre-decrementing
// it on every other producer entry keeps A+0x18 at 60/s (class e: keep the
// ordinal's stock domain), so those readers keep stock periods.
inline constexpr uint32_t kProducer = 0x317348u;
// 0x3a7058 stores +0xdc = rate / (max(a1,0x8000) * 2^-15): an interval in
// wakes that 0x3a7098 decrements by the A+0x18 delta. With A+0x18 in stock
// wakes it needs the stock rate: halve +0xdc after return (exact: 120/x =
// 2*(60/x) in binary float).
inline constexpr uint32_t kInterval = 0x3a7058u;
// 0x1e1458 (called once per update from 0x230e68): if A+0x1c - [gp-0x1034]
// >= 601 (0x1e1494 slti 0x259) it calls 0x1e14c0(1) and restamps with the
// update count (0x1e14a8): a 10 s period in stock updates. Shift each new
// stamp forward by 601 so the period is 1202 updates = 10 s at 120/s.
inline constexpr uint32_t kPeriod10s = 0x1e1458u;
inline constexpr uint32_t kPeriod10sStamp = 0x4a30f0u - 0x1034u; // gp-0x1034
inline uint32_t g_producerCalls = 0u;
inline uint32_t g_lastStamp10s = 0xffffffffu;

inline void clockPreHook(uint8_t *ram, uint32_t targetPc)
{
    if (targetPc == kProducer)
    {
        if ((g_producerCalls++ & 1u) != 0u)
        {
            uint32_t a = 0u, wake = 0u;
            if (rd32(ram, kMgrPtr, a) && a && rd32(ram, a + 0x18u, wake))
                wr32(ram, a + 0x18u, wake - 1u);
        }
    }
    else if (targetPc == kPeriod10s)
    {
        uint32_t stamp = 0u;
        if (rd32(ram, kPeriod10sStamp, stamp) && stamp != g_lastStamp10s)
        {
            stamp += 601u;
            wr32(ram, kPeriod10sStamp, stamp);
            g_lastStamp10s = stamp;
        }
    }
}

// ---- System 12 race clock, PS2X_SSX3_FULL120_FIX=raceclock ---------------
// The race tick [race+0x8] (race object in $s3 of the all-riders pass
// 0x128af0; 0x5bc708 on FR1-R1) is incremented once per update at
// 0x12913c-0x129144, right after `jal 0x1013a8` at 0x12912c. FH2 scan: it is
// the only race-wide per-update counter (0x5bc708 = t-1710 at t1900..2000).
// Pre-decrementing it on every other such call keeps it at 60/s, so every
// reader that counts race ticks as 1/60 s keeps stock units (class e).
inline constexpr uint32_t kRaceTickCallSite = 0x12912cu;
inline constexpr uint32_t kRaceTickCallee = 0x1013a8u;
inline uint32_t g_raceTickCalls = 0u;

// The race object's second frame counter [race+0xc] (0x5bc70c) is bumped at
// 0x113dc4-dd0 in 0x113db0 (vtable 0x458544), in the delay slot of
// `jal 0x12a250` at 0x113dcc; $s0 = race object. FH2 scan: the only int
// counter still 2x after raceclock/session (fh2-f2). Same parity rule.
inline constexpr uint32_t kRaceTick2CallSite = 0x113dccu;
inline constexpr uint32_t kRaceTick2Callee = 0x12a250u;
inline uint32_t g_raceTick2Calls = 0u;

inline bool raceClockFix() noexcept
{
    static const bool on = enabled() && (fixMask() & kFixRaceClock) != 0u;
    return on;
}

inline void raceClockPreHook(uint8_t *ram, R5900Context *ctx, uint32_t sourcePc, uint32_t targetPc)
{
    if (ctx && sourcePc == kRaceTick2CallSite && targetPc == kRaceTick2Callee)
    {
        if ((g_raceTick2Calls++ & 1u) != 0u)
        {
            const uint32_t race = getRegU32(ctx, 16); // $s0
            uint32_t tick = 0u;
            if (rd32(ram, race + 0xcu, tick))
                wr32(ram, race + 0xcu, tick - 1u);
        }
        return;
    }
    if (sourcePc != kRaceTickCallSite || targetPc != kRaceTickCallee || !ctx)
        return;
    if ((g_raceTickCalls++ & 1u) == 0u)
        return;
    const uint32_t race = getRegU32(ctx, 19); // $s3
    uint32_t tick = 0u;
    if (rd32(ram, race + 0x8u, tick))
        wr32(ram, race + 0x8u, tick - 1u);
}

// ---- Race session at stock cadence, PS2X_SSX3_FULL120_FIX=session ---------
// 0x26f4a8 (called once per update from the app update at 0x230bb8, a0 =
// session object, return unused) is the race-session state machine. Its
// object counts frames: +0x3cc race time (stops when +0x610 is set; the HUD
// race clock: 2x under full120, FH2 scan 0x146370c), +0x3c8 state age,
// +0x614 countdown. Servicing the whole machine on every other update (class
// d) keeps all of them in stock frames, and records/finish times in stock
// units on the memory card.
inline constexpr uint32_t kSessionCallSite = 0x230bb8u;
inline constexpr uint32_t kSession = 0x26f4a8u;
inline uint32_t g_sessionCalls = 0u;

inline bool sessionFix() noexcept
{
    static const bool on = enabled() && (fixMask() & kFixSession) != 0u;
    return on;
}

// true = skip this call (the caller continues at its fall-through).
inline bool sessionSkip(uint32_t sourcePc, uint32_t targetPc) noexcept
{
    if (sourcePc != kSessionCallSite || targetPc != kSession)
        return false;
    return (g_sessionCalls++ & 1u) != 0u;
}

// dispatchGuestBranch, after a call returned to its fall-through.
inline void clockPostHook(uint8_t *ram, R5900Context *ctx, uint32_t targetPc)
{
    if (targetPc != kInterval)
        return;
    const uint32_t obj = getRegU32(ctx, 4);
    uint32_t bits = 0u;
    if (!rd32(ram, obj + 0xdcu, bits))
        return;
    float f = 0.0f;
    std::memcpy(&f, &bits, 4);
    f *= 0.5f;
    std::memcpy(&bits, &f, 4);
    wr32(ram, obj + 0xdcu, bits);
}

// EE thread, every guest dispatch while enabled() or the tap is on.
// ---- Lab hooks (env only, full120 only; for trying a conversion without a
// rebuild). PS2X_FH1_HALF=src:tgt:reg:off[,...] (hex): on every other
// dispatch src->tgt, [reg+off] -= 1 (a per-update counter bumped just before
// that call keeps stock cadence). PS2X_FH1_SKIP=src:tgt[,...]: skip every
// other src->tgt call (service at stock cadence, class d).
struct LabHook
{
    uint32_t src = 0, tgt = 0, reg = 0, off = 0, calls = 0;
};

inline std::vector<LabHook> &labHooks(const char *env, bool withTarget)
{
    static std::vector<LabHook> half, skip;
    std::vector<LabHook> &v = withTarget ? half : skip;
    static bool parsedHalf = false, parsedSkip = false;
    bool &parsed = withTarget ? parsedHalf : parsedSkip;
    if (parsed)
        return v;
    parsed = true;
    const char *p = std::getenv(env);
    while (p && *p)
    {
        LabHook h;
        char *end = nullptr;
        h.src = static_cast<uint32_t>(std::strtoul(p, &end, 16));
        if (*end != ':') break;
        h.tgt = static_cast<uint32_t>(std::strtoul(end + 1, &end, 16));
        if (withTarget)
        {
            if (*end != ':') break;
            h.reg = static_cast<uint32_t>(std::strtoul(end + 1, &end, 16));
            if (*end != ':') break;
            h.off = static_cast<uint32_t>(std::strtoul(end + 1, &end, 16));
        }
        v.push_back(h);
        if (*end != ',') break;
        p = end + 1;
    }
    if (!v.empty())
        std::fprintf(stderr, "fh1-full120 lab %s hooks=%zu\n", env, v.size());
    return v;
}

inline bool labHook(uint8_t *ram, R5900Context *ctx, uint32_t sourcePc, uint32_t targetPc)
{
    for (LabHook &h : labHooks("PS2X_FH1_HALF", true))
        if (h.src == sourcePc && h.tgt == targetPc && ctx && (h.calls++ & 1u) != 0u)
        {
            const uint32_t addr = getRegU32(ctx, static_cast<int>(h.reg)) + h.off;
            uint32_t v = 0u;
            if (rd32(ram, addr, v))
                wr32(ram, addr, v - 1u);
        }
    bool skip = false;
    for (LabHook &h : labHooks("PS2X_FH1_SKIP", false))
        if (h.src == sourcePc && h.tgt == targetPc)
            skip = skip || (h.calls++ & 1u) != 0u;
    return skip;
}

// Returns true when the call must be skipped (session fix).
inline bool onBranch(uint8_t *ram, R5900Context *ctx, uint32_t sourcePc, uint32_t targetPc)
{
    if (enabled() && sourcePc == kHookSite && !g_patched)
    {
        g_patched = true;
        patchAtManagerInit(ram);
    }
    if (clockFix())
        clockPreHook(ram, targetPc);
    if (raceClockFix())
        raceClockPreHook(ram, ctx, sourcePc, targetPc);
    bool skip = sessionFix() && sessionSkip(sourcePc, targetPc);
    if (enabled())
        skip = labHook(ram, ctx, sourcePc, targetPc) || skip;
    Tap &t = tap();
    if (!t.on)
        return skip;
    if (targetPc == kUpdateTarget)
        ++t.updates;
    else if (targetPc == kRenderTarget)
        ++t.renders;
    for (size_t i = 0; i < t.nCount; ++i)
        if (t.countPcs[i] == targetPc)
        {
            ++t.counts[i];
            if (t.logArgs && ctx)
                for (int r = 0; r < 4; ++r)
                    t.args[i][r] = getRegU32(ctx, 4 + r);
        }
    return skip;
}

// ---- Per-VBlank frame capture (PS2X_FH1_SEQ=dir, PS2X_FH1_SEQ_TICKS=a-b[,c-d]) --
// At VBlankStart after the MTVU drain (EeScheduler order), the backend's
// Present at this stream position (GS::presentForDiagnostics, no host-latch
// side effects) is written as seq-<tick>.ppm. Output only; unlike PS2X_VQ it
// is not a dev trace, so MTVU/microVU stay on.
struct Seq
{
    const char *dir = nullptr;
    std::array<uint64_t, 16> from{};
    std::array<uint64_t, 16> to{};
    size_t n = 0u;
};

inline const Seq &seq()
{
    static const Seq q = [] {
        Seq r;
        const char *d = std::getenv("PS2X_FH1_SEQ");
        const char *t = std::getenv("PS2X_FH1_SEQ_TICKS");
        if (!d || !*d || !t || !*t)
            return r;
        r.dir = d;
        const char *p = t;
        while (*p && r.n < r.from.size())
        {
            char *end = nullptr;
            const uint64_t a = std::strtoull(p, &end, 10);
            uint64_t b = a;
            if (*end == '-')
                b = std::strtoull(end + 1, &end, 10);
            r.from[r.n] = a;
            r.to[r.n] = b;
            ++r.n;
            if (*end != ',')
                break;
            p = end + 1;
        }
        return r;
    }();
    return q;
}

inline void maybeCapture(GS &gs, uint64_t tick)
{
    const Seq &q = seq();
    if (!q.dir)
        return;
    bool hit = false;
    for (size_t i = 0; i < q.n && !hit; ++i)
        hit = tick >= q.from[i] && tick <= q.to[i];
    if (!hit)
        return;
    const PresentationFrame frame = gs.presentForDiagnostics();
    if (!frame)
        return;
    const size_t legacyStride = 640u * 4u;
    const size_t stride = (frame.width <= 640u && frame.pixels.size() >= legacyStride * frame.height)
                              ? legacyStride
                              : static_cast<size_t>(frame.width) * 4u;
    char path[1024];
    std::snprintf(path, sizeof(path), "%s/seq-%06llu.ppm", q.dir, static_cast<unsigned long long>(tick));
    FILE *f = std::fopen(path, "wb");
    if (!f)
        return;
    std::fprintf(f, "P6\n%u %u\n255\n", frame.width, frame.height);
    std::vector<uint8_t> row(static_cast<size_t>(frame.width) * 3u);
    for (uint32_t y = 0; y < frame.height; ++y)
    {
        const size_t off = static_cast<size_t>(y) * stride;
        if (off + static_cast<size_t>(frame.width) * 4u > frame.pixels.size())
            break;
        for (uint32_t x = 0; x < frame.width; ++x)
        {
            row[x * 3u + 0u] = frame.pixels[off + x * 4u + 0u];
            row[x * 3u + 1u] = frame.pixels[off + x * 4u + 1u];
            row[x * 3u + 2u] = frame.pixels[off + x * 4u + 2u];
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
}

// ---- RAM counter search (PS2X_FH1_SCAN=T1,T2,T3[:lo-hi]) -----------------
// Snapshots RDRAM words at guest ticks T1<T2<T3 and prints every aligned
// 32-bit word whose value rises by exactly (T2-T1)/D then (T3-T2)/D, D =
// PS2X_FH1_SCAN_DIV (default 1 = per-VBlank counters; 2 = per-stock-tick
// counters under full120), and whose T3 value lies in [lo, hi] (optional).
// Finds per-update frame counters (race clocks) without a known address.
// PS2X_FH1_FIND=T:v[,T:v] prints words equal to int v or float v at tick T.
// Diagnostic only; host memory, no guest effect.
struct Scan
{
    uint64_t t[3] = {0, 0, 0};
    int64_t lo = INT64_MIN, hi = INT64_MAX;
    uint32_t div = 1u;
    bool on = false;
    std::vector<uint32_t> snap[2];
};

inline Scan &scan()
{
    static Scan q = [] {
        Scan r;
        const char *v = std::getenv("PS2X_FH1_SCAN");
        if (!v || !*v)
            return r;
        char *end = nullptr;
        r.t[0] = std::strtoull(v, &end, 10);
        if (*end == ',') r.t[1] = std::strtoull(end + 1, &end, 10);
        if (*end == ',') r.t[2] = std::strtoull(end + 1, &end, 10);
        if (*end == ':')
        {
            r.lo = std::strtoll(end + 1, &end, 10);
            if (*end == '-') r.hi = std::strtoll(end + 1, &end, 10);
        }
        if (const char *d = std::getenv("PS2X_FH1_SCAN_DIV"))
            r.div = std::max<uint32_t>(1u, static_cast<uint32_t>(std::strtoul(d, nullptr, 10)));
        r.on = r.t[0] && r.t[1] > r.t[0] && r.t[2] > r.t[1];
        return r;
    }();
    return q;
}

inline void scanVBlank(const uint8_t *ram, uint64_t tick)
{
    Scan &q = scan();
    if (q.on)
    {
        const size_t n = PS2_RAM_SIZE / 4u;
        if (tick == q.t[0] || tick == q.t[1])
        {
            std::vector<uint32_t> &v = q.snap[tick == q.t[0] ? 0 : 1];
            v.resize(n);
            std::memcpy(v.data(), ram, n * 4u);
        }
        else if (tick == q.t[2] && q.snap[0].size() == n && q.snap[1].size() == n)
        {
            const int64_t d1 = static_cast<int64_t>((q.t[1] - q.t[0]) / q.div);
            const int64_t d2 = static_cast<int64_t>((q.t[2] - q.t[1]) / q.div);
            uint32_t hits = 0u;
            for (size_t i = 0; i < n; ++i)
            {
                uint32_t w = 0u;
                std::memcpy(&w, ram + i * 4u, 4);
                const int64_t a = static_cast<int32_t>(q.snap[0][i]);
                const int64_t b = static_cast<int32_t>(q.snap[1][i]);
                const int64_t c = static_cast<int32_t>(w);
                if (b - a == d1 && c - b == d2 && c >= q.lo && c <= q.hi && hits++ < 256u)
                    std::fprintf(stderr, "fh1-scan addr=%08zx v=%lld,%lld,%lld\n", i * 4u,
                                 static_cast<long long>(a), static_cast<long long>(b), static_cast<long long>(c));
            }
            std::fprintf(stderr, "fh1-scan done hits=%u ticks=%llu,%llu,%llu div=%u\n", hits,
                         static_cast<unsigned long long>(q.t[0]), static_cast<unsigned long long>(q.t[1]),
                         static_cast<unsigned long long>(q.t[2]), q.div);
            q.snap[0].clear();
            q.snap[1].clear();
        }
    }
    // PS2X_FH1_SCANF=T1,T2,T3: float words rising by (dT/D)*step on both
    // intervals, step = PS2X_FH1_SCANF_STEP (default 1/60), D = SCAN_DIV:
    // the live per-update 1/60 clocks.
    static Scan qf = [] {
        Scan r;
        const char *v = std::getenv("PS2X_FH1_SCANF");
        if (!v || !*v)
            return r;
        char *end = nullptr;
        r.t[0] = std::strtoull(v, &end, 10);
        if (*end == ',') r.t[1] = std::strtoull(end + 1, &end, 10);
        if (*end == ',') r.t[2] = std::strtoull(end + 1, &end, 10);
        if (const char *d = std::getenv("PS2X_FH1_SCAN_DIV"))
            r.div = std::max<uint32_t>(1u, static_cast<uint32_t>(std::strtoul(d, nullptr, 10)));
        r.on = r.t[0] && r.t[1] > r.t[0] && r.t[2] > r.t[1];
        return r;
    }();
    if (qf.on && (tick == qf.t[0] || tick == qf.t[1] || tick == qf.t[2]))
    {
        const size_t n = PS2_RAM_SIZE / 4u;
        if (tick != qf.t[2])
        {
            std::vector<uint32_t> &v = qf.snap[tick == qf.t[0] ? 0 : 1];
            v.resize(n);
            std::memcpy(v.data(), ram, n * 4u);
        }
        else if (qf.snap[0].size() == n && qf.snap[1].size() == n)
        {
            static const double step = [] {
                const char *e = std::getenv("PS2X_FH1_SCANF_STEP");
                return (e && *e) ? std::strtod(e, nullptr) : 1.0 / 60.0;
            }();
            const double d1 = step * static_cast<double>((qf.t[1] - qf.t[0]) / qf.div);
            const double d2 = step * static_cast<double>((qf.t[2] - qf.t[1]) / qf.div);
            uint32_t hits = 0u;
            for (size_t i = 0; i < n; ++i)
            {
                float a = 0, b = 0, c = 0;
                std::memcpy(&a, &qf.snap[0][i], 4);
                std::memcpy(&b, &qf.snap[1][i], 4);
                std::memcpy(&c, ram + i * 4u, 4);
                if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(c))
                    continue;
                const double e1 = std::fabs((b - a) - d1), e2 = std::fabs((c - b) - d2);
                if (e1 <= d1 * 2e-3 + 1e-5 && e2 <= d2 * 2e-3 + 1e-5 && hits++ < 512u)
                    std::fprintf(stderr, "fh1-scanf addr=%08zx v=%.5f,%.5f,%.5f\n", i * 4u, a, b, c);
                const double n1 = std::fabs((b - a) + d1), n2 = std::fabs((c - b) + d2);
                if (n1 <= d1 * 2e-3 + 1e-5 && n2 <= d2 * 2e-3 + 1e-5 && hits++ < 512u)
                    std::fprintf(stderr, "fh1-scanf addr=%08zx v=%.5f,%.5f,%.5f down\n", i * 4u, a, b, c);
            }
            std::fprintf(stderr, "fh1-scanf done hits=%u\n", hits);
            qf.snap[0].clear();
            qf.snap[1].clear();
        }
    }
    static const char *find = std::getenv("PS2X_FH1_FIND");
    if (!find || !*find)
        return;
    const char *p = find;
    while (*p)
    {
        char *end = nullptr;
        const uint64_t t = std::strtoull(p, &end, 10);
        if (*end != ':')
            break;
        const double val = std::strtod(end + 1, &end);
        if (t == tick)
        {
            const uint32_t iv = static_cast<uint32_t>(static_cast<int64_t>(val));
            const float fv = static_cast<float>(val);
            uint32_t hits = 0u;
            for (size_t i = 0; i < PS2_RAM_SIZE / 4u; ++i)
            {
                uint32_t w = 0u;
                std::memcpy(&w, ram + i * 4u, 4);
                float f = 0.0f;
                std::memcpy(&f, &w, 4);
                const bool fm = f == fv || (fv != 0.0f && std::fabs(f - fv) <= std::fabs(fv) * 1e-4f);
                if ((w == iv || fm) && hits++ < 256u)
                    std::fprintf(stderr, "fh1-find tick=%llu addr=%08zx %s=%g\n", static_cast<unsigned long long>(tick),
                                 i * 4u, w == iv ? "int" : "float", w == iv ? static_cast<double>(iv) : static_cast<double>(f));
            }
            std::fprintf(stderr, "fh1-find done tick=%llu value=%g hits=%u\n", static_cast<unsigned long long>(tick), val, hits);
        }
        if (*end != ',')
            break;
        p = end + 1;
    }
}

// EE thread, at VBlankStart after the tick advanced. A state loaded from a
// stock run (SS1) skips the init hook: convert at the first VBlank that finds
// the manager at stock rate/dt instead.
inline void onVBlank(uint8_t *ram, uint64_t tick, GS &gs)
{
    maybeCapture(gs, tick);
    scanVBlank(ram, tick);
    if (enabled() && !g_patched)
    {
        uint32_t a = 0u, rate = 0u, dt = 0u;
        if (rd32(ram, kMgrPtr, a) && a && !(a & 3u) && rd32(ram, a + 0x10u, rate) &&
            rd32(ram, a + 0x14u, dt) && rate == 60u && dt == kSixtieth)
        {
            g_patched = true;
            std::fprintf(stderr, "fh1-full120-late-arm tick=%llu\n", static_cast<unsigned long long>(tick));
            patchAtManagerInit(ram);
        }
    }
    Tap &t = tap();
    if (!t.on || (tick % t.every) != 0u)
        return;
    char line[4096];
    int n = 0;
    uint32_t a = 0u, wake = 0u, upd = 0u, rate = 0u, dt = 0u;
    rd32(ram, kMgrPtr, a);
    if (a)
    {
        rd32(ram, a + 0x18u, wake);
        rd32(ram, a + 0x1cu, upd);
        rd32(ram, a + 0x10u, rate);
        rd32(ram, a + 0x14u, dt);
    }
    float pos[3] = {};
    for (uint32_t i = 0; i < 3u; ++i)
    {
        uint32_t w = 0u;
        rd32(ram, 0x5409c0u + 4u * i, w);
        std::memcpy(&pos[i], &w, 4);
    }
    n += std::snprintf(line + n, sizeof(line) - n,
                       "fh1-tap tick=%llu upd=%u rend=%u A18=%u A1c=%u rate=%u dt=%08x pos=%.3f,%.3f,%.3f",
                       static_cast<unsigned long long>(tick), t.updates, t.renders, wake, upd, rate, dt,
                       pos[0], pos[1], pos[2]);
    t.updates = t.renders = 0u;
    for (size_t i = 0; i < t.nCount && n < static_cast<int>(sizeof(line)) - 32; ++i)
    {
        n += std::snprintf(line + n, sizeof(line) - n, " c%x=%u", t.countPcs[i], t.counts[i]);
        if (t.logArgs && t.counts[i])
            n += std::snprintf(line + n, sizeof(line) - n, "[%x,%x,%x,%x]", t.args[i][0], t.args[i][1],
                               t.args[i][2], t.args[i][3]);
        t.counts[i] = 0u;
    }
    for (size_t i = 0; i < t.nPeek && n < static_cast<int>(sizeof(line)) - 64; ++i)
    {
        const Tap::Peek &k = t.peeks[i];
        uint32_t addr = k.base;
        if (k.deref && !rd32(ram, k.base, addr))
            addr = 0u;
        addr += k.off;
        n += std::snprintf(line + n, sizeof(line) - n, " %s%x=", k.deref ? "@" : "", addr);
        for (uint32_t j = 0; j < k.n && n < static_cast<int>(sizeof(line)) - 24; ++j)
        {
            uint32_t w = 0u;
            rd32(ram, addr + 4u * j, w);
            float f = 0.0f;
            std::memcpy(&f, &w, 4);
            if (k.fmt == 'f')
                n += std::snprintf(line + n, sizeof(line) - n, "%s%.4f", j ? "," : "", f);
            else if (k.fmt == 'u')
                n += std::snprintf(line + n, sizeof(line) - n, "%s%u", j ? "," : "", w);
            else
                n += std::snprintf(line + n, sizeof(line) - n, "%s%08x", j ? "," : "", w);
        }
    }
    std::fprintf(stderr, "%s\n", line);
}
} // namespace ps2_fh1
