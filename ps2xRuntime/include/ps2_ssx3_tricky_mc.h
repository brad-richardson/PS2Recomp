// tk-announcer-1 Part 2: queue-time Tricky MC translator.
//
// On Tricky courses with PS2X_SSX3_TRICKY_MC=1, SSX 3 speech-queue posts
// (guest 0x2B1458) for the DIRECT families are translated into Tricky MC
// commentary: posts are remapped to PA-channel events whose banks carry the
// MC curation (data companion), or gated in place, under Tricky's own
// anti-repetition rules (Part 1: prob/age-hold/rep/prio per MC event).
// Dropped posts observe v0=0 (rejected), the queue's own reject value:
// that IS the Silence alternative. Default off / stock courses: every post
// passes through untouched (four det keys IDENTICAL).
//
// tk-audio-1 owns 0x20cf/0x20bb/0x20e5 (DJ/PA gate): this translator never
// touches those IDs, so the two wrappers compose by chaining.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ps2_ssx3_course_manifest.h"
#include "ps2_ssx3_tricky_hud.h"

namespace ps2_ssx3_tricky_mc
{
// PS2X_SSX3_TRICKY_MC=1 arms the translator. Default off.
inline bool translatorEnabled()
{
    const char *env = std::getenv("PS2X_SSX3_TRICKY_MC");
    return env && env[0] == '1';
}

// Retired SSX 3 lines: natural posts the translator owns outright. The
// sponsor (0x20C0) and finish (0x20C5) stock lines must never sound and no
// curated bank voices them, so their naturals drop. One prob-0 family:
// decide() always drops naturals. Medals (0x20C7) is untouched. Part 3c:
// start cues (venue, radio, artist, greeting) pass through untouched —
// only naturals through replaced banks voice, so a2-rewriting remaps and
// the venue retirement are gone (bb voices the flyover from its bank).
constexpr uint32_t kRetireFamily = 0x20f0u;

// Tricky MC rule row (Part 1 receipts/mc-events-full.tsv): prob = signed
// acceptance byte +9 (0..100); rep = repeat-limit byte +8 (0 = unlimited);
// hold = re-post holdoff in ticks (age field +2, conservative: raw value);
// prio = queue-replacement priority +4 (recorded; admission stays guest's).
struct Rule
{
    uint32_t mcEvent; // Tricky MC event id (0x200f..0x2056, 0x2012 = takeoff)
    uint8_t prob;
    uint8_t rep;
    uint16_t hold;
    uint16_t prio;
};

// Route: an SSX 3 post classified to an MC family. outEvent==ssx3Event means
// gate in place (content curated in the same bank); otherwise the post is
// remapped to a PA-channel event whose bank carries the MC curation.
struct Route
{
    uint32_t ssx3Event;
    uint32_t mcEvent;
    uint32_t outEvent;
    uint8_t outT0; // PA channel for remaps; 0 = keep caller's t0
};

inline const Rule *ruleTable(size_t *countOut)
{
    // Covers the (2a) DIRECT families + takeoff (2b). MS_ pools, Sweet,
    // RideClose/RideAproach, PlayerID and Land content are v2 (no vessel).
    static const Rule kRules[] = {
        {0x2030u, 50, 1, 100, 500}, // Big_Air (+Takeoff pool in 0x20C3 bank)
        {0x2012u, 100, 0, 100, 500}, // Takeoff edge (synthesized)
        {0x202eu, 50, 1, 120, 500}, // Wipeout
        {0x2050u, 100, 0, 100, 500}, // Knockdown (via 0x2077 remap)
        {0x204cu, 100, 1, 100, 600}, // Uber
        {0x204eu, 100, 1, 100, 600}, // TrickBonus (multiplier posts)
        {0x2031u, 50, 1, 100, 500}, // Combo_Trick (remapped trick tiers)
        {0x2033u, 50, 1, 100, 500}, // Turbo
        {0x2047u, 100, 0, 100, 800}, // Boost_Icon (Prompts vessel)
        {0x2046u, 50, 1, 100, 500}, // MS_Powerbar_Deplete
        {0x202au, 100, 0, 100, 500}, // Pass
        {0x202bu, 100, 0, 100, 500}, // Positioning
        {0x2049u, 100, 0, 100, 500}, // PlayerID/Position (bank holds Position)
        {0x204fu, 50, 1, 100, 500}, // Agression (rider-wide curation)
        {kRetireFamily, 0, 0, 0, 500}, // Retired lines: prob 0 drops naturals
    };
    static_assert(sizeof(kRules) / sizeof(kRules[0]) <= 16u,
                  "rule table exceeds TranslatorState family slots");
    if (countOut)
        *countOut = sizeof(kRules) / sizeof(kRules[0]);
    return kRules;
}

inline const Rule *findRule(uint32_t mcEvent)
{
    size_t n = 0u;
    const Rule *t = ruleTable(&n);
    for (size_t i = 0u; i < n; ++i)
        if (t[i].mcEvent == mcEvent)
            return &t[i];
    return nullptr;
}

inline const Route *routeTable(size_t *countOut)
{
    static const Route kRoutes[] = {
        // Big_Air family -> PA_Medal_Run_Intro vessel (0x20C3, race-silent).
        {0x2083u, 0x2030u, 0x20c3u, 0xbu},
        {0x2084u, 0x2030u, 0x20c3u, 0xbu},
        {0x2091u, 0x2030u, 0x20c3u, 0xbu},
        // Wipeout family -> PA_Rider_Intro vessel (0x20C1, race-silent).
        {0x2088u, 0x202eu, 0x20c1u, 0xbu},
        {0x20a0u, 0x202eu, 0x20c1u, 0xbu},
        {0x20a1u, 0x202eu, 0x20c1u, 0xbu},
        // Collision -> Knockdown vessel (0x20C2, race-silent), heavy hold.
        {0x2077u, 0x2050u, 0x20c2u, 0xbu},
        // Position family -> PA_Rider_Postition (0x20C4, shared context).
        {0x2080u, 0x2049u, 0x20c4u, 0xbu},
        {0x207du, 0x202au, 0x20c4u, 0xbu},
        {0x20d3u, 0x2049u, 0x20c4u, 0xbu},
        {0x20d4u, 0x2049u, 0x20c4u, 0xbu},
        // Trick tiers -> Arcade_Icons (0x2145, combo/multiplier context).
        {0x2082u, 0x2031u, 0x2145u, 0xcu},
        {0x2090u, 0x2031u, 0x2145u, 0xcu},
        {0x209au, 0x2031u, 0x2145u, 0xcu},
        {0x209bu, 0x2031u, 0x2145u, 0xcu},
        {0x209cu, 0x2031u, 0x2145u, 0xcu},
        {0x209du, 0x2031u, 0x2145u, 0xcu},
        // Arcade families: gate in place (MC content curated in same bank).
        {0x20a7u, 0x2033u, 0x20a7u, 0x0u}, // Turbo (Power_Ups vessel)
        {0x20a8u, 0x2047u, 0x20a8u, 0x0u}, // Boost_Icon (Prompts vessel)
        {0x20a9u, 0x204eu, 0x20a9u, 0x0u}, // TrickBonus (Bonus vessel)
        {0x2133u, 0x204cu, 0x2133u, 0x0u}, // Uber (Uber vessel, TK43b content)
        {0x2145u, 0x204eu, 0x2145u, 0x0u}, // Icons natural posts (multiplier)
        // Agression responses: gate in place (rider-wide MC curation).
        {0x207eu, 0x204fu, 0x207eu, 0x0u},
        // Part 3h: the flyover is removed, so the venue natural retires
        // again (loading silence; v0=0 kills the fetch). The call voices
        // from the artist bank (cf natural) via the 2f composite; only the
        // sponsor/finish stock lines retire alongside venue.
        {0x20bbu, kRetireFamily, 0x20bbu, 0x0u},
        {0x20c0u, kRetireFamily, 0x20c0u, 0x0u},
        {0x20c5u, kRetireFamily, 0x20c5u, 0x0u},
    };
    if (countOut)
        *countOut = sizeof(kRoutes) / sizeof(kRoutes[0]);
    return kRoutes;
}

inline const Route *findRoute(uint32_t ssx3Event)
{
    size_t n = 0u;
    const Route *t = routeTable(&n);
    for (size_t i = 0u; i < n; ++i)
        if (t[i].ssx3Event == ssx3Event)
            return &t[i];
    return nullptr;
}

// Deterministic acceptance draw: hash(tick, mcEvent, occurrence) % 100.
// No host RNG anywhere on this path, so Tricky-on runs stay deterministic.
inline uint32_t acceptHash(uint64_t tick, uint32_t mcEvent, uint64_t occurrence)
{
    uint64_t h = tick ^ (static_cast<uint64_t>(mcEvent) * 0x9e3779b97f4a7c15ull) ^
                 (occurrence * 0xbf58476d1ce4e5b9ull);
    h ^= h >> 29u;
    h *= 0x165667b19e3779f9ull;
    h ^= h >> 32u;
    return static_cast<uint32_t>(h % 100u);
}

struct FamilyState
{
    uint64_t lastAcceptTick = 0u;
    bool hasAccept = false;
    uint64_t occurrence = 0u; // accepted posts of this family
    uint32_t consec = 0u;     // consecutive accepts without interleave
};

struct TranslatorState
{
    FamilyState families[16];
    uint32_t lastFamily = 0u; // mcEvent of the last accepted family post
    uint64_t lastRider = 0u;  // race-boundary detector (rider ptr change)
    uint64_t lastTick = 0u;
};

inline FamilyState *familyState(TranslatorState &s, uint32_t mcEvent)
{
    size_t n = 0u;
    const Rule *t = ruleTable(&n);
    for (size_t i = 0u; i < n; ++i)
        if (t[i].mcEvent == mcEvent)
            return &s.families[i];
    return nullptr;
}

enum class Verdict
{
    Pass,  // untouched
    Remap, // apply outEvent/outT0, then call the original
    Drop,   // Silence: return v0=0 without calling the original
};

struct Decision
{
    Verdict verdict = Verdict::Pass;
    uint32_t outEvent = 0u;
    uint8_t outT0 = 0u;
};

// Pure decision function (unit-tested): classify one post.
inline Decision decide(TranslatorState &s, const Route &route, const Rule &rule, uint64_t tick,
                       bool synth)
{
    Decision d;
    FamilyState *f = familyState(s, rule.mcEvent);
    if (!f)
        return d;
    // Hold (age): minimum spacing between accepted posts of this family.
    // Synthesized trigger edges obey hold but skip prob (they ARE the event).
    if (f->hasAccept && tick >= f->lastAcceptTick && tick - f->lastAcceptTick < rule.hold)
    {
        d.verdict = Verdict::Drop;
        return d;
    }
    if (!synth && rule.prob < 100u)
    {
        if (acceptHash(tick, rule.mcEvent, f->occurrence) >= rule.prob)
        {
            d.verdict = Verdict::Drop;
            return d;
        }
    }
    // Repeat limit: more than `rep` consecutive same-family accepts (with no
    // other family interleaving) is Silence. rep==0 means unlimited.
    if (rule.rep > 0u && s.lastFamily == rule.mcEvent && f->consec >= rule.rep)
    {
        d.verdict = Verdict::Drop;
        return d;
    }
    if (route.outEvent != route.ssx3Event || route.outT0 != 0u)
    {
        d.verdict = Verdict::Remap;
        d.outEvent = route.outEvent;
        d.outT0 = route.outT0;
    }
    else
    {
        // Gate-in-place accept: still counts as an accepted family post.
        d.verdict = Verdict::Remap;
        d.outEvent = route.ssx3Event;
        d.outT0 = 0u;
    }
    return d;
}

// Record an accepted post (called by the wrapper after the original runs).
inline void noteAccepted(TranslatorState &s, uint32_t mcEvent, uint64_t tick)
{
    FamilyState *f = familyState(s, mcEvent);
    if (!f)
        return;
    if (s.lastFamily == mcEvent)
        ++f->consec;
    else
    {
        // A different family posting breaks every consecutive run.
        for (size_t i = 0u; i < 16u; ++i)
            s.families[i].consec = 0u;
        f->consec = 1u;
    }
    s.lastFamily = mcEvent;
    f->lastAcceptTick = tick;
    f->hasAccept = true;
    ++f->occurrence;
}

// Race boundary: rider pointer change or tick rewind resets repetition state.
inline void noteBoundary(TranslatorState &s, uint64_t rider, uint64_t tick)
{
    if (rider != s.lastRider || tick < s.lastTick)
    {
        for (size_t i = 0u; i < 16u; ++i)
            s.families[i].consec = 0u;
        s.lastFamily = 0u;
    }
    s.lastRider = rider;
    s.lastTick = tick;
}

// ---- (2b) motion-mode air detector --------------------------------------
// ssxdecomp rider-motion (PS2-verified): motion owner = *(rider + 0x77C),
// mode = *(owner + 0xDE0): 0 cruise, 1 air, 2 wipeout. Polled per VBlank
// with the layer's validated-read discipline; any failure skips the sample.
constexpr uint32_t kMotionOwnerOff = 0x77cu;
constexpr uint32_t kMotionModeOff = 0xde0u;

inline bool readGuestU32Mc(const uint8_t *ram, size_t ramSize, uint32_t addr, uint32_t &out)
{
    if (!ram || ramSize == 0u)
        return false;
    uint64_t a = static_cast<uint64_t>(addr);
    if (a + 4u > ramSize || (a & 3u) != 0u)
        return false;
    std::memcpy(&out, ram + a, 4);
    return true;
}

struct AirState
{
    int lastMode = -1; // -1 = unknown (menus / failed reads)
    uint64_t takeoffTick = 0u;
    uint64_t landTick = 0u;
    uint64_t landReported = 0u;
    bool takeoffPending = false;
    bool landPending = false;
    bool validSeen = false; // latched once modes 0/1/2 observed in a race
    bool validLogged = false;
};

// Returns true when a sample was taken. Edges set pending flags; the poll
// site synthesizes the takeoff post (landing is logged: no v1 vessel).
inline bool pollAirMode(AirState &a, const uint8_t *rdram, size_t ramSize, uint32_t rider,
                        uint64_t tick, bool racing)
{
    if (!racing || rider == 0u)
    {
        a.lastMode = -1;
        return false;
    }
    uint32_t owner = 0u, mode = 0u;
    if (!readGuestU32Mc(rdram, ramSize, rider + kMotionOwnerOff, owner) || owner == 0u)
        return false;
    if (!readGuestU32Mc(rdram, ramSize, owner + kMotionModeOff, mode) || mode > 5u)
        return false;
    const int m = static_cast<int>(mode);
    if (m <= 2)
        a.validSeen = true;
    if (a.lastMode >= 0)
    {
        if (a.lastMode == 0 && m == 1)
        {
            a.takeoffPending = true;
            a.takeoffTick = tick;
        }
        else if (a.lastMode == 1 && m == 0)
        {
            // Clean touchdown. A wipeout posts for mode 2 instead (the
            // translator sees it); landing content has no v1 vessel, so the
            // edge is logged and counted, not posted.
            a.landPending = true;
            a.landTick = tick;
        }
        else if (m == 2)
        {
            // Failed landing: the Wipeout family owns this moment; a pending
            // clean-land edge from the same jump must not double up.
            a.landPending = false;
        }
        else if (m > 2)
        {
            a.takeoffPending = false;
            a.landPending = false;
        }
    }
    a.lastMode = m;
    return true;
}

// ---- cross-TU shared state (single instance via inline accessors) --------
inline TranslatorState &translatorState()
{
    static TranslatorState s;
    return s;
}

inline AirState &airState()
{
    static AirState a;
    return a;
}

// Synthesized-post emitter, registered by the wrapper TU at apply time.
// Emits one guest speech post (a2/t0/a0) outside any live interception.
using EmitFn = void (*)(void *impl, uint8_t *rdram, uint32_t a0, uint32_t a2, uint8_t t0,
                        uint64_t tick);

struct PollControl
{
    bool armed = false;
    EmitFn emit = nullptr;
    void *emitImpl = nullptr;
    uint32_t lastClock = 0u;
    bool clockInit = false;
};

inline PollControl &pollControl()
{
    static PollControl p;
    return p;
}

// Per-VBlank (2b) poll, called from the scheduler next to the layer tick.
// Read-only wrt guest except through the registered emitter. One branch
// when disarmed; fully skipped on stock courses.
inline void pollMcTick(uint8_t *rdram, size_t ramSize, uint64_t tick)
{
    PollControl &pc = pollControl();
    if (!pc.armed || pc.emit == nullptr || rdram == nullptr || ramSize == 0u)
        return;
    ps2_ssx3_course::Modes &ms = ps2_ssx3_course::courseModes();
    if (!ms.armed || ps2_ssx3_course::modeCurrent(ms, rdram) == 0u)
        return;
    const uint32_t rr = ps2_ssx3_tricky_hud::resolveChainR(rdram, ramSize);
    if (rr == 0u)
        return;
    // Live-race predicate (layer shape): readable advancing clock, no replay.
    // A stuck or unreadable clock reads as still (never as racing): clock
    // granularity and single-poll read misses must not flap the GO edge, so
    // the edge keys on stillness duration, not on the previous poll.
    const uint32_t b = ps2_ssx3_tricky_hud::resolveChainB(rdram, ramSize);
    bool racing = false;
    uint32_t clock = 0u;
    if (b != 0u &&
        ps2_ssx3_tricky_hud::readGuestU32(rdram, ramSize, b,
                                         ps2_ssx3_tricky_hud::kRaceClockOff, clock))
    {
        uint32_t rs = 0u;
        const bool rsOk = ps2_ssx3_tricky_hud::readReplayState(rdram, ramSize, rs);
        racing = (!rsOk || rs == 0u) && pc.clockInit && clock != pc.lastClock;
        pc.clockInit = true;
        pc.lastClock = clock;
    }
    AirState &a = airState();
    if (!pollAirMode(a, rdram, ramSize, rr, tick, racing))
        return;
    if (!a.validSeen)
        return;
    if (!a.validLogged)
    {
        a.validLogged = true;
        std::fprintf(stderr, "[ssx3-tricky-mc] air detector live mode=%d tick=%llu\n",
                     a.lastMode, static_cast<unsigned long long>(tick));
    }
    // Landing edges are logged (no v1 content vessel); wipeouts clear them.
    if (a.landPending && a.landTick != a.landReported)
    {
        a.landReported = a.landTick;
        std::fprintf(stderr, "[ssx3-tricky-mc] landing edge tick=%llu (logged only)\n",
                     static_cast<unsigned long long>(a.landTick));
    }
    if (!a.takeoffPending)
        return;
    a.takeoffPending = false;
    // Shared 0x20C3 bank: takeoff lines ride with Big_Air. Both holds must
    // be clear (one line at a time); the Takeoff rule skips prob (synth).
    TranslatorState &s = translatorState();
    const Rule *takeoff = findRule(0x2012u);
    const Rule *bigair = findRule(0x2030u);
    const Route *rt = findRoute(0x2084u); // Big_Air route carries the vessel
    if (!takeoff || !bigair || !rt)
        return;
    noteBoundary(s, static_cast<uint64_t>(rr), tick);
    Decision dTake = decide(s, *rt, *takeoff, tick, true);
    if (dTake.verdict == Verdict::Drop)
        return;
    FamilyState *fb = familyState(s, 0x2030u);
    if (fb && fb->hasAccept && tick >= fb->lastAcceptTick && tick - fb->lastAcceptTick < bigair->hold)
        return; // Big_Air just spoke: stay silent (one at a time)
    std::fprintf(stderr, "[ssx3-tricky-mc] takeoff synth a2=0x20c3 tick=%llu\n",
                 static_cast<unsigned long long>(tick));
    pc.emit(pc.emitImpl, rdram, rr, 0x20c3u, 0xbu, tick);
    noteAccepted(s, 0x2012u, tick);
}

} // namespace ps2_ssx3_tricky_mc
