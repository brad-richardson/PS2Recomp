#pragma once

// Full-120 FIX groups (PS2X_SSX3_FULL120_FIX) and their pure parser, split out
// of ps2_fh1_full120.h (FH26) so unit tests need no runtime. The value is a
// comma list read left to right: "all" adds every default group of both
// masks, a name adds its group, "-name" removes it ("all,-stick2"; "-all"
// clears both). An unknown name refuses the whole value.

#include <cstdint>
#include <string>

namespace ps2_fh1
{
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
    // FH26 (Brad 10-02 "Yes ship both"; in "all", "all,-stick2" opts out):
    kFixStick2 = 1ull << 34,    // 0x133308 bounded offset slews [0x49bc94]/[0x49bca0] (0x1340a8/0x134224): ~0.05 per update -> half (class b, FCR1 1)
    kFixClockSign = 1ull << 35, // with clock: the 0x1e1458 stamp's exit restamp keeps the signed elapsed (FCR1 3)
    // FH27 (in "all" since DEF1, Brad 10-03 "yes make them defaults"; "all,-bonusflip" opts out):
    kFixBonusFlip = 1ull << 36, // with bonus: a live trick-bonus rate +0x3c is rescaled at each events flip (FCR1 4)
    // FH28 (opt-in, not in "all"; Brad's feel test decides defaults):
    kFixJcam = 1ull << 37, // jump camera 0x1635f8: countdown n (shot+0x2c4) at stock cadence, landing-offset retention d -> sqrt(d) per update (class d/a, FXT1 T1)
    kFixPose = 1ull << 38, // pose/lean triple slew bounds (R+0x200 writers): 1/30 -> 1/60 per update (class b, FXT1 T2)
    kFixPid = 1ull << 39,  // camera heading PID 0x162c78: recurrence + 5-slot histories at stock cadence (second update of a pair held with zero gains, then rolled back) (class d, FXT1 T3, FH29)
    kFixC2Cap = 1ull << 40, // C2 blend counter 0x162998 at stock cadence, ramp rate back to stock (class d, FXT1 T6)
    kFixSpawn = 1ull << 41, // glint spawn substream: stock dt for the 0x2e1520 caller's word (class h, FXT1 T4)
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
    // FH27 (in "all" since DEF1, Brad 10-03; "all,-loops2" opts out):
    kFix12Loops2 = 1u << 14,   // with loops: a loop controller built while active (ctor 0x341aa0, step = x/A.rate at 120) gets its step in stock units (FCR1 4)
    // FLK2 Part 3 (opt-in, not in "all"):
    kFix12Flare = 1u << 15,    // light/sun visibility probe loop 0x2e3130 (thunk 0x2e3110): odd updates skipped, flare intensity held per stock period (class d)
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
    if (item == "loops2") return kFix12Loops2;
    if (item == "flare") return kFix12Flare;
    return 0u;
}

// Main-mask group by name (0 = not a main-mask name).
inline uint64_t fixItem(const std::string &item) noexcept
{
    if (item == "rider") return kFixRider;
    if (item == "countdown") return kFixCountdown;
    if (item == "drag") return kFixDrag;
    if (item == "event") return kFixEvent;
    if (item == "slew") return kFixSlew;
    if (item == "clock") return kFixClock;
    if (item == "raceclock") return kFixRaceClock;
    if (item == "session") return kFixSession;
    if (item == "timers") return kFixTimers;
    if (item == "camera") return kFixCamera;
    if (item == "launch") return kFixLaunch;
    if (item == "stick") return kFixStick;
    if (item == "speedcap") return kFixSpeedcap;
    if (item == "rng") return kFixRng;
    if (item == "trick") return kFixTrick;
    if (item == "anim") return kFixAnim;
    if (item == "bonus") return kFixBonus;
    if (item == "lift") return kFixLift;
    if (item == "aigate") return kFixAiGate;
    if (item == "takeoff") return kFixTakeoff;
    if (item == "flags") return kFixFlags;
    if (item == "steer") return kFixSteer;
    if (item == "rail") return kFixRail;
    if (item == "reset") return kFixReset;
    if (item == "meter") return kFixMeter;
    if (item == "boost") return kFixBoost;
    if (item == "ground") return kFixGround;
    if (item == "entry") return kFixEntry;
    if (item == "rclock") return kFixRclock;
    if (item == "emitter") return kFixEmitter;
    if (item == "fx") return kFixFx;
    if (item == "clock2") return kFixClock2;
    if (item == "hudfill") return kFixHudfill;
    if (item == "stick2") return kFixStick2;
    if (item == "clocksign") return kFixClockSign;
    if (item == "bonusflip") return kFixBonusFlip;
    if (item == "jcam") return kFixJcam;
    if (item == "pose") return kFixPose;
    if (item == "pid") return kFixPid;
    if (item == "c2cap") return kFixC2Cap;
    if (item == "spawn") return kFixSpawn;
    return 0u;
}

// "all": every group except kFixLift (FH8: no window where it binds) and the
// internal kFixRamp. FH26 adds stick2 + clocksign (Brad 10-02); DEF1 adds
// bonusflip + loops2 (Brad 10-03).
inline constexpr uint64_t kFixAll =
    kFixRider | kFixCountdown | kFixDrag | kFixEvent | kFixSlew | kFixClock | kFixRaceClock | kFixSession | kFixTimers |
    kFixCamera | kFixLaunch | kFixStick | kFixSpeedcap | kFixRng | kFixTrick | kFixAnim | kFixBonus | kFixAiGate |
    kFixTakeoff | kFixFlags | kFixSteer | kFixRail | kFixReset | kFixMeter | kFixBoost | kFixGround | kFixEntry |
    kFixRclock | kFixEmitter | kFixFx | kFixClock2 | kFixHudfill | kFixStick2 | kFixClockSign | kFixBonusFlip;
inline constexpr uint32_t kFix12All = kFix12Spin | kFix12Texanim | kFix12Loops | kFix12Recover | kFix12Pulse |
                                      kFix12Crash | kFix12FxTimer | kFix12Bounce | kFix12Particles |
                                      kFix12CrashBody | kFix12Uber | kFix12Popups | kFix12SndDt | kFix12HudProg |
                                      kFix12Loops2;

struct FixMasks
{
    uint64_t main = 0u;
    uint32_t fh12 = 0u;
    bool ok = true;
    std::string bad; // first unknown item when !ok
};

inline FixMasks parseFix(const std::string &s)
{
    FixMasks r;
    size_t at = 0u;
    while (at <= s.size())
    {
        const size_t comma = s.find(',', at);
        std::string item = s.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
        const bool minus = !item.empty() && item[0] == '-';
        if (minus)
            item.erase(0, 1);
        uint64_t m = 0u;
        uint32_t m12 = 0u;
        if (item == "all")
        {
            m = kFixAll;
            m12 = kFix12All;
        }
        else
        {
            m = fixItem(item);
            m12 = fh12Item(item);
        }
        if (!item.empty() && m == 0u && m12 == 0u)
        {
            r.ok = false;
            r.bad = item;
            return r;
        }
        if (minus)
        {
            r.main &= ~m;
            r.fh12 &= ~m12;
        }
        else
        {
            r.main |= m;
            r.fh12 |= m12;
        }
        if (comma == std::string::npos)
            break;
        at = comma + 1u;
    }
    return r;
}
}
