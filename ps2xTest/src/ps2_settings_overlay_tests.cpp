#include "MiniTest.h"
#include "ps2_settings_overlay.h"

#include <string>
#include <utility>

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

        tc.Run("Start-first-then-Select is two presses, never the chord", [](TestCase &t)
               {
            ps2x::set2::ChordState st;
            auto s0 = ps2x::set2::chordStep(st, false, true, 1000);
            t.IsTrue(!s0.openEdge && !s0.stripStart, "lone Start passes through");
            auto s1 = ps2x::set2::chordStep(st, true, true, 1050);
            t.IsTrue(!s1.openEdge && s1.stripSelect && !s1.stripStart, "Select holds, Start keeps passing");
            auto s2 = ps2x::set2::chordStep(st, true, true, 1050 + ps2x::set2::kChordWindowMs);
            t.IsTrue(!s2.openEdge && !s2.stripSelect, "Select forwards after the window"); });

        tc.Run("slow Start after the window is a plain Start (no chord)", [](TestCase &t)
               {
            ps2x::set2::ChordState st;
            (void)ps2x::set2::chordStep(st, true, false, 1000);
            auto o = ps2x::set2::chordStep(st, true, true, 1000 + ps2x::set2::kChordWindowMs + 50);
            t.IsTrue(!o.openEdge && !o.stripSelect && !o.stripStart, "both pass through"); }); });

    MiniTest::Case("Set2Nav", [](TestCase &tc)
                   {
        tc.Run("Up/Down wrap across the five rows", [](TestCase &t)
               {
            ps2x::set2::NavState st;
            auto up = ps2x::set2::navStep(st, true, false, false, false, false);
            t.IsTrue(up.action == ps2x::set2::Action::None && st.sel == ps2x::set2::Row::Restart, "wrap to Restart");
            auto down = ps2x::set2::navStep(st, false, true, false, false, false);
            t.IsTrue(down.action == ps2x::set2::Action::None && st.sel == ps2x::set2::Row::Mode, "wrap to Mode"); });

        tc.Run("X cycles Mode/Res and toggles Perf live", [](TestCase &t)
               {
            ps2x::set2::NavState st;
            auto m = ps2x::set2::navStep(st, false, false, true, false, false);
            t.IsTrue(m.action == ps2x::set2::Action::Changed && st.mode == ps2x::set2::Mode::M60, "120 -> 60");
            st.sel = ps2x::set2::Row::Res;
            st.res = ps2x::set2::Res::RDefault;
            (void)ps2x::set2::navStep(st, false, false, true, false, false);
            t.IsTrue(st.res == ps2x::set2::Res::Native, "default -> native");
            (void)ps2x::set2::navStep(st, false, false, true, false, false);
            t.IsTrue(st.res == ps2x::set2::Res::R2x, "native -> 2x");
            auto r = ps2x::set2::navStep(st, false, false, true, false, false);
            t.IsTrue(r.action == ps2x::set2::Action::Changed && st.res == ps2x::set2::Res::RDefault, "2x -> default");
            st.sel = ps2x::set2::Row::Perf;
            auto p = ps2x::set2::navStep(st, false, false, true, false, false);
            t.IsTrue(p.action == ps2x::set2::Action::Changed && st.perf, "perf on");
            st.sel = ps2x::set2::Row::Upscale;
            auto u = ps2x::set2::navStep(st, false, false, true, false, false);
            t.IsTrue(u.action == ps2x::set2::Action::Changed && st.upscale, "upscale on");
            auto u2 = ps2x::set2::navStep(st, false, false, true, false, false);
            t.IsTrue(u2.action == ps2x::set2::Action::Changed && !st.upscale, "upscale off"); });

        tc.Run("Restart needs the one-line confirm; nav away disarms", [](TestCase &t)
               {
            ps2x::set2::NavState st;
            st.sel = ps2x::set2::Row::Restart;
            auto arm = ps2x::set2::navStep(st, false, false, true, false, false);
            t.IsTrue(arm.action == ps2x::set2::Action::ArmRestart && st.restartArmed, "first X arms");
            auto go = ps2x::set2::navStep(st, false, false, true, false, false);
            t.IsTrue(go.action == ps2x::set2::Action::DoRestart, "second X restarts");
            st.restartArmed = true;
            (void)ps2x::set2::navStep(st, false, true, false, false, false);
            t.IsTrue(!st.restartArmed, "Down disarms"); });

        tc.Run("Triangle or the chord closes", [](TestCase &t)
               {
            ps2x::set2::NavState st;
            st.restartArmed = true;
            auto tri = ps2x::set2::navStep(st, false, false, false, true, false);
            t.IsTrue(tri.action == ps2x::set2::Action::Close && !st.restartArmed, "triangle closes + disarms");
            auto chord = ps2x::set2::navStep(st, false, false, false, false, true);
            t.IsTrue(chord.action == ps2x::set2::Action::Close, "chord closes"); });

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
        tc.Run("row taps hit 0..4, dot taps hit 5, misses read -1", [](TestCase &t)
               {
            const auto g = ps2x::set2::panelLayout(1280.0f, 720.0f);
            for (int i = 0; i < 5; ++i)
            {
                const int hit = ps2x::set2::panelHit(g, true, g.rowX + g.rowW * 0.5f, g.rowY[i] + g.rowH * 0.5f);
                t.IsTrue(hit == i, "row centre hits");
            }
            t.IsTrue(ps2x::set2::panelHit(g, true, g.dotX, g.dotY) == 5, "dot hits while open");
            t.IsTrue(ps2x::set2::panelHit(g, false, g.dotX, g.dotY) == 5, "dot hits while closed");
            t.IsTrue(ps2x::set2::panelHit(g, true, g.px * 0.25f, g.py * 0.25f) == -1, "outside misses");
            // The dot clears the panel rect (taps can't be ambiguous).
            const bool dotInPanel =
                g.dotX >= g.px && g.dotX <= g.px + g.pw && g.dotY >= g.py && g.dotY <= g.py + g.ph;
            t.IsTrue(!dotInPanel, "dot outside the panel"); }); });

    MiniTest::Case("Set2Settings", [](TestCase &tc)
                   {
        tc.Run("parses owned keys only, trims, skips comments and junk", [](TestCase &t)
               {
            const auto e = ps2x::set2::parseSettingsEnv(
                "# panel file\n  GE1_UPSCALE = 2 \nPS2X_SETTINGS_PERF=1\nGE1_ONDEVICE_UPSCALE=1\nPS2X_SOUND=1\nnoequals\n=novalue\n");
            t.Equals(e.size(), static_cast<size_t>(3), "allowlisted keys only");
            t.IsTrue(e[0] == std::make_pair(std::string("GE1_UPSCALE"), std::string("2")), "trimmed UPSCALE");
            t.IsTrue(e[1] == std::make_pair(std::string("PS2X_SETTINGS_PERF"), std::string("1")), "PERF");
            t.IsTrue(e[2] == std::make_pair(std::string("GE1_ONDEVICE_UPSCALE"), std::string("1")), "ODU"); });

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

        tc.Run("res maps to the UPSCALE line (default clears it)", [](TestCase &t)
               {
            t.IsTrue(std::string(ps2x::set2::resToUpscale(ps2x::set2::Res::Native)) == "native", "native");
            t.IsTrue(std::string(ps2x::set2::resToUpscale(ps2x::set2::Res::R2x)) == "2", "2x");
            t.IsTrue(std::string(ps2x::set2::resToUpscale(ps2x::set2::Res::RDefault)).empty(), "default clears"); }); });
}
