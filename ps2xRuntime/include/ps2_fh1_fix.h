#pragma once

// Full-120 FIX groups (PS2X_SSX3_FULL120_FIX) and their pure parser, split out
// of ps2_fh1_full120.h (FH26) so unit tests need no runtime. The value is a
// comma list read left to right: "all" adds every default group of both
// masks, a name adds its group, "-name" removes it ("all,-stick2"; "-all"
// clears both). An unknown name refuses the whole value.

#include <cstdint>
#include <cstdlib>
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
    kFixInputChain = 1ull << 47, // INP3 opt-in prototype: coupled directional tracker at stock cadence
    kFixInput2 = 1ull << 46, // INP2 opt-in: digital guard/repeat clocks only (INP3 split)
    kFixTrails = 1ull << 48, // FH35 opt-in: direct trail callers at stock dt with rng+emitter (class h/d; owner FH35)
    kFixChase2 = 1ull << 49, // FH35 opt-in: C1 interpolation before rooting retention (class a; owner FH35)
    kFixGround2 = 1ull << 43, // FH33 opt-in: bounded steering response exact half-map (class a/j; not in all)
    kFixJcam2 = 1ull << 44, // FH32 opt-in: camera-only stock-cadence shadow predictor publication (class d/c, owner FH32)
    kFixLife2 = 1ull << 45, // LCY2 opt-in: streaming live-age units and replay snapshot input ordinals (class e/d; owner LCY2)
    kFixEnvFilt = 1ull << 42, // FH30 opt-in: 0x2c096c -> 0x2bcf38 squared-gain response at 120 (class a; not in all)
    kFixQuery = 1ull << 50, // JMP3 opt-in: cruise ground query 0x13d1b8 at stock cadence (skip on odd updates; class d; not in all)
    kFixSpringHold = 1ull << 51, // KNC5: bit reserved (JMP5 springhold removed, no-op). The name still parses so older envs (fork ios/full120.env) don't strict-refuse.
    kFixShadowPub = 1ull << 53, // JDR2 opt-in (with query): on query-skipped updates the ground-frame readers 0x2e8938 (R+0x370/0x3a0) and 0x2ed490 (R+0xaac/0xab0) see a half-step extrapolation, restored on return (render-only; not in all)
    kFixVnRet = 1ull << 52, // JMP6 opt-in: 0x13d818 per-update retentions: post-query normal-velocity removal 0x13ec18 and lean brake 0x13e028, r -> sqrt(r) (class a; not in all)
    kFixGroundStep = 1ull << 54, // JMP7 opt-in with query: publish cruise ground query at the stock-time boundary (class d; not in all)
    kFixTrailRing = 1ull << 55, // CRV1 opt-in with rng: 120 Hz carve-trail pushes, double ring capacity, half time step (trail state only; not in all)
    kFixNormcap = 1ull << 56, // NRM1 opt-in with vnret+query: G18 bounded normal-response exact half-map (cap budget carried across the pair, single rescale; class a; not in all)
    kFixSel5 = 1ull << 57, // SEL1 opt-in: selector-5 handler 0x1391a8 private dt 1/60 -> 1/120 (class h; not in all)
    kFixSel5b = 1ull << 58, // SEL2 opt-in (needs sel5): circle-handler 0x132a30 charge step 1/60 -> 1/120 (class h; not in all)
    kFixBoostFill = 1ull << 59, // BST1 opt-in: sq_boost_gate 0x114130 fill consume [0x49b4fc] 0.0015068 -> half per update (class b; not in all)
    kFixStuntChg = 1ull << 60, // FLP2 opt-in: rider stunt-charge slew 0x1211f8 (R+0x2a4 spin / R+0x2b0 flip) at stock cadence (class d; not in all)
    kFixAsymK = 1ull << 61, // TRN2 opt-in with query+vnret: publish-half stock k=0.4, skip-half k=0 at 0x49c124 (pair keeps stock 0.6; suppresses normcap; class d; not in all)
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
    if (item == "life2") return kFixLife2;
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
    if (item == "inputchain") return kFixInputChain;
    if (item == "input2") return kFixInput2;
    if (item == "ground2") return kFixGround2;
    if (item == "entry") return kFixEntry;
    if (item == "rclock") return kFixRclock;
    if (item == "emitter") return kFixEmitter;
    if (item == "fx") return kFixFx;
    if (item == "clock2") return kFixClock2;
    if (item == "hudfill") return kFixHudfill;
    if (item == "stick2") return kFixStick2;
    if (item == "clocksign") return kFixClockSign;
    if (item == "bonusflip") return kFixBonusFlip;
    if (item == "jcam2") return kFixJcam2;
    if (item == "jcam") return kFixJcam;
    if (item == "pose") return kFixPose;
    if (item == "pid") return kFixPid;
    if (item == "c2cap") return kFixC2Cap;
    if (item == "spawn") return kFixSpawn;
    if (item == "envfilt") return kFixEnvFilt;
    if (item == "trails") return kFixTrails;
    if (item == "chase2") return kFixChase2;
    if (item == "query") return kFixQuery;
    if (item == "springhold") return kFixSpringHold;
    if (item == "vnret") return kFixVnRet;
    if (item == "groundstep") return kFixGroundStep;
    if (item == "trailring") return kFixTrailRing;
    if (item == "normcap") return kFixNormcap;
    if (item == "shadowpub") return kFixShadowPub;
    if (item == "sel5") return kFixSel5;
    if (item == "sel5b") return kFixSel5b;
    if (item == "boostfill") return kFixBoostFill;
    if (item == "stuntchg") return kFixStuntChg;
    if (item == "asymk") return kFixAsymK;
    return 0u;
}

