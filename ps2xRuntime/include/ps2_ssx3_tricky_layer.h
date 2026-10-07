#pragma once
// TKL1: one EE-owned Tricky layer (TKA1 stages 1-3).
//
// Stage 1: test seams + an immutable config wrapper around current behaviour.
// Stage 2: the EE produces an immutable per-VBlank snapshot packet (tick +
//   epoch); the GL and AHB compositors consume it and never touch RDRAM
//   (publishRdram is gone). Song bursts are triggered by the EE owner, so a
//   missing atlas no longer silences a burst (TKA1 F9).
// Stage 3: lifecycle. One race epoch resets letters, latches, splash and the
//   gem bitmap; pause suspends the song while leaving the race cancels it;
//   quick-load rebuilds the CD aliases from restored guest state (TKA1 F1).
//   F4 (shared engine knobs in the Tricky launcher) is out of scope.
//
// Threading: the reducer runs on the EE thread at VBlankStart, once per
// guest vsync, after the interval's EE work. The uber-post wrapper (TK47)
// runs on the same thread and only enqueues tick-tagged post events. The
// present paths (host loop, GS worker) and the audio callbacks consume the
// immutable packet / song commands without touching guest memory.
//
// Change class: output-only for the HUD/song path (the guest det-hash cannot
// move: the reducer only reads committed guest words); the gems poll and the
// alias rebuild stay guest-affecting behind their existing knobs.

