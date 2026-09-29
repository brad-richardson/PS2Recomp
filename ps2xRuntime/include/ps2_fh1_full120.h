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
#include <atomic>
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
// PS2X_SSX3_FULL120: "1" = always (from the manager init hook on), "events"
// = full120 only while a race event runs (FH5): stock 60 in boot, menus,
// loading, pause and results.
enum class Mode { Off, Always, Events };

inline Mode mode() noexcept
{
    static const Mode m = [] {
        const char *v = std::getenv("PS2X_SSX3_FULL120");
        if (!v || !*v || std::strcmp(v, "0") == 0)
            return Mode::Off;
        if (std::strcmp(v, "1") == 0)
            return Mode::Always;
        if (std::strcmp(v, "events") == 0)
            return Mode::Events;
        std::fprintf(stderr, "fh1-full120-refused PS2X_SSX3_FULL120=%s (want 1|events)\n", v);
        std::abort();
    }();
    return m;
}

inline bool enabled() noexcept
{
    return mode() != Mode::Off;
}

inline bool eventsMode() noexcept
{
    return mode() == Mode::Events;
}

// ---- Event switching state (EE thread only; FH5) ---------------------------
// Request: decided at each app-update start from the previous update's facts.
// Commit: at the next VBlankStart (the next interval was scheduled from the
// request just before; EE shift, pacer, stock-time accumulator follow).
// Guest flip: at the next app-update dispatch (0x3171b4), where words and
// hooks switch, parities reset and the 0x1e1458 stamp is restamped.
inline bool g_schedActive = false;  // requested; read when a VBlank is scheduled
inline bool g_commitActive = false; // committed at the last VBlankStart
inline bool g_guestActive = false;  // guest words/hooks active
inline bool g_flipPending = false;
inline uint32_t g_divNext = 1u;     // divisor of the interval scheduled last
inline uint32_t g_divThis = 1u;     // divisor of the interval that just ended
inline std::atomic<uint64_t> g_stockHalf{0}; // elapsed stock half-periods (events)
inline bool g_stockInit = false;

// Guest VBlanks per stock VBlank period (static view: 2 in always mode).
inline uint32_t vblankDivisor() noexcept
{
    if (mode() == Mode::Always)
        return 2u;
    if (mode() == Mode::Events)
        return g_schedActive ? 2u : 1u;
    return 1u;
}

// Scheduler: the divisor for the VBlank interval being scheduled now.
inline uint32_t schedDivisor() noexcept
{
    const uint32_t d = vblankDivisor();
    g_divNext = d;
    return d;
}

