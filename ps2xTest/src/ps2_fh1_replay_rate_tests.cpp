#include "MiniTest.h"
#include "ps2_fh1_full120.h"
#include "runtime/ps2_savestate.h"

#include <cstdint>
#include <vector>

void ps2_fh1_linkSavestateSection();

namespace
{
void put(std::vector<uint8_t> &ram, uint32_t a, uint32_t v) { ps2_fh1::wr32(ram.data(), a, v); }

// Input object at 0x1000, records at 0x2000: runs {3 stock-ish, 5}.
void craftInput(std::vector<uint8_t> &ram, uint32_t idx, uint32_t rem)
{
    put(ram, 0x1000u, 2u);
    put(ram, 0x1004u, idx);
    put(ram, 0x1008u, rem);
    put(ram, 0x100cu, 0x2000u);
    put(ram, 0x2000u, 0xabc000u | 3u);
    put(ram, 0x2008u, 0xdef000u | 5u);
}

// Replay mode chain [[[gp-0x848]+0x84]+0x28] = mode, gp = 0x10000.
void craftMode(std::vector<uint8_t> &ram, uint32_t mode)
{
    put(ram, 0x10000u - 0x848u, 0x3000u);
    put(ram, 0x3084u, 0x3100u);
    put(ram, 0x3128u, 0x3200u);
    put(ram, 0x3200u, mode);
}
} // namespace

