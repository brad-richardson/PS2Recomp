#include "MiniTest.h"
#include "ps2_settings_overlay.h"
#include "ps2_virtual_pad.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

// SET2: chord hold-back, panel nav, hit test, settings file (pure parts; the
// render loop owns pause/draw/restart).
void register_ps2_settings_overlay_tests()
{
    MiniTest::Case("Set2Chord", [](TestCase &tc)
                   {
        tc.Run("lone Select held past the window forwards (stripped inside, kept after)", [](TestCase &t)
               {
            ps2x::set2::ChordState st;
            auto o0 = ps2x::set2::chordStep(st, true, false, 1000);
            t.IsTrue(!o0.openEdge && o0.stripSelect && !o0.stripStart && !o0.addSelect, "held in-window");
            auto o1 = ps2x::set2::chordStep(st, true, false, 1000 + ps2x::set2::kChordWindowMs - 1);
            t.IsTrue(o1.stripSelect && !o1.openEdge, "still held at window-1ms");
            auto o2 = ps2x::set2::chordStep(st, true, false, 1000 + ps2x::set2::kChordWindowMs);
            t.IsTrue(!o2.stripSelect && !o2.openEdge, "forwarded at window expiry");
            auto o3 = ps2x::set2::chordStep(st, true, false, 2000);
            t.IsTrue(!o3.stripSelect, "kept while held");
            auto o4 = ps2x::set2::chordStep(st, false, false, 2001);
            t.IsTrue(!o4.addSelect, "no pulse after a forwarded hold"); });

        tc.Run("Select released inside the window forwards a short pulse", [](TestCase &t)
               {
            ps2x::set2::ChordState st;
            (void)ps2x::set2::chordStep(st, true, false, 5000);
            (void)ps2x::set2::chordStep(st, true, false, 5050);
            (void)ps2x::set2::chordStep(st, false, false, 5060); // release: arms the pulse
            for (int i = 0; i < ps2x::set2::kSelectPulseFrames; ++i)
            {
                auto o = ps2x::set2::chordStep(st, false, false, 5070 + i);
                t.IsTrue(o.addSelect && !o.openEdge, "pulse frame");
            }
            auto done = ps2x::set2::chordStep(st, false, false, 5100);
            t.IsTrue(!done.addSelect, "pulse ends"); });

        tc.Run("Start edge inside the window is the chord: open + strip both until released", [](TestCase &t)
               {
            ps2x::set2::ChordState st;
            (void)ps2x::set2::chordStep(st, true, false, 1000);
            auto o = ps2x::set2::chordStep(st, true, true, 1050);
            t.IsTrue(o.openEdge && o.stripSelect && o.stripStart, "chord fires, both stripped");
            auto hold = ps2x::set2::chordStep(st, true, true, 2000);
            t.IsTrue(!hold.openEdge && hold.stripSelect && hold.stripStart, "stays stripped");
            auto half = ps2x::set2::chordStep(st, false, true, 2010);
            t.IsTrue(!half.stripSelect && half.stripStart, "Start still stripped until it releases too");
            auto free = ps2x::set2::chordStep(st, false, false, 2020);
            t.IsTrue(!free.stripSelect && !free.stripStart, "released");
            auto again = ps2x::set2::chordStep(st, true, false, 3000);
            t.IsTrue(again.stripSelect && !again.openEdge, "next Select holds again"); });

        tc.Run("Start-first-then-Select inside the window is the chord (symmetric hold)", [](TestCase &t)
               {
            ps2x::set2::ChordState st;
            auto s0 = ps2x::set2::chordStep(st, false, true, 1000);
            t.IsTrue(!s0.openEdge && s0.stripStart && !s0.stripSelect, "first Start held back, Select untouched");
            auto s1 = ps2x::set2::chordStep(st, true, true, 1050);
            t.IsTrue(s1.openEdge && s1.stripSelect && s1.stripStart, "Select completes the chord");
            auto hold = ps2x::set2::chordStep(st, true, true, 2000);
            t.IsTrue(!hold.openEdge && hold.stripSelect && hold.stripStart, "stays stripped");
            auto free = ps2x::set2::chordStep(st, false, false, 2010);
            t.IsTrue(!free.stripSelect && !free.stripStart, "released"); });

        tc.Run("lone Start tap forwards a short Start pulse, never the chord", [](TestCase &t)
               {
            ps2x::set2::ChordState st;
            auto s0 = ps2x::set2::chordStep(st, false, true, 1000);
            t.IsTrue(!s0.openEdge && s0.stripStart, "held back in-window");
            auto s1 = ps2x::set2::chordStep(st, false, false, 1060); // release: arms the pulse
            t.IsTrue(!s1.openEdge && !s1.addStart, "release frame only arms");
            for (int i = 0; i < ps2x::set2::kSelectPulseFrames; ++i)
            {
                auto o = ps2x::set2::chordStep(st, false, false, 1070 + i);
                t.IsTrue(o.addStart && !o.addSelect && !o.openEdge, "Start pulse frame");
            }
            auto done = ps2x::set2::chordStep(st, false, false, 1100);
            t.IsTrue(!done.addStart && !done.addSelect, "pulse ends"); });

        tc.Run("lone Start held past the window forwards (pause keeps working)", [](TestCase &t)
               {
            ps2x::set2::ChordState st;
            auto s0 = ps2x::set2::chordStep(st, false, true, 1000);
            t.IsTrue(s0.stripStart && !s0.openEdge, "held in-window");
            auto s1 = ps2x::set2::chordStep(st, false, true, 1000 + ps2x::set2::kChordWindowMs);
            t.IsTrue(!s1.stripStart && !s1.openEdge, "forwarded at window expiry");
            auto s2 = ps2x::set2::chordStep(st, false, false, 2000);
            t.IsTrue(!s2.addStart, "no pulse after a forwarded hold"); });

        tc.Run("slow Start after the window is a plain Start (no chord)", [](TestCase &t)
               {
            ps2x::set2::ChordState st;
            (void)ps2x::set2::chordStep(st, true, false, 1000);
            auto o = ps2x::set2::chordStep(st, true, true, 1000 + ps2x::set2::kChordWindowMs + 50);
            t.IsTrue(!o.openEdge && !o.stripSelect && !o.stripStart, "both pass through"); }); });

    // chord-sym-1: frame-quantized driver (60 Hz host polls, 17 ms steps)
    // with realistic two-thumb timings. Events are (timeMs, select, start)
    // levels held until the next event; every 17 ms frame steps chordStep.
    struct ChordDrive
    {
        int openCount = 0;
        int64_t firstOpenMs = -1;
        bool sawStripStart = false;
        bool sawAddStart = false;
        bool sawAddSelect = false;
        bool sawStripSelect = false;
    };
    auto driveChord = [](std::vector<std::pair<int64_t, uint8_t>> events, int64_t endMs)
    {
        ChordDrive d;
        ps2x::set2::ChordState st;
        size_t ei = 0;
        bool sel = false, start = false;
        for (int64_t ms = 0; ms <= endMs; ms += 17)
        {
            while (ei < events.size() && events[ei].first <= ms)
            {
                sel = (events[ei].second & 1u) != 0u;
                start = (events[ei].second & 2u) != 0u;
                ++ei;
            }
            auto o = ps2x::set2::chordStep(st, sel, start, ms);
            if (o.openEdge)
            {
                ++d.openCount;
                if (d.firstOpenMs < 0)
                    d.firstOpenMs = ms;
            }
            d.sawStripStart = d.sawStripStart || o.stripStart;
            d.sawAddStart = d.sawAddStart || o.addStart;
            d.sawAddSelect = d.sawAddSelect || o.addSelect;
            d.sawStripSelect = d.sawStripSelect || o.stripSelect;
        }
        return d;
    };
    MiniTest::Case("Set2ChordTiming", [&driveChord](TestCase &tc)
                   {
        tc.Run("two-thumb skews in either order all open the panel", [&driveChord](TestCase &t)
               {
            // (firstMs, skewMs, firstIsStart): first button down, second
            // joins skewMs later, both held 250 ms, then released.
            for (int skew : {0, 17, 34, 51, 100, 140})
            {
                for (bool startFirst : {false, true})
                {
                    const uint8_t first = startFirst ? 2u : 1u;
                    const uint8_t both = 3u;
                    auto d = driveChord({{100, first}, {100 + skew, both}, {400, 0u}}, 600);
                    char msg[128];
                    std::snprintf(msg, sizeof(msg), "skew=%dms %s-first opens exactly once",
                                  skew, startFirst ? "start" : "select");
                    t.IsTrue(d.openCount == 1, msg);
                }
            } });

        tc.Run("skew past the window never chords", [&driveChord](TestCase &t)
               {
            for (bool startFirst : {false, true})
            {
                const uint8_t first = startFirst ? 2u : 1u;
                auto d = driveChord({{100, first}, {400, 3u}, {600, 0u}}, 800);
                t.IsTrue(d.openCount == 0, startFirst ? "start-first 300ms skew stays shut"
                                                     : "select-first 300ms skew stays shut");
            } });

        tc.Run("quick simultaneous tap opens once and strips until release", [&driveChord](TestCase &t)
               {
            auto d = driveChord({{100, 3u}, {160, 0u}}, 400);
            t.IsTrue(d.openCount == 1, "one open edge");
            t.IsTrue(d.sawStripSelect && d.sawStripStart, "both stripped while down");
            t.IsTrue(!d.sawAddSelect && !d.sawAddStart, "no forward pulse after a chord"); });

        tc.Run("lone taps of either button pulse, never open", [&driveChord](TestCase &t)
               {
            auto selTap = driveChord({{100, 1u}, {160, 0u}}, 400);
            t.IsTrue(selTap.openCount == 0 && selTap.sawAddSelect && !selTap.sawAddStart,
                     "lone Select pulses Select only");
            auto startTap = driveChord({{100, 2u}, {160, 0u}}, 400);
            t.IsTrue(startTap.openCount == 0 && startTap.sawAddStart && !startTap.sawAddSelect,
                     "lone Start pulses Start only"); });

        tc.Run("second chord works after a full release", [&driveChord](TestCase &t)
               {
            auto d = driveChord({{100, 3u}, {200, 0u}, {400, 2u}, {450, 3u}, {600, 0u}}, 800);
            t.IsTrue(d.openCount == 2, "re-arms after both release"); }); });

    MiniTest::Case("Set2HoldStart", [](TestCase &tc)
                   {
        tc.Run("tap shorter than the threshold pulses Start once, never opens", [](TestCase &t)
               {
            ps2x::set2::HoldStartState st;
            // 17 ms host frames: Start down 100 ms then released.
            auto o0 = ps2x::set2::holdStartStep(st, false, true, 1000, true);
            t.IsTrue(!o0.openEdge && o0.stripStart && !o0.addStart, "held: stripped, no open");
            auto o1 = ps2x::set2::holdStartStep(st, false, true, 1100, true);
            t.IsTrue(!o1.openEdge && o1.stripStart, "still held: stripped");
            auto rel = ps2x::set2::holdStartStep(st, false, false, 1117, true);
            t.IsTrue(!rel.openEdge && !rel.addStart && !rel.stripStart, "release frame only arms");
            for (int i = 0; i < ps2x::set2::kHoldStartPulseFrames; ++i)
            {
                auto o = ps2x::set2::holdStartStep(st, false, false, 1134 + i, true);
                t.IsTrue(o.addStart && !o.openEdge, "pulse frame");
            }
            auto done = ps2x::set2::holdStartStep(st, false, false, 1200, true);
            t.IsTrue(!done.addStart && !done.openEdge, "pulse ends"); });

        tc.Run("hold past 600 ms opens and never sends Start", [](TestCase &t)
               {
            ps2x::set2::HoldStartState st;
            bool opened = false;
            for (int64_t ms = 0; ms <= 700; ms += 17)
            {
                auto o = ps2x::set2::holdStartStep(st, false, true, 1000 + ms, true);
                if (o.openEdge)
                    opened = true;
                if (!o.openEdge)
                    t.IsTrue(o.stripStart, "held Start stripped before the threshold");
                else
                    t.IsTrue(o.stripStart, "opening frame still stripped");
                t.IsTrue(!o.addStart, "never pulses on a hold");
            }
            t.IsTrue(opened, "opened at/after 600 ms");
            // Still held after open: stripped, no second open.
            auto again = ps2x::set2::holdStartStep(st, false, true, 2000, true);
            t.IsTrue(!again.openEdge && again.stripStart, "held after open: stripped, no re-open");
            // Release re-arms (the held Start is never sent, so no pulse);
            // a fresh tap pulses again.
            auto rel2 = ps2x::set2::holdStartStep(st, false, false, 2017, true);
            t.IsTrue(!rel2.openEdge && !rel2.addStart, "release after open: no pulse, re-armed");
            (void)ps2x::set2::holdStartStep(st, false, true, 2050, true);
            (void)ps2x::set2::holdStartStep(st, false, false, 2100, true); // release: arms the pulse
            auto p0 = ps2x::set2::holdStartStep(st, false, false, 2117, true);
            t.IsTrue(p0.addStart, "re-armed tap pulses"); });

        tc.Run("Select cancels the hold (the chord owns Select+Start)", [](TestCase &t)
               {
            ps2x::set2::HoldStartState st;
            (void)ps2x::set2::holdStartStep(st, false, true, 1000, true);
            auto o = ps2x::set2::holdStartStep(st, true, true, 1050, true);
            t.IsTrue(!o.openEdge && !o.stripStart && !o.addStart, "Select yields to chord");
            auto rel = ps2x::set2::holdStartStep(st, false, false, 1100, true);
            t.IsTrue(!rel.addStart, "cancelled hold never pulses"); });

        tc.Run("Off passes Start straight through (chord still opens)", [](TestCase &t)
               {
            ps2x::set2::HoldStartState st;
            auto o = ps2x::set2::holdStartStep(st, false, true, 1000, false);
            t.IsTrue(!o.openEdge && !o.stripStart && !o.addStart, "off: passthrough");
            auto o2 = ps2x::set2::holdStartStep(st, false, true, 2000, false);
            t.IsTrue(!o2.openEdge && !o2.stripStart, "off hold: still passthrough");
            // The chord path (unchanged) still opens on Select+Start.
            ps2x::set2::ChordState cst;
            (void)ps2x::set2::chordStep(cst, true, false, 3000);
            auto co = ps2x::set2::chordStep(cst, true, true, 3050);
            t.IsTrue(co.openEdge, "chord still opens when hold is off"); });

        tc.Run("settings mapping: default on, 0 is off, on clears", [](TestCase &t)
               {
            t.IsTrue(ps2x::set2::holdStartFromSetting(""), "unset -> on");
            t.IsTrue(ps2x::set2::holdStartFromSetting("1"), "1 -> on");
            t.IsTrue(ps2x::set2::holdStartFromSetting("junk"), "junk -> on");
            t.IsTrue(!ps2x::set2::holdStartFromSetting("0"), "0 -> off");
            t.IsTrue(std::string(ps2x::set2::holdStartToSetting(true)).empty(), "on clears");
            t.IsTrue(std::string(ps2x::set2::holdStartToSetting(false)) == "0", "off -> 0");
            t.IsTrue(ps2x::set2::settingsKeyAllowed("PS2X_HOLD_START"), "key allowlisted");
            t.IsTrue(std::string(ps2x::set2::holdStartName(true)) == "on", "on label");
            t.IsTrue(std::string(ps2x::set2::holdStartName(false)) == "off", "off label"); });

        tc.Run("nav X stages Hold Start on/off", [](TestCase &t)
               {
            ps2x::set2::NavState st;
            st.sel = ps2x::set2::Row::HoldStart;
            auto o1 = ps2x::set2::navStep(st, false, false, true, false, false);
            t.IsTrue(o1.action == ps2x::set2::Action::Changed && !st.holdStart, "on -> off");
            t.IsTrue(ps2x::set2::holdStartDirty(st) && st.runHoldStart, "staged, running untouched");
            auto o2 = ps2x::set2::navStep(st, false, false, true, false, false);
            t.IsTrue(o2.action == ps2x::set2::Action::Changed && st.holdStart, "off -> on"); }); });

    MiniTest::Case("Set2Nav", [](TestCase &tc)
                   {
        tc.Run("Up/Down wrap across the nine rows (Apply/Cancel at the bottom)", [](TestCase &t)
               {
            ps2x::set2::NavState st;
            auto up = ps2x::set2::navStep(st, true, false, false, false, false);
            t.IsTrue(up.action == ps2x::set2::Action::None && st.sel == ps2x::set2::Row::Cancel, "wrap to Cancel");
            auto down = ps2x::set2::navStep(st, false, true, false, false, false);
            t.IsTrue(down.action == ps2x::set2::Action::None && st.sel == ps2x::set2::Row::Mode, "wrap to Mode");
            // Down from HoldStart reaches Apply, then Cancel.
            st.sel = ps2x::set2::Row::HoldStart;
            (void)ps2x::set2::navStep(st, false, true, false, false, false);
            t.IsTrue(st.sel == ps2x::set2::Row::Apply, "HoldStart -> Apply");
            (void)ps2x::set2::navStep(st, false, true, false, false, false);
            t.IsTrue(st.sel == ps2x::set2::Row::Cancel, "Apply -> Cancel"); });

        tc.Run("X only stages (never restarts): Mode toggles, Res steps in height order", [](TestCase &t)
               {
            // settings-ui-1 (Brad 10-19): rows never apply or restart.
            // Repeated X keeps staging; only the Apply row confirms.
            ps2x::set2::NavState st;
            st.runMode = ps2x::set2::Mode::M120;
            st.mode = ps2x::set2::Mode::M120;
            st.runRes = ps2x::set2::Res::Screen;
            st.res = ps2x::set2::Res::Screen;
            st.nativeH = 1080; // Odin: 1x, 2x, Screen, 3x, 4x
            auto m = ps2x::set2::navStep(st, false, false, true, false, false);
            t.IsTrue(m.action == ps2x::set2::Action::Changed && st.mode == ps2x::set2::Mode::M60, "120 -> 60");
            t.IsTrue(ps2x::set2::modeDirty(st), "dirty Mode staged");
            auto m2 = ps2x::set2::navStep(st, false, false, true, false, false);
            t.IsTrue(m2.action == ps2x::set2::Action::Changed && st.mode == ps2x::set2::Mode::M120,
                     "second X stages back, never restarts");
            t.IsTrue(!ps2x::set2::anyStaged(st), "clean again");
            st.sel = ps2x::set2::Row::Res;
            auto r1 = ps2x::set2::navStep(st, false, false, true, false, false);
            t.IsTrue(r1.action == ps2x::set2::Action::Changed && st.res == ps2x::set2::Res::R3x, "Screen -> 3x");
            auto r2 = ps2x::set2::navStep(st, false, false, true, false, false);
            t.IsTrue(r2.action == ps2x::set2::Action::Changed && st.res == ps2x::set2::Res::R4x, "3x -> 4x");
            auto r3 = ps2x::set2::navStep(st, false, false, true, false, false);
            t.IsTrue(r3.action == ps2x::set2::Action::Changed && st.res == ps2x::set2::Res::R1x, "4x wraps to 1x");
            st.sel = ps2x::set2::Row::Perf;
            auto p = ps2x::set2::navStep(st, false, false, true, false, false);
            t.IsTrue(p.action == ps2x::set2::Action::Changed && st.perf, "perf stages on");
            t.IsTrue(!st.runPerf, "running perf untouched until Apply");
            st.sel = ps2x::set2::Row::Upscale;
            auto u = ps2x::set2::navStep(st, false, false, true, false, false);
            t.IsTrue(u.action == ps2x::set2::Action::Changed && st.upscale, "upscale stages on");
            auto u2 = ps2x::set2::navStep(st, false, false, true, false, false);
            t.IsTrue(u2.action == ps2x::set2::Action::Changed && !st.upscale, "upscale stages off"); });

        tc.Run("Left steps back with wrap; Right steps forward (d-pad + touch halves)", [](TestCase &t)
               {
            using ps2x::set2::Res;
            ps2x::set2::NavState st;
            st.nativeH = 1080; // Odin: 1x, 2x, Screen, 3x, 4x
            st.sel = ps2x::set2::Row::Res;
            st.res = Res::Screen;
            auto back = ps2x::set2::navStep(st, false, false, false, false, false, true, false);
            t.IsTrue(back.action == ps2x::set2::Action::Changed && st.res == Res::R2x, "Screen steps back to 2x");
            auto back2 = ps2x::set2::navStep(st, false, false, false, false, false, true, false);
            t.IsTrue(back2.action == ps2x::set2::Action::Changed && st.res == Res::R1x, "2x steps back to 1x");
            auto wrapBack = ps2x::set2::navStep(st, false, false, false, false, false, true, false);
            t.IsTrue(wrapBack.action == ps2x::set2::Action::Changed && st.res == Res::R4x, "1x wraps back to 4x");
            auto fwd = ps2x::set2::navStep(st, false, false, false, false, false, false, true);
            t.IsTrue(fwd.action == ps2x::set2::Action::Changed && st.res == Res::R1x, "Right wraps forward to 1x");
            t.IsTrue(ps2x::set2::resPrev(Res::R1x, 1080) == Res::R4x, "resPrev wraps");
            t.IsTrue(ps2x::set2::resPrev(Res::Screen, 1080) == Res::R2x, "resPrev mirrors resNext");
            // Touch cycles both ways too.
            st.sel = ps2x::set2::Row::Touch;
            st.touch = ps2x::set2::Touch::Auto;
            auto tback = ps2x::set2::navStep(st, false, false, false, false, false, true, false);
            t.IsTrue(tback.action == ps2x::set2::Action::Changed && st.touch == ps2x::set2::Touch::Off,
                     "Auto steps back to Off");
            auto tback2 = ps2x::set2::navStep(st, false, false, false, false, false, true, false);
            t.IsTrue(tback2.action == ps2x::set2::Action::Changed && st.touch == ps2x::set2::Touch::On,
                     "Off steps back to On");
            auto tfwd = ps2x::set2::navStep(st, false, false, false, false, false, false, true);
            t.IsTrue(tfwd.action == ps2x::set2::Action::Changed && st.touch == ps2x::set2::Touch::Off,
                     "Right steps forward to Off");
            // Left on a binary row toggles, same as Right/X.
            st.sel = ps2x::set2::Row::Mode;
            st.mode = ps2x::set2::Mode::M120;
            auto mleft = ps2x::set2::navStep(st, false, false, false, false, false, true, false);
            t.IsTrue(mleft.action == ps2x::set2::Action::Changed && st.mode == ps2x::set2::Mode::M60,
                     "Left toggles Mode");
            // Left/Right on Apply and Cancel do nothing.
            st.sel = ps2x::set2::Row::Apply;
            auto aleft = ps2x::set2::navStep(st, false, false, false, false, false, true, false);
            t.IsTrue(aleft.action == ps2x::set2::Action::None, "Left on Apply is inert");
            st.sel = ps2x::set2::Row::Cancel;
            auto cright = ps2x::set2::navStep(st, false, false, false, false, false, false, true);
            t.IsTrue(cright.action == ps2x::set2::Action::None, "Right on Cancel is inert"); });

        tc.Run("Apply confirms staged changes; Cancel closes (discards by reseed)", [](TestCase &t)
               {
            ps2x::set2::NavState st;
            st.runMode = ps2x::set2::Mode::M120;
            st.mode = ps2x::set2::Mode::M120;
            st.sel = ps2x::set2::Row::Apply;
            auto clean = ps2x::set2::navStep(st, false, false, true, false, false);
            t.IsTrue(clean.action == ps2x::set2::Action::None, "Apply with nothing staged does nothing");
            t.IsTrue(std::string(ps2x::set2::applyLabel(st)) == "Apply", "clean label is Apply");
            t.IsTrue(ps2x::set2::applyValue(st) == "no changes", "clean value");
            // Live-only stage: Apply (no restart).
            st.sel = ps2x::set2::Row::Touch;
            (void)ps2x::set2::navStep(st, false, false, true, false, false);
            t.IsTrue(ps2x::set2::stagedCount(st) == 1, "one staged change");
            t.IsTrue(std::string(ps2x::set2::applyLabel(st)) == "Apply", "live-only label stays Apply");
            t.IsTrue(ps2x::set2::applyValue(st) == "1 change", "one change");
            st.sel = ps2x::set2::Row::Apply;
            auto go = ps2x::set2::navStep(st, false, false, true, false, false);
            t.IsTrue(go.action == ps2x::set2::Action::Apply, "Apply returns Apply for live-only stages");
            // Restart-needing stage: Apply & restart.
            st.sel = ps2x::set2::Row::Mode;
            (void)ps2x::set2::navStep(st, false, false, true, false, false);
            t.IsTrue(ps2x::set2::stagedCount(st) == 2, "two staged changes");
            t.IsTrue(std::string(ps2x::set2::applyLabel(st)) == "Apply & restart", "label switches");
            t.IsTrue(ps2x::set2::applyValue(st) == "2 changes", "two changes");
            st.sel = ps2x::set2::Row::Apply;
            auto go2 = ps2x::set2::navStep(st, false, false, true, false, false);
            t.IsTrue(go2.action == ps2x::set2::Action::DoRestart, "Apply returns DoRestart when restart staged");
            t.IsTrue(st.mode == ps2x::set2::Mode::M60, "pending kept for the restart");
            // Cancel closes whether or not anything is staged.
            st.sel = ps2x::set2::Row::Cancel;
            auto cancel = ps2x::set2::navStep(st, false, false, true, false, false);
            t.IsTrue(cancel.action == ps2x::set2::Action::Close, "Cancel closes with stages");
            t.IsTrue(std::string(ps2x::set2::cancelValue(st)) == "discard changes", "cancel value when staged");
            ps2x::set2::NavState clean2;
            clean2.sel = ps2x::set2::Row::Cancel;
            t.IsTrue(std::string(ps2x::set2::cancelValue(clean2)) == "close", "cancel value when clean"); });

        tc.Run("X stages Touch auto -> on -> off -> auto (touchNext cycle)", [](TestCase &t)
               {
            ps2x::set2::NavState st;
            st.sel = ps2x::set2::Row::Touch;
            auto o1 = ps2x::set2::navStep(st, false, false, true, false, false);
            t.IsTrue(o1.action == ps2x::set2::Action::Changed && st.touch == ps2x::set2::Touch::On, "auto -> on");
            auto o2 = ps2x::set2::navStep(st, false, false, true, false, false);
            t.IsTrue(o2.action == ps2x::set2::Action::Changed && st.touch == ps2x::set2::Touch::Off, "on -> off");
            auto o3 = ps2x::set2::navStep(st, false, false, true, false, false);
            t.IsTrue(o3.action == ps2x::set2::Action::Changed && st.touch == ps2x::set2::Touch::Auto, "off -> auto");
            t.IsTrue(ps2x::set2::touchNext(ps2x::set2::Touch::Auto) == ps2x::set2::Touch::On, "next helper");
            t.IsTrue(ps2x::set2::touchPrev(ps2x::set2::Touch::Auto) == ps2x::set2::Touch::Off, "prev helper"); });

        tc.Run("X stages Menu button shown/hidden", [](TestCase &t)
               {
            ps2x::set2::NavState st;
            st.sel = ps2x::set2::Row::MenuBtn;
            auto o1 = ps2x::set2::navStep(st, false, false, true, false, false);
            t.IsTrue(o1.action == ps2x::set2::Action::Changed && st.menubtn == ps2x::set2::MenuBtn::Hidden,
                     "shown -> hidden");
            t.IsTrue(ps2x::set2::menubtnDirty(st), "staged, running untouched");
            auto o2 = ps2x::set2::navStep(st, false, false, true, false, false);
            t.IsTrue(o2.action == ps2x::set2::Action::Changed && st.menubtn == ps2x::set2::MenuBtn::Shown,
                     "hidden -> shown"); });

        tc.Run("Triangle, the chord and Cancel close with stages up (caller discards)", [](TestCase &t)
               {
            // settings-ui-1: closing discards staged changes — the runtime
            // drops NavState and reseeds at the next open, so Close carries
            // no disarm bookkeeping.
            ps2x::set2::NavState st;
            st.mode = ps2x::set2::Mode::M60; // staged (runMode M120)
            auto tri = ps2x::set2::navStep(st, false, false, false, true, false);
            t.IsTrue(tri.action == ps2x::set2::Action::Close, "triangle closes with stages");
            t.IsTrue(ps2x::set2::anyStaged(st), "stages stay in NavState for the caller to drop");
            auto chord = ps2x::set2::navStep(st, false, false, false, false, true);
            t.IsTrue(chord.action == ps2x::set2::Action::Close, "chord closes");
            // ui-pad-only-1 (Brad 10-20): Circle (O) is the same back edge
            // as Triangle on the controller-only panel.
            auto circle = ps2x::set2::navStep(st, false, false, false, false, false, false, false, true);
            t.IsTrue(circle.action == ps2x::set2::Action::Close, "circle closes with stages"); });

        tc.Run("close-press overhang strips until each button releases", [](TestCase &t)
               {
            ps2x::set2::SwallowState st;
            st.mask = 0x1009u; // Triangle + Select + Start down at close
            t.Equals(ps2x::set2::swallowStep(st, 0x1009u), static_cast<uint16_t>(0x1009u), "all held: all stripped");
            t.Equals(ps2x::set2::swallowStep(st, 0x1008u), static_cast<uint16_t>(0x1008u), "Select released: drops out");
            t.Equals(ps2x::set2::swallowStep(st, 0x5008u), static_cast<uint16_t>(0x1008u), "new X passes (never in mask)");
            t.Equals(ps2x::set2::swallowStep(st, 0x4000u), static_cast<uint16_t>(0u), "all released: empty"); }); });

    MiniTest::Case("Set2Panel", [](TestCase &tc)
                   {
        tc.Run("panel geometry lays every row out for draw (no hit test: controller-only)", [](TestCase &t)
               {
            // ui-pad-only-1 (Brad 10-20): panelHit is removed — the rows
            // are pad-navigated (navStep), never touch hit-tested. The
            // layout still drives the draw code, so it stays covered here.
            const auto g = ps2x::set2::panelLayout(1280.0f, 720.0f);
            t.IsTrue(g.rowH > 0.0f, "rows have height");
            for (int i = 0; i < ps2x::set2::kRowCount; ++i)
            {
                t.IsTrue(g.rowY[i] >= g.py, "row inside panel top");
                t.IsTrue(g.rowY[i] + g.rowH <= g.py + g.ph, "row inside panel bottom");
                if (i > 0)
                    t.IsTrue(g.rowY[i] > g.rowY[i - 1], "rows run top to bottom");
            } });

        tc.Run("row labels are short and complete (panel-polish-1, no truncation)", [](TestCase &t)
               {
            using ps2x::set2::Row;
            t.Equals(ps2x::set2::kRowCount, 9, "nine rows");
            t.IsTrue(std::string(ps2x::set2::rowLabel(Row::Mode)) == "Mode", "mode");
            t.IsTrue(std::string(ps2x::set2::rowLabel(Row::Res)) == "Resolution", "resolution");
            t.IsTrue(std::string(ps2x::set2::rowLabel(Row::Perf)) == "Perf", "perf");
            t.IsTrue(std::string(ps2x::set2::rowLabel(Row::Upscale)) == "Upscale", "upscale");
            t.IsTrue(std::string(ps2x::set2::rowLabel(Row::Touch)) == "On-screen controls", "touch renamed");
            t.IsTrue(std::string(ps2x::set2::rowLabel(Row::MenuBtn)) == "Menu button", "menu button");
            t.IsTrue(std::string(ps2x::set2::rowLabel(Row::HoldStart)) == "Hold Start", "hold start");
            t.IsTrue(std::string(ps2x::set2::rowLabel(Row::Apply)) == "Apply", "apply");
            t.IsTrue(std::string(ps2x::set2::rowLabel(Row::Cancel)) == "Cancel", "cancel");
            for (int i = 0; i < ps2x::set2::kRowCount; ++i)
            {
                const std::string label(
                    ps2x::set2::rowLabel(static_cast<Row>(i)));
                // Labels draw left-aligned with the value right-aligned, so
                // nothing truncates; 18 covers "On-screen controls" (Brad
                // 10-19 rename; the old 12-cap covered "Input marker").
                t.IsTrue(!label.empty() && label.size() <= 18, "short non-empty label");
            } });

        tc.Run("every row has a one-line ASCII blurb (settings-ui-1)", [](TestCase &t)
               {
            using ps2x::set2::Row;
            for (int i = 0; i < ps2x::set2::kRowCount; ++i)
            {
                const std::string blurb(
                    ps2x::set2::rowBlurb(static_cast<Row>(i)));
                t.IsTrue(!blurb.empty(), "non-empty blurb");
                bool ascii = true;
                for (unsigned char c : blurb)
                    ascii = ascii && c < 0x80;
                t.IsTrue(ascii, "blurb is ASCII (no font substitution)");
            }
            t.IsTrue(std::string(ps2x::set2::rowBlurb(Row::Touch)).find("controller") != std::string::npos,
                     "controls blurb names the controller rule");
            t.IsTrue(std::string(ps2x::set2::rowBlurb(Row::Apply)).find("discard") != std::string::npos,
                     "apply blurb says closing discards"); });

        tc.Run("menu button clears the cutout (the panel's only touch target)", [](TestCase &t)
               {
            // ui-pad-only-1 (Brad 10-20): rowTapBack is removed with the
            // row hit test — Left/Right come from the pad. The "..."
            // launcher stays a touch target, so its geometry stays covered.
            const auto plain = ps2x::set2::menuButtonLayout(1920.0f, 1080.0f);
            const auto inset = ps2x::set2::menuButtonLayout(1920.0f, 1080.0f, 59.0f);
            t.IsTrue(inset.y == plain.y + 59.0f, "safeTop shifts the button below the cutout");
            t.IsTrue(inset.x == plain.x && inset.w == plain.w, "horizontal geometry unchanged"); });

        tc.Run("render heights round to the nearest common label", [](TestCase &t)
               {
            using ps2x::set2::resHeightLabel;
            t.IsTrue(resHeightLabel(0).empty(), "0 -> empty (caller falls back)");
            t.IsTrue(resHeightLabel(448) == "480p", "native 448 -> 480p");
            t.IsTrue(resHeightLabel(480) == "480p", "480 -> 480p");
            t.IsTrue(resHeightLabel(640) == "720p", "640 -> 720p");
            t.IsTrue(resHeightLabel(896) == "960p", "2x 896 -> 960p");
            t.IsTrue(resHeightLabel(960) == "960p", "960 -> 960p");
            t.IsTrue(resHeightLabel(1080) == "1080p", "1080 -> 1080p");
            t.IsTrue(resHeightLabel(1328) == "1440p", "iPad game rect -> 1440p");
            t.IsTrue(resHeightLabel(1440) == "1440p", "1440 -> 1440p");
            t.IsTrue(resHeightLabel(2160) == "2160p", "2160 -> 2160p");
            t.IsTrue(resHeightLabel(3840) == "2160p", "clamps at 2160p"); });

        tc.Run("choices order by resulting height; Screen sits at the device height", [](TestCase &t)
               {
            using ps2x::set2::Res;
            using ps2x::set2::resOrder;
            const auto names = [](const std::array<Res, 5> &o) {
                std::string s;
                for (auto r : o)
                    s += (r == Res::R1x ? "1" : r == Res::R2x ? "2" : r == Res::Screen ? "S" : r == Res::R3x ? "3" : "4");
                return s;
            };
            t.IsTrue(names(resOrder(1080)) == "12S34", "Odin 1080p: 1x, 2x, Screen, 3x, 4x");
            t.IsTrue(names(resOrder(2160)) == "1234S", "2160p display: Screen last");
            t.IsTrue(names(resOrder(448)) == "1S234", "448p display: 1x ties, Screen after it");
            t.IsTrue(names(resOrder(1328)) == "123S4", "iPad ~1328: within 5% of 3x, Screen after it");
            t.IsTrue(names(resOrder(1500)) == "123S4", "1500p: Screen between 3x and 4x");
            t.IsTrue(names(resOrder(500)) == "1S234", "500p: Screen between 1x and 2x");
            t.IsTrue(ps2x::set2::resNext(Res::Screen, 1080) == Res::R3x, "Odin next after Screen is 3x");
            t.IsTrue(ps2x::set2::resNext(Res::R4x, 1080) == Res::R1x, "Odin 4x wraps to 1x");
            t.IsTrue(ps2x::set2::resNext(Res::Screen, 2160) == Res::R1x, "tall display wraps Screen to 1x"); });

        tc.Run("1290p-tall iPhone order (correction 2: tie rule applies at 4.0%)", [](TestCase &t)
               {
            // |1290-1344|/1344 = 4.0% <= 5%: both show, Screen after 3x.
            // The correction's example lists 2x, Native, 3x, 4x; the tie
            // rule as written gives 2x, 3x, Native, 4x. Locked here so
            // Brad's call either way is a one-line change.
            using ps2x::set2::Res;
            const auto o = ps2x::set2::resOrder(1290);
            t.IsTrue(o[0] == Res::R1x && o[1] == Res::R2x && o[2] == Res::R3x &&
                     o[3] == Res::Screen && o[4] == Res::R4x, "1x, 2x, 3x, Screen, 4x");
            t.IsTrue(ps2x::set2::resNearHeight(1290, 1344), "4.0% counts as near"); });

        tc.Run("option labels show multiplier then height; default is marked", [](TestCase &t)
               {
            using ps2x::set2::Res;
            using ps2x::set2::resOptionLabel;
            // \xc3\x97 is x (U+00D7), \xc2\xb7 is the middle dot (U+00B7):
            // proper two-byte UTF-8, both in the panel font. settings-ui-1
            // (Brad 10-19 "?"): the dot was a lone \xb7 byte, which raylib
            // decoded as '?'.
            t.IsTrue(resOptionLabel(Res::R1x, 1080, Res::Screen) == "1\xc3\x97 \xc2\xb7 448p", "1x");
            t.IsTrue(resOptionLabel(Res::R2x, 1080, Res::Screen) == "2\xc3\x97 \xc2\xb7 896p", "2x");
            t.IsTrue(resOptionLabel(Res::R3x, 1080, Res::Screen) == "3\xc3\x97 \xc2\xb7 1344p", "3x");
            t.IsTrue(resOptionLabel(Res::R4x, 1080, Res::Screen) == "4\xc3\x97 \xc2\xb7 1792p", "4x");
            t.IsTrue(resOptionLabel(Res::Screen, 1080, Res::Screen) == "Screen \xc2\xb7 1080p (default)",
                     "Odin Screen default");
            t.IsTrue(resOptionLabel(Res::R2x, 1080, Res::R2x) == "2\xc3\x97 \xc2\xb7 896p (default)",
                     "iOS 2x default");
            t.IsTrue(resOptionLabel(Res::Screen, 1328, Res::R2x) == "Screen \xc2\xb7 1440p",
                     "iPad Screen rounds like resHeightLabel");
            t.IsTrue(resOptionLabel(Res::Screen, 0, Res::Screen) == "Screen (default)",
                     "no frame yet falls back to plain Screen"); });

        tc.Run("every drawn label is valid UTF-8 inside the panel font (settings-ui-1 ?)", [](TestCase &t)
               {
            // The panel font is raylib's default (U+0000..U+00FF). Any byte
            // sequence that is not valid UTF-8, or decodes above U+00FF,
            // draws as '?'. Audit every string the panel and resume prompt
            // can draw: row labels, blurbs, resolution options, value names,
            // hints, Apply/Cancel values, "..." and the resume strings.
            using ps2x::set2::Res;
            using ps2x::set2::Row;
            std::vector<std::string> drawn;
            for (int i = 0; i < ps2x::set2::kRowCount; ++i)
            {
                drawn.emplace_back(ps2x::set2::rowLabel(static_cast<Row>(i)));
                drawn.emplace_back(ps2x::set2::rowBlurb(static_cast<Row>(i)));
            }
            const Res allRes[] = {Res::R1x, Res::R2x, Res::Screen, Res::R3x, Res::R4x};
            for (Res r : allRes)
                for (unsigned h : {0u, 1080u, 1290u})
                    for (Res d : {Res::Screen, Res::R2x})
                        drawn.push_back(ps2x::set2::resOptionLabel(r, h, d));
            for (auto touch : {ps2x::set2::Touch::Auto, ps2x::set2::Touch::On, ps2x::set2::Touch::Off})
                drawn.emplace_back(ps2x::set2::touchName(touch));
            for (auto mb : {ps2x::set2::MenuBtn::Shown, ps2x::set2::MenuBtn::Hidden})
                drawn.emplace_back(ps2x::set2::menuBtnName(mb));
            for (bool b : {false, true})
            {
                drawn.emplace_back(ps2x::set2::holdStartName(b));
                drawn.emplace_back(ps2x::set2::markerName(b));
            }
            drawn.emplace_back("Settings");
            drawn.emplace_back("...");
            drawn.emplace_back("120");
            drawn.emplace_back("60");
            drawn.emplace_back("on");
            drawn.emplace_back("off");
            drawn.emplace_back("auto");
            drawn.emplace_back("no changes");
            drawn.emplace_back("Apply & restart");
            drawn.emplace_back("Resume where you left off?");
            drawn.emplace_back("Resume");
            drawn.emplace_back("Start fresh");
            for (const std::string &s : drawn)
            {
                bool ok = true;
                for (size_t i = 0; i < s.size() && ok;)
                {
                    const unsigned char c = static_cast<unsigned char>(s[i]);
                    uint32_t cp = 0;
                    size_t len = 0;
                    if (c < 0x80)
                    {
                        cp = c;
                        len = 1;
                    }
                    else if ((c & 0xE0) == 0xC0)
                    {
                        cp = c & 0x1F;
                        len = 2;
                    }
                    else if ((c & 0xF0) == 0xE0)
                    {
                        cp = c & 0x0F;
                        len = 3;
                    }
                    else if ((c & 0xF8) == 0xF0)
                    {
                        cp = c & 0x07;
                        len = 4;
                    }
                    else
                        ok = false;
                    for (size_t k = 1; k < len && ok; ++k)
                    {
                        if (i + k >= s.size())
                            ok = false;
                        else
                        {
                            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
                            if ((cc & 0xC0) != 0x80)
                                ok = false;
                            else
                                cp = (cp << 6) | (cc & 0x3F);
                        }
                    }
                    // Overlongs and out-of-font-range sort as failures: the
                    // panel font only carries U+0000..U+00FF.
                    if (ok && ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) ||
                               (len == 4 && cp < 0x10000) || cp > 0xFF))
                        ok = false;
                    i += (len > 0) ? len : 1;
                }
                t.IsTrue(ok, ("draw-safe: " + s).c_str());
            } });

        tc.Run("menu button geometry: top middle, thumb-sized hit area", [](TestCase &t)
               {
            const auto g = ps2x::set2::menuButtonLayout(1920.0f, 1080.0f);
            t.IsTrue(g.x > 0 && g.x + g.w < 1920.0f, "centred horizontally");
            t.IsTrue(g.y >= 0 && g.y < 1080.0f * 0.1f, "near the top");
            t.IsTrue(g.w >= 44.0f && g.h >= 44.0f, "thumb-sized hit area");
            t.IsTrue(ps2x::set2::menuButtonHit(g, 960.0f, g.y + g.h * 0.5f), "centre taps");
            t.IsTrue(!ps2x::set2::menuButtonHit(g, 100.0f, 100.0f), "corner misses");
            t.IsTrue(!ps2x::set2::menuButtonHit(g, 960.0f, 900.0f), "low tap misses");
            // Shown by default on every device: visibility never consults Touch.
            t.IsTrue(ps2x::set2::menuButtonShown(ps2x::set2::MenuBtn::Shown), "shown");
            t.IsTrue(!ps2x::set2::menuButtonShown(ps2x::set2::MenuBtn::Hidden), "hidden"); }); });

    MiniTest::Case("Set2Settings", [](TestCase &tc)
                   {
        tc.Run("parses owned keys only, trims, skips comments and junk", [](TestCase &t)
               {
            const auto e = ps2x::set2::parseSettingsEnv(
                "# panel file\n  GE1_UPSCALE = 2 \nPS2X_SETTINGS_PERF=1\nGE1_ONDEVICE_UPSCALE=1\nPS2X_VIRTUAL_PAD=0\nPS2X_MENU_BUTTON=0\nPS2X_INPUT_MARKER=1\nPS2X_SOUND=1\nnoequals\n=novalue\n");
            t.Equals(e.size(), static_cast<size_t>(6), "allowlisted keys only");
            t.IsTrue(e[0] == std::make_pair(std::string("GE1_UPSCALE"), std::string("2")), "trimmed UPSCALE");
            t.IsTrue(e[1] == std::make_pair(std::string("PS2X_SETTINGS_PERF"), std::string("1")), "PERF");
            t.IsTrue(e[2] == std::make_pair(std::string("GE1_ONDEVICE_UPSCALE"), std::string("1")), "ODU");
            t.IsTrue(e[3] == std::make_pair(std::string("PS2X_VIRTUAL_PAD"), std::string("0")), "VPAD");
            t.IsTrue(e[4] == std::make_pair(std::string("PS2X_MENU_BUTTON"), std::string("0")), "MENUBTN");
            t.IsTrue(e[5] == std::make_pair(std::string("PS2X_INPUT_MARKER"), std::string("1")), "MARKER"); });

        tc.Run("no row persists PS2X_NET (online stays on; just don't pick Online)", [](TestCase &t)
               {
            // online-default-1's panel row is gone (Brad 10-16): the settings
            // layer must not own PS2X_NET, so a stale line cannot keep it off.
            const auto e = ps2x::set2::parseSettingsEnv("PS2X_NET=0\nGE1_UPSCALE=2\n");
            t.Equals(e.size(), static_cast<size_t>(1), "NET line dropped");
            t.IsTrue(e[0].first == "GE1_UPSCALE", "only the owned key survives");
            t.IsFalse(ps2x::set2::settingsKeyAllowed("PS2X_NET"), "PS2X_NET not panel-owned"); });

        tc.Run("upsert replaces, appends and removes; format round-trips", [](TestCase &t)
               {
            ps2x::set2::SettingsPairs cur{{"GE1_UPSCALE", "native"}};
            auto u1 = ps2x::set2::upsertSettings(cur, "PS2X_SETTINGS_PERF", "1");
            t.Equals(u1.size(), static_cast<size_t>(2), "append");
            auto u2 = ps2x::set2::upsertSettings(u1, "GE1_UPSCALE", "2");
            t.IsTrue(u2[0].second == "2" && u2.size() == 2, "replace keeps order");
            auto u3 = ps2x::set2::upsertSettings(u2, "GE1_UPSCALE", "");
            t.Equals(u3.size(), static_cast<size_t>(1), "empty removes");
            const auto back = ps2x::set2::parseSettingsEnv(ps2x::set2::formatSettingsEnv(u2));
            t.IsTrue(back == u2, "format/parse round-trip"); });

        tc.Run("res maps to the UPSCALE line; legacy values load onto the new rows", [](TestCase &t)
               {
            using ps2x::set2::Res;
            t.IsTrue(std::string(ps2x::set2::resToUpscale(Res::Screen)) == "native", "screen");
            t.IsTrue(std::string(ps2x::set2::resToUpscale(Res::R1x)) == "1", "1x");
            t.IsTrue(std::string(ps2x::set2::resToUpscale(Res::R2x)) == "2", "2x");
            t.IsTrue(std::string(ps2x::set2::resToUpscale(Res::R3x)) == "3", "3x");
            t.IsTrue(std::string(ps2x::set2::resToUpscale(Res::R4x)) == "4", "4x");
            // Legacy loads: stored values land on the matching row.
            t.IsTrue(ps2x::set2::resFromSetting("native", Res::R2x) == Res::Screen, "legacy native");
            t.IsTrue(ps2x::set2::resFromSetting("2", Res::Screen) == Res::R2x, "legacy 2");
            t.IsTrue(ps2x::set2::resFromSetting("1", Res::Screen) == Res::R1x, "1");
            t.IsTrue(ps2x::set2::resFromSetting("3", Res::Screen) == Res::R3x, "3");
            t.IsTrue(ps2x::set2::resFromSetting("4", Res::Screen) == Res::R4x, "4");
            // Unset goes to the platform default (Odin Screen, iOS 2x).
            t.IsTrue(ps2x::set2::resFromSetting("", Res::Screen) == Res::Screen, "unset -> Screen");
            t.IsTrue(ps2x::set2::resFromSetting("", Res::R2x) == Res::R2x, "unset -> 2x");
            t.IsTrue(ps2x::set2::resFromSetting("junk", Res::Screen) == Res::Screen, "junk -> default"); });

        tc.Run("default marker follows the device rules (correction 4)", [](TestCase &t)
               {
            using ps2x::set2::Res;
            using ps2x::set2::resDefaultRow;
            constexpr std::uint64_t kGiB = 1024ull * 1024ull * 1024ull;
            // Play builds keep their env rules.
            t.IsTrue(resDefaultRow(false, false, 0, 1920, 1080) == Res::Screen, "Odin play: Screen");
            t.IsTrue(resDefaultRow(false, true, 0, 2048, 2732) == Res::R2x, "iOS play: 2x");
            t.IsTrue(resDefaultRow(false, false, 0, 1280, 720) == Res::Screen, "Mac/dev: Screen");
            // Public builds follow the tier rule (capable = iOS or >= 5 GiB).
            t.IsTrue(resDefaultRow(true, false, 8 * kGiB, 2400, 1080) == Res::Screen,
                     "public capable 1080p: Screen");
            t.IsTrue(resDefaultRow(true, false, 8 * kGiB, 2560, 1440) == Res::R2x,
                     "public capable taller: 2x");
            t.IsTrue(resDefaultRow(true, true, 0, 1206, 2622) == Res::R2x,
                     "public iOS tall panel: 2x");
            t.IsTrue(resDefaultRow(true, false, 4 * kGiB, 1080, 2400) == Res::R1x,
                     "public low tier: 1x");
            t.IsTrue(resDefaultRow(true, false, 4 * kGiB, 2400, 1080) == Res::R1x,
                     "public low tier 1080p panel is still 1x");
            // The 1080p boundary is exact (the tier rule compares == 1080).
            t.IsTrue(resDefaultRow(true, false, 8 * kGiB, 2400, 1081) == Res::R2x,
                     "1081p is not a 1080p panel"); });

        tc.Run("Apply persists every staged row, never the effective env", [](TestCase &t)
               {
            using ps2x::set2::NavState;
            using ps2x::set2::Res;
            using ps2x::set2::SettingsPairs;
            const auto get = [](const SettingsPairs &pairs, const char *key) {
                for (const auto &kv : pairs)
                    if (kv.first == key)
                        return kv.second;
                return std::string();
            };
            // Brad 10-17 iPhone bug: stale file {native} + staged 3x must
            // write 3, keeping the other rows and their order.
            NavState st;
            st.res = Res::R3x;
            st.perf = true;
            st.marker = true; // no row, still round-trips the file
            const SettingsPairs stale{{"GE1_UPSCALE", "native"},
                                      {"PS2X_SETTINGS_PERF", "0"},
                                      {"PS2X_VIRTUAL_PAD", "1"}};
            const auto w1 = ps2x::set2::settingsForApply(stale, st);
            t.IsTrue(get(w1, "GE1_UPSCALE") == "3", "staged 3x wins over stale native");
            t.IsTrue(get(w1, "PS2X_SETTINGS_PERF") == "1", "staged perf");
            t.IsTrue(get(w1, "PS2X_INPUT_MARKER") == "1", "marker round-trips on");
            t.IsTrue(get(w1, "PS2X_VIRTUAL_PAD").empty(), "staged Auto clears the touch line");
            t.IsTrue(get(w1, "GE1_ONDEVICE_UPSCALE") == "0", "staged upscale off persists explicit");
            t.IsTrue(get(w1, "PS2X_MENU_BUTTON") == "1", "staged Shown persists explicit");
            t.IsTrue(get(w1, "PS2X_HOLD_START").empty(), "staged Hold on clears the line");
            t.IsTrue(w1[0] == std::make_pair(std::string("GE1_UPSCALE"), std::string("3")),
                     "file order kept for overwritten keys");
            // Screen writes native explicitly (no default-clearing choice);
            // marker off writes an explicit 0 (config-fix-1 item 10: a
            // base-layer 1 must not reappear through a removed line).
            NavState clean;
            clean.res = Res::Screen;
            const auto w2 = ps2x::set2::settingsForApply({}, clean);
            t.IsTrue(w2.size() == 5 && get(w2, "GE1_UPSCALE") == "native" &&
                         get(w2, "PS2X_SETTINGS_PERF") == "0" && get(w2, "PS2X_INPUT_MARKER") == "0" &&
                         get(w2, "GE1_ONDEVICE_UPSCALE") == "0" && get(w2, "PS2X_MENU_BUTTON") == "1",
                     "empty file gains the explicit staged lines");
            // 1x writes 1 (it is never a default, but it is writable).
            NavState one;
            one.res = Res::R1x;
            t.IsTrue(get(ps2x::set2::settingsForApply({}, one), "GE1_UPSCALE") == "1", "1x persists");
            // A staged touch On keeps its line; staged Hold off writes 0.
            NavState live;
            live.touch = ps2x::set2::Touch::On;
            live.holdStart = false;
            const auto w3 = ps2x::set2::settingsForApply({}, live);
            t.IsTrue(get(w3, "PS2X_VIRTUAL_PAD") == "1", "staged touch On persists");
            t.IsTrue(get(w3, "PS2X_HOLD_START") == "0", "staged Hold off persists explicit");
            // A stale marker 1 becomes an explicit staged-off 0.
            const SettingsPairs staleOn{{"PS2X_INPUT_MARKER", "1"}};
            NavState off;
            off.res = Res::Screen;
            const auto w4 = ps2x::set2::settingsForApply(staleOn, off);
            t.IsTrue(get(w4, "PS2X_INPUT_MARKER") == "0",
                     "stale marker 1 overwritten by explicit staged off"); });

        tc.Run("menu button maps to its line (both persist explicit)", [](TestCase &t)
               {
            using ps2x::set2::MenuBtn;
            t.IsTrue(ps2x::set2::menuBtnFromSetting("0") == MenuBtn::Hidden, "0 -> hidden");
            t.IsTrue(ps2x::set2::menuBtnFromSetting("") == MenuBtn::Shown, "unset -> shown");
            t.IsTrue(ps2x::set2::menuBtnFromSetting("1") == MenuBtn::Shown, "1 -> shown");
            t.IsTrue(ps2x::set2::menuBtnFromSetting("junk") == MenuBtn::Shown, "junk -> shown");
            t.IsTrue(std::string(ps2x::set2::menuBtnToSetting(MenuBtn::Hidden)) == "0", "hidden -> 0");
            t.IsTrue(std::string(ps2x::set2::menuBtnToSetting(MenuBtn::Shown)) == "1", "shown -> 1");
            t.IsTrue(ps2x::set2::settingsKeyAllowed("PS2X_MENU_BUTTON"), "key allowlisted"); });

        tc.Run("input marker maps to its line (both persist explicit); default off", [](TestCase &t)
               {
            // shortcuts-1 (Brad 10-18): dev option, off unless the env or
            // the setting turns it on. Only the literal "1" arms, matching
            // lim1Apply's strict check.
            t.IsTrue(ps2x::set2::markerFromSetting("1"), "1 -> on");
            t.IsTrue(!ps2x::set2::markerFromSetting(""), "unset -> off");
            t.IsTrue(!ps2x::set2::markerFromSetting("0"), "0 -> off");
            t.IsTrue(!ps2x::set2::markerFromSetting("junk"), "junk -> off");
            t.IsTrue(std::string(ps2x::set2::markerToSetting(true)) == "1", "on -> 1");
            t.IsTrue(std::string(ps2x::set2::markerToSetting(false)) == "0", "off -> 0");
            t.IsTrue(std::string(ps2x::set2::markerName(true)) == "on", "on label");
            t.IsTrue(std::string(ps2x::set2::markerName(false)) == "off", "off label");
            t.IsTrue(ps2x::set2::settingsKeyAllowed("PS2X_INPUT_MARKER"), "key allowlisted");
            const ps2x::set2::NavState st;
            t.IsTrue(!st.marker && !st.runMarker, "pending + running default off"); });

        tc.Run("on-device upscale shares the provider default-on (unset/0/1)", [](TestCase &t)
               {
            // config-fix-1 item 13: unset, empty and anything but "0"
            // are ON; only "0" is OFF. Adapter, panel seed and font
            // flag all use this.
            t.IsTrue(ps2x::set2::ondeviceUpscaleOn(nullptr), "unset -> on");
            t.IsTrue(ps2x::set2::ondeviceUpscaleOn(""), "empty -> on");
            t.IsTrue(ps2x::set2::ondeviceUpscaleOn("1"), "1 -> on");
            t.IsTrue(!ps2x::set2::ondeviceUpscaleOn("0"), "0 -> off"); });

        tc.Run("touch maps to the VIRTUAL_PAD line (auto clears it)", [](TestCase &t)
               {
            t.IsTrue(ps2x::set2::touchFromSetting("1") == ps2x::set2::Touch::On, "1 -> on");
            t.IsTrue(ps2x::set2::touchFromSetting("0") == ps2x::set2::Touch::Off, "0 -> off");
            t.IsTrue(ps2x::set2::touchFromSetting("") == ps2x::set2::Touch::Auto, "unset -> auto");
            t.IsTrue(ps2x::set2::touchFromSetting("yes") == ps2x::set2::Touch::Auto, "junk -> auto");
            t.IsTrue(std::string(ps2x::set2::touchToSetting(ps2x::set2::Touch::On)) == "1", "on -> 1");
            t.IsTrue(std::string(ps2x::set2::touchToSetting(ps2x::set2::Touch::Off)) == "0", "off -> 0");
            t.IsTrue(std::string(ps2x::set2::touchToSetting(ps2x::set2::Touch::Auto)).empty(), "auto clears"); }); });

    // settings-ui-1 part 2 (SSX font): game-font byte substitution. The
    // baked FEFONT lacks U+00D7 (C3 97) and U+00B7 (C2 B7); every other
    // panel/resume byte is ASCII, so the mapped strings stay single-byte
    // for the byte-wise layout (a future non-ASCII addition fails the
    // all-ASCII test below and forces a map entry here).
    MiniTest::Case("Set2GameFont", [](TestCase &tc)
                   {
        tc.Run("separator bytes map, ASCII passes through", [](TestCase &t)
               {
            t.IsTrue(ps2x::set2::gameFontByte(0xC2) == 0, "C2 skipped");
            t.IsTrue(ps2x::set2::gameFontByte(0xC3) == 0, "C3 skipped");
            t.IsTrue(ps2x::set2::gameFontByte(0x97) == static_cast<unsigned char>('x'), "97 -> x");
            t.IsTrue(ps2x::set2::gameFontByte(0xB7) == static_cast<unsigned char>('-'), "B7 -> -");
            for (int c = 0x20; c < 0x7F; ++c)
                t.IsTrue(ps2x::set2::gameFontByte(static_cast<unsigned char>(c)) ==
                             static_cast<unsigned char>(c),
                         "ASCII identity"); });

        tc.Run("res labels map to single-byte strings", [](TestCase &t)
               {
            t.IsTrue(ps2x::set2::gameFontMapped("2\xc3\x97 \xc2\xb7 896p") == "2x - 896p",
                     "multiply + dot");
            t.IsTrue(ps2x::set2::gameFontMapped("Screen \xc2\xb7 1080p") == "Screen - 1080p",
                     "screen dot");
            t.IsTrue(ps2x::set2::gameFontMapped("...") == "...", "dots kept");
            t.IsTrue(ps2x::set2::gameFontMapped("") == "", "empty");
            t.IsTrue(ps2x::set2::gameFontMapped(nullptr) == "", "null"); });

        tc.Run("every drawn panel/resume string maps to ASCII", [](TestCase &t)
               {
            std::vector<std::string> corpus;
            corpus.emplace_back("Settings");
            corpus.emplace_back("...");
            corpus.emplace_back("Resume where you left off?");
            corpus.emplace_back("Resume");
            corpus.emplace_back("Start fresh");
            for (int i = 0; i < ps2x::set2::kRowCount; ++i)
            {
                const ps2x::set2::Row row = static_cast<ps2x::set2::Row>(i);
                corpus.emplace_back(ps2x::set2::rowLabel(row));
                corpus.emplace_back(ps2x::set2::rowBlurb(row));
            }
            const ps2x::set2::Res reses[] = {ps2x::set2::Res::R1x, ps2x::set2::Res::R2x,
                                             ps2x::set2::Res::Screen, ps2x::set2::Res::R3x,
                                             ps2x::set2::Res::R4x};
            for (const ps2x::set2::Res r : reses)
                for (const ps2x::set2::Res d :
                     {ps2x::set2::Res::R1x, ps2x::set2::Res::Screen})
                    corpus.push_back(ps2x::set2::resOptionLabel(r, 1080u, d));
            for (const unsigned h : {0u, 448u, 896u, 1080u, 2160u})
                corpus.push_back(ps2x::set2::resHeightLabel(h));
            for (const ps2x::set2::Touch tc2 :
                 {ps2x::set2::Touch::Auto, ps2x::set2::Touch::On, ps2x::set2::Touch::Off})
                corpus.emplace_back(ps2x::set2::touchName(tc2));
            for (const ps2x::set2::MenuBtn mb :
                 {ps2x::set2::MenuBtn::Shown, ps2x::set2::MenuBtn::Hidden})
                corpus.emplace_back(ps2x::set2::menuBtnName(mb));
            corpus.emplace_back(ps2x::set2::holdStartName(false));
            corpus.emplace_back(ps2x::set2::holdStartName(true));
            for (const char *s : {"60", "120", "on", "off", "  (restart)", "< 120 >", "Apply & restart",
                                  "Apply", "Cancel"})
                corpus.emplace_back(s);
            for (const std::string &s : corpus)
            {
                const std::string m = ps2x::set2::gameFontMapped(s);
                bool ascii = true;
                for (const unsigned char c : m) ascii = ascii && c < 0x80;
                t.IsTrue(ascii, ("mapped ASCII: " + s).c_str());
            } }); });

    // ui-pad-only-1 (Brad 10-20): controller-only host UIs. Sticks join the
    // d-pad on the panel; the resume prompt steps from the same held
    // directions on any pad (d-pad, stick or the virtual pad).
    MiniTest::Case("Set2StickNav", [](TestCase &tc)
                   {
        tc.Run("stick directions threshold at half deflection", [](TestCase &t)
               {
            using ps2x::set2::stickDirBits;
            t.Equals(stickDirBits(0.0f, 0.0f), static_cast<uint8_t>(0), "centred: none");
            t.Equals(stickDirBits(0.0f, -0.6f), ps2x::set2::kStickUp, "up");
            t.Equals(stickDirBits(0.0f, 0.6f), ps2x::set2::kStickDown, "down");
            t.Equals(stickDirBits(-0.6f, 0.0f), ps2x::set2::kStickLeft, "left");
            t.Equals(stickDirBits(0.6f, 0.0f), ps2x::set2::kStickRight, "right");
            t.Equals(stickDirBits(0.49f, -0.49f), static_cast<uint8_t>(0), "inside dead zone");
            t.Equals(stickDirBits(-0.5f, -0.5f),
                     static_cast<uint8_t>(ps2x::set2::kStickUp | ps2x::set2::kStickLeft), "diagonal");
            t.Equals(stickDirBits(-1.0f, 1.0f),
                     static_cast<uint8_t>(ps2x::set2::kStickDown | ps2x::set2::kStickLeft), "full diagonal"); });

        tc.Run("d-pad buttons map to the same direction bits", [](TestCase &t)
               {
            using ps2x::set2::dirsFromDpad;
            t.Equals(dirsFromDpad(false, false, false, false), static_cast<uint8_t>(0), "none");
            t.Equals(dirsFromDpad(true, false, false, true),
                     static_cast<uint8_t>(ps2x::set2::kStickUp | ps2x::set2::kStickRight), "up+right"); });

        tc.Run("edges fire once per press, release re-arms", [](TestCase &t)
               {
            ps2x::set2::StickNav st;
            t.Equals(ps2x::set2::stickNavEdges(st, ps2x::set2::kStickDown), ps2x::set2::kStickDown,
                     "press edges");
            t.Equals(ps2x::set2::stickNavEdges(st, ps2x::set2::kStickDown), static_cast<uint8_t>(0),
                     "held: no repeat");
            t.Equals(ps2x::set2::stickNavEdges(st,
                                              static_cast<uint8_t>(ps2x::set2::kStickDown | ps2x::set2::kStickLeft)),
                     ps2x::set2::kStickLeft, "second direction edges while first held");
            t.Equals(ps2x::set2::stickNavEdges(st, static_cast<uint8_t>(0)), static_cast<uint8_t>(0),
                     "release: no edge");
            t.Equals(ps2x::set2::stickNavEdges(st, ps2x::set2::kStickDown), ps2x::set2::kStickDown,
                     "re-press edges again"); }); });

    MiniTest::Case("Set2ResumeNav", [](TestCase &tc)
                   {
        tc.Run("fresh slot defaults to Resume; up/left select it, down/right select Start fresh", [](TestCase &t)
               {
            ps2x::set2::ResumeNav st;
            t.IsTrue(st.resumeSelected, "fresh default is Resume");
            t.IsTrue(!ps2x::set2::resumeStep(st, 0u), "no direction: no step");
            t.IsTrue(st.resumeSelected, "still Resume");
            t.IsTrue(ps2x::set2::resumeStep(st, ps2x::set2::kStickRight), "right edges");
            t.IsTrue(!st.resumeSelected, "Start fresh selected");
            t.IsTrue(!ps2x::set2::resumeStep(st, ps2x::set2::kStickRight), "held right: no repeat");
            t.IsTrue(ps2x::set2::resumeStep(st, ps2x::set2::kStickUp), "up edges");
            t.IsTrue(st.resumeSelected, "back to Resume"); });

        tc.Run("stale slot defaults to Start fresh; both choices reachable by pad", [](TestCase &t)
               {
            // resume-ios-1: a disclosed-stale slot defaults to Start fresh.
            ps2x::set2::ResumeNav st;
            st.resumeSelected = false;
            // Controller switch sequence (Brad 10-19): left, right, left,
            // down — every switch lands, held dirs never stick.
            t.IsTrue(ps2x::set2::resumeStep(st, ps2x::set2::kStickLeft), "left edges");
            t.IsTrue(st.resumeSelected, "Resume chosen");
            t.IsTrue(ps2x::set2::resumeStep(st, ps2x::set2::kStickDown), "down edges");
            t.IsTrue(!st.resumeSelected, "Start fresh chosen");
            t.IsTrue(ps2x::set2::resumeStep(st, ps2x::set2::kStickLeft), "left edges again");
            t.IsTrue(st.resumeSelected, "Resume chosen again");
            // A diagonal re-affirms the Resume side, then release clears.
            t.IsTrue(ps2x::set2::resumeStep(
                         st, static_cast<uint8_t>(ps2x::set2::kStickUp | ps2x::set2::kStickRight)),
                     "diagonal edges");
            t.IsTrue(!ps2x::set2::resumeStep(st, 0u), "release: no step"); });

        tc.Run("scripted prompt run, fresh slot: d-pad right + X confirms Start fresh", [](TestCase &t)
               {
            // ui-pad-only-1: the device loop's exact frame function,
            // scripted as a pad run. Fresh slot defaults to Resume; Right
            // moves to Start fresh; X confirms it.
            ps2x::set2::ResumeNav nav; // fresh default: Resume
            bool prevConfirm = false;
            auto frame = [&](uint8_t dirs, bool x, bool enter) {
                return ps2x::set2::resumePromptStep(
                    nav, prevConfirm, ps2x::set2::ResumeFrameIn{dirs, x}, enter);
            };
            auto idle = frame(0u, false, false);
            t.IsTrue(!idle.confirmed && idle.resumeSelected, "idle: Resume, unconfirmed");
            auto move = frame(ps2x::set2::kStickRight, false, false);
            t.IsTrue(!move.confirmed && !move.resumeSelected, "right: Start fresh, unconfirmed");
            auto held = frame(ps2x::set2::kStickRight, true, false);
            t.IsTrue(held.confirmed && !held.resumeSelected, "X confirms Start fresh");
            t.IsTrue(!held.resumeSelected, "choice returned is Start fresh"); });

        tc.Run("scripted prompt run, stale slot: stick left + X confirms Resume, Enter works", [](TestCase &t)
               {
            // Disclosed-stale slot defaults to Start fresh; the left stick
            // moves to Resume; X confirms. A second run shows Enter
            // confirms without any pad, and held X never double-confirms.
            ps2x::set2::ResumeNav nav;
            nav.resumeSelected = false; // stale default: Start fresh
            bool prevConfirm = false;
            auto frame = [&](uint8_t dirs, bool x, bool enter) {
                return ps2x::set2::resumePromptStep(
                    nav, prevConfirm, ps2x::set2::ResumeFrameIn{dirs, x}, enter);
            };
            t.IsTrue(!frame(0u, false, false).resumeSelected, "stale default holds");
            auto move = frame(ps2x::set2::kStickLeft, false, false);
            t.IsTrue(move.resumeSelected && !move.confirmed, "stick left: Resume, unconfirmed");
            auto go = frame(0u, true, false);
            t.IsTrue(go.confirmed && go.resumeSelected, "X confirms Resume");
            auto again = frame(0u, true, false);
            t.IsTrue(!again.confirmed, "held X: no second confirm");
            ps2x::set2::ResumeNav nav2;
            bool pc2 = false;
            auto enterOnly = ps2x::set2::resumePromptStep(
                nav2, pc2, ps2x::set2::ResumeFrameIn{0u, false}, true);
            t.IsTrue(enterOnly.confirmed && enterOnly.resumeSelected, "Enter confirms default Resume"); }); });

    MiniTest::Case("Set2VpadNav", [](TestCase &tc)
                   {
        tc.Run("scripted virtual-pad run: move, stick-move, stage, close", [](TestCase &t)
               {
            // ui-pad-only-1 (Brad 10-20): synthetic touches through the
            // real layout + hit test drive the same open-panel input the
            // render loop runs (union edges + stick edges into navStep).
            // Window 1280x720 (u = 720): the D-pad disc, face buttons and
            // stick zone below come from makeLayout, never hand-placed.
            const float w = 1280.0f, h = 720.0f;
            const auto layout = ps2x::vpad::makeLayout(w, h);
            ps2x::vpad::PadState pad;
            ps2x::set2::NavState st; // sel starts at Mode, like the open seed
            uint16_t prevUnion = 0u;
            ps2x::set2::StickNav sn;
            ps2x::set2::Action lastAction = ps2x::set2::Action::None;
            auto frame = [&](std::initializer_list<ps2x::vpad::TouchPoint> list) {
                std::vector<ps2x::vpad::TouchPoint> ts(list);
                const ps2x::vpad::PadFrame f =
                    ps2x::vpad::updatePad(pad, layout, ts.data(), static_cast<int>(ts.size()), true);
                const uint8_t held = ps2x::set2::stickDirBits(f.stick.active ? f.stick.x : 0.0f,
                                                              f.stick.active ? f.stick.y : 0.0f);
                const uint8_t se = ps2x::set2::stickNavEdges(sn, held);
                const uint16_t edge =
                    static_cast<uint16_t>(f.pressed & static_cast<uint16_t>(~prevUnion));
                prevUnion = f.pressed;
                const auto out = ps2x::set2::navStep(
                    st, ((edge & ps2x::vpad::kUp) != 0u) || ((se & ps2x::set2::kStickUp) != 0u),
                    ((edge & ps2x::vpad::kDown) != 0u) || ((se & ps2x::set2::kStickDown) != 0u),
                    (edge & ps2x::vpad::kCross) != 0u, (edge & ps2x::vpad::kTriangle) != 0u, false,
                    ((edge & ps2x::vpad::kLeft) != 0u) || ((se & ps2x::set2::kStickLeft) != 0u),
                    ((edge & ps2x::vpad::kRight) != 0u) || ((se & ps2x::set2::kStickRight) != 0u),
                    (edge & ps2x::vpad::kCircle) != 0u);
                lastAction = out.action;
                return f.pressed;
            };
            const float dcx = w - 0.532f * h, dcy = 0.78f * h; // D-pad disc centre
            const float dOff = 0.16f * h * (11.0f / 19.0f);    // arrow offset
            const float rx = w - 0.205f * h, cy = 0.60f * h;   // face cluster centre
            const float off = 0.13f * h;                       // face offset
            t.Equals(frame({}), static_cast<uint16_t>(0), "idle: nothing pressed");
            // Down-tap on the D-pad disc moves Mode -> Resolution.
            t.Equals(frame({{1, dcx, dcy + dOff}}), ps2x::vpad::kDown, "down arrow presses");
            t.IsTrue(st.sel == ps2x::set2::Row::Res, "down moves to Resolution");
            t.Equals(frame({{1, dcx, dcy + dOff}}), ps2x::vpad::kDown, "held: still down");
            t.IsTrue(st.sel == ps2x::set2::Row::Res, "held: no repeat");
            t.Equals(frame({}), static_cast<uint16_t>(0), "lift releases");
            // Stick flick up (anchor, then drag past half deflection).
            t.Equals(frame({{2, 0.28f * h, cy}}), static_cast<uint16_t>(0), "stick anchors, centred");
            t.Equals(frame({{2, 0.28f * h, cy - 0.15f * h}}), static_cast<uint16_t>(0),
                     "stick drag presses no button");
            t.IsTrue(st.sel == ps2x::set2::Row::Mode, "stick up moves back to Mode");
            t.Equals(frame({}), static_cast<uint16_t>(0), "lift releases the stick");
            // X-tap on Cross stages the Mode row (change, then Cancel).
            t.Equals(frame({{3, rx, cy + off}}), ps2x::vpad::kCross, "cross presses");
            t.IsTrue(lastAction == ps2x::set2::Action::Changed, "cross stages");
            t.IsTrue(ps2x::set2::modeDirty(st), "mode staged M120 -> M60");
            t.Equals(frame({}), static_cast<uint16_t>(0), "lift releases");
            // Triangle-tap closes (Cancel path: staged changes discarded).
            t.Equals(frame({{4, rx, cy - off}}), ps2x::vpad::kTriangle, "triangle presses");
            t.IsTrue(lastAction == ps2x::set2::Action::Close, "triangle closes");
            t.IsTrue(ps2x::set2::modeDirty(st), "stages stay for the caller to drop"); }); });

}
