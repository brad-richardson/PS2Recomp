#include "MiniTest.h"
#include "ps2_settings_overlay.h"

#include <array>
#include <cstdint>
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
            t.IsTrue(up.action == ps2x::set2::Action::None && st.sel == ps2x::set2::Row::MenuBtn, "wrap to Menu button");
            auto down = ps2x::set2::navStep(st, false, true, false, false, false);
            t.IsTrue(down.action == ps2x::set2::Action::None && st.sel == ps2x::set2::Row::Mode, "wrap to Mode"); });

        tc.Run("X toggles Mode and steps Res in height order, arming the restart", [](TestCase &t)
               {
            ps2x::set2::NavState st;
            st.runMode = ps2x::set2::Mode::M120;
            st.mode = ps2x::set2::Mode::M120;
            st.runRes = ps2x::set2::Res::Screen;
            st.res = ps2x::set2::Res::Screen;
            st.nativeH = 1080; // Odin: 1x, 2x, Screen, 3x, 4x
            auto m = ps2x::set2::navStep(st, false, false, true, false, false);
            t.IsTrue(m.action == ps2x::set2::Action::Changed && st.mode == ps2x::set2::Mode::M60, "120 -> 60");
            t.IsTrue(st.restartArmed, "dirty Mode arms");
            st.sel = ps2x::set2::Row::Res;
            auto r1 = ps2x::set2::navStep(st, false, false, true, false, false);
            t.IsTrue(r1.action == ps2x::set2::Action::Changed && st.res == ps2x::set2::Res::R3x, "Screen -> 3x");
            // A second X on the dirty row would restart, so browsing steps
            // nav away and back (disarms) between presses.
            (void)ps2x::set2::navStep(st, false, true, false, false, false);
            (void)ps2x::set2::navStep(st, true, false, false, false, false);
            auto r2 = ps2x::set2::navStep(st, false, false, true, false, false);
            t.IsTrue(r2.action == ps2x::set2::Action::Changed && st.res == ps2x::set2::Res::R4x, "3x -> 4x");
            (void)ps2x::set2::navStep(st, false, true, false, false, false);
            (void)ps2x::set2::navStep(st, true, false, false, false, false);
            auto r3 = ps2x::set2::navStep(st, false, false, true, false, false);
            t.IsTrue(st.res == ps2x::set2::Res::R1x, "4x wraps to 1x");
            st.sel = ps2x::set2::Row::Perf;
            auto p = ps2x::set2::navStep(st, false, false, true, false, false);
            t.IsTrue(p.action == ps2x::set2::Action::Changed && st.perf, "perf on");
            t.IsTrue(!st.restartArmed, "live row disarms");
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

        tc.Run("X toggles Menu button shown/hidden and disarms", [](TestCase &t)
               {
            ps2x::set2::NavState st;
            st.sel = ps2x::set2::Row::MenuBtn;
            st.restartArmed = true;
            auto o1 = ps2x::set2::navStep(st, false, false, true, false, false);
            t.IsTrue(o1.action == ps2x::set2::Action::Changed && st.menubtn == ps2x::set2::MenuBtn::Hidden,
                     "shown -> hidden");
            t.IsTrue(!st.restartArmed, "live row disarms");
            auto o2 = ps2x::set2::navStep(st, false, false, true, false, false);
            t.IsTrue(o2.action == ps2x::set2::Action::Changed && st.menubtn == ps2x::set2::MenuBtn::Shown,
                     "hidden -> shown"); });

        tc.Run("dirty Mode/Res rows arm; a second X restarts; nav away disarms", [](TestCase &t)
               {
            ps2x::set2::NavState st;
            st.runMode = ps2x::set2::Mode::M120;
            st.mode = ps2x::set2::Mode::M120;
            st.runRes = ps2x::set2::Res::Screen;
            st.res = ps2x::set2::Res::Screen;
            st.nativeH = 1080;
            auto arm = ps2x::set2::navStep(st, false, false, true, false, false);
            t.IsTrue(arm.action == ps2x::set2::Action::Changed && st.restartArmed, "first X toggles + arms");
            auto go = ps2x::set2::navStep(st, false, false, true, false, false);
            t.IsTrue(go.action == ps2x::set2::Action::DoRestart, "second X restarts (no third toggle)");
            t.IsTrue(st.mode == ps2x::set2::Mode::M60, "pending kept for the restart");
            (void)ps2x::set2::navStep(st, false, true, false, false, false);
            t.IsTrue(!st.restartArmed, "Down disarms");
            // Toggling back to the running value disarms when nothing else is dirty.
            st.sel = ps2x::set2::Row::Mode;
            auto back = ps2x::set2::navStep(st, false, false, true, false, false);
            t.IsTrue(back.action == ps2x::set2::Action::Changed && st.mode == ps2x::set2::Mode::M120,
                     "toggle back");
            t.IsTrue(!st.restartArmed, "clean again disarms");
            // Res row: same arm-then-restart shape.
            st.sel = ps2x::set2::Row::Res;
            (void)ps2x::set2::navStep(st, false, false, true, false, false);
            t.IsTrue(st.restartArmed && st.res == ps2x::set2::Res::R3x, "Res dirty + armed");
            auto goRes = ps2x::set2::navStep(st, false, false, true, false, false);
            t.IsTrue(goRes.action == ps2x::set2::Action::DoRestart, "second X on Res restarts"); });

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
        tc.Run("row taps hit 0..Count-1, misses read -1, closed panel has no tap target", [](TestCase &t)
               {
            const auto g = ps2x::set2::panelLayout(1280.0f, 720.0f);
            for (int i = 0; i < ps2x::set2::kRowCount; ++i)
            {
                const int hit = ps2x::set2::panelHit(g, true, g.rowX + g.rowW * 0.5f, g.rowY[i] + g.rowH * 0.5f);
                t.IsTrue(hit == i, "row centre hits");
            }
            t.IsTrue(ps2x::set2::panelHit(g, true, g.px * 0.25f, g.py * 0.25f) == -1, "outside misses");
            // set2-polish-1 (Brad 10-16): the gear dot is gone — the old
            // dot corner is a plain miss now, and every closed-panel tap
            // misses (the chord / two-finger tap opens it instead).
            t.IsTrue(ps2x::set2::panelHit(g, true, 1280.0f - 0.055f * 720.0f, 0.20f * 720.0f) == -1,
                     "old dot corner misses while open");
            t.IsTrue(ps2x::set2::panelHit(g, false, g.rowX + g.rowW * 0.5f, g.rowY[0] + g.rowH * 0.5f) == -1,
                     "row centre misses while closed");
            t.IsTrue(ps2x::set2::panelHit(g, false, g.px * 0.25f, g.py * 0.25f) == -1,
                     "outside misses while closed"); });

        tc.Run("row labels are short and complete (panel-polish-1, no truncation)", [](TestCase &t)
               {
            using ps2x::set2::Row;
            t.Equals(ps2x::set2::kRowCount, 6, "six rows");
            t.IsTrue(std::string(ps2x::set2::rowLabel(Row::Mode)) == "Mode", "mode");
            t.IsTrue(std::string(ps2x::set2::rowLabel(Row::Res)) == "Resolution", "resolution");
            t.IsTrue(std::string(ps2x::set2::rowLabel(Row::Perf)) == "Perf", "perf");
            t.IsTrue(std::string(ps2x::set2::rowLabel(Row::Upscale)) == "Upscale", "upscale");
            t.IsTrue(std::string(ps2x::set2::rowLabel(Row::Touch)) == "Touch", "touch");
            t.IsTrue(std::string(ps2x::set2::rowLabel(Row::MenuBtn)) == "Menu button", "menu button");
            for (int i = 0; i < ps2x::set2::kRowCount; ++i)
            {
                const std::string label(
                    ps2x::set2::rowLabel(static_cast<Row>(i)));
                // Labels draw left-aligned with the value right-aligned, so
                // nothing truncates; 12 covers "Menu button" (11).
                t.IsTrue(!label.empty() && label.size() <= 12, "short non-empty label");
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
            // \xc3\x97 is x, \xb7 is the middle dot (both in the panel font).
            t.IsTrue(resOptionLabel(Res::R1x, 1080, Res::Screen) == "1\xc3\x97 \xb7 448p", "1x");
            t.IsTrue(resOptionLabel(Res::R2x, 1080, Res::Screen) == "2\xc3\x97 \xb7 896p", "2x");
            t.IsTrue(resOptionLabel(Res::R3x, 1080, Res::Screen) == "3\xc3\x97 \xb7 1344p", "3x");
            t.IsTrue(resOptionLabel(Res::R4x, 1080, Res::Screen) == "4\xc3\x97 \xb7 1792p", "4x");
            t.IsTrue(resOptionLabel(Res::Screen, 1080, Res::Screen) == "Screen \xb7 1080p (default)",
                     "Odin Screen default");
            t.IsTrue(resOptionLabel(Res::R2x, 1080, Res::R2x) == "2\xc3\x97 \xb7 896p (default)",
                     "iOS 2x default");
            t.IsTrue(resOptionLabel(Res::Screen, 1328, Res::R2x) == "Screen \xb7 1440p",
                     "iPad Screen rounds like resHeightLabel");
            t.IsTrue(resOptionLabel(Res::Screen, 0, Res::Screen) == "Screen (default)",
                     "no frame yet falls back to plain Screen"); });

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
                "# panel file\n  GE1_UPSCALE = 2 \nPS2X_SETTINGS_PERF=1\nGE1_ONDEVICE_UPSCALE=1\nPS2X_VIRTUAL_PAD=0\nPS2X_MENU_BUTTON=0\nPS2X_SOUND=1\nnoequals\n=novalue\n");
            t.Equals(e.size(), static_cast<size_t>(5), "allowlisted keys only");
            t.IsTrue(e[0] == std::make_pair(std::string("GE1_UPSCALE"), std::string("2")), "trimmed UPSCALE");
            t.IsTrue(e[1] == std::make_pair(std::string("PS2X_SETTINGS_PERF"), std::string("1")), "PERF");
            t.IsTrue(e[2] == std::make_pair(std::string("GE1_ONDEVICE_UPSCALE"), std::string("1")), "ODU");
            t.IsTrue(e[3] == std::make_pair(std::string("PS2X_VIRTUAL_PAD"), std::string("0")), "VPAD");
            t.IsTrue(e[4] == std::make_pair(std::string("PS2X_MENU_BUTTON"), std::string("0")), "MENUBTN"); });

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

        tc.Run("restart persists the pending rows, never the effective env", [](TestCase &t)
               {
            using ps2x::set2::Res;
            using ps2x::set2::SettingsPairs;
            // Brad 10-17 iPhone bug: stale file {native} + pending 3x must
            // write 3, keeping the other rows and their order.
            const SettingsPairs stale{{"GE1_UPSCALE", "native"},
                                      {"PS2X_SETTINGS_PERF", "0"},
                                      {"PS2X_VIRTUAL_PAD", "1"}};
            const auto w1 = ps2x::set2::settingsForRestart(stale, Res::R3x, true);
            t.Equals(w1.size(), static_cast<size_t>(3), "row count kept");
            t.IsTrue(w1[0] == std::make_pair(std::string("GE1_UPSCALE"), std::string("3")),
                     "pending 3x wins over stale native");
            t.IsTrue(w1[1] == std::make_pair(std::string("PS2X_SETTINGS_PERF"), std::string("1")),
                     "live perf flag");
            t.IsTrue(w1[2] == std::make_pair(std::string("PS2X_VIRTUAL_PAD"), std::string("1")),
                     "untouched row kept");
            // Screen writes native explicitly (no default-clearing choice).
            const auto w2 = ps2x::set2::settingsForRestart({}, Res::Screen, false);
            t.IsTrue(w2.size() == 2 && w2[0].second == "native" && w2[1].second == "0",
                     "empty file gains both lines");
            // 1x writes 1 (it is never a default, but it is writable).
            const auto w3 = ps2x::set2::settingsForRestart({}, Res::R1x, false);
            t.IsTrue(w3[0].second == "1", "1x persists"); });

        tc.Run("menu button maps to its line (shown clears it)", [](TestCase &t)
               {
            using ps2x::set2::MenuBtn;
            t.IsTrue(ps2x::set2::menuBtnFromSetting("0") == MenuBtn::Hidden, "0 -> hidden");
            t.IsTrue(ps2x::set2::menuBtnFromSetting("") == MenuBtn::Shown, "unset -> shown");
            t.IsTrue(ps2x::set2::menuBtnFromSetting("1") == MenuBtn::Shown, "1 -> shown");
            t.IsTrue(ps2x::set2::menuBtnFromSetting("junk") == MenuBtn::Shown, "junk -> shown");
            t.IsTrue(std::string(ps2x::set2::menuBtnToSetting(MenuBtn::Hidden)) == "0", "hidden -> 0");
            t.IsTrue(std::string(ps2x::set2::menuBtnToSetting(MenuBtn::Shown)).empty(), "shown clears");
            t.IsTrue(ps2x::set2::settingsKeyAllowed("PS2X_MENU_BUTTON"), "key allowlisted"); });

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