// "all": every group except kFixLift (FH8: no window where it binds),
// kFixGround (GRD1: its force is already scaled by converted dt), and the
// internal kFixRamp. FH26 adds stick2 + clocksign (Brad 10-02); DEF1 adds
// bonusflip + loops2 (Brad 10-03). PRM1 adds pose,c2cap,spawn,envfilt,input2,chase2 (Brad 10-11);
// life2 stays opt-in (lifecycle gates blocked: no finished race at clean tips).
inline constexpr uint64_t kFixAll =
    kFixRider | kFixCountdown | kFixDrag | kFixEvent | kFixSlew | kFixClock | kFixRaceClock | kFixSession | kFixTimers |
    kFixCamera | kFixLaunch | kFixStick | kFixSpeedcap | kFixRng | kFixTrick | kFixAnim | kFixBonus | kFixAiGate |
    kFixTakeoff | kFixFlags | kFixSteer | kFixRail | kFixReset | kFixMeter | kFixBoost | kFixEntry |
    kFixRclock | kFixEmitter | kFixFx | kFixClock2 | kFixHudfill | kFixStick2 | kFixClockSign | kFixBonusFlip |
    kFixPose | kFixC2Cap | kFixSpawn | kFixEnvFilt | kFixInput2 | kFixChase2;
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

// REV1 (RV27 F2 / RV30 #1 / RV32 F4): asymk writes pool word 0x49c124 every
// cruise update, and the only apply/restore entry for that word is keyed to
// kFixVnRet. asymk therefore requires vnret: with query+groundstep+asymk but
// no vnret the word was driven per-update yet never restored at the exit
// flip, leaving k=0 in stock-rate play. Requiring vnret is the smaller exact
// change (an asymk-owned word-table entry would duplicate the vnret entry
// and need dedup in applyWords); the play masks all carry vnret. Pure, so
// the suite can pin the accepted mask sets (asymkFix adds enabled()).
inline bool asymkMaskOk(uint64_t mask) noexcept
{
    return (mask & (kFixAsymK | kFixQuery | kFixVnRet)) ==
           (static_cast<uint64_t>(kFixAsymK) | kFixQuery | kFixVnRet);
}

// NRM1 normcap's complete requested bit set. normcap is effective only when
// asymk is not (asymk suppresses it: its pair math assumes sqrt/sqrt halves).
inline bool normcapMaskRequested(uint64_t mask) noexcept
{
    return (mask & (kFixNormcap | kFixVnRet | kFixQuery)) ==
           (static_cast<uint64_t>(kFixNormcap) | kFixVnRet | kFixQuery);
}
inline bool normcapMaskOk(uint64_t mask) noexcept
{
    return normcapMaskRequested(mask) && !asymkMaskOk(mask);
}

// SVL1 2a: same-mode sanctioned FIX-delta support
// (PS2X_SAVESTATE_ALLOW_FIX_DELTA, diagnostic, default off). Pure unless
// noted; the savestate header (R1) and the fh1 section (R2) share this so
// the two layers cannot disagree. Mode changes are NOT classified here:
// 2a refuses them at R1 with a "2b" message before masks are compared.
inline bool fixDeltaKnobOn() noexcept
{
    const char *v = std::getenv("PS2X_SAVESTATE_ALLOW_FIX_DELTA");
    return v != nullptr && v[0] == '1';
}

