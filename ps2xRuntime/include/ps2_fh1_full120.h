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
#include "ps2_runtime_macros.h"
#include "ps2_input_diag.h"
#include "ps2_fh1_restamp.h"

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
#include <utility>
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
inline uint32_t g_divThis = 1u;     // FH5 (legacy): set to g_divNext at VBlankStart, i.e. the interval starting
inline uint32_t g_divEnded = 1u;    // FH17: divisor of the interval that just ended
inline std::atomic<uint64_t> g_stockHalf{0};       // FH5 accumulator (half a tick late while events is active)
inline std::atomic<uint64_t> g_stockHalfExact{0};  // FH17: elapsed stock half-periods = guest time
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
    g_divEnded = g_divNext;
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

enum Fix : uint64_t // FH17: 64-bit (bits 0-31 used by FH13)
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
    kFixSpeedcap = 1u << 12, // system 6: 0x11b3f8 speed-cap blend retentions d -> sqrt(d) (class a, FA1 3.2)
    kFixRng = 1u << 13,      // RNG cadence: per-update draw sites at stock cadence (class d/g, FA1 2)
    kFixTrick = 1u << 15,    // system 6: 0x117638 combo/style accrual (from 0x11a3f0) at stock cadence (class d, FH7)
    kFixRamp = 1u << 14,     // internal: 0x115d98 ramp word at 1/120, only while 0x115d48 runs per update (timers without rng)
    kFixAnim = 1u << 16,     // animation node steps 0x103c80..0x1049c4: 13 per-type 1/60 words (class h, FH8)
    kFixBonus = 1u << 17,    // 0x119708 trick bonus: per-update points rate +0x3c at stock rate per second (class b, FH8)
    kFixLift = 1u << 18,     // camera terrain lift 0x15ee00: per-update step bounds halved (class j, CM2 4, FH8)
    kFixAiGate = 1u << 19,   // AI race_tick % N gates on even updates only + 0x10da10 hold timer at 1/120 (class e/g, FH9)
    kFixTakeoff = 1u << 20,  // takeoff state 4 (0x12f730): control-triple slew rate 0.05/update -> 0.025 (class b, FH9)
    kFixFlags = 1u << 21,    // cFlagManager 0x34b818: per-flag wave/scroll phases advance by a per-update step (class b, FH11)
    kFixSteer = 1u << 22,    // steering/crouch control triples 0x113e80/0x113f38/0x113f88: private 1/60 steps and per-update rates (class h/b, FH10)
    kFixRail = 1u << 23,     // rail slide handler 0x13af28 (selector 4): private dt 1/60 -> 1/120 (class h, FH10)
    kFixReset = 1u << 24,    // course-reset fade state 9 (0x12f398): ramp step 1/40 per update -> 1/80 (class b, FH10)
    kFixMeter = 1u << 25,    // HUD boost meter fill (0x117fe0 messages 5/6): display step 1/60 -> 1/120 per update (class b, FH10)
    kFixBoost = 1u << 26,    // boost/uber machine 0x1200d0: private dt + one-step thresholds 1/60 -> 1/120 (class h, FH10)
    kFixGround = 1u << 27,   // ground adhesion 0x13c878: per-update pull toward the snow while above it, halved (class b, FV1)
    kFixEntry = 1u << 28,    // events mode: enter at the race start (gate drop-in) even while riders are airborne (FH14)
    kFixRclock = 1u << 29,   // render-frame clock (getter 0x395510, device +0x5a74) read at stock cadence: plant sway, frame deadlines (class e, FH13)
    kFixEmitter = 1u << 30,  // particle emitter frame step 0x370788 (callers 0x3459a8/0x345d80/0x345ef8): private 1/60 -> 1/120 (class h, FH13)
    kFixFx = 1u << 31,       // rider effects controller P+0xb40 (0x2e1120 glint timers, 0x2e1f70 fader) + board-wake scroll 0x2ef6d0: per-update steps halved (class b/h, FH13)
    kFixClock2 = 1ull << 32, // events mode: stock-time accumulator counts the interval that ended, not the one scheduled (FH17)
    kFixHudfill = 1ull << 33, // race-HUD trick-combo fill (HUD object +0x70, 0x1ebf70/0x1ebf98 in 0x1e9a30): per-update step 1/48 -> 1/96 (class b, FH20)
    // FH26 candidates, opt-in (not in "all"; add them as "all,stick2"):
    kFixStick2 = 1ull << 34,    // 0x133308 bounded offset slews [0x49bc94]/[0x49bca0] (0x1340a8/0x134224): ~0.05 per update -> half (class b, FCR1 1)
    kFixClockSign = 1ull << 35, // with clock: the 0x1e1458 stamp's exit restamp keeps the signed elapsed (FCR1 3)
};

// FH12 groups live in their own mask (the main mask's bits are taken). Same
// PS2X_SSX3_FULL120_FIX string; "all" includes them; fixMask() accepts the names.
enum : uint32_t
{
    kFix12Spin = 1u << 0,    // rotating objects (vtable 0x491220, update 0x346258): int degrees per update (class b, pickups)
    kFix12Texanim = 1u << 1, // texture anims 0x352c70: UV scroll/rotation 0x35f7d0 + flipbook 0x35f410 per-update steps (class b)
    kFix12Loops = 1u << 2,   // loop-timer controllers 0x341d48 (modes 0x341e48/0x341ec0/0x341f38): step baked at A.rate 60 (class b)
    kFix12Recover = 1u << 3, // wipeout recovery bar decay 0x12cb68: +-1/240 per update (class b)
    kFix12Pulse = 1u << 4,   // rider colour pulse 0x2e39d8: 2pi/60 per update (class b)
    kFix12Crash = 1u << 5,   // wipeout body: motion solver 0x113648 fed a private 1/60 dt by 0x137750/0x1391a8 (class h)
    kFix12FxTimer = 1u << 6, // rider-FX timer P+0xb44 (0x2e2388 in 0x2e2260): per-update 1/60 step shared with the trail-push dt (post-call add-back, FH21)
    kFix12Bounce = 1u << 7,  // rider bounce oscillator phase (0x13f0d4 in 0x13d818 -> R+0x31c): per-update step 2.65 -> 1.325 (class b, FH22)
    kFix12Particles = 1u << 8, // rider particle pass 0x2dd0b8 (calls 0x128f20/0x11198c): private 1/60 dt + per-update gravity -> stock cadence (class d, FH22)
    kFix12CrashBody = 1u << 9, // wipeout body/board integrator 0x136f30: private 1/60 step (pos, gravity, drag, spin) (class h, FH23)
    kFix12Uber = 1u << 10,     // HUD uber meter (0x117fe0 message 9): display slew 1/60 per update (class b, FH23)
    kFix12Popups = 1u << 11,   // HUD timed slots (0x116fb8, crash score popups): elapsed += 1/60 per update (class b, FH24)
    kFix12SndDt = 1u << 12,    // race sound-tree update 0x285bf8 (from 0x22c014): private dt vblanks * 1/60 (class h, FH24)
    kFix12HudProg = 1u << 13,  // race-HUD progress table 0x4c8bc8 (0x210618/0x20eda0): per-update slew limits (class b, FH24)
};

inline uint32_t fh12Item(const std::string &item) noexcept
{
    if (item == "spin") return kFix12Spin;
    if (item == "texanim") return kFix12Texanim;
    if (item == "loops") return kFix12Loops;
    if (item == "recover") return kFix12Recover;
    if (item == "pulse") return kFix12Pulse;
    if (item == "crash") return kFix12Crash;
    if (item == "fxtimer") return kFix12FxTimer;
    if (item == "bounce") return kFix12Bounce;
    if (item == "particles") return kFix12Particles;
    if (item == "crashbody") return kFix12CrashBody;
    if (item == "uber") return kFix12Uber;
    if (item == "popups") return kFix12Popups;
    if (item == "snddt") return kFix12SndDt;
    if (item == "hudprog") return kFix12HudProg;
    return 0u;
}

inline uint32_t fixMask12() noexcept
{
    static const uint32_t mask = [] {
        const char *v = std::getenv("PS2X_SSX3_FULL120_FIX");
        if (!enabled() || !v || !*v)
            return 0u;
        const std::string s(v);
        const uint32_t all = kFix12Spin | kFix12Texanim | kFix12Loops | kFix12Recover | kFix12Pulse | kFix12Crash |
                             kFix12FxTimer | kFix12Bounce | kFix12Particles | kFix12CrashBody | kFix12Uber | kFix12Popups |
                             kFix12SndDt | kFix12HudProg;
        if (s == "all")
            return all;
        uint32_t m = 0u;
        size_t at = 0u;
        while (at <= s.size())
        {
            const size_t comma = s.find(',', at);
            const std::string item = s.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
            m |= item == "all" ? all : fh12Item(item); // FH26: "all,<opt-in>"
            if (comma == std::string::npos)
                break;
            at = comma + 1u;
        }
        return m;
    }();
    return mask;
}

