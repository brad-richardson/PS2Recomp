#include "MiniTest.h"
#include "ps2_fh1_full120.h"

#include <cstdint>
#include <cstdlib>
#include <vector>

static ps2_fh1::HookConfig hk1AllActive()
{
    ps2_fh1::HookConfig c;
    c.mode = ps2_fh1::Mode::Events;
    c.main = ps2_fh1::kFixAll;
    c.fix12 = ps2_fh1::kFix12All;
    c.guestActive = true;
    c.draw = true;
    c.tapOn = true;
    c.tapCountPcs.push_back(0x445566u);
    c.histRanges = false;
    c.srcTgts.push_back(0xdead00u);
    c.fh26s = 0x1234u;
    c.fh9r = 0u;
    c.labPairs.emplace_back(0xaaa000u, 0xbbb000u);
    return c;
}

void register_ps2_fh1_hooktable_tests()
{
    using namespace ps2_fh1;
    MiniTest::Case("Ps2Fh1HookTable", [](TestCase &tc)
    {
        tc.Run("census sources hit (FIX=all, events, active)", [](TestCase &t)
        {
            const HookInterest hi = buildHookInterest(hk1AllActive());
            t.IsFalse(hi.always, "no always-run without fh9/hist");
            const uint32_t dead = 0x000001u; // not a real PC: row must hit via src only
            const uint32_t srcs[] = {
                0x12912cu, 0x113dccu, 0x3171b4u, // events facts + parity
                0x230bb8u,                       // sessionSkip
                0x2f3be0u, 0x390c98u,            // rng Bernoulli rolls
                0x11a3f0u,                       // trick combo
                0x10bdccu, 0x10da6cu, 0x10dc30u, 0x10b8acu, // aiGate gates
                0x128f20u, 0x11198cu,            // particle pass callers
                0x1197acu,                       // bonus rate store
                0x15f0ecu,                       // lift probe
                0x317208u, 0x31723cu,            // draw gates
                0xaaa000u,                       // lab HALF/SKIP source
            };
            for (uint32_t s : srcs)
                t.IsTrue(hookTableHit(hi, s, dead), "src hits");
        });

        tc.Run("census targets hit (FIX=all, events, active)", [](TestCase &t)
        {
            const HookInterest hi = buildHookInterest(hk1AllActive());
            const uint32_t dead = 0x000001u;
            const uint32_t tgts[] = {
                0x111408u, 0x1013a8u, 0x12a250u, 0x26f4a8u, // events facts
                0x317348u, 0x1e1458u,                        // clock domains
                0x114298u,                                   // launch wall response
                0x3710d0u, 0x115d48u, 0x3177f0u, 0x390c60u,  // rng cadence
                0x117638u,                                   // trick accrual
                0x1298c8u,                                   // aiGate race-tick read
                0x2dd0b8u,                                   // particle pass
                0x1176f8u,                                   // bonus rate halve
                0x32e100u,                                   // lift probe
                0x34b818u,                                   // flag update
                0x395510u,                                   // rclock getter
                0x346258u, 0x35f7d0u, 0x35f410u, 0x341e48u, 0x341ec0u, 0x341f38u,
                0x2e2260u, // fh12 spin/texanim/loops/fxtimer
                0xdead00u,  // PS2X_FH1_SRC watch
                0x133308u,  // FH26 stick tap
                0xbbb000u,  // lab pair target
                0x111728u,  // IN4 rider dispatcher
                0x22b008u,  // draw render slot
                0x2306b8u,  // tap update target
                0x445566u,  // tap count pc
            };
            for (uint32_t g : tgts)
                t.IsTrue(hookTableHit(hi, dead, g), "tgt hits");
        });

        tc.Run("random other PCs miss", [](TestCase &t)
        {
            const HookInterest hi = buildHookInterest(hk1AllActive());
            uint32_t s = 0x12345678u;
            auto next = [&] {
                s = s * 1664525u + 1013904223u;
                return (s >> 8) & 0xffffffu;
            };
            int checked = 0;
            while (checked < 512)
            {
                const uint32_t src = next(), tgt = next();
                if (hookTableHit(hi, src, tgt))
                    continue; // a census PC by chance: not a miss sample
                ++checked;
            }
            t.Equals(checked, 512, "512 miss samples");
            // Spot checks far from any game PC miss.
            t.IsFalse(hookTableHit(hi, 0x000002u, 0x000003u), "tiny pcs miss");
            t.IsFalse(hookTableHit(hi, 0xbcbcbcu, 0xcdcdcdu), "mid pcs miss");
        });

        tc.Run("always-run diagnostics", [](TestCase &t)
        {
            HookConfig fh9 = hk1AllActive();
            fh9.fh9r = 0x77u;
            t.IsTrue(buildHookInterest(fh9).always, "fh9 tap forces always");
            HookConfig hist = hk1AllActive();
            hist.histRanges = true;
            t.IsTrue(buildHookInterest(hist).always, "hist ranges force always");
        });

        tc.Run("inactive events drop on-gated PCs, keep entry/exit state", [](TestCase &t)
        {
            HookConfig c = hk1AllActive();
            c.guestActive = false;
            const HookInterest hi = buildHookInterest(c);
            t.IsFalse(hi.always, "no always");
            // On-gated hooks are out while the guest flip is inactive.
            t.IsFalse(hookTableHit(hi, 0x000001u, 0x317348u), "clock tgt out");
            t.IsFalse(hookTableHit(hi, 0x230bb8u, 0x000001u), "session src out");
            t.IsFalse(hookTableHit(hi, 0x000001u, 0x22b008u), "draw tgt out");
            t.IsFalse(hookTableHit(hi, 0xaaa000u, 0x000001u), "lab src out");
            // Entry/exit detection, rclock continuity and mode-agnostic taps stay.
            t.IsTrue(hookTableHit(hi, 0x3171b4u, 0x000001u), "app-update src stays");
            t.IsTrue(hookTableHit(hi, 0x000001u, 0x26f4a8u), "session tgt stays (finish fact)");
            t.IsTrue(hookTableHit(hi, 0x000001u, 0x395510u), "rclock stays");
            t.IsTrue(hookTableHit(hi, 0x000001u, 0x111728u), "in4 stays");
        });

        tc.Run("always mode lists the manager-init site", [](TestCase &t)
        {
            HookConfig c = hk1AllActive();
            c.mode = Mode::Always;
            const HookInterest hi = buildHookInterest(c);
            t.IsTrue(hookTableHit(hi, 0x316da8u, 0x000001u), "kHookSite src hits");
        });

        tc.Run("knob defaults to table when unset", [](TestCase &t)
        {
            if (std::getenv("PS2X_SSX3_FULL120_HOOKS") == nullptr)
                t.IsTrue(hookPreclassify(), "default is table");
        });
    });
}