// Hooks and word conversions apply: always mode as before (static), events
// mode only while the guest flip is active.
inline bool hooksOn() noexcept
{
    return mode() == Mode::Always || (mode() == Mode::Events && g_guestActive);
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

// Current EE shift (events: only while committed active).
inline uint32_t eeClockShiftNow() noexcept
{
    if (mode() == Mode::Events)
        return g_commitActive ? eeClockShift() : 0u;
    return eeClockShift();
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
    kFixLaunch = 1u << 10,   // system 6: 0x114298 transition wall response only for riders moving into the wall (class f)
    kFixStick = 1u << 11,    // system 6: 0x133308 stick-angle tracker slews and hold timers (class b/h, FH5 trick input)
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
                   kFixTimers | kFixCamera | kFixLaunch | kFixStick;
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
            else if (item == "launch") m |= kFixLaunch;
            else if (item == "stick") m |= kFixStick;
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

inline void applyWords(uint8_t *ram, uint32_t a, bool toActive);

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
    applyWords(ram, a, true);
    std::fprintf(stderr, "fh1-full120-armed manager=%08x rate=120 dt=%08x vblank_div=%u ee_x=%u fix=0x%x\n",
                 a, kHundredTwentieth, vblankDivisor(), 1u << eeClockShift(), fixMask());
}

inline std::vector<Word> labWords();

// The word table, applied in either direction (FH5): toActive expects the
// stock value and writes the replacement; the reverse expects the
// replacement. Every word is verified before any write; a mismatch refuses.
inline void applyWords(uint8_t *ram, uint32_t a, bool toActive)
{
    const std::array<Word, 67> words = {{
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
        // FH4: rider trick/air state 0x117c28 (from 0x1218ac): air timer +0x30
        // += tscale*[gp-0x7b00] at 0x117cf8/0x117d14, fed to 0x119210 (trick
        // curve; points accrue into +0x84 at 0x117d18); +0xa4 -= the same.
        {kFixTimers, 0x49b5f0u, kSixtieth, kHundredTwentieth, "air_trick_clock_117c28"},
        // FH5: stick-angle tracker 0x133308 (P+0x3f0, from 0x111728), each word a
        // single reader: +0x54 += [gp-0x7508] (0x133878, steady-angle timer),
        // hold timers +0x48/+0x4c += [gp-0x74f8]/[gp-0x74f4] (0x133974/0x1339a0),
        // angle slews +0x14->+0x1c and +0x10->+0x18 at owner rate*[gp-0x7458]/
        // [gp-0x744c] (0x134138/0x1342b4). Stock 1/60 per update ran 2x in the
        // air (fh5-k1/k2 SCANL); the lab (fh5-k4) moved the first-cliff score
        // 3930 -> 3530 (stock 3200).
        {kFixStick, 0x49bbe8u, kSixtieth, kHundredTwentieth, "stick_steady_133878"},
        {kFixStick, 0x49bbf8u, kSixtieth, kHundredTwentieth, "stick_hold0_133974"},
        {kFixStick, 0x49bbfcu, kSixtieth, kHundredTwentieth, "stick_hold1_1339a0"},
        {kFixStick, 0x49bc98u, kSixtieth, kHundredTwentieth, "stick_slew0_134138"},
        {kFixStick, 0x49bca4u, kSixtieth, kHundredTwentieth, "stick_slew1_1342b4"},
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
    std::vector<Word> all;
    for (const Word &w : words)
        if (w.fix == 0u || (mask & w.fix) != 0u)
            all.push_back(w);
    for (const Word &w : labWords())
        all.push_back(w);
    for (const Word &w : all)
    {
        const uint32_t want = toActive ? w.expected : w.replacement;
        uint32_t got = 0u;
        if (!rd32(ram, w.address, got) || got != want)
        {
            std::fprintf(stderr, "fh1-full120-refused word=%s addr=%08x got=%08x expected=%08x (%s)\n",
                         w.label, w.address, got, want, toActive ? "enter" : "exit");
            std::abort();
        }
    }
    for (const Word &w : all)
        wr32(ram, w.address, toActive ? w.replacement : w.expected);
    std::fprintf(stderr, "fh1-full120 words=%zu %s\n", all.size(), toActive ? "active" : "stock");
}

// Lab words (PS2X_FH1_WORDS=addr:expected:replacement,...; hex): try a
// pool-word conversion without a rebuild. Same verify-then-write rule.
inline std::vector<Word> labWords()
{
    std::vector<Word> lab;
    const char *p = std::getenv("PS2X_FH1_WORDS");
    while (p && *p)
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
    return lab;
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
    if (targetPc != kInterval || !hooksOn())
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

// ---- Launch wall response, PS2X_SSX3_FULL120_FIX=launch (class f) ----------
// 0x114298 (launch/landing transition helper; callers 0x12eafc, 0x130890,
// 0x13bfe8, 0x13f19c) projects the rider velocity R+0x1e0 off the contact
// normal n = R+0x380 and pushes R+0x110 by 4n when n.z is in the wall band
// ([0x49b53c] -0.05 < n.z < [0x49b538] 0.05; 0x1149a0-0x114a58; the band words
// are read only here). FH3 (All-Peak first cliff, same save state): full120
// samples the lip's vertical face on three half steps (stock: one), and the
// launch fires while that face normal (0.10, 0.99, 0.04) is still current;
// the rider is moving away from it (v.n > 0), yet the projection turns the
// launch velocity (455, 1942, -670) into (275, -25, -670). Stock never takes
// this branch at the lip. Rule: keep the wall response for riders moving
// into the wall (v.n <= 0) and skip it for riders leaving it: per call, the
// band words are set to an empty band (lo 1, hi -1) or to stock.
inline constexpr uint32_t kLaunchHelper = 0x114298u;
inline constexpr uint32_t kBandHi = 0x49b538u, kBandLo = 0x49b53cu;
inline constexpr uint32_t kBandHiStock = 0x3d4ccccdu, kBandLoStock = 0xbd4ccccdu;

inline bool launchFix() noexcept
{
    static const bool on = enabled() && (fixMask() & kFixLaunch) != 0u;
    return on;
}

inline void launchPreHook(uint8_t *ram, R5900Context *ctx, uint32_t targetPc)
{
    if (targetPc != kLaunchHelper || !ctx)
        return;
    const uint32_t r = getRegU32(ctx, 4);
    float n[3] = {}, v[3] = {};
    bool ok = true;
    for (int i = 0; i < 3; ++i)
    {
        uint32_t a = 0u, b = 0u;
        ok = ok && rd32(ram, r + 0x380u + 4u * i, a) && rd32(ram, r + 0x1e0u + 4u * i, b);
        std::memcpy(&n[i], &a, 4);
        std::memcpy(&v[i], &b, 4);
    }
    const bool leaving = ok && n[2] > -0.05f && n[2] < 0.05f && (v[0] * n[0] + v[1] * n[1] + v[2] * n[2]) > 0.0f;
    wr32(ram, kBandHi, leaving ? 0xbf800000u : kBandHiStock); // -1.0 : 0.05
    wr32(ram, kBandLo, leaving ? 0x3f800000u : kBandLoStock); //  1.0 : -0.05
    static uint32_t lines = 0u;
    if (leaving && lines++ < 16u)
        std::fprintf(stderr, "fh1-launch-skip-wall r=%08x n=%.3f,%.3f,%.3f v=%.1f,%.1f,%.1f\n", r, n[0], n[1], n[2],
                     v[0], v[1], v[2]);
}

// ---- Event switching (PS2X_SSX3_FULL120=events; FH5) -----------------------
// Facts per update (EE thread, from dispatches):
//   rider pass ran: `jal 0x1013a8` at 0x12912c (then the race tick bump);
//   any rider airborne: 0x111408 entered with selector [a0+0xde0] != 0;
//   race time ran: the race clock bump [race+0xc] (`jal 0x12a250` at
//   0x113dcc, the HUD race time; FH2 raceclock). It starts at race start and
//   stops at the finish, while the rider pass keeps running under the
//   Rival-card intro and the results fly-by (FH5 e1: entry blip at 1606-1611,
//   re-entry on results at 34178 with a pass-only rule).
// At each app-update dispatch (0x3171b4): active is requested iff the pass
// and the race time ran, and (already active, or every rider was grounded:
// entry only at a case-0 update). Pause, results, loading and menus request
// stock.
inline constexpr uint32_t kAppUpdateSite = 0x3171b4u;
inline constexpr uint32_t kSelectorDispatch = 0x111408u;
inline bool g_passRan = false, g_anyAir = false, g_finished = false, g_clockRan = false;

inline void restamp10s(uint8_t *ram, uint32_t a, bool toActive)
{
    uint32_t stamp = 0u, upd = 0u;
    if (!rd32(ram, kPeriod10sStamp, stamp) || !rd32(ram, a + 0x1cu, upd))
        return;
    int32_t e = static_cast<int32_t>(upd - stamp);
    if (e < 0) e = 0;
    uint32_t next = 0u;
    if (toActive)
    {
        // Elapsed stock updates E of 601 -> 2E of 1202; the game still fires at
        // (A1c - stamp) >= 601, so stamp' = A1c + 601 - 2E (no further shift).
        if (e > 601) e = 601;
        next = upd + 601u - 2u * static_cast<uint32_t>(e);
        g_lastStamp10s = next;
    }
    else
    {
        // Remaining 120 Hz updates R = 601 - (A1c - stamp) -> R/2 stock.
        int32_t r = 601 - e;
        if (r < 0) r = 0;
        next = upd - 601u + static_cast<uint32_t>(r / 2);
    }
    wr32(ram, kPeriod10sStamp, next);
}

inline void guestFlip(uint8_t *ram, uint64_t tick, bool toActive)
{
    uint32_t a = 0u;
    if (!rd32(ram, kMgrPtr, a) || !a || (a & 3u))
    {
        std::fprintf(stderr, "fh1-full120-refused events flip: manager ptr=%08x\n", a);
        std::abort();
    }
    const char *sim = std::getenv("PS2X_SSX3_SIM_MODE");
    if (sim && std::strncmp(sim, "split", 5) == 0)
    {
        std::fprintf(stderr, "fh1-full120-refused PS2X_SSX3_SIM_MODE=%s (full120 replaces split120)\n", sim);
        std::abort();
    }
    applyWords(ram, a, toActive);
    if (clockFix())
        restamp10s(ram, a, toActive);
    if (!toActive)
    {
        wr32(ram, kBandHi, kBandHiStock);
        wr32(ram, kBandLo, kBandLoStock);
    }
    g_producerCalls = g_raceTickCalls = g_raceTick2Calls = g_sessionCalls = 0u;
    g_guestActive = toActive;
    static uint32_t lines = 0u;
    if (lines++ < 64u)
        std::fprintf(stderr, "fh1-events flip %s tick=%llu\n", toActive ? "enter" : "exit",
                     static_cast<unsigned long long>(tick));
}

inline uint64_t g_lastTick = 0u;

inline void eventsOnBranch(uint8_t *ram, R5900Context *ctx, uint32_t sourcePc, uint32_t targetPc)
{
    if (targetPc == kSelectorDispatch && ctx)
    {
        uint32_t sel = 0u;
        if (rd32(ram, getRegU32(ctx, 4) + 0xde0u, sel) && sel != 0u)
            g_anyAir = true;
    }
    else if (sourcePc == kRaceTickCallSite && targetPc == kRaceTickCallee)
        g_passRan = true;
    else if (sourcePc == kRaceTick2CallSite && targetPc == kRaceTick2Callee)
        g_clockRan = true;
    else if (targetPc == kSession && ctx)
    {
        uint32_t fin = 0u;
        if (rd32(ram, getRegU32(ctx, 4) + 0x610u, fin))
            g_finished = fin != 0u;
    }
    else if (sourcePc == kAppUpdateSite)
    {
        if (g_flipPending)
        {
            g_flipPending = false;
            if (g_commitActive != g_guestActive)
                guestFlip(ram, g_lastTick, g_commitActive);
        }
        const bool want = g_passRan && g_clockRan && (g_guestActive || !g_anyAir);
        if (want != g_schedActive)
        {
            g_schedActive = want;
            static uint32_t lines = 0u;
            if (lines++ < 64u)
                std::fprintf(stderr, "fh1-events request %s tick=%llu pass=%d clock=%d air=%d fin=%d\n",
                             want ? "enter" : "exit", static_cast<unsigned long long>(g_lastTick), g_passRan ? 1 : 0,
                             g_clockRan ? 1 : 0, g_anyAir ? 1 : 0, g_finished ? 1 : 0);
        }
        g_passRan = g_anyAir = g_clockRan = false;
    }
}

// Returns true when the call must be skipped (session fix).
inline bool onBranch(uint8_t *ram, R5900Context *ctx, uint32_t sourcePc, uint32_t targetPc)
{
    if (mode() == Mode::Always && sourcePc == kHookSite && !g_patched)
    {
        g_patched = true;
        patchAtManagerInit(ram);
    }
    if (mode() == Mode::Events)
        eventsOnBranch(ram, ctx, sourcePc, targetPc);
    const bool on = hooksOn();
    if (on && clockFix())
        clockPreHook(ram, targetPc);
    if (on && raceClockFix())
        raceClockPreHook(ram, ctx, sourcePc, targetPc);
    if (on && launchFix())
        launchPreHook(ram, ctx, targetPc);
    bool skip = on && sessionFix() && sessionSkip(sourcePc, targetPc);
    if (on)
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
    // PS2X_FH1_SCANL=T1,T2,T3[:lo-hi] (FH5): float words in [lo,hi) that
    // change linearly (same rate per tick on both intervals, within 1%) and
    // not trivially: prints addr and rate per tick. Run it at equal guest
    // times in two modes from one save state (same heap) and compare rates.
    static Scan ql = [] {
        Scan r;
        const char *v = std::getenv("PS2X_FH1_SCANL");
        if (!v || !*v)
            return r;
        char *end = nullptr;
        r.t[0] = std::strtoull(v, &end, 10);
        if (*end == ',') r.t[1] = std::strtoull(end + 1, &end, 10);
        if (*end == ',') r.t[2] = std::strtoull(end + 1, &end, 10);
        r.lo = 0;
        r.hi = PS2_RAM_SIZE;
        if (*end == ':')
        {
            r.lo = static_cast<int64_t>(std::strtoull(end + 1, &end, 16));
            if (*end == '-') r.hi = static_cast<int64_t>(std::strtoull(end + 1, &end, 16));
        }
        r.on = r.t[0] && r.t[1] > r.t[0] && r.t[2] > r.t[1];
        return r;
    }();
    if (ql.on && (tick == ql.t[0] || tick == ql.t[1] || tick == ql.t[2]))
    {
        const size_t n = PS2_RAM_SIZE / 4u;
        if (tick != ql.t[2])
        {
            std::vector<uint32_t> &v = ql.snap[tick == ql.t[0] ? 0 : 1];
            v.resize(n);
            std::memcpy(v.data(), ram, n * 4u);
        }
        else if (ql.snap[0].size() == n && ql.snap[1].size() == n)
        {
            const double d1 = static_cast<double>(ql.t[1] - ql.t[0]);
            const double d2 = static_cast<double>(ql.t[2] - ql.t[1]);
            uint32_t hits = 0u;
            for (size_t i = static_cast<size_t>(ql.lo) / 4u; i < static_cast<size_t>(ql.hi) / 4u && i < n; ++i)
            {
                float a = 0, b = 0, c = 0;
                std::memcpy(&a, &ql.snap[0][i], 4);
                std::memcpy(&b, &ql.snap[1][i], 4);
                std::memcpy(&c, ram + i * 4u, 4);
                if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(c))
                    continue;
                const double r1 = (static_cast<double>(b) - a) / d1, r2 = (static_cast<double>(c) - b) / d2;
                const double mag = std::fabs(r1);
                if (mag < 1e-6 || mag > 1e6 || std::fabs(a) > 1e7)
                    continue;
                if (std::fabs(r1 - r2) <= 0.01 * mag && hits++ < 2048u)
                    std::fprintf(stderr, "fh1-scanl addr=%08zx rate=%.6g v=%.5g,%.5g,%.5g\n", i * 4u, r1, a, b, c);
            }
            std::fprintf(stderr, "fh1-scanl done hits=%u\n", hits);
            ql.snap[0].clear();
            ql.snap[1].clear();
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
    if (mode() == Mode::Events)
    {
        g_lastTick = tick;
        g_divThis = g_divNext;
        if (!g_stockInit)
        {
            // A fresh boot starts at tick 1; a state loaded from a stock run
            // carries stock history up to tick-1.
            g_stockInit = true;
            g_stockHalf.store(2u * (tick - 1u), std::memory_order_relaxed);
        }
        g_stockHalf.fetch_add(g_divThis == 2u ? 1u : 2u, std::memory_order_relaxed);
        if (g_commitActive != g_schedActive)
        {
            g_commitActive = g_schedActive;
            g_flipPending = true;
            static uint32_t lines = 0u;
            if (lines++ < 64u)
                std::fprintf(stderr, "fh1-events commit %s tick=%llu\n", g_commitActive ? "enter" : "exit",
                             static_cast<unsigned long long>(tick));
        }
    }
    if (mode() == Mode::Always && !g_patched)
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
    if (mode() == Mode::Events)
        n += std::snprintf(line + n, sizeof(line) - n, " act=%d%d%d", g_schedActive ? 1 : 0,
                           g_commitActive ? 1 : 0, g_guestActive ? 1 : 0);
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
// Stock-time views for host-side clocks (pad-script vsync clock, CD field
// clock, [vsync-rate]): events mode reads the accumulator; the other modes
// keep tick / vblankDivisor().
inline uint64_t stockHalfTicks() noexcept
{
    return g_stockHalf.load(std::memory_order_relaxed);
}
} // namespace ps2_fh1