// Bit index -> parse name (nullptr when unnamed). Bit 14 (kFixRamp) is
// internal: fixMask() never carries it (the word-table builder derives it
// in a local), so it stays unnamed and any delta touching an unnamed bit
// refuses (fail-closed for internal and future bits).
inline const char *fixMainBitName(unsigned bit) noexcept
{
    static constexpr const char *kNames[62] = {
        "rider", "countdown", "drag", "event", "slew", "clock", "raceclock", "session", // 0-7
        "timers", "camera", "launch", "stick", "speedcap", "rng", nullptr, "trick", // 8-15 (14 = ramp)
        "anim", "bonus", "lift", "aigate", "takeoff", "flags", "steer", "rail", // 16-23
        "reset", "meter", "boost", "ground", "entry", "rclock", "emitter", "fx", // 24-31
        "clock2", "hudfill", "stick2", "clocksign", "bonusflip", "jcam", "pose", "pid", // 32-39
        "c2cap", "spawn", "envfilt", "ground2", "jcam2", "life2", "input2", "inputchain", // 40-47
        "trails", "chase2", "query", "springhold", "vnret", "shadowpub", "groundstep", "trailring", // 48-55
        "normcap", "sel5", "sel5b", "boostfill", "stuntchg", "asymk", // 56-61
    };
    return bit < 62u ? kNames[bit] : nullptr;
}
inline const char *fix12BitName(unsigned bit) noexcept
{
    static constexpr const char *kNames[16] = {
        "spin", "texanim", "loops", "recover", "pulse", "crash", "fxtimer", "bounce", // 0-7
        "particles", "crashbody", "uber", "popups", "snddt", "hudprog", "loops2", "flare", // 8-15
    };
    return bit < 16u ? kNames[bit] : nullptr;
}

inline std::string fixBitHex(uint64_t v)
{
    if (v == 0u)
        return "0x0";
    std::string out;
    while (v != 0u)
    {
        out = "0123456789abcdef"[v & 0xfu] + out;
        v >>= 4;
    }
    return "0x" + out;
}

// Deterministic delta render: main bits ascending, then mask-12 ascending;
// run-side gains take '+', save-side losses take '-'; unnamed bits render
// as bit<N>. Empty when the masks are equal.
inline std::string renderFixDelta(uint64_t saveMain, uint32_t save12, uint64_t runMain, uint32_t run12)
{
    std::string out;
    const uint64_t dMain = saveMain ^ runMain;
    for (unsigned b = 0u; b < 64u; ++b)
    {
        if (((dMain >> b) & 1u) == 0u)
            continue;
        if (!out.empty())
            out += ',';
        out += ((runMain >> b) & 1u) != 0u ? '+' : '-';
        const char *name = fixMainBitName(b);
        out += name != nullptr ? name : ("bit" + std::to_string(b));
    }
    const uint32_t d12 = save12 ^ run12;
    for (unsigned b = 0u; b < 32u; ++b)
    {
        if (((d12 >> b) & 1u) == 0u)
            continue;
        if (!out.empty())
            out += ',';
        out += ((run12 >> b) & 1u) != 0u ? '+' : '-';
        const char *name = fix12BitName(b);
        out += name != nullptr ? name : ("bit12." + std::to_string(b));
    }
    return out;
}

struct FixDeltaVerdict
{
    bool allow = false;
    bool same = false; // delta empty (the allow path matches today's apply)
    std::string detail; // allow: "+a,-b" render ("" when same); refuse: naming reason
};

