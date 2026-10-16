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
        tc.Run("Up/Down wrap across the six rows", [](TestCase &t)
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

        tc.Run("X cycles Touch auto -> on -> off -> auto", [](TestCase &t)
               {
            ps2x::set2::NavState st;
            st.sel = ps2x::set2::Row::Touch;
            auto o1 = ps2x::set2::navStep(st, false, false, true, false, false);
            t.IsTrue(o1.action == ps2x::set2::Action::Changed && st.touch == ps2x::set2::Touch::On, "auto -> on");
            auto o2 = ps2x::set2::navStep(st, false, false, true, false, false);
            t.IsTrue(o2.action == ps2x::set2::Action::Changed && st.touch == ps2x::set2::Touch::Off, "on -> off");
            auto o3 = ps2x::set2::navStep(st, false, false, true, false, false);
            t.IsTrue(o3.action == ps2x::set2::Action::Changed && st.touch == ps2x::set2::Touch::Auto, "off -> auto"); });

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
        tc.Run("row taps hit 0..Count-1, dot taps hit Count, misses read -1", [](TestCase &t)
               {
            const auto g = ps2x::set2::panelLayout(1280.0f, 720.0f);
            for (int i = 0; i < ps2x::set2::kRowCount; ++i)
            {
                const int hit = ps2x::set2::panelHit(g, true, g.rowX + g.rowW * 0.5f, g.rowY[i] + g.rowH * 0.5f);
                t.IsTrue(hit == i, "row centre hits");
            }
            t.IsTrue(ps2x::set2::panelHit(g, true, g.dotX, g.dotY) == ps2x::set2::kRowCount,
                     "dot hits while open");
            t.IsTrue(ps2x::set2::panelHit(g, false, g.dotX, g.dotY) == ps2x::set2::kRowCount,
                     "dot hits while closed");
            t.IsTrue(ps2x::set2::panelHit(g, true, g.px * 0.25f, g.py * 0.25f) == -1, "outside misses");
            // The dot clears the panel rect (taps can't be ambiguous).
            const bool dotInPanel =
                g.dotX >= g.px && g.dotX <= g.px + g.pw && g.dotY >= g.py && g.dotY <= g.py + g.ph;
            t.IsTrue(!dotInPanel, "dot outside the panel"); });

        tc.Run("row labels are short and complete (panel-polish-1, no truncation)", [](TestCase &t)
               {
            using ps2x::set2::Row;
            t.Equals(ps2x::set2::kRowCount, 6, "Online row removed");
            t.IsTrue(std::string(ps2x::set2::rowLabel(Row::Mode)) == "Mode", "mode");
            t.IsTrue(std::string(ps2x::set2::rowLabel(Row::Res)) == "Resolution", "resolution");
            t.IsTrue(std::string(ps2x::set2::rowLabel(Row::Perf)) == "Perf", "perf");
            t.IsTrue(std::string(ps2x::set2::rowLabel(Row::Upscale)) == "Upscale", "upscale");
            t.IsTrue(std::string(ps2x::set2::rowLabel(Row::Touch)) == "Touch", "touch");
            t.IsTrue(std::string(ps2x::set2::rowLabel(Row::Restart)) == "Restart", "restart");
            for (int i = 0; i < ps2x::set2::kRowCount; ++i)
            {
                const std::string label(
                    ps2x::set2::rowLabel(static_cast<Row>(i)));
                t.IsTrue(!label.empty() && label.size() <= 10, "short non-empty label");
            } });

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

        tc.Run("pending options project from the measured height", [](TestCase &t)
               {
            using ps2x::set2::Res;
            using ps2x::set2::resProjectedH;
            t.Equals(resProjectedH(Res::Native, "2", 896), 448u, "native of measured 2x");
            t.Equals(resProjectedH(Res::R2x, "2", 896), 896u, "2x keeps measured");
            t.Equals(resProjectedH(Res::RDefault, "2", 896), 896u, "default shows measured");
            t.Equals(resProjectedH(Res::Native, "native", 1080), 1080u, "native effective: base is measured");
            t.Equals(resProjectedH(Res::R2x, "native", 1080), 2160u, "2x of native display height");
            t.Equals(resProjectedH(Res::Native, "1x", 480), 480u, "1x base");
            t.Equals(resProjectedH(Res::Native, "2.5", 1200), 480u, "fractional scale");
            t.Equals(resProjectedH(Res::Native, "junk", 900), 900u, "junk scale falls back to measured");
            t.Equals(resProjectedH(Res::R2x, "2", 0), 0u, "no frame yet -> 0"); }); });

    MiniTest::Case("Set2Settings", [](TestCase &tc)
                   {
        tc.Run("parses owned keys only, trims, skips comments and junk", [](TestCase &t)
               {
            const auto e = ps2x::set2::parseSettingsEnv(
                "# panel file\n  GE1_UPSCALE = 2 \nPS2X_SETTINGS_PERF=1\nGE1_ONDEVICE_UPSCALE=1\nPS2X_VIRTUAL_PAD=0\nPS2X_SOUND=1\nnoequals\n=novalue\n");
            t.Equals(e.size(), static_cast<size_t>(4), "allowlisted keys only");
            t.IsTrue(e[0] == std::make_pair(std::string("GE1_UPSCALE"), std::string("2")), "trimmed UPSCALE");
            t.IsTrue(e[1] == std::make_pair(std::string("PS2X_SETTINGS_PERF"), std::string("1")), "PERF");
            t.IsTrue(e[2] == std::make_pair(std::string("GE1_ONDEVICE_UPSCALE"), std::string("1")), "ODU");
            t.IsTrue(e[3] == std::make_pair(std::string("PS2X_VIRTUAL_PAD"), std::string("0")), "VPAD"); });

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

        tc.Run("res maps to the UPSCALE line (default clears it)", [](TestCase &t)
               {
            t.IsTrue(std::string(ps2x::set2::resToUpscale(ps2x::set2::Res::Native)) == "native", "native");
            t.IsTrue(std::string(ps2x::set2::resToUpscale(ps2x::set2::Res::R2x)) == "2", "2x");
            t.IsTrue(std::string(ps2x::set2::resToUpscale(ps2x::set2::Res::RDefault)).empty(), "default clears"); });

        tc.Run("touch maps to the VIRTUAL_PAD line (auto clears it)", [](TestCase &t)
               {
            t.IsTrue(ps2x::set2::touchFromSetting("1") == ps2x::set2::Touch::On, "1 -> on");
            t.IsTrue(ps2x::set2::touchFromSetting("0") == ps2x::set2::Touch::Off, "0 -> off");
            t.IsTrue(ps2x::set2::touchFromSetting("") == ps2x::set2::Touch::Auto, "unset -> auto");
            t.IsTrue(ps2x::set2::touchFromSetting("yes") == ps2x::set2::Touch::Auto, "junk -> auto");
            t.IsTrue(std::string(ps2x::set2::touchToSetting(ps2x::set2::Touch::On)) == "1", "on -> 1");
            t.IsTrue(std::string(ps2x::set2::touchToSetting(ps2x::set2::Touch::Off)) == "0", "off -> 0");
            t.IsTrue(std::string(ps2x::set2::touchToSetting(ps2x::set2::Touch::Auto)).empty(), "auto clears"); }); });
}