void register_ps2_fh1_replay_rate_tests()
{
    using namespace ps2_fh1_rpl;
    MiniTest::Case("Ps2Fh1ReplayRate", [](TestCase &tc) {
        tc.Run("knob: unset/1 on, 0 off", [](TestCase &t) {
            t.IsTrue(parseEnabled(nullptr), "unset");
            t.IsTrue(parseEnabled("1"), "1");
            t.IsFalse(parseEnabled("0"), "0");
        });
        tc.Run("track: race start stock, 120, pause stretch, re-entry", [](TestCase &t) {
            Track k;
            k.input = 0x1000u;
            note(k, 0u, false); // race update 1 runs stock (start-edge request mid-update)
            for (uint32_t i = 1u; i < 100u; ++i)
                note(k, i, true);
            for (uint32_t i = 100u; i < 103u; ++i) // resumed, waiting for every rider grounded
                note(k, i, false);
            for (uint32_t i = 103u; i < 200u; ++i)
                note(k, i, true);
            t.Equals(k.edges.size(), size_t{4}, "four runs");
            t.Equals(rateAt(k, 0u), 0, "first sample stock");
            t.Equals(rateAt(k, 1u), 1, "second 120");
            t.Equals(rateAt(k, 99u), 1, "before pause");
            t.Equals(rateAt(k, 100u), 0, "after resume stock");
            t.Equals(rateAt(k, 102u), 0, "still stock");
            t.Equals(rateAt(k, 103u), 1, "re-entry");
            t.Equals(rateAt(k, 199u), 1, "last");
            t.Equals(rateAt(k, 200u), -1, "past the end: no rate");
        });
        tc.Run("track: a gap invalidates, a new recording restarts", [](TestCase &t) {
            Track k;
            note(k, 0u, false);
            note(k, 1u, true);
            note(k, 1u, true); // capped record array: count did not advance
            t.IsFalse(k.valid, "gap invalidates");
            t.Equals(rateAt(k, 0u), -1, "no rate after a gap");
            note(k, 2u, true);
            t.IsFalse(k.valid, "stays invalid");
            note(k, 0u, true);
            t.IsTrue(k.valid, "new recording");
            t.Equals(rateAt(k, 0u), 1, "fresh");
            Track fresh;
            note(fresh, 5u, true);
            t.IsFalse(fresh.valid, "a track never starts mid-recording");
        });
        tc.Run("cursor: consumed samples across run boundaries", [](TestCase &t) {
            std::vector<uint8_t> ram(PS2_RAM_SIZE);
            uint32_t n = 0u;
            craftInput(ram, 0u, 0u);
            t.IsTrue(recorded(ram.data(), 0x1000u, n), "recorded");
            t.Equals(n, 8u, "3 + 5");
            const struct { uint32_t idx, rem, want; } rows[] = {{0u, 0u, 0u}, {0u, 3u, 0u}, {0u, 2u, 1u}, {0u, 1u, 2u},
                                                              {1u, 0u, 3u}, {1u, 4u, 4u}, {1u, 1u, 7u}, {2u, 0u, 8u}};
            for (const auto &r : rows)
            {
                craftInput(ram, r.idx, r.rem);
                t.IsTrue(consumed(ram.data(), 0x1000u, n), "readable");
                t.Equals(n, r.want, "consumed");
            }
            craftInput(ram, 3u, 0u);
            t.IsFalse(consumed(ram.data(), 0x1000u, n), "cursor past the records");
            craftInput(ram, 0u, 9u);
            t.IsFalse(consumed(ram.data(), 0x1000u, n), "remaining above the run");
        });
        tc.Run("forced rate only in playback modes, from the cursor", [](TestCase &t) {
            std::vector<uint8_t> ram(PS2_RAM_SIZE);
            State s;
            Track *k = track(s, 0x1000u, true);
            for (uint32_t i = 0u; i < 8u; ++i)
                note(*k, i, i >= 1u && i < 4u);
            craftInput(ram, 0u, 0u);
            craftMode(ram, 1u);
            t.Equals(forcedRate(s, ram.data(), 0x10000u), -1, "no play call seen yet");
            s.playInput = 0x1000u;
            t.Equals(forcedRate(s, ram.data(), 0x10000u), 0, "sample 0 stock");
            craftInput(ram, 0u, 2u);
            t.Equals(forcedRate(s, ram.data(), 0x10000u), 1, "sample 1 at 120 (mid-burst)");
            craftInput(ram, 1u, 4u);
            t.Equals(forcedRate(s, ram.data(), 0x10000u), 0, "sample 4 stock");
            for (uint32_t m : {2u, 4u})
            {
                craftMode(ram, m);
                t.Equals(forcedRate(s, ram.data(), 0x10000u), 0, "modes 2 and 4 play");
            }
            craftInput(ram, 2u, 0u);
            t.Equals(forcedRate(s, ram.data(), 0x10000u), -1, "end of the clip");
            craftMode(ram, 0u);
            t.Equals(forcedRate(s, ram.data(), 0x10000u), -1, "recording mode");
            t.Equals(s.playInput, 0u, "leaving playback forgets the input");
            s.playInput = 0x1000u;
            craftMode(ram, 3u);
            t.Equals(forcedRate(s, ram.data(), 0x10000u), -1, "mode 3 does not play");
            s.playInput = 0x1000u;
            craftMode(ram, 1u);
            craftInput(ram, 1u, 0u);
            t.Equals(forcedRate(s, ram.data(), 0x10000u), 1, "sample 3 at 120");
            put(ram, 0x2008u, 0xdef000u | 6u);
            t.Equals(forcedRate(s, ram.data(), 0x10000u), -1, "buffer no longer the noted recording");
            forget(s, 0x1000u);
            t.IsTrue(track(s, 0x1000u, false) == nullptr, "reset forgets the track");
        });
        tc.Run("hook interest: replay sites only with the knob, in both phases", [](TestCase &t) {
            using namespace ps2_fh1;
            HookConfig c;
            c.mode = Mode::Events;
            for (bool active : {false, true})
            {
                c.guestActive = active;
                c.replay = true;
                auto h = buildHookInterest(c);
                t.IsTrue(hookTableHit(h, kRecordSite, 1u), "record site");
                t.IsTrue(hookTableHit(h, kPlaySite, 1u), "play site");
                t.IsTrue(hookTableHit(h, 1u, kInputReset), "reset");
                t.IsTrue(hookTableHit(h, 1u, kInputFree), "destroy");
                c.replay = false;
                h = buildHookInterest(c);
                t.IsFalse(hookTableHit(h, kRecordSite, 1u), "off: no record site");
                t.IsFalse(hookTableHit(h, kPlaySite, 1u), "off: no play site");
            }
        });
        tc.Run("query holds clear at an entry flip", [](TestCase &t) {
            using namespace ps2_fh1;
            g_queryHold[0].p = 0x5409b0u;
            g_queryHold[0].v0 = 1u;
            g_queryHold[0].w[3] = 0x3f800000u;
            g_queryHold[5].p = 0x540a60u;
            queryHoldReset();
            bool empty = true;
            for (const QueryHold &h : g_queryHold)
                empty = empty && h.p == 0u && h.v0 == 0u && h.w[3] == 0u;
            t.IsTrue(empty, "no owner keeps a hold across an entry");
        });
        tc.Run("save-state section round trip", [](TestCase &t) {
            ps2_fh1_linkSavestateSection();
            auto &g = ps2_fh1::g_rpl;
            g = State{};
            Track *k = track(g, 0x1000u, true);
            note(*k, 0u, false);
            note(*k, 1u, true);
            note(*k, 2u, true);
            g.playInput = 0x1000u;
            g.forced = true;
            auto &sec = ps2_savestate::registeredSections().at("rpl1");
            ps2_savestate::Writer w;
            sec.save(w);
            g = State{};
            ps2_savestate::Reader r(w.buf.data(), w.buf.size());
            t.IsTrue(sec.load(r) && r.ok() && r.atEnd(), "loads");
            const Track *l = track(g, 0x1000u, false);
            t.IsTrue(l && l->valid && l->total == 3u && l->edges.size() == 2u, "track restored");
            t.Equals(l ? rateAt(*l, 2u) : -2, 1, "rate restored");
            t.Equals(g.playInput, 0x1000u, "play input");
            t.IsTrue(g.forced, "forced");
            g = State{};
        });
    });
}