// Same-mode mask-delta verdict. Directional residue (input2/inputchain,
// life2) refuses save-ON -> run-OFF and allows OFF -> ON (self-converting);
// bonusflip refuses both ways in 2a (no entry-rescale bridge); unnamed bits
// refuse; everything else named allows (the fh1 section still enforces the
// active-window, pair-boundary and post-armed rules).
inline FixDeltaVerdict classifyFixDelta(uint64_t saveMain, uint32_t save12, uint64_t runMain, uint32_t run12)
{
    FixDeltaVerdict r;
    const uint64_t dMain = saveMain ^ runMain;
    const uint32_t d12 = save12 ^ run12;
    if (dMain == 0u && d12 == 0u)
    {
        r.allow = true;
        r.same = true;
        return r;
    }
    uint64_t unknownMain = 0u;
    for (unsigned b = 0u; b < 64u; ++b)
        if (((dMain >> b) & 1u) != 0u && fixMainBitName(b) == nullptr)
            unknownMain |= 1ull << b;
    uint32_t unknown12 = 0u;
    for (unsigned b = 0u; b < 32u; ++b)
        if (((d12 >> b) & 1u) != 0u && fix12BitName(b) == nullptr)
            unknown12 |= 1u << b;
    if (unknownMain != 0u || unknown12 != 0u)
    {
        r.detail = "unknown FIX bit(s) " + fixBitHex(unknownMain) + ":" + fixBitHex(unknown12);
        return r;
    }
    const uint64_t lost = saveMain & ~runMain; // bits the run drops
    if ((lost & kFixInput2) != 0u || (lost & kFixInputChain) != 0u)
    {
        r.detail = "input2/inputchain ON->OFF needs the same mask (tagged guest records; delta " +
                   renderFixDelta(saveMain, save12, runMain, run12) + ")";
        return r;
    }
    if ((lost & kFixLife2) != 0u)
    {
        r.detail = "life2 ON->OFF needs the same mask (recording metadata; delta " +
                   renderFixDelta(saveMain, save12, runMain, run12) + ")";
        return r;
    }
    if ((dMain & kFixBonusFlip) != 0u)
    {
        r.detail = "bonusflip differs (entry rescale has no load bridge in 2a; delta " +
                   renderFixDelta(saveMain, save12, runMain, run12) + ")";
        return r;
    }
    r.allow = true;
    r.detail = renderFixDelta(saveMain, save12, runMain, run12);
    return r;
}

// SVL1 2a Gap 2: the normcap triple is captured on first halves
// (g_rngOdd == run groundstep) and consumed on second halves, and it is not
// saved. A sanctioned load whose run has normcap is seamless only when the
// save sits on a pair boundary, i.e. the first post-load update (parity
// !savedOdd) is a first half: savedOdd != runGroundStep.
inline bool fixDeltaPairBoundaryOk(bool savedOdd, bool runGroundStep, bool runNormcap) noexcept
{
    return !runNormcap || savedOdd != runGroundStep;
}

// Parses the header's "main:mask12" decimal pair. False on any garbage
// (fail-closed: unparsable values refuse with today's generic message).
inline bool parseFixPair(const std::string &s, uint64_t &mainOut, uint32_t &f12Out) noexcept
{
    mainOut = 0u;
    f12Out = 0u;
    const size_t colon = s.find(':');
    if (colon == std::string::npos || colon == 0u || colon + 1u >= s.size() ||
        s.find(':', colon + 1u) != std::string::npos)
        return false;
    uint64_t main = 0u;
    for (size_t i = 0u; i < colon; ++i)
    {
        const char c = s[i];
        if (c < '0' || c > '9')
            return false;
        const uint64_t d = static_cast<uint64_t>(c - '0');
        if (main > (0xffffffffffffffffull - d) / 10u)
            return false;
        main = main * 10u + d;
    }
    uint64_t f12 = 0u;
    for (size_t i = colon + 1u; i < s.size(); ++i)
    {
        const char c = s[i];
        if (c < '0' || c > '9')
            return false;
        const uint64_t d = static_cast<uint64_t>(c - '0');
        if (f12 > (0xffffffffull - d) / 10u)
            return false;
        f12 = f12 * 10u + d;
    }
    mainOut = main;
    f12Out = static_cast<uint32_t>(f12);
    return true;
}

inline std::string fixDeltaHeaderLog(const std::string &saveMode, const std::string &runMode,
                                     const std::string &saveFix, const std::string &runFix,
                                     const std::string &delta)
{
    return "[savestate] ALLOW_FIX_DELTA header: mode " + saveMode + "->" + runMode + " fix " + saveFix +
           "->" + runFix + " delta=" + delta + " (section checks pending)";
}

inline std::string fixDeltaSectionLog(const std::string &delta, bool savedOdd)
{
    return "[savestate] ALLOW_FIX_DELTA fh1: delta=" + delta + " saveparity=" +
           std::string(savedOdd ? "odd" : "even") + " applied bookkeeping, cleared pair caches";
}
}