#include "ps2_cd_overlay.h"
#include "ps2_ssx3_course_manifest.h"
#include "ps2_ssx3_tricky_gems.h"
#include "ps2_ssx3_tricky_hud.h"
#include "ps2_ssx3_tricky_menu.h"
#include "ps2_ssx3_tricky_song.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace ps2_ssx3_tricky_layer
{

// ---- Stage 1: immutable config -------------------------------------------
// One parse of the layer's knobs (HUD/ART/FORCE/LETTERS/SONG/GEMS/TABLE).
// Values equal what the old per-site getenv caches read; parsing once only
// removes the per-frame getenv calls, it changes no default.
struct Config
{
    bool hud = false;
    std::string artPath;
    ps2_ssx3_tricky_hud::ForceValue forced;
    int lettersPreset = -1;
    std::string songPath;
    bool gems = false;
    std::string gemsTable;
    uint32_t apPtrAddr = 0u; // PS2X_TK12_AP_PTR override address (0 = unset)
    bool apPtrSet = false;
};

inline Config parseConfig(const char *hud, const char *art, const char *force, const char *preset,
                          const char *song, const char *gems, const char *gemsTable, const char *apPtr)
{
    Config c;
    c.hud = hud && hud[0] == '1';
    if (art && *art)
        c.artPath = art;
    c.forced = ps2_ssx3_tricky_hud::parseForce(force);
    c.lettersPreset = ps2_ssx3_tricky_hud::parseLettersPreset(preset);
    if (song && *song)
        c.songPath = song;
    c.gems = gems && gems[0] == '1';
    if (gemsTable && *gemsTable)
        c.gemsTable = gemsTable;
    if (apPtr && *apPtr)
    {
        c.apPtrSet = true;
        c.apPtrAddr = static_cast<uint32_t>(std::strtoul(apPtr, nullptr, 0));
    }
    return c;
}

inline Config configFromEnv()
{
    return parseConfig(std::getenv("PS2X_SSX3_TRICKY_HUD"), std::getenv("PS2X_SSX3_TRICKY_HUD_ART"),
                       std::getenv("PS2X_SSX3_TRICKY_HUD_FORCE"),
                       std::getenv("PS2X_SSX3_TRICKY_LETTERS_PRESET"),
                       std::getenv("PS2X_SSX3_TRICKY_SONG"), std::getenv("PS2X_SSX3_TRICKY_GEMS"),
                       std::getenv("PS2X_SSX3_TRICKY_GEMS_TABLE"), std::getenv("PS2X_TK12_AP_PTR"));
}

namespace detail
{
inline Config &configOverride()
{
    static Config c;
    return c;
}
inline bool &configHasOverride()
{
    static bool b = false;
    return b;
}
} // namespace detail

inline const Config &config()
{
    if (detail::configHasOverride())
        return detail::configOverride();
    static const Config c = configFromEnv();
    return c;
}

// Test seam: the cached config is order-dependent (any earlier load/VBlank
// in the process caches it), so tests override it explicitly instead of
// racing setenv. Production never calls these.
inline void setConfigForTest(const Config &c)
{
    detail::configOverride() = c;
    detail::configHasOverride() = true;
}

inline void clearConfigForTest()
{
    detail::configHasOverride() = false;
}

inline bool active()
{
    const Config &c = config();
    return c.hud || c.gems;
}

// ---- Stage 1: one course context gate -------------------------------------
// Course identity + phase + race epoch. Phase is derived conservatively from
// signals the EE already reads: the manifest mode, the app-update/loading
// state (the same predicate that blocks picker switches) and the HUD race
// clock. Countdown-with-running-clock is Racing (TK44: the meter shows
// there, matching the live HUD); frozen-in-Tricky (pause, pre-race card,
// results) is Paused: it suspends the song but never cancels it (stage 3:
// pause suspends, leaving the race cancels).
enum class CourseKind
{
    Stock,
    Tricky,
};

enum class RacePhase
{
    Frontend, // menus / no live event and nothing loading
    Loading,  // a location slot is loading
    Racing,   // live event with the race clock advancing
    Paused,   // live event with the clock frozen (pause/card/results)
};

struct CourseContext
{
    CourseKind kind = CourseKind::Stock;
    RacePhase phase = RacePhase::Frontend;
    size_t modeIndex = 0u; // 0 = Stock, else modes[modeIndex - 1]
    uint64_t raceEpoch = 0u;
    uint64_t tick = 0u;
};

// Pure derivation from sampled signals (the seam the tests drive).
inline CourseContext deriveContext(size_t modeIndex, bool eventLive, bool loading, bool racing,
                                   uint64_t epoch, uint64_t tick)
{
    CourseContext cx;
    cx.modeIndex = modeIndex;
    cx.kind = modeIndex == 0u ? CourseKind::Stock : CourseKind::Tricky;
    cx.raceEpoch = epoch;
    cx.tick = tick;
    if (cx.kind == CourseKind::Stock)
    {
        cx.phase = RacePhase::Frontend;
        return cx;
    }
    if (loading)
        cx.phase = RacePhase::Loading;
    else if (!eventLive)
        cx.phase = RacePhase::Frontend;
    else if (racing)
        cx.phase = RacePhase::Racing;
    else
        cx.phase = RacePhase::Paused;
    return cx;
}

inline bool contextAllowsHud(const CourseContext &cx)
{
    return cx.kind == CourseKind::Tricky && cx.phase == RacePhase::Racing;
}

// The intended gems gate (same shape as the HUD's). The TK45c poll keeps its
// proven gate (Tricky mode + advancing race clock) at its own per-step safe
// point; it adopts this context only after the Loading-phase behaviour is
// validated on device, since that changes a guest-affecting gate.
inline bool contextAllowsGems(const CourseContext &cx)
{
    return cx.kind == CourseKind::Tricky && cx.phase == RacePhase::Racing;
}

// The song may sound only while racing; it suspends (not cancels) in every
// other Tricky phase and cancels on Stock/mode exit, epoch change or load.
inline bool contextSuspendsSong(const CourseContext &cx)
{
    return cx.kind == CourseKind::Tricky && cx.phase != RacePhase::Racing;
}

inline const char *phaseName(RacePhase p)
{
    switch (p)
    {
    case RacePhase::Frontend:
        return "Frontend";
    case RacePhase::Loading:
        return "Loading";
    case RacePhase::Racing:
        return "Racing";
    case RacePhase::Paused:
        return "Paused";
    }
    return "?";
}

// ---- Stage 2: the immutable per-VBlank packet ------------------------------
struct PresentationPacket
{
    uint64_t tick = 0u;   // the VBlank tick this sample belongs to
    uint64_t epoch = 0u;  // layer epoch (race/load generation)
    bool draw = false;    // the compositor draws (art valid, racing, readable)
    bool racing = false;  // the race clock is advancing
    float fill = 0.0f;    // displayed fill (FORCE applied)
    bool full = false;    // displayed full (FORCE applied)
    uint64_t splashUntil = 0u;
    int litLetters = 0;
    uint64_t flashUntil = 0u;
    const ps2_ssx3_tricky_hud::Atlas *atlas = nullptr; // immutable once ok
};

namespace detail
{
struct Mailbox
{
    std::mutex mu;
    PresentationPacket latest;
    bool have = false;
};
inline Mailbox &mailbox()
{
    static Mailbox m;
    return m;
}
inline std::atomic<uint64_t> &liveEpoch()
{
    static std::atomic<uint64_t> e(0u);
    return e;
}
} // namespace detail

inline void publishPacket(const PresentationPacket &p)
{
    detail::Mailbox &m = detail::mailbox();
    std::lock_guard<std::mutex> lock(m.mu);
    m.latest = p;
    m.have = true;
}

// Copies the newest packet. False when the EE has not published one yet.
inline bool latestPacket(PresentationPacket &out)
{
    detail::Mailbox &m = detail::mailbox();
    std::lock_guard<std::mutex> lock(m.mu);
    if (!m.have)
        return false;
    out = m.latest;
    return true;
}

inline uint64_t epoch()
{
    return detail::liveEpoch().load(std::memory_order_acquire);
}

// ---- Stage 2: tick-tagged uber-post events (EE only) -----------------------
// The TK47 wrapper runs on the EE thread and enqueues here; the VBlank
// reducer (same thread) drains the queue. Letters light at the post's own
// tick, exactly as the game posted them, with no presentation dependence.
struct PostQueue
{
    static constexpr size_t kCap = 64u;
    uint64_t ticks[kCap] = {0u};
    size_t head = 0u; // consumed
    size_t tail = 0u; // produced
    uint64_t count = 0u;
};

inline PostQueue &postQueue()
{
    static PostQueue q;
    return q;
}

// Records one accepted uber post (a2 == 0x2133, v0 != 0). Keeps TK47's log
// line. Drops (loudly) only past 64 unconsumed posts, which cannot happen:
// the reducer drains every VBlank and posts are minutes apart.
inline void noteUberPost(uint64_t tick)
{
    PostQueue &q = postQueue();
    const uint64_t n = q.count + 1u;
    if (q.tail - q.head < PostQueue::kCap)
    {
        q.ticks[q.tail % PostQueue::kCap] = tick;
        ++q.tail;
    }
    else
        std::fprintf(stderr, "[ssx3-tricky-hud] uber #%llu tick=%llu DROPPED (queue full)\n",
                     static_cast<unsigned long long>(n), static_cast<unsigned long long>(tick));
    q.count = n;
    std::fprintf(stderr, "[ssx3-tricky-hud] uber #%llu tick=%llu\n", static_cast<unsigned long long>(n),
                 static_cast<unsigned long long>(tick));
}

// ---- Stages 2-3: the EE-owned reducer --------------------------------------
struct Reducer
{
    bool inTricky = false;
    bool atlasTried = false;
    ps2_ssx3_tricky_hud::Atlas atlas;
    ps2_ssx3_tricky_hud::LetterState letters;
    bool lastFull = false;
    uint64_t splashUntil = 0u;
    ps2_ssx3_tricky_hud::RaceClock raceClock;
    bool lastRacing = false;
    uint32_t lastReplayState = 0u;
    RacePhase lastPhase = RacePhase::Frontend;
    bool phaseInit = false;
    bool wasOutsideLive = false;
    // Race-epoch detection (the same predicate the gems poll uses at its own
    // safe point; one definition in raceBoundaryCrossed).
    bool epochInit = false;
    uint32_t lastR = 0u;
    uint32_t lastClock = 0u;
};

inline Reducer &reducer()
{
    static Reducer r;
    return r;
}

inline void resetRunState(Reducer &r)
{
    r.letters = ps2_ssx3_tricky_hud::LetterState{};
    r.letters.seen = postQueue().count;
    r.lastFull = false;
    r.splashUntil = 0u;
    r.raceClock = ps2_ssx3_tricky_hud::RaceClock{};
    r.lastRacing = false;
    r.lastReplayState = 0u;
    r.phaseInit = false;
    r.lastPhase = RacePhase::Frontend;
    r.wasOutsideLive = false;
    r.epochInit = false;
    r.lastR = 0u;
    r.lastClock = 0u;
}

// One race boundary: bump the epoch, reset the run-scoped presentation
// state (letters, latches, splash), cancel any in-flight burst (leaving the
// race cancels) and open the post-boundary adopt window so the guest's
// meter re-init cannot edge. The gem bitmap resets via the poll's own
// same-predicate check at the next player-1 step (TK45c, unchanged).
inline void onRaceBoundary(Reducer &r, uint64_t tick, const char *reason)
{
    const uint64_t e = detail::liveEpoch().fetch_add(1u, std::memory_order_acq_rel) + 1u;
    r.letters.lit = 0;
    r.letters.flashUntil = 0;
    r.letters.seen = postQueue().count;
    r.lastFull = false;
    r.splashUntil = 0u;
    r.lastRacing = false;
    r.raceClock.adoptUntil = tick + ps2_ssx3_tricky_hud::kGoAdoptTicks;
    ps2_ssx3_tricky_song::cancelBurst();
    std::fprintf(stderr, "[ssx3-tricky-layer] race epoch %llu tick=%llu (%s)\n",
                 static_cast<unsigned long long>(e), static_cast<unsigned long long>(tick), reason);
}

// The per-VBlank EE entry. Reads committed guest words; the only guest
// writes in the layer are the gems poll's own (separate hook, unchanged).
// `tick` is the scheduler's post-increment VBlank tick, like every sibling
// onVBlank hook (fh1/ach/tel): one tick later than the presents that used to
// sample mid-interval, so splash/adopt/flash windows end one tick later;
// race booleans use relative tick math and are unaffected.
inline void onVBlankTick(uint8_t *rdram, size_t ramSize, uint64_t tick)
{
    const Config &cfg = config();
    if (!cfg.hud || !rdram || ramSize == 0u)
        return;
    Reducer &r = reducer();
    ps2_ssx3_course::Modes &ms = ps2_ssx3_course::courseModes();
    const size_t mode = ms.armed ? ps2_ssx3_course::modeCurrent(ms, rdram) : 0u;
    if (mode == 0u)
    {
        if (r.inTricky)
        {
            // Leaving Tricky: the generation changes (stale packets die),
            // the run state resets and any burst cancels.
            detail::liveEpoch().fetch_add(1u, std::memory_order_acq_rel);
            resetRunState(r);
            r.inTricky = false;
            ps2_ssx3_tricky_song::cancelBurst();
            ps2_ssx3_tricky_song::setSuspended(false);
            PresentationPacket p;
            p.tick = tick;
            p.epoch = epoch();
            publishPacket(p);
        }
        return;
    }
    if (!r.inTricky)
    {
        r.inTricky = true;
        r.letters.seen = postQueue().count;
        r.letters.lit = cfg.lettersPreset >= 0 ? cfg.lettersPreset : 0;
        r.letters.flashUntil = 0;
        if (cfg.lettersPreset >= 0)
            std::fprintf(stderr, "[ssx3-tricky-hud] letters preset=%d tick=%llu\n", cfg.lettersPreset,
                         static_cast<unsigned long long>(tick));
    }
    if (!r.atlasTried)
    {
        r.atlasTried = true;
        r.atlas = ps2_ssx3_tricky_hud::loadAtlasFile(cfg.artPath.empty() ? nullptr : cfg.artPath.c_str());
        std::fprintf(stderr, "[ssx3-tricky-hud] art '%s': %s\n", cfg.artPath.c_str(),
                     r.atlas.ok ? "loaded" : "missing/invalid, overlay off");
    }
    // Letters consume posts even when the meter words are unreadable, so no
    // stale backlog lights letters late. Each post lights at its own tick.
    {
        PostQueue &q = postQueue();
        while (q.head < q.tail)
        {
            const uint64_t pt = q.ticks[q.head % PostQueue::kCap];
            ++q.head;
            const int wasLit = r.letters.lit;
            const uint64_t wasFlash = r.letters.flashUntil;
            ps2_ssx3_tricky_hud::updateLetters(r.letters, r.letters.seen + 1u, pt);
            if (r.letters.lit != wasLit && r.letters.lit > 0)
            {
                static const char kName[7] = "TRICKY";
                std::fprintf(stderr, "[ssx3-tricky-hud] letter %c lit (%d/6) tick=%llu\n",
                             kName[r.letters.lit - 1], r.letters.lit,
                             static_cast<unsigned long long>(pt));
            }
            if (r.letters.flashUntil != 0u && wasFlash == 0u)
                std::fprintf(stderr, "[ssx3-tricky-hud] TRICKY spelled: fanfare flash until=%llu\n",
                             static_cast<unsigned long long>(r.letters.flashUntil));
            if (r.letters.lit == 0 && wasLit == 6)
                std::fprintf(stderr, "[ssx3-tricky-hud] letters reset tick=%llu\n",
                             static_cast<unsigned long long>(pt));
        }
        // Fanfare expiry runs every tick, with or without new posts.
        {
            const int wasLit = r.letters.lit;
            ps2_ssx3_tricky_hud::updateLetters(r.letters, r.letters.seen, tick);
            if (r.letters.lit == 0 && wasLit == 6)
                std::fprintf(stderr, "[ssx3-tricky-hud] letters reset tick=%llu\n",
                             static_cast<unsigned long long>(tick));
        }
    }
    // Rider + meter + race clock (F7-validated reads; failures skip the
    // edge but keep the letters above).
    uint32_t apR = 0u;
    if (cfg.apPtrSet)
    {
        uint32_t off = 0u;
        if (ps2_ssx3_tricky_hud::validateGuestPtr(cfg.apPtrAddr, 4u, ramSize, off))
            std::memcpy(&apR, rdram + off, 4);
    }
    const uint32_t chainR = ps2_ssx3_tricky_hud::resolveChainR(rdram, ramSize);
    const uint32_t rr = cfg.apPtrSet ? apR : chainR;
    const ps2_ssx3_tricky_hud::MeterFrame mf =
        ps2_ssx3_tricky_hud::readMeterFrameAt(rdram, ramSize, rr);
    // Race-only visibility from [B+0xc] (TK44; unreadable keeps the legacy
    // show, as before).
    bool racing = true;
    uint32_t clock = 0u;
    bool clockOk = false;
    {
        const uint32_t b = ps2_ssx3_tricky_hud::resolveChainB(rdram, ramSize);
        if (b != 0u && ps2_ssx3_tricky_hud::readGuestU32(rdram, ramSize, b,
                                                        ps2_ssx3_tricky_hud::kRaceClockOff, clock))
        {
            clockOk = true;
            racing = ps2_ssx3_tricky_hud::updateRaceClock(r.raceClock, clock, tick);
            // TR3: the results screen's auto replay restarts and runs the
            // race clock; it is not a live race (meter hidden, no edges,
            // song suspended). An unreadable replay keeps the clock's word.
            uint32_t rs = 0u;
            const bool rsOk = ps2_ssx3_tricky_hud::readReplayState(rdram, ramSize, rs);
            if (rsOk && rs != r.lastReplayState)
            {
                r.lastReplayState = rs;
                std::fprintf(stderr, "[ssx3-tricky-hud] replay state %u tick=%llu\n", rs,
                             static_cast<unsigned long long>(tick));
            }
            if (rsOk && rs != 0u)
                racing = false;
            if (racing != r.lastRacing)
            {
                r.lastRacing = racing;
                std::fprintf(stderr, "[ssx3-tricky-hud] race clock %s B=0x%08x clock=%u tick=%llu\n",
                             racing ? "running, meter shown" : "frozen, meter hidden", b, clock,
                             static_cast<unsigned long long>(tick));
            }
        }
    }
    // Race-epoch boundary (same predicate as the gems poll). New R or a
    // rewound clock resets the run state; the adopt window (opened here and
    // in updateRaceClock) adopts the guest's re-init writes silently.
    if (clockOk && rr != 0u)
    {
        if (!r.epochInit)
        {
            r.epochInit = true;
            r.lastR = rr;
            r.lastClock = clock;
        }
        else if (ps2_ssx3_tricky_hud::raceBoundaryCrossed(r.lastR, r.lastClock, rr, clock))
        {
            onRaceBoundary(r, tick, rr != r.lastR ? "new rider" : "clock rewound");
            r.lastR = rr;
            r.lastClock = clock;
            // The boundary sample adopts below (the window is open); the
            // race clock keeps its own continuity for the grace math.
        }
        else
        {
            r.lastR = rr;
            r.lastClock = clock;
        }
    }
    // The single context gate: draw only in the Racing phase (this also
    // kills TK44's 3-tick cleanup blips, which land outside a live event).
    // The full edge below stays on the proven `racing` predicate: a
    // transient phase dip must hide pixels, never swallow a burst.
    const bool eventLive =
        ps2_ssx3_course::appUpdateFn(rdram) == ps2_ssx3_course::kGameUpdate;
    bool loading = false;
    for (uint32_t i = 0; i < ps2_ssx3_course::kSlots; ++i)
        if (ps2_ssx3_course::rd32(rdram,
                                  ps2_ssx3_course::kSlotBase + i * ps2_ssx3_course::kSlotStride + 8u) ==
            ps2_ssx3_course::kSlotRequested)
        {
            loading = true;
            break;
        }
    const CourseContext cx = deriveContext(mode, eventLive, loading, racing, epoch(), tick);
    if (!r.phaseInit || cx.phase != r.lastPhase)
    {
        r.phaseInit = true;
        r.lastPhase = cx.phase;
        std::fprintf(stderr, "[ssx3-tricky-layer] phase %s tick=%llu (live=%d loading=%d racing=%d)\n",
                     phaseName(cx.phase), static_cast<unsigned long long>(tick), eventLive ? 1 : 0,
                     loading ? 1 : 0, racing ? 1 : 0);
    }
    // Rising-edge only: a racing clock outside a live event means the phase
    // gate hides pixels the old racing-only gate showed (cleanup blips are
    // the known case; anything mid-race needs a look).
    const bool outsideLive = racing && cx.phase != RacePhase::Racing;
    if (outsideLive && !r.wasOutsideLive)
        std::fprintf(stderr,
                     "[ssx3-tricky-layer] phase diag: racing clock outside a live event tick=%llu "
                     "(live=%d loading=%d clock=%u)\n",
                     static_cast<unsigned long long>(tick), eventLive ? 1 : 0, loading ? 1 : 0, clock);
    r.wasOutsideLive = outsideLive;
    const bool meterOk = mf.ok;
    const bool full = meterOk && mf.level >= 1;
    if (meterOk)
    {
        if (!racing)
            r.lastFull = full; // silent: a hidden edge fires no splash/burst
        else
        {
            if (tick <= r.raceClock.adoptUntil)
                r.lastFull = full; // post-boundary adopt window (TK47)
            if (full && !r.lastFull)
            {
                r.splashUntil = tick + 45u;
                ps2_ssx3_tricky_song::startBurst(tick);
            }
            r.lastFull = full;
        }
    }
    // Pause suspends the song; leaving the race (above) cancels it. Racing
    // resumes an in-flight burst exactly where it suspended (mixInto never
    // consumes while suspended).
    ps2_ssx3_tricky_song::setSuspended(contextSuspendsSong(cx));
    PresentationPacket p;
    p.tick = tick;
    p.epoch = epoch();
    p.racing = racing;
    p.splashUntil = r.splashUntil;
    p.litLetters = r.letters.lit;
    p.flashUntil = r.letters.flashUntil;
    if (contextAllowsHud(cx) && meterOk && r.atlas.ok)
    {
        p.draw = true;
        p.atlas = &r.atlas;
        // F9: FORCE is a display override only; the edge above always uses
        // the real meter words, so a diagnostic can neither create a burst
        // nor (with missing art) silence one.
        p.fill = cfg.forced.ok ? cfg.forced.fill : mf.fill;
        p.full = cfg.forced.ok ? (cfg.forced.level >= 1) : full;
    }
    publishPacket(p);
}

// ---- Stage 3: quick-load reconciliation (TKA1 F1) ---------------------------
// Runs once at load completion (boot-time and quick loads funnel through
// loadImpl), before guest execution resumes and before any next CD read:
// the restored RAM's mode selects the pending CD aliases directly, bypassing
// modeSet's cur==want early return. Then a new epoch retires stale packets
// and bursts, the run state resets, and the gems poll restarts reset-only
// (TK45c's stated policy: the bitmap drops; the saved multiplier word is
// guest state and restores exactly).
inline void onStateLoaded(uint8_t *rdram, size_t ramSize, uint64_t tick)
{
    if (!rdram || ramSize == 0u)
        return;
    ps2_ssx3_course::Modes &ms = ps2_ssx3_course::courseModes();
    if (ms.armed)
    {
        const size_t cur = ps2_ssx3_course::modeCurrent(ms, rdram);
        // TKL1 merge: the menu wrapper's Tricky flag means "modeSet(true)
        // is in effect" (Tricky rows + aliases); reconcile it with the
        // restored mode so a later menu exit cleans up correctly (F5 tail).
        ps2_ssx3_tricky::state().tricky = (cur != 0u);
        if (cur != 0u && cur - 1u < ms.modes.size())
        {
            const ps2_ssx3_course::Mode &m = ms.modes[cur - 1u];
            std::vector<std::pair<std::string, std::string>> list;
            for (const ps2_ssx3_course::ModeAlias &a : m.aliases)
                list.emplace_back(a.disc, a.host);
            ps2_cd_overlay::setModeAliases(list);
            std::fprintf(stderr, "[ssx3-tricky-layer] load: mode=%s aliases=%u tick=%llu\n",
                         m.name.c_str(), static_cast<unsigned>(list.size()),
                         static_cast<unsigned long long>(tick));
        }
        else
        {
            ps2_cd_overlay::clearModeAlias();
            std::fprintf(stderr, "[ssx3-tricky-layer] load: mode=Stock aliases=0 tick=%llu\n",
                         static_cast<unsigned long long>(tick));
        }
    }
    if (!active())
        return;
    detail::liveEpoch().fetch_add(1u, std::memory_order_acq_rel);
    Reducer &r = reducer();
    resetRunState(r);
    r.inTricky = false;
    PostQueue &q = postQueue();
    q.head = q.tail; // drop pre-load posts (the old timeline's events)
    ps2_ssx3_tricky_song::cancelBurst();
    ps2_ssx3_tricky_song::setSuspended(false);
    if (config().gems)
        ps2_tk45c::fullReset(ps2_tk45c::state(), ps2_tk45c::table(), rdram, ramSize, tick);
    PresentationPacket p;
    p.tick = tick;
    p.epoch = epoch();
    publishPacket(p);
}

} // namespace ps2_ssx3_tricky_layer
