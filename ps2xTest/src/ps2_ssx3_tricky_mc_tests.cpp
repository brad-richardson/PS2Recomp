#include "MiniTest.h"
#include "ps2_ssx3_tricky_mc.h"

#include <cstdlib>
#include <string>

// tk-announcer-1 Part 2: translator rule-table, routing and gating decisions.
namespace
{
struct EnvGuard
{
    std::string name;
    std::string old;
    bool had = false;
    explicit EnvGuard(const char *n) : name(n)
    {
        if (const char *v = std::getenv(n))
        {
            old = v;
            had = true;
        }
    }
    ~EnvGuard()
    {
        if (had)
            ::setenv(name.c_str(), old.c_str(), 1);
        else
            ::unsetenv(name.c_str());
    }
};

const ps2_ssx3_tricky_mc::Route *route(uint32_t ssx3)
{
    return ps2_ssx3_tricky_mc::findRoute(ssx3);
}

const ps2_ssx3_tricky_mc::Rule *rule(uint32_t mc)
{
    return ps2_ssx3_tricky_mc::findRule(mc);
}
} // namespace

void register_ps2_ssx3_tricky_mc_tests()
{
    using namespace ps2_ssx3_tricky_mc;
    MiniTest::Case(
        "Ps2Ssx3TrickyMc", [](TestCase &tc)
        {
            tc.Run("knob defaults off, =1 arms", [](TestCase &t)
                   {
            EnvGuard g("PS2X_SSX3_TRICKY_MC");
            ::unsetenv("PS2X_SSX3_TRICKY_MC");
            t.IsTrue(!translatorEnabled(), "unset is off");
            ::setenv("PS2X_SSX3_TRICKY_MC", "0", 1);
            t.IsTrue(!translatorEnabled(), "0 is off");
            ::setenv("PS2X_SSX3_TRICKY_MC", "1", 1);
            t.IsTrue(translatorEnabled(), "=1 is on"); });

            tc.Run("Part 3b start cues remap to the Tricky vessels", [](TestCase &t)
                   {
            const Route *r = route(0x20e5u);
            t.IsTrue(r != nullptr && r->mcEvent == 0x2020u && r->outEvent == 0x20bbu &&
                         r->outT0 == 0xbu,
                     "radio intro -> venue call");
            r = route(0x20cfu);
            t.IsTrue(r != nullptr && r->mcEvent == 0x201eu && r->outEvent == 0x20c5u &&
                         r->outT0 == 0xbu,
                     "artist intro -> Tricky Go");
            r = route(0x20bcu);
            t.IsTrue(r != nullptr && r->mcEvent == kFlyoverFamily &&
                         r->outEvent == 0x20c0u && r->outT0 == 0xbu,
                     "menu greeting -> flyover");
            // Rules accept always (prob 100, no hold, no rep limit).
            for (uint32_t fam : {0x2020u, 0x201eu, kFlyoverFamily})
            {
                const Rule *rl = rule(fam);
                t.IsTrue(rl != nullptr && rl->prob == 100u && rl->hold == 0u && rl->rep == 0u,
                         "start-cue rule always accepts");
            }
            // decide() remaps the first occurrence live.
            TranslatorState s;
            r = route(0x20e5u);
            Decision d = decide(s, *r, *rule(0x2020u), 4095u, false);
            t.IsTrue(d.verdict == Verdict::Remap && d.outEvent == 0x20bbu, "venue remaps");
            r = route(0x20cfu);
            d = decide(s, *r, *rule(0x201eu), 4095u, false);
            t.IsTrue(d.verdict == Verdict::Remap && d.outEvent == 0x20c5u, "Go remaps");
            r = route(0x20bcu);
            d = decide(s, *r, *rule(kFlyoverFamily), 1540u, false);
            t.IsTrue(d.verdict == Verdict::Remap && d.outEvent == 0x20c0u,
                     "flyover remaps"); });

            tc.Run("Part 3 retirements drop naturals", [](TestCase &t)
                   {
            for (uint32_t a2 : {0x20bbu, 0x20c0u, 0x20c5u})
            {
                const Route *rt = route(a2);
                t.IsTrue(rt != nullptr && rt->mcEvent == kRetireFamily, "retire route");
                const Rule *rl = rule(kRetireFamily);
                t.IsTrue(rl != nullptr && rl->prob == 0u, "prob-0 family");
                TranslatorState s;
                Decision d = decide(s, *rt, *rl, 5000u, false);
                t.IsTrue(d.verdict == Verdict::Drop, "natural drops");
            }
            t.IsTrue(route(0x20c7u) == nullptr, "medals untouched"); });

            tc.Run("remap routes hit the PA vessels", [](TestCase &t)
                   {
            const Route *r = route(0x2084u);
            t.IsTrue(r != nullptr && r->mcEvent == 0x2030u, "Big_Air family");
            t.IsTrue(r->outEvent == 0x20c3u && r->outT0 == 0xbu, "PA medal-run vessel");
            r = route(0x20a0u);
            t.IsTrue(r != nullptr && r->outEvent == 0x20c1u, "Wipeout vessel");
            r = route(0x2077u);
            t.IsTrue(r != nullptr && r->outEvent == 0x20c2u, "Knockdown vessel");
            r = route(0x207du);
            t.IsTrue(r != nullptr && r->outEvent == 0x20c4u, "Position vessel");
            r = route(0x209bu);
            t.IsTrue(r != nullptr && r->outEvent == 0x2145u && r->outT0 == 0xcu,
                      "trick tiers to Icons"); });

            tc.Run("arcade and aggression gate in place", [](TestCase &t)
                   {
            const Route *r = route(0x2133u);
            t.IsTrue(r != nullptr && r->outEvent == 0x2133u && r->outT0 == 0u, "Uber in place");
            r = route(0x207eu);
            t.IsTrue(r != nullptr && r->outEvent == 0x207eu, "aggression in place");
            t.IsTrue(route(0x2081u) == nullptr, "hits untouched");
            t.IsTrue(route(0x20b8u) == nullptr, "Whooh untouched"); });

            tc.Run("Part-1 rule rows are present", [](TestCase &t)
                   {
            const Rule *r = rule(0x2030u);
            t.IsTrue(r != nullptr && r->prob == 50u && r->rep == 1u && r->hold == 100u,
                      "Big_Air 50/1/100");
            r = rule(0x204cu);
            t.IsTrue(r != nullptr && r->prob == 100u && r->rep == 1u && r->prio == 600u,
                      "Uber 100/1/prio600");
            r = rule(0x2047u);
            t.IsTrue(r != nullptr && r->prio == 800u, "Boost_Icon prio800");
            r = rule(0x204fu);
            t.IsTrue(r != nullptr && r->prob == 50u, "Agression prob50"); });

            tc.Run("prob-100 accepts; rep-1 drops the immediate repeat", [](TestCase &t)
                   {
            TranslatorState s;
            // Uber (prob 100, rep 1): first post accepts (remap in place).
            const Route *ru = route(0x2133u);
            const Rule *rl = rule(0x204cu);
            Decision d = decide(s, *ru, *rl, 1000u, false);
            t.IsTrue(d.verdict == Verdict::Remap && d.outEvent == 0x2133u,
                      "first Uber accepts");
            noteAccepted(s, 0x204cu, 1000u);
            // Past hold but consecutive: rep-1 drops (prob never blocks).
            d = decide(s, *ru, *rl, 1200u, false);
            t.IsTrue(d.verdict == Verdict::Drop, "rep-1 drops repeat"); });

            tc.Run("hold blocks inside the window", [](TestCase &t)
                   {
            TranslatorState s;
            const Route *ru = route(0x2133u);
            const Rule *rl = rule(0x204cu);
            noteAccepted(s, 0x204cu, 1000u);
            Decision d = decide(s, *ru, *rl, 1050u, false);
            t.IsTrue(d.verdict == Verdict::Drop, "hold blocks at +50");
            // Past hold the rep rule still applies (consecutive Uber).
            d = decide(s, *ru, *rl, 1200u, false);
            t.IsTrue(d.verdict == Verdict::Drop, "rep still drops past hold");
            // A Knockdown accept is a different family: hold is per-family.
            const Route *rk = route(0x2077u);
            const Rule *rkl = rule(0x2050u);
            d = decide(s, *rk, *rkl, 1050u, false);
            t.IsTrue(d.verdict == Verdict::Remap && d.outEvent == 0x20c2u,
                      "per-family hold, Knockdown remaps"); });

            tc.Run("rep-1 drops back-to-back, interleave resets", [](TestCase &t)
                   {
            TranslatorState s;
            const Route *ru = route(0x2133u);
            const Rule *rl = rule(0x204cu);
            noteAccepted(s, 0x204cu, 1000u);
            // Second Uber past hold but consecutive: rep=1 drops.
            Decision d = decide(s, *ru, *rl, 1300u, false);
            t.IsTrue(d.verdict == Verdict::Drop, "rep-1 drops repeat");
            // A different family interleaves, then Uber accepts again.
            noteAccepted(s, 0x202au, 1400u);
            d = decide(s, *ru, *rl, 1500u, false);
            t.IsTrue(d.verdict == Verdict::Remap, "interleave resets rep"); });

            tc.Run("prob-50 is deterministic", [](TestCase &t)
                   {
            const uint32_t a = acceptHash(7995u, 0x2030u, 0u);
            const uint32_t b = acceptHash(7995u, 0x2030u, 0u);
            t.IsTrue(a == b && a < 100u, "same inputs, same draw");
            // Half the first 200 (tick, occurrence) draws accept, roughly.
            int hits = 0;
            for (uint64_t k = 0u; k < 200u; ++k)
                hits += (acceptHash(5000u + k, 0x2030u, k) < 50u) ? 1 : 0;
            t.IsTrue(hits > 60 && hits < 140, "draw is ~uniform"); });

            tc.Run("synth skips prob but obeys hold and rep", [](TestCase &t)
                   {
            TranslatorState s;
            const Route *rt = route(0x2084u); // Big_Air remap, prob 50
            const Rule *rl = rule(0x2030u);
            noteAccepted(s, 0x2030u, 1000u);
            Decision d = decide(s, *rt, *rl, 1010u, true);
            t.IsTrue(d.verdict == Verdict::Drop, "synth obeys hold");
            // Past hold, the shared-bank rep rule still bites (same family).
            d = decide(s, *rt, *rl, 1200u, true);
            t.IsTrue(d.verdict == Verdict::Drop, "synth obeys rep");
            // On a tick whose natural draw would fail, a fresh synth accepts:
            // find a failing draw first (prob path would drop it).
            uint64_t badTick = 0u;
            for (uint64_t k = 2000u; k < 3000u; ++k)
                if (acceptHash(k, 0x2030u, 0u) >= 50u)
                {
                    badTick = k;
                    break;
                }
            t.IsTrue(badTick != 0u, "a failing draw exists");
            TranslatorState s2;
            d = decide(s2, *rt, *rl, badTick, false);
            t.IsTrue(d.verdict == Verdict::Drop, "natural prob drops it");
            d = decide(s2, *rt, *rl, badTick, true);
            t.IsTrue(d.verdict == Verdict::Remap && d.outEvent == 0x20c3u,
                      "synth skips prob"); });

            tc.Run("boundary resets repetition", [](TestCase &t)
                   {
            TranslatorState s;
            noteAccepted(s, 0x204cu, 1000u);
            noteBoundary(s, 7u, 1000u);
            const Route *ru = route(0x2133u);
            const Rule *rl = rule(0x204cu);
            noteBoundary(s, 9u, 500u); // new rider, rewound tick
            Decision d = decide(s, *ru, *rl, 1300u, false);
            t.IsTrue(d.verdict == Verdict::Remap, "fresh boundary accepts"); });

            tc.Run("air poll edges", [](TestCase &t)
                   {
            AirState a;
            uint8_t ram[0x2000];
            std::memset(ram, 0, sizeof ram);
            // rider=0x1000, owner=0x1100, mode at 0x1100+0xde0... too far;
            // use a compact rider at 0x100 with owner at 0x200.
            const uint32_t rider = 0x100u, owner = 0x200u;
            std::memcpy(ram + rider + 0x77cu, &owner, 4);
            uint32_t m0 = 0u;
            std::memcpy(ram + owner + 0xde0u, &m0, 4);
            t.IsTrue(pollAirMode(a, ram, sizeof ram, rider, 10u, true), "cruise samples");
            t.IsTrue(!a.takeoffPending, "no edge yet");
            uint32_t m1 = 1u;
            std::memcpy(ram + owner + 0xde0u, &m1, 4);
            t.IsTrue(pollAirMode(a, ram, sizeof ram, rider, 11u, true), "air samples");
            t.IsTrue(a.takeoffPending && a.takeoffTick == 11u, "takeoff edge");
            t.IsTrue(pollAirMode(a, ram, sizeof ram, rider, 12u, true), "still air");
            t.IsTrue(a.takeoffPending, "edge latches until consumed");
            std::memcpy(ram + owner + 0xde0u, &m0, 4);
            t.IsTrue(pollAirMode(a, ram, sizeof ram, rider, 20u, true), "touchdown samples");
            t.IsTrue(a.landPending && a.landTick == 20u, "landing edge");
            uint32_t m2 = 2u;
            std::memcpy(ram + owner + 0xde0u, &m2, 4);
            t.IsTrue(pollAirMode(a, ram, sizeof ram, rider, 21u, true), "wipeout samples");
            t.IsTrue(!a.landPending, "wipeout clears pending land");
            t.IsTrue(!pollAirMode(a, ram, sizeof ram, 0u, 22u, true), "null rider skips");
            t.IsTrue(!pollAirMode(a, ram, sizeof ram, rider, 23u, false), "non-racing skips"); });
        });
}