inline uint64_t fixMask() noexcept
{
    static const uint64_t mask = []() -> uint64_t {
        const char *v = std::getenv("PS2X_SSX3_FULL120_FIX");
        if (!enabled() || !v || !*v)
            return 0u;
        const std::string s(v);
        const uint64_t all = kFixRider | kFixCountdown | kFixDrag | kFixEvent | kFixSlew | kFixClock | kFixRaceClock |
                             kFixSession | kFixTimers | kFixCamera | kFixLaunch | kFixStick | kFixSpeedcap | kFixRng |
                             kFixTrick | kFixAnim | kFixBonus | kFixAiGate | kFixTakeoff | kFixFlags | kFixSteer |
                             kFixRail | kFixReset | kFixMeter | kFixBoost | kFixGround | kFixEntry | kFixRclock |
                             kFixEmitter | kFixFx | kFixClock2 | kFixHudfill; // kFixLift is opt-in (FH8: no window where it binds)
        if (s == "all")
            return all;
        uint64_t m = 0u;
        size_t at = 0u;
        while (at <= s.size())
        {
            const size_t comma = s.find(',', at);
            const std::string item = s.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
            if (item == "all") m |= all; // FH26: "all,<opt-in>"
            else if (item == "rider") m |= kFixRider;
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
            else if (item == "speedcap") m |= kFixSpeedcap;
            else if (item == "rng") m |= kFixRng;
            else if (item == "trick") m |= kFixTrick;
            else if (item == "anim") m |= kFixAnim;
            else if (item == "bonus") m |= kFixBonus;
            else if (item == "lift") m |= kFixLift;
            else if (item == "aigate") m |= kFixAiGate;
            else if (item == "takeoff") m |= kFixTakeoff;
            else if (item == "flags") m |= kFixFlags;
            else if (item == "steer") m |= kFixSteer;
            else if (item == "rail") m |= kFixRail;
            else if (item == "reset") m |= kFixReset;
            else if (item == "meter") m |= kFixMeter;
            else if (item == "boost") m |= kFixBoost;
            else if (item == "ground") m |= kFixGround;
            else if (item == "entry") m |= kFixEntry;
            else if (item == "rclock") m |= kFixRclock;
            else if (item == "emitter") m |= kFixEmitter;
            else if (item == "fx") m |= kFixFx;
            else if (item == "clock2") m |= kFixClock2;
            else if (item == "hudfill") m |= kFixHudfill;
            else if (item == "stick2") m |= kFixStick2;
            else if (item == "clocksign") m |= kFixClockSign;
            else if (fh12Item(item) != 0u) {} // FH12 mask (fixMask12)
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

// Stock-time views for host-side clocks (pad-script vsync clock, pad
// recorder, CD field clock, [vsync-rate]): events mode reads the accumulator;
// the other modes keep tick / vblankDivisor(). FIX clock2 (FH17) selects the
// exact accumulator; without it the FH5 one (half a tick late while active).
inline bool exactStockClock() noexcept
{
    return (fixMask() & kFixClock2) != 0u;
}
inline uint64_t stockHalfTicksLegacy() noexcept
{
    return g_stockHalf.load(std::memory_order_relaxed);
}
inline uint64_t stockHalfTicks() noexcept
{
    return exactStockClock() ? g_stockHalfExact.load(std::memory_order_relaxed) : stockHalfTicksLegacy();
}
// Half-periods the accumulator adds at the next VBlankStart.
inline uint32_t stockHalfStepNext() noexcept
{
    return (exactStockClock() ? g_divNext : g_divThis) == 2u ? 1u : 2u;
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
    uint64_t fix; // 0 = always (manager)
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
    std::fprintf(stderr, "fh1-full120-armed manager=%08x rate=120 dt=%08x vblank_div=%u ee_x=%u fix=0x%llx\n",
                 a, kHundredTwentieth, vblankDivisor(), 1u << eeClockShift(),
                 static_cast<unsigned long long>(fixMask()));
}

inline std::vector<Word> labWords();

// The word table, applied in either direction (FH5): toActive expects the
// stock value and writes the replacement; the reverse expects the
// replacement. Every word is verified before any write; a mismatch refuses.
inline void applyWords(uint8_t *ram, uint32_t a, bool toActive)
{
    const std::array<Word, 114> words = {{
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
        // FH8: 0x10496c is one of 14 animation-node step functions (dispatch
        // 0x103578 by node type). Each advances its track by f12*[own 1/60
        // word] through 0x3135b0 (time += rate*speed*step; anim events fire as
        // the time crosses them, 0x313868 -> track +0xb0 bits). The other 13
        // words (single readers) stayed 1/60, so rider animations played 2x per
        // second at 120: All-Peak first-cliff trick track (0x1048ec/0x1049ec)
        // +0.0217 per 120 Hz update, trick-bonus event (0x135460 -> 0x119708)
        // 10 stock frames early (fh8-d3/d4); lab fh8-d6: bonus at stock 4509.5
        // vs 4509, in-air 3230 -> 3080 (stock 3040).
        {kFixAnim, 0x49b180u, kSixtieth, kHundredTwentieth, "anim_step_103c80"},
        {kFixAnim, 0x49b184u, kSixtieth, kHundredTwentieth, "anim_step_103da4"},
        {kFixAnim, 0x49b188u, kSixtieth, kHundredTwentieth, "anim_step_103f74"},
        {kFixAnim, 0x49b18cu, kSixtieth, kHundredTwentieth, "anim_step_104048"},
        {kFixAnim, 0x49b190u, kSixtieth, kHundredTwentieth, "anim_step_1040b4"},
        {kFixAnim, 0x49b194u, kSixtieth, kHundredTwentieth, "anim_step_1040c8"},
        {kFixAnim, 0x49b198u, kSixtieth, kHundredTwentieth, "anim_step_1043b4"},
        {kFixAnim, 0x49b19cu, kSixtieth, kHundredTwentieth, "anim_step_104554"},
        {kFixAnim, 0x49b1a0u, kSixtieth, kHundredTwentieth, "anim_step_104618"},
        {kFixAnim, 0x49b1a4u, kSixtieth, kHundredTwentieth, "anim_step_1047ac"},
        {kFixAnim, 0x49b1a8u, kSixtieth, kHundredTwentieth, "anim_step_10487c"},
        {kFixAnim, 0x49b1acu, kSixtieth, kHundredTwentieth, "anim_step_1048c4"},
        {kFixAnim, 0x49b1b4u, kSixtieth, kHundredTwentieth, "anim_step_1049c4"},
        // 0x114124 applies this fraction of the remaining R+0x214 gap once per
        // update: alpha_half = 1-sqrt(1-alpha_stock).
        {kFixSlew, 0x49b4f0u, 0x3d2aa635u, 0x3cac76f5u, "mode0_slew_114124"},
        // FH2 live 1/60 clocks (PS2X_FH1_SCANF, stock t1900..2000; writers by DIAG_WATCH):
        // HUD object +0x190 += [gp-0x5758] at 0x1eb034/44 (0x1e9a30 race HUD),
        {kFixTimers, 0x49d998u, kSixtieth, kHundredTwentieth, "hud_clock_1eb02c"},
        // rider object +0x35c += [gp-0x7b54] up to 1.0 at 0x115d98-db4,
        // FH7: with FIX=rng, 0x115d48 (which holds this ramp) runs at stock
        // cadence, so the ramp word stays 1/60 (kFixRamp = timers && !rng).
        {kFixRamp, 0x49b59cu, kSixtieth, kHundredTwentieth, "rider_ramp_115d98"},
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
        // FH26 stick2 (FCR1 1, opt-in): the bounded steps before those slews move the offsets S+0x28/S+0x2c
        // toward their targets by [0x49bc94]/[0x49bca0] (~0.05, single GP readers 0x1340a8/0x134224) times a
        // gap factor <= 1 and (0.5 + 0.5|in|), clamped at the target, with no dt: 2x per stock tick at 120.
        // Halved (exponent - 1, exact): one stock step per two updates while the clamp doesn't bind.
        {kFixStick2, 0x49bc94u, 0x3d4cc9c7u, 0x3cccc9c7u, "stick2_offset0_1340a8"},
        {kFixStick2, 0x49bca0u, 0x3d4cd331u, 0x3cccd331u, "stick2_offset1_134224"},
        // FH7 (FA1 3.2): 0x11b3f8 per rider per update R+0x2e4 = d*cap +
        // (1-d)*target with no dt: at 120 the post-air/post-boost cap relaxes
        // 2x as fast in real time. Retentions d -> sqrt(d), each word a single
        // reader (0x11b60c easing down 0.97, 0x11b5cc easing up 0.9). FA1 lab
        // f3: FR1-R1 rival +7.4 % -> +2.3 %, stock line again.
        {kFixSpeedcap, 0x49b730u, 0x3f7851ecu, 0x3f7c217au, "speedcap_down_11b60c"},
        {kFixSpeedcap, 0x49b728u, 0x3f666666u, 0x3f72dce8u, "speedcap_up_11b5cc"},
        // FH7 trick: style accumulator +0x1c of the rider trick state
        // (0x117fe0 at 0x1188b8-cc) += tweak * [gp-0x7a74] once per update;
        // the HUD style bonus is round10(+0x1c * 10000 + 5) (0x117908, shown
        // via 0x1171a8 from 0x118ebc). 0.05/60 per update -> 0.05/120 (class
        // b, single reader, not a timestep value so the census missed it).
        // Lab fh7-n6: All-Peak first-cliff style bonus +300 -> +150 (stock +160).
        {kFixTrick, 0x49b67cu, 0x3a5a740eu, 0x39da740eu, "style_rate_1188b8"},
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
        {kFixAiGate, 0x49b40cu, kSixtieth, kHundredTwentieth, "ai_react_hold"},
        // FH9 takeoff: 0x12f730 (rider control state 4, every rider) sets the slew rate of the
        // control triples +0x1f4/+0x200/+0x218/+0x224 to this word each update; the slewer
        // 0x1211f8 steps value toward target by <= rate per update (no dt), and state 4 exits
        // once +0x1f0/+0x214/+0x220 reach 0, so at 120 takeoff ended in half the time (AI Mac,
        // FR1-R1 race 7.77 s: 0.18 s vs stock 0.35 s). Single reader 0x12f9f4. With it the
        // rival finishes 03:16 vs stock 03:15 with 0 AI wipeouts (fh9-p1; before: 03:23, 2).
        {kFixTakeoff, 0x49ba0cu, 0x3d4cccceu, 0x3cccccceu, "takeoff_slew"},
        // FH10 steer: 0x113e80 (steering triple R+0x1f0, every rider in ground/air/pre-jump states) sets
        // target = stick and rate = clamp(12|gap|, 0.1, 14) * [0x49b4c4] per update, 0x113f38 (triple
        // +0x22c) the same with [0x49b4cc]: a per-second rate times a private 1/60 step (class h).
        // 0x113f88 (lean +0x214 / crouch +0x220 triples) uses per-update rates: fixed returns
        // [0x49b4d0]/[0x49b4d4] and the floor [0x49b4e0] halve (class b); the gap gain [0x49b4dc] 0.1
        // -> 1-sqrt(0.9) (class a, like TS1's 0x49b4f0). All single readers. Brad's full-right carve on
        // the All-Peak state (stock 4432-4461): events turned in ~1.5-2x faster (heading lead to 1.46 deg,
        // RMS 0.92); with these words the onset tracks stock within 0.05 deg, RMS 0.49 (fh10-l2).
        {kFixSteer, 0x49b4c4u, kSixtieth, kHundredTwentieth, "steer_dt_113e80"},
        {kFixSteer, 0x49b4ccu, kSixtieth, kHundredTwentieth, "steer_dt_113f38"},
        {kFixSteer, 0x49b4d0u, 0x3d2aa635u, 0x3caaa635u, "lean_return_113fa8"},
        {kFixSteer, 0x49b4d4u, 0x3dcccdc2u, 0x3d4ccdc2u, "crouch_return_113fd4"},
        {kFixSteer, 0x49b4dcu, 0x3dcccdc2u, 0x3d523279u, "crouch_gain_114024"},
        {kFixSteer, 0x49b4e0u, 0x3c23d7cfu, 0x3ba3d7cfu, "crouch_floor_114030"},
        // FH10 rail: the rail slide handler 0x13af28 (selector P+0xde0 = 4) steps with f23 = R+0x300 *
        // [0x49bf84] (single reader 0x13afc8), a private 1/60 dt: at 120 the rider slid along the rail
        // at 2.2x speed (3455 vs 1558 u/s, same velocity vector). With 1/120: 1779/2159 vs 1558/2354 u/s,
        // rail left at stock 6012 vs 6015 (common state s2-5700, fh10-r1/r2/r3).
        {kFixRail, 0x49bf84u, kSixtieth, kHundredTwentieth, "rail_dt_13afc8"},
        // FH10 reset: rider state 9 (0x12f398, the white-fade course reset) ramps +4 to 1 by R+0x300 *
        // [0x49b9d4] (1/40, single reader 0x12f39c) per update: 41 updates in either mode, so 20.5 stock
        // ticks at 120. Halved: 40.5 stock ticks vs stock 41 (fh10-r1/r2/r3).
        {kFixReset, 0x49b9d4u, 0x3cccccceu, 0x3c4cccceu, "reset_fade_12f39c"},
        // FH10 meter: 0x117fe0 posts the HUD boost meter (messages 5/6, 0x118950/0x1189b4); the shown
        // fill moves toward the boost R+0x2f8 by [0x49b680]/[0x49b684] (1/60, single readers) per update,
        // and the HUD (0x29ab40) clicks once per coil floor(10*fill). Stock fills 1/60 per stock tick;
        // events filled 2x (coils every 3 vs 6 stock ticks, fh10-m1/m2).
        {kFixMeter, 0x49b680u, kSixtieth, kHundredTwentieth, "meter_fill5_118950"},
        {kFixMeter, 0x49b684u, kSixtieth, kHundredTwentieth, "meter_fill6_1189b4"},
        // FH10 boost: the boost/uber machine 0x1200d0 (per rider, per update) steps its timers R+0x2e8,
        // R+0x2ec, the uber timer R+0x2f0 (20 s / 60 s, set by 0x10e098/0x10e9e8) and the boost drains
        // (0.005/s, 0.024/s in mode 2) by f20 = R+0x300 * [0x49b798] (1/60); [0x49b79c]/[0x49b7a0] are
        // one-step thresholds on +0x2ec/+0x2f0. All single readers. Uber lab (boost poked to 0.99 before
        // the first All-Peak landing): stock uber timer -1.00/s, drain -0.005/s; events -2.00/s (uber
        // over in 10 s), -0.010/s; with these words -1.00/s, -0.005/s (fh10-u1/u2/u3).
        {kFixBoost, 0x49b798u, kSixtieth, kHundredTwentieth, "boost_dt_1200d4"},
        {kFixBoost, 0x49b79cu, kSixtieth, kHundredTwentieth, "boost_step_120124"},
        {kFixBoost, 0x49b7a0u, kSixtieth, kHundredTwentieth, "uber_step_1201d8"},
        // FV1 ground: the ground handler 0x13d818 gets its normal force from 0x13c878(h = R+0x454, the
        // rider's height above the snow). While above it (h > 0) the pull back down is h * [0x49c008]
        // (-0.033333, single reader 0x13c88c) scaled by the rider table, applied once per update: at 120
        // the rider was pulled down twice per stock frame and stayed on the slope over crests (All-Peak
        // straight run, crest at 7.6 s: stock airborne at 4824, events at 4860 with |v| held at the
        // ground cap 2083, 0.14 s lost). Halved: airborne at 4823, lag <= 0.03 s and distance within
        // 0.6 % through 10.5 s (fv1-s2/s3, lab word fv1-w1).
        {kFixGround, 0x49c008u, 0xbd08882fu, 0xbc88882fu, "ground_pull_13c88c"},
        // FH13 emitter: the particle emitter step 0x370788 ages the emitter by 1/A.rate (per time) but
        // advances its frame phase +0x184 by +0x188 * f12 per update, f12 = a private 1/60 passed by
        // the three emitter classes (0x3459a8 at 0x345b78/0x345b8c; 0x345d80 at 0x345e3c; 0x345ef8 at
        // 0x345fd0/0x346000/0x34601c), each word a single reader (codegen grep). The frame index wraps at
        // the int +0x180, so emitter animations (spray/snow puffs) played 2x at 120 (class h).
        {kFixEmitter, 0x4a059cu, kSixtieth, kHundredTwentieth, "emitter_dt_345b78"},
        {kFixEmitter, 0x4a05a0u, kSixtieth, kHundredTwentieth, "emitter_dt_345b8c"},
        {kFixEmitter, 0x4a05a4u, kSixtieth, kHundredTwentieth, "emitter_dt_345e3c"},
        {kFixEmitter, 0x4a05a8u, kSixtieth, kHundredTwentieth, "emitter_dt_345fd0"},
        {kFixEmitter, 0x4a05acu, kSixtieth, kHundredTwentieth, "emitter_dt_346000"},
        {kFixEmitter, 0x4a05b0u, kSixtieth, kHundredTwentieth, "emitter_dt_34601c"},
        // FH13 fx: the rider effects controller (P+0xb40, 0x2e1120 from the rider pass, per rider per
        // update): +0x1c -= [0x49f60c] (1/600, floored), timer +0x20 += [0x49f610] (1/60) against a period
        // in seconds (+0x24, 0.4..1.0), and +0x14 += rate * [0x49f628] (1/60); its fader 0x2e1f70 moves
        // +0x10 (P+0xb50) up by [0x49f68c] 0.01 or down by [0x49f688] 1/60 per update. All single readers,
        // all measured 2x per stock second at 120 (FR1-R1 P+0xb50/b5c/b60, FH13 r1s/r1e dumps). The
        // controller's trail-push dt words (0x49f62c/30/9c/a0 -> 0x3717c0 -> 0x3710d0) stay 1/60: FH7 rng
        // already pushes trails at stock cadence. The board-wake scroll 0x2ef6d0 (per rider, from the rider
        // pass) moves its phase +0x10 by |v| * [0x49f7b4] (1/6000) per update (AI Mac 0x16d1ef0: 2x).
        {kFixFx, 0x49f60cu, 0x3ada740fu, 0x3a5a740fu, "fx_glint_decay_2e1230"},
        {kFixFx, 0x49f610u, kSixtieth, kHundredTwentieth, "fx_glint_timer_2e1240"},
        {kFixFx, 0x49f628u, kSixtieth, kHundredTwentieth, "fx_glint_dt_2e1384"},
        {kFixFx, 0x49f688u, kSixtieth, kHundredTwentieth, "fx_fader_down_2e1fc8"},
        {kFixFx, 0x49f68cu, 0x3c23d70bu, 0x3ba3d70bu, "fx_fader_up_2e1fe0"},
        {kFixFx, 0x49f7b4u, 0x392ec33eu, 0x38aec33eu, "wake_scroll_2ef8bc"},
        // FH20 hudfill: the race-HUD trick-combo readout (HUD object +0x70 of 0x16bf728, gated by +0x6c)
        // fills by [gp-0x570C] = [0x49d9e4] (1/48) per update at 0x1ebf70 (store 0x1ebf98), clamped at 1.0,
        // inside the race-HUD update sub_0x1e9a30 (writer 0x1ebf98, ra 0x1ebb6c). The pool word's only
        // reader in the whole codegen is 0x1ebf70 (class b, single reader -> halve). KD lab: stock
        // +0.0208333/stock-tick, events +0.0416666 (ratio 2.000); halved: 1.000x (fh20-E21).
        {kFixHudfill, 0x49d9e4u, 0x3caaaaabu, 0x3c2aaaabu, "hudfill_1ebf70"},
    }};
    uint64_t mask = fixMask();
    if ((mask & kFixTimers) != 0u && (mask & kFixRng) == 0u)
        mask |= kFixRamp;
    std::vector<Word> all;
    for (const Word &w : words)
        if (w.fix == 0u || (mask & w.fix) != 0u)
            all.push_back(w);
    // FH12 words (own mask). recover: the wipeout state 0x12cb68 decays the recovery bar P+0x2c0+0x70 by
    // [0x49b914]/[0x49b918] (+-1/240, single readers 0x12cbc4/0x12cbdc) per update; a circle press adds
    // 17*[0x49b928] once per press (edge: held 818 ms adds once, fh12-R1), so only the decay is halved.
    // pulse: 0x2e39d8 advances the rider colour pulse angle P+0xb04 by 2pi*[0x49f6c0] (1/60, single
    // reader 0x2e3a10) per update: 2 Hz at 120 (0x144acc4 +0.10472 -> +0.20944 rad per stock tick, fh12-L1/L2).
    // crash: in a wipeout the rider body is advanced by the fixed-step motion solver 0x113648 (quantum and
    // z_up already converted), but its callers pass the time to advance as a private 1/60: 0x137750
    // (R+0x300 * [0x49be78], the case run in the Kick Doubt heat-1 wipeout) and 0x1391a8 ([0x49bef4],
    // [0x49befc], same class, not seen live). Each is a single reader. At 120 the solver ran two quanta
    // per update and the crashed rider fell and slid 2x (vz -31.67 per update; stock -31.67 per stock
    // tick); with 1/120: -15.83 per update, position within stock per stock tick (fh12-C5/C6/C7).
    // bounce (FH22): sub_0x13d818 advances the rider bounce phase S+0 (MN1 0x1464e30) by
    // f20 * [0x49c13c] (2.65, single reader 0x13f0d4) per update, wraps at 2pi, and stores
    // 7.5 * f20 * sin(phase) to R+0x31c, which sub_0x11eb98 adds (when > 0) along R+0x370 to the rider
    // pose. At 120 the phase ran 2x per stock tick (+0.148 -> +0.295); 1.325 restores stock frequency.
    // crashbody (FH23): the wipeout body B (0x144a1f0 on Kick Doubt, R = [B+0x40]) is stepped by 0x136f30
    // with f4 = R+0x300 * [0x49be38] (1/60, single reader 0x136f48): R+0x130 += v*f4 (store 0x136f8c),
    // v += (-0.2vx, -0.2vy, -1800)*f4, spin *= 1 - 0.5*f4, quaternion R+0x140 integrated by f4. The board
    // bone (23, 0x152de50) and the 8 board-spray objects (0x1456a60, stride 0x210) follow R+0x130. At 120
    // the body moved a full stock step per update (16 -> 2x per stock tick, fh23-W4/W5); 1/120: 1.00x (E2).
    // uber (FH23): 0x117fe0 slews HUD message 9 (the uber meter, slot +0x57c) toward 1 - R+0x2f0*[0x49b698]
    // by [0x49b69c] (1/60, single reader 0x118cfc) per update; in a wipeout it falls to -1 and the slot is
    // dropped (the meter greys). Events greyed ~20 stock ticks early (s6321 vs 6341, fh23-S1/E1), like
    // FH10's meter messages 5/6.
    // popups (FH24): the HUD timed-slot step 0x116fb8 (only caller 0x117f70) adds [0x49b5d8] (1/60, single
    // reader 0x116fe4) to slot +8 per update and drops the slot at +4 (1.5 s): the crash score popups
    // (slots 35/36) left in half the time at 120 (fh24-S1b/E1b +1/60 vs +1/30 per stock tick).
    // snddt (FH24): the race calls the sound-tree update 0x285bf8 at 0x22c014 with dt = int(S+0xac) *
    // [0x49df10] (1/60, single reader 0x22c008; S+0xac = 1 per update), passed through 0x2ab958/0x2ab7a0/
    // 0x2ab6b0 to the tree's timers: e.g. the 5.0 s fade started at the wipeout (0x5c5c10+0x188, 0x2a68b0).
    // hudprog (FH24): the race-HUD progress table 0x4c8bc8 (stride 0x18, per rider; read by 0x20fb40 from
    // the race-HUD update 0x1e9a30) follows its targets with per-update slew limits: progress +8 by
    // +-[0x49dc44] (138.9, single reader 0x210694, negated in place) in 0x210618, the gap +0x10 by
    // [0x49dc08] / [0x49dc04] (+-46.3, single readers 0x20eef8 / 0x20ee78) in 0x20eda0. After the wipeout
    // the readout caught up at 2x (fh24-E1b); all three halved: 1.000x (fh24-V2).
    const std::array<std::pair<uint32_t, Word>, 14> words12 = {{
        {kFix12Crash, {0u, 0x49be78u, kSixtieth, kHundredTwentieth, "crash_dt_137754"}},
        {kFix12Crash, {0u, 0x49bef4u, kSixtieth, kHundredTwentieth, "crash_dt_13940c"}},
        {kFix12Crash, {0u, 0x49befcu, kSixtieth, kHundredTwentieth, "crash_dt_1394f4"}},
        {kFix12Recover, {0u, 0x49b914u, 0x3b88a358u, 0x3b08a358u, "recover_decay_12cbc4"}},
        {kFix12Recover, {0u, 0x49b918u, 0xbb88a358u, 0xbb08a358u, "recover_decay_12cbdc"}},
        {kFix12Pulse, {0u, 0x49f6c0u, kSixtieth, kHundredTwentieth, "pulse_step_2e3a10"}},
        {kFix12Bounce, {0u, 0x49c13cu, 0x4029999au, 0x3fa9999au, "bounce_phase_13f0d4"}},
        {kFix12CrashBody, {0u, 0x49be38u, kSixtieth, kHundredTwentieth, "crashbody_dt_136f48"}},
        {kFix12Uber, {0u, 0x49b69cu, kSixtieth, kHundredTwentieth, "uber_slew_118cfc"}},
        {kFix12Popups, {0u, 0x49b5d8u, kSixtieth, kHundredTwentieth, "popups_step_116fe4"}},
        {kFix12SndDt, {0u, 0x49df10u, kSixtieth, kHundredTwentieth, "snddt_22c008"}},
        {kFix12HudProg, {0u, 0x49dc44u, 0x430ae38fu, 0x428ae38fu, "hudprog_slew_210694"}},
        {kFix12HudProg, {0u, 0x49dc08u, 0x42392f69u, 0x41b92f69u, "hudprog_gap_up_20eef8"}},
        {kFix12HudProg, {0u, 0x49dc04u, 0xc2392f69u, 0xc1b92f69u, "hudprog_gap_down_20ee78"}},
    }};
    for (const auto &w : words12)
        if ((fixMask12() & w.first) != 0u)
            all.push_back(w.second);
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
// FH14 (FIX=entry): the race start is an exception to the grounded rule. The
// race time [race+0xc] restarts at the gate drop-in, and the rider pass, the
// selector dispatch and the race time all first run in that same update, with
// every rider already dropping (selector 1): All-Peak drop 4380, landing 4408,
// so entry waited ~0.47 s (FV1). When the race time bump leaves it at <= 1
// (the first race update; a resume after pause continues from its old value),
// entry is requested right there, mid-update: commit at the next VBlankStart
// and flip at the next dispatch, so only the drop's first update runs at 60.
inline constexpr uint32_t kAppUpdateSite = 0x3171b4u;
// FH7 RNG cadence state (FIX=rng, see rngHook); reset at an events entry flip.
inline uint32_t g_rngUpdates = 0u, g_lcgSaved = 0u;
inline bool g_rngOdd = false, g_lcgHeld = false;
// DL1 draw-limiter pattern counter (see drawHook); reset at an events entry flip.
inline uint32_t g_drawK = 0u;
inline uint64_t g_drawn = 0u, g_drawSkipped = 0u;
inline constexpr uint32_t kSelectorDispatch = 0x111408u;
inline bool g_passRan = false, g_anyAir = false, g_finished = false, g_clockRan = false;
inline bool g_startEdge = false;

inline bool entryFix() noexcept
{
    static const bool on = eventsMode() && (fixMask() & kFixEntry) != 0u;
    return on;
}

inline bool clockSignFix() noexcept
{
    static const bool on = enabled() && (fixMask() & kFixClockSign) != 0u;
    return on;
}

inline void restamp10s(uint8_t *ram, uint32_t a, bool toActive)
{
    uint32_t stamp = 0u, upd = 0u;
    if (!rd32(ram, kPeriod10sStamp, stamp) || !rd32(ram, a + 0x1cu, upd))
        return;
    uint32_t next = 0u;
    if (toActive)
    {
        // Elapsed stock updates E of 601 -> 2E of 1202; the game still fires at
        // (A1c - stamp) >= 601, so stamp' = A1c + 601 - 2E (no further shift).
        next = ps2_fh1_restamp::enter(upd, stamp);
        g_lastStamp10s = next;
    }
    else
    {
        // Remaining 120 Hz updates R = 601 - (A1c - stamp) -> R/2 stock. The
        // active stamp is usually in the future (signed compare at 0x1e1490):
        // FIX clocksign (FH26) keeps that negative elapsed; without it the FH5
        // clamp leaves 300 stock updates (FCR1 §3).
        next = clockSignFix() ? ps2_fh1_restamp::exitSigned(upd, stamp) : ps2_fh1_restamp::exitClamped(upd, stamp);
    }
    static uint32_t lines = 0u;
    if (tapOn() && lines++ < 64u)
        std::fprintf(stderr, "fh1-restamp10s %s upd=%u stamp=%u next=%u remaining_stock=%d\n", toActive ? "enter" : "exit",
                     upd, stamp, next, toActive ? ps2_fh1_restamp::stockRemaining(upd, next) / 2
                                                : ps2_fh1_restamp::stockRemaining(upd, next));
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
    g_rngUpdates = 0u;
    g_drawK = 0u;
    g_rngOdd = false;
    g_lcgHeld = false;
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
    {
        g_clockRan = true;
        uint32_t raceTime = 0u;
        // The bump's store sits in the call's delay slot, so +0xc is already the new value.
        if (entryFix() && ctx && !g_schedActive && rd32(ram, getRegU32(ctx, 16) + 0xcu, raceTime) && raceTime <= 1u)
        {
            g_schedActive = true;
            g_startEdge = true;
            static uint32_t lines = 0u;
            if (lines++ < 64u)
                std::fprintf(stderr, "fh1-events request enter tick=%llu start race_time=%u pass=%d air=%d\n",
                             static_cast<unsigned long long>(g_lastTick), raceTime, g_passRan ? 1 : 0,
                             g_anyAir ? 1 : 0);
        }
    }
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
        // A start-edge request stands until its flip (the drop is airborne; the flip precedes this test).
        const bool want = g_passRan && g_clockRan && (g_guestActive || g_startEdge || !g_anyAir);
        if (g_guestActive || !want)
            g_startEdge = false;
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
// ---- RNG cadence, PS2X_SSX3_FULL120_FIX=rng (FH7; FA1 2) --------------------
// Update parity counts app-update dispatches (0x3171b4; reset at an events
// entry flip, so the first 120 Hz update is even). On odd updates:
// - class d, owners at stock cadence (their return value is unused):
//   0x3710d0 trail/ribbon ring push (stream 0 at 0x3711c8; the ring keeps
//   its stock time span) and 0x115d48 rider look-at-rival (stream-1 pair
//   0x115e4c/0x115fa4, 99 % of stream 1 in the race; the AI freestyle roll
//   0x10c1dc reads stream 1 after them, so its outcome follows stock order);
// - class g, per-update Bernoulli rolls on even updates only: the draw at
//   0x2f3be0 (lens-drop split) and 0x390c98 (light-flash start) is skipped
//   and returns 0x7fffff (u = 1 - 2^-23: a miss for both tests, u*n/24 < 0.01
//   and u < k^2 with k < 1), and the flash countdown [gp+0x15c4] holds;
// - the inline float LCG 0x4a3afc (gp+0xa0c; 18 effect functions) is saved
//   at the start of an odd update and restored at the start of the next,
//   so the odd update's draws replay and the sequence advances once per
//   stock frame.
inline constexpr uint32_t kRngDraw0 = 0x3177f0u;
inline constexpr uint32_t kTrailPush = 0x3710d0u, kLookAt = 0x115d48u;
inline constexpr uint32_t kLensRollSite = 0x2f3be0u, kFlashRollSite = 0x390c98u, kFlashSched = 0x390c60u;
inline constexpr uint32_t kFlashCountdown = 0x4a30f0u + 0x15c4u, kInlineLcg = 0x4a30f0u + 0xa0cu;

inline bool rngFix() noexcept
{
    static const bool on = enabled() && (fixMask() & kFixRng) != 0u;
    return on;
}

// FH7 trick group: 0x117638 (single caller 0x11a3f0 in 0x11a228) adds a
// point award times a multiplier clamp((count+10)*k, 0.5, 2) where count
// (+0x9c) is bumped per call; it runs once per update while a combo accrues,
// so at 120 the count ramps and the points add up twice as fast. Serviced on
// even updates (class d); lab fh7-n3 (env SKIP): All-Peak first-cliff combo
// +420 = stock (was +1330 in FH4).
inline constexpr uint32_t kComboSite = 0x11a3f0u, kComboAccrue = 0x117638u;

// FH8 bonus group: 0x119708 (trick bonus, from the trick animation machine
// 0x1352a8 on an anim event) adds a lump to the in-air points float +0x14 of
// the rider trick state and stores a points rate in +0x3c; 0x117c28 then adds
// +0x3c to +0x14 once per update while the +0x40 timer runs (0x117d38-64), so
// at 120 the post-bonus accrual ran 2x per second. The only reader of +0x3c as
// a rate is 0x117d40 (0x117838 zeroes it), so the stored rate is halved right
// after the store, at 0x119708's own `jal 0x1176f8` (0x1197ac; s1 = state):
// exact (a power-of-two scale), class b.
inline constexpr uint32_t kBonusSite = 0x1197acu, kBonusNext = 0x1176f8u;

// FH8 lift group: GameCamera::TerrainLift 0x15ee00 (once per update) moves the
// lift cam+0x460 toward the terrain-clear height by a bounded step per update:
// f21 = 300/x and f20 = 50/x (x = max(ratio, 0.05), 0x15efa8/0x15efc0), used
// only in the step at 0x15f138-0x15f1cc after the probe calls. At 120 the lift
// rose up to 42.5 per stock frame vs stock's 22.1 (fh8-s4/s5, slopestyle).
// Class j (CM2 4 "halve the per-step caps"): f20/f21 are halved at the probe's
// `jal 0x32e100` (0x15f0ec; both are callee-saved and read only by the step).
// The step's "> 150 -> no step" rule stays at 150 per update, i.e. 300 per stock
// frame; a stock step never came near it (max 22.1 in fh8-s4).
// Not in `all`: on the slopestyle lab (fh8-s5 vs fh8-s6, same state) the only
// fast lift ramp (events stock-time 4494-4502, a line stock never rode) peaked
// at 319 with it vs 348 without; no window showed the bounds binding. Opt in
// with FIX=...,lift.
inline constexpr uint32_t kLiftProbeSite = 0x15f0ecu, kLiftProbe = 0x32e100u;

inline bool liftFix() noexcept
{
    static const bool on = enabled() && (fixMask() & kFixLift) != 0u;
    return on;
}

inline void liftHook(R5900Context *ctx, uint32_t sourcePc, uint32_t targetPc)
{
    if (sourcePc != kLiftProbeSite || targetPc != kLiftProbe || !ctx)
        return;
    ctx->f[20] *= 0.5f;
    ctx->f[21] *= 0.5f;
}

inline bool bonusFix() noexcept
{
    static const bool on = enabled() && (fixMask() & kFixBonus) != 0u;
    return on;
}

inline void bonusHook(uint8_t *ram, R5900Context *ctx, uint32_t sourcePc, uint32_t targetPc)
{
    if (sourcePc != kBonusSite || targetPc != kBonusNext || !ctx)
        return;
    const uint32_t rate = getRegU32(ctx, 17) + 0x3cu;
    uint32_t bits = 0u;
    if (!rd32(ram, rate, bits))
        return;
    float v = 0.0f;
    std::memcpy(&v, &bits, 4);
    v *= 0.5f;
    std::memcpy(&bits, &v, 4);
    wr32(ram, rate, bits);
}

// FH9 aigate group: the AI ground handlers gate decisions on the race tick,
// which runs at stock cadence (FH2 raceclock) while the handlers run per
// update, so at 120 each gate passed on both updates of a race tick (FH9 tap:
// 1.92x passes per stock second):
//   0x10bdcc  race_tick % 12  0x10bd10 target rescan (6 candidates -> E70)
//   0x10da6c  race_tick % 6   0x10da10 react roll: a rider within 200 in anim
//                             0xd -> stream-1 draw 0x10db3c (rand%100 < 25 ->
//                             F34), hold F30 = 2 s
//   0x10dc30  race_tick % 6   0x10dbf0 rider scan (sets +0xf4 = race tick)
//   0x10b8ac  race_tick % 20  0x10b790 steer-bias refresh (2 stream-1 draws)
// On odd updates the race-tick read at those four sites is skipped and returns
// 1 (1 % N != 0; v0 is used only by the modulo), so each gate passes once per
// race tick, as at 60 (class g). The F30 hold countdown in 0x10da10 (the gate
// is skipped while F30 > 0) steps by 1/60 per update from the single-reader
// pool word 0x49b40c; it goes to 1/120 (class e).
// Alone (fh9-v1) it brought the passes to 0.96x of stock per stock second with
// the race outcome unchanged; with takeoff (fh9-p2 vs p1) the AI state sequence
// matches within 3 stock frames. In `all` as the stock-cadence form of the gates.
inline constexpr uint32_t kRaceTickGet = 0x1298c8u;

inline bool aiGateFix() noexcept
{
    static const bool on = enabled() && (fixMask() & kFixAiGate) != 0u;
    return on;
}

inline bool aiGateHook(R5900Context *ctx, uint32_t sourcePc, uint32_t targetPc)
{
    if (!g_rngOdd || targetPc != kRaceTickGet || !ctx)
        return false;
    if (sourcePc != 0x10bdccu && sourcePc != 0x10da6cu && sourcePc != 0x10dc30u && sourcePc != 0x10b8acu)
        return false;
    SET_GPR_U32(ctx, 2, 1u);
    return true;
}

inline bool trickFix() noexcept
{
    static const bool on = enabled() && (fixMask() & kFixTrick) != 0u;
    return on;
}

inline bool rngFix() noexcept;

// Update parity for the stock-cadence groups (rng, trick): counted at the
// app-update dispatch, reset at an events entry flip.
inline void parityHook(uint8_t *ram, uint32_t sourcePc)
{
    if (sourcePc != kAppUpdateSite)
        return;
    g_rngOdd = (g_rngUpdates++ & 1u) != 0u;
    if (!rngFix())
        return;
    if (g_rngOdd)
        g_lcgHeld = rd32(ram, kInlineLcg, g_lcgSaved);
    else if (g_lcgHeld)
    {
        wr32(ram, kInlineLcg, g_lcgSaved);
        g_lcgHeld = false;
    }
}

inline bool rngHook(uint8_t *ram, R5900Context *ctx, uint32_t sourcePc, uint32_t targetPc)
{
    if (!g_rngOdd)
        return false;
    if (targetPc == kTrailPush || targetPc == kLookAt)
        return true;
    if (targetPc == kRngDraw0 && (sourcePc == kLensRollSite || sourcePc == kFlashRollSite))
    {
        if (ctx)
            SET_GPR_U32(ctx, 2, 0x007fffffu);
        return ctx != nullptr;
    }
    if (targetPc == kFlashSched)
    {
        uint32_t n = 0u;
        if (rd32(ram, kFlashCountdown, n) && static_cast<int32_t>(n) > 0)
            wr32(ram, kFlashCountdown, n + 1u);
    }
    return false;
}

// ---- FH13 rclock: the render-frame clock at stock cadence -------------------
// The render device ([gp+0x2a90], vtable [+0x10d8]) counts rendered frames at
// +0x5a74 (++ at 0x382b30, once per render) and hands it out only through the
// vtable getter 0x395510 (slot +0x394, `jr ra; lw v0, 0x5a74(a0)`); 17 game
// functions call it as a clock: the plant sway 0x38ec40 (phase = frames *
// freq / 600), frame deadlines (0x1a38c0 stores frames + 14, 0x1a39f0 tests
// them), the World request queue 0x3a8668 (waits 5 frames), rider/race code.
// At 120 it counts 120 (or DRAW_HZ) per stock second, so every one of them ran
// fast. Group rclock answers the getter with a stock-cadence count S instead
// (the call is skipped, v0 = S): while the hooks are on, S = S_entry + elapsed
// stock ticks; outside, S = frames - offset, the offset frozen at exit, so S
// is continuous across flips and deadlines stored in S units stay valid.
// Render-internal code never reads +0x5a74 (codegen grep: 0x37be38 clears it,
// 0x382b20/30 bump it, 0x395514 is the getter), so rendering is unaffected.
inline constexpr uint32_t kRenderFrameGet = 0x395510u;
inline constexpr uint32_t kRenderFrameOff = 0x5a74u;
inline bool g_rcActive = false;
inline int64_t g_rcOffset = 0, g_rcEntryS = 0;
inline uint64_t g_rcEntryHalf = 0u;

inline bool rclockFix() noexcept
{
    static const bool on = enabled() && (fixMask() & kFixRclock) != 0u;
    return on;
}

// Stock half-periods elapsed: the events accumulator (FH17: the exact one
// under clock2; the FH5 one counts the exit VBlank's half interval as full),
// or the 120 Hz VBlank count in always mode.
inline uint64_t rcHalfNow() noexcept
{
    return mode() == Mode::Events ? stockHalfTicks() : g_lastTick;
}

// Returns true when the getter call is answered here (v0 = S, call skipped).
inline bool rclockHook(uint8_t *ram, R5900Context *ctx, uint32_t targetPc)
{
    if (targetPc != kRenderFrameGet || !ctx)
        return false;
    const bool on = hooksOn();
    if (!on && !g_rcActive && g_rcOffset == 0)
        return false; // before the first entry the getter runs as in stock
    uint32_t c = 0u;
    if (!rd32(ram, getRegU32(ctx, 4) + kRenderFrameOff, c))
        return false;
    const uint64_t h = rcHalfNow();
    if (on && !g_rcActive)
    {
        g_rcEntryS = static_cast<int64_t>(c) - g_rcOffset;
        g_rcEntryHalf = h;
        g_rcActive = true;
    }
    int64_t sv = 0;
    if (g_rcActive)
    {
        sv = g_rcEntryS + static_cast<int64_t>((h - g_rcEntryHalf) / 2u);
        if (!on)
        {
            g_rcOffset = static_cast<int64_t>(c) - sv;
            g_rcActive = false;
        }
    }
    else
        sv = static_cast<int64_t>(c) - g_rcOffset;
    SET_GPR_U32(ctx, 2, static_cast<uint32_t>(sv));
    return true;
}

// ---- Draw limiter, PS2X_SSX3_DRAW_HZ=60|90|120 (DL1; default 120 = off) ----
// While full120 runs (always mode, or events mode with the guest flip
// active), the sim still updates 120 times per guest second but only
// DRAW_HZ of those updates render: on a skipped update the main loop's call
// to the app-render slot 0x22b008 (0x317208 mode-1 gate, 0x31723c mode-0
// gate) is not made and returns 1, so cAppMan_mainLoop takes its "rendered"
// path exactly as at 120 (checkHalt 0x317328, the A+0x2c rate EMA with
// $s1 = 1, $s1 cleared). Nothing is built or kicked for that update (no
// VU1 lists, GS or GPU work); the display keeps the last drawn frame and
// the per-VBlank present shows it again. Pattern over 120 updates: draw
// update k iff floor((k+1)*HZ/120) > floor(k*HZ/120) (60: every 2nd;
// 90: 3 of 4, i.e. holds of 2,1,1 vsyncs on a 120 Hz panel). The counter
// resets at an events entry flip. Stock 60 (outside events) is untouched.
inline constexpr uint32_t kRenderGateMode1 = 0x317208u, kRenderGateMode0 = 0x31723cu;

inline uint32_t drawHz() noexcept
{
    static const uint32_t hz = [] {
        const char *v = std::getenv("PS2X_SSX3_DRAW_HZ");
        if (!v || !*v || std::strcmp(v, "120") == 0)
            return 120u;
        if (std::strcmp(v, "60") == 0)
            return 60u;
        if (std::strcmp(v, "90") == 0)
            return 90u;
        std::fprintf(stderr, "fh1-full120-refused PS2X_SSX3_DRAW_HZ=%s (want 60|90|120)\n", v);
        std::abort();
    }();
    return hz;
}

inline bool drawLimit() noexcept
{
    static const bool on = [] {
        const bool r = enabled() && drawHz() != 120u;
        if (r)
            std::fprintf(stderr, "fh1-draw-limit hz=%u (full120 renders %u of 120 updates)\n", drawHz(), drawHz());
        return r;
    }();
    return on;
}


// Returns true when this render call is skipped (v0 = 1 set).
inline bool drawHook(R5900Context *ctx, uint32_t sourcePc, uint32_t targetPc)
{
    if (targetPc != kRenderTarget || (sourcePc != kRenderGateMode0 && sourcePc != kRenderGateMode1) || !ctx)
        return false;
    const uint32_t hz = drawHz();
    const uint32_t k = g_drawK;
    g_drawK = (k + 1u) % 120u;
    if ((k + 1u) * hz / 120u > k * hz / 120u)
    {
        ++g_drawn;
        return false;
    }
    ++g_drawSkipped;
    SET_GPR_U32(ctx, 2, 1u);
    return true;
}

// PS2X_FH1_SRC=tgt[:a0][,...] (hex; diagnostic, any mode): print the source
// pc, a0-a3 and f0/f1/f12/f20/f21 (FH26: then f13/f14) of calls to tgt (optionally only with that
// a0), 2000 lines max.
struct SrcWant { uint32_t tgt, a0; bool anyA0; };

inline const std::vector<SrcWant> &srcWants()
{
    using Want = SrcWant;
    static const std::vector<Want> wants = [] {
        std::vector<Want> v;
        const char *p = std::getenv("PS2X_FH1_SRC");
        while (p && *p)
        {
            char *end = nullptr;
            Want w{static_cast<uint32_t>(std::strtoul(p, &end, 16)), 0u, true};
            if (*end == ':')
            {
                w.a0 = static_cast<uint32_t>(std::strtoul(end + 1, &end, 16));
                w.anyA0 = false;
            }
            v.push_back(w);
            if (*end != ',') break;
            p = end + 1;
        }
        return v;
    }();
    return wants;
}

inline void srcTap(uint8_t *ram, R5900Context *ctx, uint32_t sourcePc, uint32_t targetPc)
{
    const std::vector<SrcWant> &wants = srcWants();
    if (wants.empty() || !ctx)
        return;
    static uint32_t lines = 0u;
    for (const SrcWant &w : wants)
        if (w.tgt == targetPc && (w.anyA0 || getRegU32(ctx, 4) == w.a0) && lines < 2000u)
        {
            ++lines;
            std::fprintf(stderr,
                         "fh1-src tick=%llu src=%08x tgt=%08x a0=%08x a1=%08x a2=%08x a3=%08x odd=%d f0=%g f1=%g f12=%g "
                         "f20=%g f21=%g f13=%g f14=%g\n",
                         static_cast<unsigned long long>(g_lastTick), sourcePc, targetPc, getRegU32(ctx, 4),
                         getRegU32(ctx, 5), getRegU32(ctx, 6), getRegU32(ctx, 7), g_rngOdd ? 1 : 0, ctx->f[0], ctx->f[1],
                         ctx->f[12], ctx->f[20], ctx->f[21], ctx->f[13], ctx->f[14]);
        }
    (void)ram;
}

// PS2X_FH26_TAP=<S>[:<from>-<to>] (hex S, decimal VBlank ticks; diagnostic, any mode with
// PS2X_FH1_TAP=1 or full120): at each call of the stick-angle tracker 0x133308 with a0 == S (P+0x3f0),
// before the call, one line: tick, update A+0x1c, stock half-ticks, active, the input word [a1+0],
// S+0..+0x5c and, for R = [S+0x58], R+0x1e0..+0x1e8 (velocity), R+0x2f4, R+0x438, R+0xde0..+0xde4.
// Raw hex words; decode offline. 8000 lines max.
struct Fh26Tap
{
    uint32_t s = 0u;
    uint64_t from = 0u, to = ~0ull;
};

inline const Fh26Tap &fh26Tap()
{
    static const Fh26Tap t = [] {
        Fh26Tap r;
        const char *p = std::getenv("PS2X_FH26_TAP");
        if (!p || !*p)
            return r;
        char *end = nullptr;
        r.s = static_cast<uint32_t>(std::strtoul(p, &end, 16));
        if (*end == ':')
        {
            r.from = std::strtoull(end + 1, &end, 10);
            if (*end == '-')
                r.to = std::strtoull(end + 1, &end, 10);
        }
        return r;
    }();
    return t;
}

inline void fh26TapCall(uint8_t *ram, R5900Context *ctx, uint32_t targetPc)
{
    const Fh26Tap &t = fh26Tap();
    if (targetPc != 0x133308u || !ctx || getRegU32(ctx, 4) != t.s || g_lastTick < t.from || g_lastTick > t.to)
        return;
    static uint32_t lines = 0u;
    if (lines++ >= 8000u)
        return;
    char line[1024];
    int n = 0;
    uint32_t a = 0u, upd = 0u, in = 0u, r = 0u, w = 0u;
    if (rd32(ram, kMgrPtr, a) && a)
        rd32(ram, a + 0x1cu, upd);
    rd32(ram, getRegU32(ctx, 5), in);
    rd32(ram, t.s + 0x58u, r);
    n += std::snprintf(line + n, sizeof(line) - n, "fh26-s tick=%llu upd=%u half=%llu act=%d in=%08x S",
                       static_cast<unsigned long long>(g_lastTick), upd,
                       static_cast<unsigned long long>(stockHalfTicks()), g_guestActive ? 1 : 0, in);
    for (uint32_t o = 0u; o < 0x60u; o += 4u)
    {
        rd32(ram, t.s + o, w);
        n += std::snprintf(line + n, sizeof(line) - n, " %08x", w);
    }
    n += std::snprintf(line + n, sizeof(line) - n, " R=%08x", r);
    for (uint32_t o : {0x1e0u, 0x1e4u, 0x1e8u, 0x2f4u, 0x438u, 0xde0u, 0xde4u})
    {
        w = 0u;
        if (r)
            rd32(ram, r + o, w);
        n += std::snprintf(line + n, sizeof(line) - n, " %08x", w);
    }
    std::fprintf(stderr, "%s\n", line);
}

// ---- FH9 AI animation/gate tap (PS2X_FH9_AI=<rider R, hex>; diagnostic, observation-only) ----
// Needs PS2X_FH1_TAP=1 (window = PS2X_FH1_TAP_EVERY). For the rider R (P = [R+0x77c], anim controller
// ctrl = [R+0x784], channel ch's tracks = [[ctrl+0x50]+8ch+4] chained by +0xc8) it counts, per window:
// control-state (P+0xde4) entries via 0x111538 (logged with the caller), the sub-state-7 gate 0x12e8e8
// (0x312ae8(ctrl, 2) = track+0xc0 "reached the end this advance"; logged with P+0xde0 when set),
// per-channel advances 0x3135b0 (a1 = 0; summed step rate*speed*f12), end hits 0x3139a8, event bits the
// three setters (0x313868 crossing, 0x313938 exact, 0x3139a8 end) set on those tracks (replicated from
// the node list [track+0xac]: +0 bit, +4 end flag, +8 time, +0xc next), and the AI race_tick % N gates
// (0x10b8ac %20 in 0x10b790, 0x10bdcc %12, 0x10da6c %6, 0x10dc30 %6): evaluations and passes.
struct Fh9Tap
{
    uint32_t r = 0u;
    uint32_t st[16]{};
    uint32_t g12e8 = 0u, g12e8Flag = 0u, g12e8Flag4 = 0u;
    uint32_t adv[8]{};
    float step[8]{};
    uint32_t endc[8]{};
    uint32_t bits[8][64]{};
    uint32_t gateCall[4]{}, gatePass[4]{};
    uint32_t rng1[8]{};
    uint32_t wrapSrc = 0u;
    uint32_t lines = 0u;
};

inline Fh9Tap &fh9Tap()
{
    static Fh9Tap t = [] {
        Fh9Tap x;
        if (const char *v = std::getenv("PS2X_FH9_AI"))
            x.r = static_cast<uint32_t>(std::strtoul(v, nullptr, 16));
        return x;
    }();
    return t;
}

inline int fh9Channel(const uint8_t *ram, uint32_t ctrl, uint32_t track) noexcept
{
    uint32_t base = 0u;
    if (!rd32(ram, ctrl + 0x50u, base) || !base)
        return -1;
    for (int ch = 0; ch < 8; ++ch)
    {
        uint32_t t = 0u;
        if (!rd32(ram, base + 8u * static_cast<uint32_t>(ch) + 4u, t))
            return -1;
        for (int d = 0; d < 4 && t; ++d)
        {
            if (t == track)
                return ch;
            if (!rd32(ram, t + 0xc8u, t))
                break;
        }
    }
    return -1;
}

inline float fh9F(const uint8_t *ram, uint32_t a) noexcept
{
    uint32_t w = 0u;
    rd32(ram, a, w);
    float f = 0.0f;
    std::memcpy(&f, &w, 4);
    return f;
}

inline void fh9OnBranch(uint8_t *ram, R5900Context *ctx, uint32_t sourcePc, uint32_t targetPc, bool skipped)
{
    Fh9Tap &t = fh9Tap();
    if (!t.r || !ctx)
        return;
    static const uint32_t gateSite[4] = {0x10b8acu, 0x10bdccu, 0x10da6cu, 0x10dc30u};
    static const uint32_t gateMod[4] = {20u, 12u, 6u, 6u};
    static const uint32_t rngSite[8] = {0x10db3cu, 0x10b8d0u, 0x10b910u, 0x10c1dcu, 0x115e4cu, 0x115fa4u, 0x10b468u, 0u};
    if (targetPc == 0x317810u)
    {
        for (int i = 0; i < 8; ++i)
            if (sourcePc == rngSite[i])
                ++t.rng1[i];
        return;
    }
    if (targetPc == 0x1298c8u)
    {
        for (int i = 0; i < 4; ++i)
            if (sourcePc == gateSite[i])
            {
                uint32_t a = 0u, b = 0u, c = 0u, tick = 0u;
                if (rd32(ram, 0x4a28a8u, a) && rd32(ram, a + 0x84u, b) && rd32(ram, b + 0xcu, c) &&
                    rd32(ram, c + 8u, tick))
                {
                    ++t.gateCall[i];
                    if (!skipped && static_cast<int32_t>(tick) % static_cast<int32_t>(gateMod[i]) == 0)
                        ++t.gatePass[i];
                }
            }
        return;
    }
    uint32_t p = 0u, ctrl = 0u;
    if (!rd32(ram, t.r + 0x77cu, p) || !rd32(ram, t.r + 0x784u, ctrl))
        return;
    const uint32_t a0 = getRegU32(ctx, 4), a1 = getRegU32(ctx, 5);
    uint32_t de0 = 0u, de4 = 0u;
    rd32(ram, p + 0xde0u, de0);
    rd32(ram, p + 0xde4u, de4);
    if (targetPc == 0x11fec8u && a0 == t.r)
    {
        t.wrapSrc = sourcePc;
        return;
    }
    if (targetPc == 0x111538u && a0 == p)
    {
        const uint32_t src = sourcePc == 0x11fed0u ? t.wrapSrc : sourcePc;
        t.wrapSrc = 0u;
        if (a1 < 16u)
            ++t.st[a1];
        if (t.lines++ < 6000u)
            std::fprintf(stderr, "fh9-st tick=%llu sh=%llu src=%06x %u->%u de0=%u\n",
                         static_cast<unsigned long long>(g_lastTick),
                         static_cast<unsigned long long>(stockHalfTicks()), src, de4, a1,
                         de0);
        return;
    }
    if (targetPc == 0x312ae8u && sourcePc == 0x12e8e8u && a0 == ctrl)
    {
        uint32_t base = 0u, track = 0u, flag = 0u;
        rd32(ram, ctrl + 0x50u, base);
        rd32(ram, base + 8u * a1 + 4u, track);
        rd32(ram, track + 0xc0u, flag);
        ++t.g12e8;
        if (flag)
        {
            ++t.g12e8Flag;
            if (de0 == 4u)
                ++t.g12e8Flag4;
            if (t.lines++ < 6000u)
                std::fprintf(stderr, "fh9-g7 tick=%llu sh=%llu de4=%u de0=%u\n",
                             static_cast<unsigned long long>(g_lastTick),
                             static_cast<unsigned long long>(stockHalfTicks()), de4, de0);
        }
        return;
    }
    const bool cross = targetPc == 0x313868u, exact = targetPc == 0x313938u, end = targetPc == 0x3139a8u;
    const bool adv = targetPc == 0x3135b0u;
    if (!cross && !exact && !end && !adv)
        return;
    const int ch = fh9Channel(ram, ctrl, a0);
    if (ch < 0)
        return;
    if (adv)
    {
        if (a1 == 0u)
        {
            ++t.adv[ch];
            t.step[ch] += fh9F(ram, a0 + 0xcu) * fh9F(ram, a0 + 0x90u) * ctx->f[12];
        }
        return;
    }
    if (end)
    {
        ++t.endc[ch];
        if (ch == 2 && t.lines++ < 6000u)
            std::fprintf(stderr, "fh9-end tick=%llu sh=%llu ch=%d de4=%u de0=%u time=%g len=%g\n",
                         static_cast<unsigned long long>(g_lastTick),
                         static_cast<unsigned long long>(stockHalfTicks()), ch, de4, de0,
                         fh9F(ram, a0 + 8u), fh9F(ram, a0 + 0x10u));
    }
    const float f12 = ctx->f[12], f13 = ctx->f[13];
    if (cross && f12 == f13)
        return;
    uint32_t node = 0u;
    rd32(ram, a0 + 0xacu, node);
    for (int guard = 0; node && guard < 64; ++guard)
    {
        uint32_t bit = 0u, isEnd = 0u, next = 0u;
        rd32(ram, node, bit);
        rd32(ram, node + 4u, isEnd);
        rd32(ram, node + 0xcu, next);
        const float tm = fh9F(ram, node + 8u);
        bool hit = false;
        if (end)
            hit = isEnd != 0u;
        else if (isEnd == 0u)
        {
            if (exact)
                hit = tm == f12;
            else if (f12 <= f13)
                hit = f12 < tm && tm <= f13;
            else
                hit = f13 <= tm && tm < f12;
        }
        if (hit && bit < 64u)
            ++t.bits[ch][bit];
        node = next;
    }
}

inline void fh9OnVBlank(uint64_t tick)
{
    Fh9Tap &t = fh9Tap();
    if (!t.r)
        return;
    char line[4096];
    int n = std::snprintf(line, sizeof(line), "fh9-tap tick=%llu sh=%llu g7=%u/%u/%u",
                          static_cast<unsigned long long>(tick),
                          static_cast<unsigned long long>(stockHalfTicks()), t.g12e8,
                          t.g12e8Flag, t.g12e8Flag4);
    for (int k = 0; k < 16; ++k)
        if (t.st[k])
            n += std::snprintf(line + n, sizeof(line) - n, " st%d=%u", k, t.st[k]);
    for (int c = 0; c < 8; ++c)
    {
        if (t.adv[c])
            n += std::snprintf(line + n, sizeof(line) - n, " adv%d=%u:%.4f", c, t.adv[c], t.step[c]);
        if (t.endc[c])
            n += std::snprintf(line + n, sizeof(line) - n, " end%d=%u", c, t.endc[c]);
        for (int b = 0; b < 64 && n < static_cast<int>(sizeof(line)) - 40; ++b)
            if (t.bits[c][b])
                n += std::snprintf(line + n, sizeof(line) - n, " b%d.%d=%u", c, b, t.bits[c][b]);
    }
    static const uint32_t rngSite[7] = {0x10db3cu, 0x10b8d0u, 0x10b910u, 0x10c1dcu, 0x115e4cu, 0x115fa4u, 0x10b468u};
    for (int i = 0; i < 7; ++i)
        if (t.rng1[i])
            n += std::snprintf(line + n, sizeof(line) - n, " r%x=%u", rngSite[i], t.rng1[i]);
    for (int i = 0; i < 4; ++i)
        if (t.gateCall[i])
            n += std::snprintf(line + n, sizeof(line) - n, " gate%d=%u/%u", i, t.gatePass[i], t.gateCall[i]);
    std::fprintf(stderr, "%s\n", line);
    const uint32_t r = t.r, wrap = t.wrapSrc, lines = t.lines;
    t = Fh9Tap{};
    t.r = r;
    t.wrapSrc = wrap;
    t.lines = lines;
}

// FH11 flags group: cFlagManager (alloc "cFlagManager" at 0x22f368, update
// 0x34c668) runs 0x34b818 once per update for each of its 15 flag slots
// (stride 0x188). Its wind clock +0x1c steps by 1/rate (already per time), but
// each flag adds a per-update step with no dt to its phases, each wrapped
// into [0, 1): the four wave phases +0x78..+0x84 (speed +0x4..+0x10 times the
// wind mix) and the scroll phases +0x88/+0x8c (steps +0x50/+0x54). At 120 the
// flags flapped twice per stock second (Brad PB10). Class b: the advance of
// each phase over the call is halved after it returns (post-call hook; the
// in-call wrap is undone first, a step is < 1 per update). The mesh rebuild
// inside the call (0x34bca0, every other update by frame parity) sees the
// full step for that one update: a half-step phase lead, not a rate change.
inline constexpr uint32_t kFlagUpdate = 0x34b818u;
inline constexpr uint32_t kFlagPhaseOffs[6] = {0x78u, 0x7cu, 0x80u, 0x84u, 0x88u, 0x8cu};

inline bool flagsFix() noexcept
{
    static const bool on = enabled() && (fixMask() & kFixFlags) != 0u;
    return on;
}

// One pending post-call record (the hooked callee isn't reentrant): armed by
// a pre-hook in onBranch, consumed by onReturn from dispatchGuestBranch right
// after the callee returns (matched by target and sp, so calls nested inside
// it don't consume it). A callee suspended at a checkpoint disarms it and
// that update keeps its unscaled step (deterministic).
struct PostCall
{
    uint32_t target = 0u, sp = 0u, obj = 0u;
    uint32_t kind = 0u; // 0 flags, 1 texanim UV/rotation, 2 loop controller mode step, 3 fxtimer add-back
    uint32_t saved[6] = {};
};
inline PostCall g_post;
inline bool g_postArmed = false;

inline void flagsPreHook(uint8_t *ram, R5900Context *ctx, uint32_t targetPc)
{
    if (targetPc != kFlagUpdate || !ctx)
        return;
    g_post.target = targetPc;
    g_post.sp = getRegU32(ctx, 29);
    g_post.obj = getRegU32(ctx, 4);
    g_post.kind = 0u;
    for (int i = 0; i < 6; ++i)
        if (!rd32(ram, g_post.obj + kFlagPhaseOffs[i], g_post.saved[i]))
            return;
    g_postArmed = true;
}

inline void fh12OnReturn(uint8_t *ram);

inline void onReturn(uint8_t *ram, R5900Context *ctx, uint32_t targetPc, bool returned)
{
    if (targetPc != g_post.target || !ctx || getRegU32(ctx, 29) != g_post.sp)
        return;
    g_postArmed = false;
    if (!returned)
        return;
    if (g_post.kind != 0u)
    {
        fh12OnReturn(ram);
        return;
    }
    for (int i = 0; i < 6; ++i)
    {
        const uint32_t a = g_post.obj + kFlagPhaseOffs[i];
        uint32_t bits = 0u;
        if (!rd32(ram, a, bits) || bits == g_post.saved[i])
            continue;
        float o = 0.0f, n = 0.0f;
        std::memcpy(&o, &g_post.saved[i], 4);
        std::memcpy(&n, &bits, 4);
        float d = n - o;
        if (d < 0.0f)
            d += 1.0f;
        if (!(d >= 0.0f && d < 1.0f))
            continue;
        float v = o + d * 0.5f;
        if (v >= 1.0f)
            v -= 1.0f;
        std::memcpy(&bits, &v, 4);
        wr32(ram, a, bits);
    }
}

// ---- FH12 hooks (own mask fixMask12; events only, like every hook) ---------
// spin: the rotating-object class (ctor 0x3461c0, vtable 0x491220 at +8) updates through slot 0x49124c ->
// 0x346258: angle +0x30 (degrees) += (float)(int)+0x2c per update, reset to 0 at +-360; the render
// 0x3462a0 builds its matrix from +0x30. Kick Doubt has four live instances (steps +6, +5, -9, -9), each
// 2.00x per stock second in events (Brad PB10/PB11: sky pickups spin too fast; fh12-P1/P2). Pre-hook:
// +0x30 -= step/2, so the update advances half a step (integer steps keep the 360 reset exact).
// texanim: the engine texture anim 0x352c70 calls 0x35f7d0 (time +0x4 += 1/rate is per time; rotation
// +0x10 += +0x14 and, outside mode 6, UV scroll +0x30/+0x34 += +0x38/+0x3c wrapped at +-1, both per
// update) and the flipbook 0x35f410 (phase +0x4 += +0x8, frame on wrap; mode 2 at entry means +0x8 =
// 10/rate, per time; modes 0/1 are per update, mode 1 with the RNG flicker step). Post-call: the
// 0x35f7d0 advances are halved (wrap undone first); pre-hook: the flipbook phase gives back half a step
// unless mode 2. Kick Doubt: ~50 UV words 2.000x (fh12-L1/L2, writers 0x35fa70/74 in w1).
// loops: loop-timer controllers (ctor 0x341aa0 bakes step +0x10 = r/30/A.rate at level load, A.rate 60;
// update 0x341d48 -> mode steps 0x341e48 loop, 0x341ec0 ping-pong, 0x341f38 clamp on +0x1c in
// [+0x14, +0x18], output +0x28). Post-call: the step is recomputed from the saved value with half the
// saved step, same float order as the guest (ping-pong writes back +-step). 2.000x on Kick Doubt
// (writers 0x341e6c/0x341e10, fh12-w1).
inline constexpr uint32_t kSpinUpdate = 0x346258u;
inline constexpr uint32_t kTexUv = 0x35f7d0u;
inline constexpr uint32_t kTexFlip = 0x35f410u;
inline constexpr uint32_t kLoopMode1 = 0x341e48u;
inline constexpr uint32_t kLoopMode2 = 0x341ec0u;
inline constexpr uint32_t kLoopMode3 = 0x341f38u;
// FH21 fxtimer: rider-FX timer updater sub_0x2e2260 (P+0xb44 word at +0x4,
// stored at 0x2e2388, writer pc/ra 0x2e2388/0x2e2350 in the Happiness
// mid-race lab). Its step f21 = [0x49f6a8] (1/60) is also the trail-push dt
// (mov.s f12,f21 -> jal 0x3717c0), so the word must not be halved: the
// pre-hook stashes obj/P+0xb44 and fh12OnReturn (kind 3) adds back half the
// taken step, leaving trails at full rate.
inline constexpr uint32_t kFxTimerUpdate = 0x2e2260u;

inline float rdf(const uint8_t *ram, uint32_t a) noexcept
{
    uint32_t b = 0u;
    rd32(ram, a, b);
    float f = 0.0f;
    std::memcpy(&f, &b, 4);
    return f;
}

inline void wrf(uint8_t *ram, uint32_t a, float f) noexcept
{
    uint32_t b = 0u;
    std::memcpy(&b, &f, 4);
    wr32(ram, a, b);
}

// FH22 particles: the rider particle pass 0x2dd0b8 (per rider from the all-riders loop 0x128f20 and
// from 0x11198c; object R+0x3b0) ages each particle by [0x49f4bc] (1/60, single reader 0x2dd10c),
// moves it by vel * dt and adds a per-update constant (emitter table +0x28) to each segment's z
// velocity. Halving dt alone would leave that add at 2x (gravity doubled), so on odd updates the call
// is skipped: the pass runs at stock cadence with stock arithmetic (class d, like trick/rng).
inline constexpr uint32_t kParticlePass = 0x2dd0b8u, kParticleSiteAll = 0x128f20u, kParticleSiteOne = 0x11198cu;

inline bool particlesFix() noexcept
{
    static const bool on = enabled() && (fixMask12() & kFix12Particles) != 0u;
    return on;
}

inline bool fh12Hooks() noexcept
{
    static const bool on = enabled() && (fixMask12() & (kFix12Spin | kFix12Texanim | kFix12Loops | kFix12FxTimer)) != 0u;
    return on;
}

inline void fh12PreHook(uint8_t *ram, R5900Context *ctx, uint32_t targetPc)
{
    if (!ctx || (targetPc != kSpinUpdate && targetPc != kTexUv && targetPc != kTexFlip &&
                 targetPc != kLoopMode1 && targetPc != kLoopMode2 && targetPc != kLoopMode3 &&
                 targetPc != kFxTimerUpdate))
        return;
    const uint32_t m = fixMask12();
    const uint32_t o = getRegU32(ctx, 4);
    uint32_t w = 0u;
    if (targetPc == kFxTimerUpdate)
    {
        if ((m & kFix12FxTimer) == 0u || !rd32(ram, o + 0x4u, g_post.saved[0]))
            return;
        g_post.kind = 3u;
        g_post.target = targetPc;
        g_post.sp = getRegU32(ctx, 29);
        g_post.obj = o;
        g_postArmed = true;
        return;
    }
    if (targetPc == kSpinUpdate)
    {
        if ((m & kFix12Spin) == 0u || !rd32(ram, o + 0x2cu, w) || w == 0u)
            return;
        wrf(ram, o + 0x30u, rdf(ram, o + 0x30u) - static_cast<float>(static_cast<int32_t>(w)) * 0.5f);
        return;
    }
    if (targetPc == kTexFlip)
    {
        const float step = rdf(ram, o + 0x8u);
        if ((m & kFix12Texanim) == 0u || !rd32(ram, o, w) || w == 2u || !(step > 0.0f))
            return;
        wrf(ram, o + 0x4u, rdf(ram, o + 0x4u) - step * 0.5f);
        return;
    }
    if (targetPc == kTexUv)
    {
        if ((m & kFix12Texanim) == 0u || !rd32(ram, o, w) || w == 6u)
            return;
        g_post.kind = 1u;
        rd32(ram, o + 0x10u, g_post.saved[0]);
        rd32(ram, o + 0x30u, g_post.saved[1]);
        rd32(ram, o + 0x34u, g_post.saved[2]);
    }
    else
    {
        if ((m & kFix12Loops) == 0u)
            return;
        g_post.kind = 2u;
        rd32(ram, o + 0x1cu, g_post.saved[0]);
        rd32(ram, o + 0x10u, g_post.saved[1]);
        g_post.saved[2] = targetPc;
    }
    g_post.target = targetPc;
    g_post.sp = getRegU32(ctx, 29);
    g_post.obj = o;
    g_postArmed = true;
}

inline void fh12OnReturn(uint8_t *ram)
{
    const uint32_t o = g_post.obj;
    auto bitsToF = [](uint32_t b) { float f = 0.0f; std::memcpy(&f, &b, 4); return f; };
    if (g_post.kind == 3u)
    {
        // FH21 fxtimer: P+0xb44 was decremented by its full 1/60 step inside
        // 0x2e2260 after the trail push consumed the same dt. Add back half
        // the taken step so the timer runs at stock cadence per stock tick
        // while trails keep the full dt. Untouched (early-out) words and the
        // 0.0 clamp floor (stock clamps there too) are left alone.
        const float t0 = bitsToF(g_post.saved[0]), t1 = rdf(ram, o + 0x4u);
        if (t1 != t0 && t1 != 0.0f)
            wrf(ram, o + 0x4u, t0 + (t1 - t0) * 0.5f);
        return;
    }
    if (g_post.kind == 1u)
    {
        const float r0 = bitsToF(g_post.saved[0]), r1 = rdf(ram, o + 0x10u);
        if (r1 != r0)
            wrf(ram, o + 0x10u, r0 + (r1 - r0) * 0.5f);
        for (int i = 1; i <= 2; ++i)
        {
            const uint32_t a = o + (i == 1 ? 0x30u : 0x34u);
            const float u0 = bitsToF(g_post.saved[i]), u1 = rdf(ram, a);
            if (u1 == u0)
                continue;
            float d = u1 - u0;
            if (d > 0.5f)
                d -= 1.0f;
            else if (d < -0.5f)
                d += 1.0f;
            float v = u0 + d * 0.5f;
            if (v > 1.0f)
                v -= 1.0f;
            else if (v < -1.0f)
                v += 1.0f;
            wrf(ram, a, v);
        }
        return;
    }
    // kind 2: recompute the loop controller step with half the saved step.
    const float v0 = bitsToF(g_post.saved[0]), s = bitsToF(g_post.saved[1]);
    const float lo = rdf(ram, o + 0x14u), hi = rdf(ram, o + 0x18u);
    const float h = s * 0.5f;
    const float v = v0 + h;
    float out = v, step = s;
    if (g_post.saved[2] == kLoopMode1)
    {
        if (0.0f <= s)
        {
            if (hi < v)
                out = lo + (v - hi);
        }
        else if (v < lo)
            out = hi - (lo - v);
    }
    else if (g_post.saved[2] == kLoopMode2)
    {
        if (0.0f <= s)
        {
            if (hi < v)
            {
                step = -s;
                out = hi - (v - hi);
            }
        }
        else if (v < lo)
        {
            step = -s;
            out = lo + (lo - v);
        }
        wrf(ram, o + 0x10u, step);
    }
    else
    {
        if (0.0f <= s)
        {
            if (hi < v)
                out = hi;
        }
        else if (v < lo)
            out = lo;
    }
    wrf(ram, o + 0x1cu, out);
    wrf(ram, o + 0x28u, v);
}

// ---- FH10 lab tools (env-only, diagnostic) ----------------------------------
// PS2X_FH10_HIST=a-b[,c-d] (VBlank ticks, needs PS2X_FH1_TAP=1): counts every
// dispatched call target inside each range and prints the counts when the
// range ends, "fh10-hist range=a-b tgt=... n=...", largest first (1500 lines
// per range; PS2X_FH10_HIST_MAX=n raises the cap). Comparing a crash, rail or meter window with a plain riding
// window (or stock with events per stock second) names the functions that
// run only there, or once per update.
// PS2X_FH10_POKE=tick:addr:value[,...] (tick decimal, addr/value hex): writes
// the 32-bit word at that VBlank (lab input, e.g. a full meter). It changes
// the guest: lab boots only.
struct Fh10Hist
{
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    std::vector<uint32_t> keys, vals; // open addressing, tgt -> n
};

inline Fh10Hist &fh10Hist()
{
    static Fh10Hist h = [] {
        Fh10Hist r;
        const char *p = std::getenv("PS2X_FH10_HIST");
        while (p && *p)
        {
            char *end = nullptr;
            const uint64_t a = std::strtoull(p, &end, 10);
            if (*end != '-') break;
            const uint64_t b = std::strtoull(end + 1, &end, 10);
            r.ranges.emplace_back(a, b);
            if (*end != ',') break;
            p = end + 1;
        }
        if (!r.ranges.empty())
        {
            r.keys.assign(1u << 16, 0u);
            r.vals.assign(1u << 16, 0u);
        }
        return r;
    }();
    return h;
}

inline void fh10HistBranch(uint32_t targetPc)
{
    Fh10Hist &h = fh10Hist();
    if (h.ranges.empty() || targetPc == 0u)
        return;
    bool in = false;
    for (const auto &r : h.ranges)
        in = in || (g_lastTick >= r.first && g_lastTick <= r.second);
    if (!in)
        return;
    const uint32_t mask = (1u << 16) - 1u;
    for (uint32_t i = (targetPc >> 2) & mask, n = 0; n <= mask; i = (i + 1u) & mask, ++n)
    {
        if (h.keys[i] == targetPc || h.keys[i] == 0u)
        {
            h.keys[i] = targetPc;
            ++h.vals[i];
            return;
        }
    }
}

inline void fh10OnVBlank(uint8_t *ram, uint64_t tick)
{
    static const std::vector<std::array<uint64_t, 3>> pokes = [] {
        std::vector<std::array<uint64_t, 3>> v;
        const char *p = std::getenv("PS2X_FH10_POKE");
        while (p && *p)
        {
            char *end = nullptr;
            const uint64_t t = std::strtoull(p, &end, 10);
            if (*end != ':') break;
            const uint64_t a = std::strtoull(end + 1, &end, 16);
            if (*end != ':') break;
            const uint64_t val = std::strtoull(end + 1, &end, 16);
            v.push_back({t, a, val});
            if (*end != ',') break;
            p = end + 1;
        }
        return v;
    }();
    for (const auto &k : pokes)
        if (k[0] == tick)
        {
            uint32_t old = 0u;
            rd32(ram, static_cast<uint32_t>(k[1]), old);
            wr32(ram, static_cast<uint32_t>(k[1]), static_cast<uint32_t>(k[2]));
            std::fprintf(stderr, "fh10-poke tick=%llu addr=%08llx old=%08x new=%08llx\n",
                         static_cast<unsigned long long>(tick), static_cast<unsigned long long>(k[1]), old,
                         static_cast<unsigned long long>(k[2]));
        }
    // PS2X_FH10_MONO=T1,...,Tk[:lo-hi] (k 3..8, ticks decimal, range hex): at Tk
    // prints every word in [lo,hi) that changed strictly monotonically across
    // the k snapshots, as a float (finite, |v| < 1e7) or as an int (|step| <=
    // 4096): "fh10-mono addr=... f|i=v1,...,vk" (4000 lines). Finds ramps that
    // aren't linear (meter fills, eased counters); compare two modes offline.
    static struct Mono
    {
        std::vector<uint64_t> t;
        uint32_t lo = 0u, hi = PS2_RAM_SIZE;
        std::vector<std::vector<uint32_t>> snap;
    } mono = [] {
        Mono m;
        const char *p = std::getenv("PS2X_FH10_MONO");
        while (p && *p && m.t.size() < 8u)
        {
            char *end = nullptr;
            m.t.push_back(std::strtoull(p, &end, 10));
            if (*end == ':')
            {
                m.lo = static_cast<uint32_t>(std::strtoul(end + 1, &end, 16));
                if (*end == '-') m.hi = static_cast<uint32_t>(std::strtoul(end + 1, &end, 16));
                break;
            }
            if (*end != ',') break;
            p = end + 1;
        }
        if (m.t.size() < 3u)
            m.t.clear();
        m.hi = std::min<uint32_t>(m.hi, PS2_RAM_SIZE);
        return m;
    }();
    for (size_t k = 0; k < mono.t.size(); ++k)
    {
        if (mono.t[k] != tick)
            continue;
        mono.snap.emplace_back((mono.hi - mono.lo) / 4u);
        std::memcpy(mono.snap.back().data(), ram + mono.lo, mono.snap.back().size() * 4u);
        if (k + 1u != mono.t.size() || mono.snap.size() != mono.t.size())
            continue;
        uint32_t hits = 0u;
        const size_t n = mono.snap[0].size(), K = mono.snap.size();
        for (size_t i = 0; i < n && hits < 4000u; ++i)
        {
            bool fUp = true, fDn = true, iUp = true, iDn = true, fin = true;
            for (size_t j = 0; j < K; ++j)
            {
                float f = 0.0f;
                std::memcpy(&f, &mono.snap[j][i], 4);
                fin = fin && std::isfinite(f) && std::fabs(f) < 1e7f;
            }
            for (size_t j = 1; j < K; ++j)
            {
                float a = 0.0f, b = 0.0f;
                std::memcpy(&a, &mono.snap[j - 1][i], 4);
                std::memcpy(&b, &mono.snap[j][i], 4);
                fUp = fUp && b > a;
                fDn = fDn && b < a;
                const int64_t d = static_cast<int64_t>(static_cast<int32_t>(mono.snap[j][i])) -
                                  static_cast<int32_t>(mono.snap[j - 1][i]);
                iUp = iUp && d > 0 && d <= 4096;
                iDn = iDn && d < 0 && d >= -4096;
            }
            const bool asFloat = fin && (fUp || fDn), asInt = iUp || iDn;
            if (!asFloat && !asInt)
                continue;
            ++hits;
            char line[512];
            int w = std::snprintf(line, sizeof(line), "fh10-mono addr=%08zx %s=", mono.lo + i * 4u, asFloat ? "f" : "i");
            for (size_t j = 0; j < K; ++j)
            {
                float f = 0.0f;
                std::memcpy(&f, &mono.snap[j][i], 4);
                w += asFloat ? std::snprintf(line + w, sizeof(line) - w, "%s%.6g", j ? "," : "", f)
                             : std::snprintf(line + w, sizeof(line) - w, "%s%d", j ? "," : "",
                                             static_cast<int32_t>(mono.snap[j][i]));
            }
            std::fprintf(stderr, "%s\n", line);
        }
        std::fprintf(stderr, "fh10-mono done hits=%u\n", hits);
        mono.snap.clear();
    }
    // PS2X_FH10_DUMP=dir:T1,T2,... (ticks decimal, up to 64): writes RDRAM as
    // <dir>/ram-<tick>.bin at those VBlanks, for offline scans (32 MiB each).
    static const std::pair<std::string, std::vector<uint64_t>> dump = [] {
        std::pair<std::string, std::vector<uint64_t>> d;
        const char *p = std::getenv("PS2X_FH10_DUMP");
        const char *colon = p ? std::strchr(p, ':') : nullptr;
        if (!colon)
            return d;
        d.first.assign(p, colon);
        p = colon + 1;
        while (*p && d.second.size() < 64u)
        {
            char *end = nullptr;
            d.second.push_back(std::strtoull(p, &end, 10));
            if (*end != ',') break;
            p = end + 1;
        }
        return d;
    }();
    for (const uint64_t t : dump.second)
        if (t == tick)
        {
            const std::string path = dump.first + "/ram-" + std::to_string(tick) + ".bin";
            if (FILE *f = std::fopen(path.c_str(), "wb"))
            {
                std::fwrite(ram, 1, PS2_RAM_SIZE, f);
                std::fclose(f);
                std::fprintf(stderr, "fh10-dump tick=%llu path=%s\n", static_cast<unsigned long long>(tick), path.c_str());
            }
        }
    Fh10Hist &h = fh10Hist();
    for (const auto &r : h.ranges)
    {
        if (tick != r.second + 1u)
            continue;
        std::vector<std::pair<uint32_t, uint32_t>> out;
        for (size_t i = 0; i < h.keys.size(); ++i)
            if (h.keys[i])
                out.emplace_back(h.vals[i], h.keys[i]);
        std::sort(out.begin(), out.end(), [](const auto &x, const auto &y) {
            return x.first != y.first ? x.first > y.first : x.second < y.second;
        });
        static const size_t maxLines = [] {
            const char *m = std::getenv("PS2X_FH10_HIST_MAX"); // FH13: full list when set
            return (m && *m) ? static_cast<size_t>(std::strtoul(m, nullptr, 10)) : size_t{1500u};
        }();
        size_t lines = 0u;
        for (const auto &e : out)
        {
            if (lines++ >= maxLines)
                break;
            std::fprintf(stderr, "fh10-hist range=%llu-%llu tgt=%06x n=%u\n", static_cast<unsigned long long>(r.first),
                         static_cast<unsigned long long>(r.second), e.second, e.first);
        }
        std::fprintf(stderr, "fh10-hist done range=%llu-%llu targets=%zu\n", static_cast<unsigned long long>(r.first),
                     static_cast<unsigned long long>(r.second), out.size());
        std::fill(h.keys.begin(), h.keys.end(), 0u);
        std::fill(h.vals.begin(), h.vals.end(), 0u);
    }
}

// GT3: onBranch runs on every dispatched guest branch. Its predicates are
// function-local statics, each an out-of-line guarded call (Odin All-Peak
// full-120: onBranch ~10 % of GameThread cycles, nearly all predicate calls).
// PS2X_SSX3_FULL120_FASTHOOKS=1 (default off) reads them from one struct
// computed at the first branch; off keeps the original calls. Same hooks,
// same order, same results (every flag below is fixed after its first read).
struct BranchFlags
{
    bool always, events, clock, raceClock, launch, session, parity, rng, trick, aiGate, bonus, lift, flags, rclock, fh12,
        particles;
    bool src, fh9, lab, draw, tap;
};

inline const BranchFlags &branchFlags() noexcept
{
    static const BranchFlags f = [] {
        BranchFlags r{};
        r.always = mode() == Mode::Always;
        r.events = mode() == Mode::Events;
        r.clock = clockFix();
        r.raceClock = raceClockFix();
        r.launch = launchFix();
        r.session = sessionFix();
        r.rng = rngFix();
        r.trick = trickFix();
        r.aiGate = aiGateFix();
        r.particles = particlesFix();
        r.parity = r.rng || r.trick || r.aiGate || r.particles;
        r.bonus = bonusFix();
        r.lift = liftFix();
        r.flags = flagsFix();
        r.rclock = rclockFix();
        r.fh12 = fh12Hooks();
        r.src = !srcWants().empty() || fh26Tap().s != 0u;
        r.fh9 = fh9Tap().r != 0u;
        r.lab = !labHooks("PS2X_FH1_HALF", true).empty() || !labHooks("PS2X_FH1_SKIP", false).empty();
        r.draw = drawLimit();
        r.tap = tap().on;
        return r;
    }();
    return f;
}

inline bool fastHooks() noexcept
{
    static const bool on = [] {
        const char *v = std::getenv("PS2X_SSX3_FULL120_FASTHOOKS");
        return v && std::strcmp(v, "1") == 0;
    }();
    return on;
}

// IN4 rider layer (PS2X_INPUT_DIAG=1): the minimal port of IN3's in3-in tap
// (branch in3, commit 4774566): at each entry of the rider control
// dispatcher 0x111728 (a1 = block filled by 0x121068; low 20 bits of +0 are
// the action-map bits), report watch-bit edges (L1 0x40000 / R2 0x80000 /
// square bits) per rider P into the input log's guest ring. The note keeps
// per-P baselines and a 20000-line cap; off (or past the cap) it is one
// cached-bool check.
inline void in4NoteRider(uint8_t *ram, R5900Context *ctx)
{
    static const bool on = [] { return ps2x::inputdiag::enabled(); }();
    if (!on)
        return;
    const uint32_t P = getRegU32(ctx, 4), a1 = getRegU32(ctx, 5);
    uint32_t w0 = 0u;
    rd32(ram, a1, w0);
    w0 &= 0xfffffu; // action bits only (the stick bytes live above bit 20)
    static uint32_t lastP[8] = {};
    static uint32_t lastBits[8] = {};
    static bool have[8] = {};
    uint32_t slot = 0u;
    while (slot < 8u && lastP[slot] != 0u && lastP[slot] != P)
        ++slot;
    if (slot == 8u)
        return;
    const uint32_t old = have[slot] && lastP[slot] == P ? lastBits[slot] : w0;
    lastP[slot] = P;
    lastBits[slot] = w0;
    have[slot] = true;
    if (((old ^ w0) & ps2x::inputdiag::kRiderWatch) == 0u)
        return;
    ps2x::inputdiag::noteRider(P, old, w0, g_lastTick);
}

template <bool Fast>
inline bool onBranchT(uint8_t *ram, R5900Context *ctx, uint32_t sourcePc, uint32_t targetPc)
{
    // GT3: Fast reads the once-computed flags; !Fast makes the original calls
    // in the original order. Every flag is fixed after its first read.
    const BranchFlags *const bf = Fast ? &branchFlags() : nullptr;
    const auto flag = [bf](bool BranchFlags::*m, bool (*fn)()) { return Fast ? bf->*m : fn(); };
    if ((Fast ? bf->always : mode() == Mode::Always) && sourcePc == kHookSite && !g_patched)
    {
        g_patched = true;
        patchAtManagerInit(ram);
    }
    if (Fast ? bf->events : mode() == Mode::Events)
        eventsOnBranch(ram, ctx, sourcePc, targetPc);
    const bool on = Fast ? (bf->always || (bf->events && g_guestActive)) : hooksOn();
    if (on && flag(&BranchFlags::clock, clockFix))
        clockPreHook(ram, targetPc);
    if (on && flag(&BranchFlags::raceClock, raceClockFix))
        raceClockPreHook(ram, ctx, sourcePc, targetPc);
    if (on && flag(&BranchFlags::launch, launchFix))
        launchPreHook(ram, ctx, targetPc);
    bool skip = on && flag(&BranchFlags::session, sessionFix) && sessionSkip(sourcePc, targetPc);
    if (on && (Fast ? bf->parity : (rngFix() || trickFix() || aiGateFix() || particlesFix())))
        parityHook(ram, sourcePc);
    if (on && flag(&BranchFlags::rng, rngFix))
        skip = rngHook(ram, ctx, sourcePc, targetPc) || skip;
    if (on && flag(&BranchFlags::trick, trickFix) && g_rngOdd && sourcePc == kComboSite && targetPc == kComboAccrue)
        skip = true;
    if (on && flag(&BranchFlags::aiGate, aiGateFix))
        skip = aiGateHook(ctx, sourcePc, targetPc) || skip;
    if (on && flag(&BranchFlags::particles, particlesFix) && g_rngOdd && targetPc == kParticlePass &&
        (sourcePc == kParticleSiteAll || sourcePc == kParticleSiteOne))
        skip = true;
    if (on && flag(&BranchFlags::bonus, bonusFix))
        bonusHook(ram, ctx, sourcePc, targetPc);
    if (on && flag(&BranchFlags::lift, liftFix))
        liftHook(ctx, sourcePc, targetPc);
    if (on && flag(&BranchFlags::flags, flagsFix))
        flagsPreHook(ram, ctx, targetPc);
    if (flag(&BranchFlags::rclock, rclockFix)) // also after exit: S keeps its frozen offset
        skip = rclockHook(ram, ctx, targetPc) || skip;
    if (on && flag(&BranchFlags::fh12, fh12Hooks))
        fh12PreHook(ram, ctx, targetPc);
    if (!Fast || bf->src)
    {
        srcTap(ram, ctx, sourcePc, targetPc);
        fh26TapCall(ram, ctx, targetPc);
    }
    if (!Fast || bf->fh9)
        fh9OnBranch(ram, ctx, sourcePc, targetPc, skip);
    if (on && (!Fast || bf->lab))
        skip = labHook(ram, ctx, sourcePc, targetPc) || skip;
    // IN4 rider layer (PS2X_INPUT_DIAG=1): edges of the rider action-block
    // watch bits at each entry of the rider control dispatcher 0x111728
    // (a1 = block filled by 0x121068; low 20 bits of +0 = action-map bits;
    // the IN3 in3-in tap's site, branch in3 4774566). One compare per branch
    // when off; the note itself early-outs on the cached knob.
    if (targetPc == 0x111728u && ctx)
        in4NoteRider(ram, ctx);
    const bool drawSkip = on && flag(&BranchFlags::draw, drawLimit) && !skip && drawHook(ctx, sourcePc, targetPc);
    skip = skip || drawSkip;
    if (Fast && !bf->tap)
        return skip;
    Tap &t = tap();
    if (!t.on)
        return skip;
    fh10HistBranch(targetPc);
    if (targetPc == kUpdateTarget)
        ++t.updates;
    else if (targetPc == kRenderTarget && !drawSkip)
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

// Out of line as before, so dispatchGuestBranch keeps its shape (and its PGO
// profile match).
__attribute__((noinline)) inline bool onBranch(uint8_t *ram, R5900Context *ctx, uint32_t sourcePc, uint32_t targetPc)
{
    return fastHooks() ? onBranchT<true>(ram, ctx, sourcePc, targetPc) : onBranchT<false>(ram, ctx, sourcePc, targetPc);
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
    fh10OnVBlank(ram, tick);
    g_lastTick = tick;
    if (mode() == Mode::Events)
    {
        g_divThis = g_divNext;
        if (!g_stockInit)
        {
            // A fresh boot starts at tick 1; a state loaded from a stock run
            // carries stock history up to tick-1.
            g_stockInit = true;
            g_stockHalf.store(2u * (tick - 1u), std::memory_order_relaxed);
            g_stockHalfExact.store(2u * (tick - 1u), std::memory_order_relaxed);
        }
        // FH5 counted by the interval scheduled at this VBlank (schedDivisor
        // runs before processEvent): the full interval before the entry VBlank
        // counts as a half, and the half before the exit VBlank as a full.
        g_stockHalf.fetch_add(g_divThis == 2u ? 1u : 2u, std::memory_order_relaxed);
        // FH17: count the interval that just ended.
        g_stockHalfExact.fetch_add(g_divEnded == 2u ? 1u : 2u, std::memory_order_relaxed);
        if (g_commitActive != g_schedActive)
        {
            g_commitActive = g_schedActive;
            g_flipPending = true;
            static uint32_t lines = 0u;
            if (lines++ < 64u)
                std::fprintf(stderr, "fh1-events commit %s tick=%llu half=%llu legacy=%llu\n",
                             g_commitActive ? "enter" : "exit", static_cast<unsigned long long>(tick),
                             static_cast<unsigned long long>(g_stockHalfExact.load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(stockHalfTicksLegacy()));
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
    fh9OnVBlank(tick);
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
    if (drawLimit())
        n += std::snprintf(line + n, sizeof(line) - n, " drawn=%llu dskip=%llu",
                           static_cast<unsigned long long>(g_drawn), static_cast<unsigned long long>(g_drawSkipped));
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
