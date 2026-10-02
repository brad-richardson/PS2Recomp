#include "MiniTest.h"
#include "ps2_input_diag.h"
#include "ps2_touch_events.h"
#include "ps2_virtual_pad.h"

#include <cstdint>
#include <string>

// IN4: event-fed touches (ps2_touch_events.h merge + two-finger marker) and
// the input-diagnostics line format + ring (ps2_input_diag.h pure parts).
void register_ps2_input_diag_tests()
{
    using namespace ps2x::touchev;
    using namespace ps2x::inputdiag;
    MiniTest::Case("Ps2TouchEvents", [](TestCase &tc)
                   {
        tc.Run("down+up inside one pass is returned exactly once, then dropped", [](TestCase &t)
               {
            Table tab;
            LiveFinger none[1]{};
            OutTouch out[8];
            tab.down(7, 0.25f, 0.10f, 1000u);
            tab.up(7, 1005u);
            const int n1 = tab.merge(none, 0, false, out, 8);
            t.Equals(n1, 1, "one touch");
            t.Equals(out[0].id, static_cast<int64_t>(7), "same finger");
            t.Equals(out[0].x, 0.25f, "touch-down x");
            t.Equals(out[0].y, 0.10f, "touch-down y");
            const int n2 = tab.merge(none, 0, false, out, 8);
            t.Equals(n2, 0, "dropped on the next pass"); });

        tc.Run("a held finger comes from the live list, not the record", [](TestCase &t)
               {
            Table tab;
            OutTouch out[8];
            tab.down(3, 0.5f, 0.5f, 1000u);
            LiveFinger live[1] = {{3, 0.52f, 0.51f}};
            const int n1 = tab.merge(live, 1, false, out, 8);
            t.Equals(n1, 1, "one touch, no duplicate");
            t.Equals(out[0].x, 0.52f, "live position wins");
            tab.up(3, 1100u);
            const int n2 = tab.merge(live, 1, false, out, 8);
            t.Equals(n2, 1, "still live: still one");
            LiveFinger none[1]{};
            const int n3 = tab.merge(none, 0, false, out, 8);
            t.Equals(n3, 0, "lifted after being seen live: nothing owed"); });

        tc.Run("multi-peer records are dropped while a controller is connected", [](TestCase &t)
               {
            Table tab;
            OutTouch out[8];
            LiveFinger none[1]{};
            // Two-finger tap with a controller connected: the marker pair is
            // multi-peer, so neither touch reaches the guest.
            tab.down(11, 0.3f, 0.3f, 1000u);
            tab.down(12, 0.6f, 0.3f, 1100u);
            tab.up(11, 1150u);
            tab.up(12, 1180u);
            const int n1 = tab.merge(none, 0, true, out, 8);
            t.Equals(n1, 0, "tap pair dropped with a controller");
            uint64_t mMs = 0u;
            float mx = 0.0f, my = 0.0f;
            t.IsTrue(tab.takeMarker(mMs, mx, my), "marker still fires for the log");
            t.Equals(mMs, static_cast<uint64_t>(1180u), "marker at the second up");
            t.IsFalse(tab.takeMarker(mMs, mx, my), "marker fires once");
            // Same tap with no controller: the touches pass (vpad owns them).
            Table tab2;
            tab2.down(11, 0.3f, 0.3f, 1000u);
            tab2.down(12, 0.6f, 0.3f, 1100u);
            tab2.up(11, 1150u);
            tab2.up(12, 1180u);
            const int n2 = tab2.merge(none, 0, false, out, 8);
            t.Equals(n2, 2, "no controller: both touches pass"); });

        tc.Run("a lone quick tap is not a marker and is not dropped", [](TestCase &t)
               {
            Table tab;
            OutTouch out[8];
            LiveFinger none[1]{};
            tab.down(5, 0.4f, 0.4f, 1000u);
            tab.up(5, 1030u);
            const int n = tab.merge(none, 0, true, out, 8);
            t.Equals(n, 1, "single-finger tap still passes with a controller");
            uint64_t mMs = 0u;
            float mx = 0.0f, my = 0.0f;
            t.IsFalse(tab.takeMarker(mMs, mx, my), "no marker for one finger"); });

        tc.Run("a third finger spoils the tap pair", [](TestCase &t)
               {
            Table tab;
            LiveFinger none[1]{};
            tab.down(1, 0.3f, 0.3f, 1000u);
            tab.down(2, 0.6f, 0.3f, 1100u);
            tab.down(3, 0.5f, 0.6f, 1150u); // third finger: not a tap
            tab.up(1, 1200u);
            tab.up(2, 1210u);
            tab.up(3, 1220u);
            uint64_t mMs = 0u;
            float mx = 0.0f, my = 0.0f;
            t.IsFalse(tab.takeMarker(mMs, mx, my), "three fingers are not a marker"); });

        tc.Run("a sliding pair is not a marker", [](TestCase &t)
               {
            Table tab;
            LiveFinger none[1]{};
            tab.down(1, 0.3f, 0.3f, 1000u);
            tab.down(2, 0.6f, 0.3f, 1100u);
            tab.motion(2, 0.8f, 0.3f, 1120u); // 0.2 slide: past the slop
            tab.up(1, 1150u);
            tab.up(2, 1180u);
            uint64_t mMs = 0u;
            float mx = 0.0f, my = 0.0f;
            t.IsFalse(tab.takeMarker(mMs, mx, my), "slide spoils the marker"); });

        tc.Run("a late second down is not a pair", [](TestCase &t)
               {
            Table tab;
            LiveFinger none[1]{};
            tab.down(1, 0.3f, 0.3f, 1000u);
            tab.up(1, 1100u);
            tab.down(2, 0.6f, 0.3f, 2000u); // 1 s later: no pair
            tab.up(2, 2100u);
            uint64_t mMs = 0u;
            float mx = 0.0f, my = 0.0f;
            t.IsFalse(tab.takeMarker(mMs, mx, my), "no marker across a 1 s gap"); });

        tc.Run("the TEST_TOUCHES replay passes the merge unchanged", [](TestCase &t)
               {
            // The iOS Sim smoke path: synthetic touches (no table records)
            // merge to the identical list, so the replay is unaffected.
            Table tab;
            LiveFinger none[1]{};
            OutTouch out[8];
            const int n = tab.merge(none, 0, false, out, 8);
            t.Equals(n, 0, "no records: empty in, empty out");
            using namespace ps2x::vpad;
            const auto touches = parseTestTouches("100:0.5:0.5:10");
            t.Equals(static_cast<uint64_t>(touches.size()), static_cast<uint64_t>(1u), "replay parses");
            TouchPoint ts[8];
            const int m = activeTestTouchesWithIds(touches, 105u, 800.0f, 600.0f, ts, 0, 8);
            t.Equals(m, 1, "one synthetic touch active");
            t.Equals(ts[0].x, 400.0f, "replay x unchanged");
            t.Equals(ts[0].y, 300.0f, "replay y unchanged"); }); });

    MiniTest::Case("Ps2InputDiagFormat", [](TestCase &tc)
                   {
        tc.Run("sdl button line", [](TestCase &t)
               {
            Event e;
            e.wallMs = 1234u;
            e.tick = 5678u;
            e.layer = Layer::Sdl;
            e.code = static_cast<uint8_t>(SdlCode::Btn);
            e.a = 3u;
            e.b = 9u;
            e.c = 1;
            t.Equals(formatEvent(e),
                     std::string("[input-sdl] wall=1234ms tick=5678 btn which=3 button=leftshoulder to=down"),
                     "exact line"); });

        tc.Run("sdl trigger axis line", [](TestCase &t)
               {
            Event e;
            e.wallMs = 100u;
            e.tick = 200u;
            e.layer = Layer::Sdl;
            e.code = static_cast<uint8_t>(SdlCode::Axis);
            e.a = 1u;
            e.b = 5u;
            e.c = 20000;
            t.Equals(formatEvent(e),
                     std::string("[input-sdl] wall=100ms tick=200 axis which=1 axis=rtrigger value=20000"),
                     "exact line"); });

        tc.Run("publish line names changed bits", [](TestCase &t)
               {
            Event e;
            e.wallMs = 10u;
            e.tick = 20u;
            e.layer = Layer::Publish;
            e.a = 0x0400u;
            e.b = 0x8400u; // l1 held, square pressed
            t.Equals(formatEvent(e),
                     std::string("[input-pub] wall=10ms tick=20 pub 0x0400->0x8400 +square -none"),
                     "exact line"); });

        tc.Run("guest line is active-low", [](TestCase &t)
               {
            Event e;
            e.wallMs = 10u;
            e.tick = 20u;
            e.layer = Layer::Guest;
            e.a = 0xFFFFu;
            e.b = 0xFBFFu; // l1 newly pressed (bit 10 cleared)
            t.Equals(formatEvent(e),
                     std::string("[input-guest] wall=10ms tick=20 port0 0xffff->0xfbff +l1 -none"),
                     "exact line"); });

        tc.Run("rider line names grab bits", [](TestCase &t)
               {
            Event e;
            e.wallMs = 10u;
            e.tick = 20u;
            e.layer = Layer::Rider;
            e.a = 0x1464e30u;
            e.b = 0x00000u;
            e.c = static_cast<int32_t>(0x40000u);
            t.Equals(formatEvent(e),
                     std::string("[input-rider] wall=10ms tick=20 P=0x1464e30 0x00000->0x40000 +grab_l1 -none"),
                     "exact line"); });

        tc.Run("marker and stat lines", [](TestCase &t)
               {
            Event m;
            m.wallMs = 99u;
            m.tick = 100u;
            m.layer = Layer::Marker;
            m.a = 1u;
            m.x = 0.450f;
            m.y = 0.300f;
            t.Equals(formatEvent(m),
                     std::string("[input-marker] wall=99ms tick=100 two_finger_tap x=0.450 y=0.300 ctl=1"),
                     "exact marker line");
            Event s;
            s.wallMs = 1000u;
            s.tick = 7200u;
            s.layer = Layer::Stat;
            s.x = 119.88f;
            s.b = 120u;
            s.a = 0u;
            std::snprintf(s.text, sizeof(s.text), "slots=\"0:1 1:0\" which=\"3\"");
            t.Equals(formatEvent(s),
                     std::string("[input-stat] wall=1000ms tick=7200 vsyncs_per_s=119.88 presents=120 "
                                 "slots=\"0:1 1:0\" which=\"3\" drops=0"),
                     "exact stat line"); }); });

    MiniTest::Case("Ps2InputDiagRing", [](TestCase &tc)
                   {
        tc.Run("push drains in order", [](TestCase &t)
               {
            EventRing<8> r;
            Event e1, e2;
            e1.wallMs = 1u;
            e2.wallMs = 2u;
            t.IsTrue(r.push(e1), "push 1");
            t.IsTrue(r.push(e2), "push 2");
            Event out[8];
            t.Equals(r.drain(out, 8), static_cast<size_t>(2u), "two drained");
            t.Equals(out[0].wallMs, static_cast<uint64_t>(1u), "order kept");
            t.Equals(out[1].wallMs, static_cast<uint64_t>(2u), "order kept");
            t.Equals(r.drain(out, 8), static_cast<size_t>(0u), "empty after drain"); });

        tc.Run("a full ring drops and counts, never blocks", [](TestCase &t)
               {
            EventRing<4> r;
            Event e;
            for (int i = 0; i < 4; ++i)
                t.IsTrue(r.push(e), "fills");
            t.IsFalse(r.push(e), "full: dropped");
            t.Equals(r.drops(), static_cast<uint64_t>(1u), "one drop counted");
            Event out[4];
            t.Equals(r.drain(out, 4), static_cast<size_t>(4u), "the four kept drain"); });

        tc.Run("knob parsing", [](TestCase &t)
               {
            t.IsTrue(enabledFromEnv("1"), "1 is on");
            t.IsFalse(enabledFromEnv(nullptr), "unset is off");
            t.IsFalse(enabledFromEnv("0"), "0 is off");
            t.IsFalse(enabledFromEnv("10"), "10 is off"); }); });
}
