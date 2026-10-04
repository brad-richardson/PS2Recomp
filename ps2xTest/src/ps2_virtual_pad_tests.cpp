#include "MiniTest.h"
#include "ps2_virtual_pad.h"
#include "runtime/ps2_pad.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

// I26: virtual controls layout, hit test and the pad-backend union.
void register_ps2_virtual_pad_tests()
{
    using namespace ps2x::vpad;
    MiniTest::Case("Ps2VirtualPad", [](TestCase &tc)
                   {
        tc.Run("every button's own centre presses exactly that button", [](TestCase &t)
               {
            const Layout l = makeLayout(874.0f, 402.0f);
            t.Equals(l.buttons.size(), static_cast<size_t>(14), "14 buttons");
            for (const Button &b : l.buttons)
            {
                const float x = b.x, y = b.y;
                t.Equals(static_cast<uint32_t>(pressedMask(l, &x, &y, 1)), static_cast<uint32_t>(b.mask), b.label);
            } });

        tc.Run("no overlap between buttons; all inside the window", [](TestCase &t)
               {
            const Layout l = makeLayout(874.0f, 402.0f);
            for (const Button &b : l.buttons)
            {
                t.IsTrue(b.x - b.r >= 0.0f && b.x + b.r <= 874.0f && b.y - b.r >= 0.0f && b.y + b.r <= 402.0f, b.label);
            }
            const float x = 437.0f, y = 201.0f; // picture centre
            t.Equals(static_cast<uint32_t>(pressedMask(l, &x, &y, 1)), 0u, "centre of the screen presses nothing"); });

        tc.Run("D-pad: dead zone, diagonals, multi-touch", [](TestCase &t)
               {
            const Layout l = makeLayout(874.0f, 402.0f);
            const float cx = l.dpadX, cy = l.dpadY, o = l.dpadR * 0.6f;
            float xs[3] = {cx, 0, 0}, ys[3] = {cy, 0, 0};
            t.Equals(static_cast<uint32_t>(pressedMask(l, xs, ys, 1)), 0u, "dead zone");
            xs[0] = cx + o; ys[0] = cy - o;
            t.Equals(static_cast<uint32_t>(pressedMask(l, xs, ys, 1)), static_cast<uint32_t>(kUp | kRight), "up-right diagonal");
            xs[0] = cx - o; ys[0] = cy + o * 0.1f;
            t.Equals(static_cast<uint32_t>(pressedMask(l, xs, ys, 1)), static_cast<uint32_t>(kLeft), "left only");
            const Button *cross = nullptr, *start = nullptr;
            for (const Button &b : l.buttons) { if (b.mask == kCross) cross = &b; if (b.mask == kStart) start = &b; }
            xs[1] = cross->x; ys[1] = cross->y; xs[2] = start->x; ys[2] = start->y;
            t.Equals(static_cast<uint32_t>(pressedMask(l, xs, ys, 3)), static_cast<uint32_t>(kLeft | kCross | kStart), "three fingers"); });

        tc.Run("dev test touches parse and drive the hit test", [](TestCase &t)
               {
            const auto v = parseTestTouches("640:0.848:0.92:15,bad,770:0.906:0.73:15,1:2:3:0");
            t.Equals(v.size(), static_cast<size_t>(2), "2 valid items (bad text and hold 0 skipped)");
            const Layout l = makeLayout(874.0f, 402.0f);
            float xs[4], ys[4];
            int n = activeTestTouches(v, 645, 874.0f, 402.0f, xs, ys, 0, 4);
            t.Equals(static_cast<uint32_t>(pressedMask(l, xs, ys, n)), static_cast<uint32_t>(kStart), "START at 645");
            n = activeTestTouches(v, 775, 874.0f, 402.0f, xs, ys, 0, 4);
            t.Equals(static_cast<uint32_t>(pressedMask(l, xs, ys, n)), static_cast<uint32_t>(kCross), "cross at 775");
            t.Equals(activeTestTouches(v, 700, 874.0f, 402.0f, xs, ys, 0, 4), 0, "none at 700"); });

        tc.Run("I32 layout: stick on the left, D-pad on the right, no overlap", [](TestCase &t)
               {
            const Layout l = makeLayout(874.0f, 402.0f);
            t.IsTrue(l.stickRestX < 874.0f * 0.5f, "stick rests on the left");
            t.IsTrue(l.stickRestX < l.stickZoneX, "stick rest is inside the stick zone");
            t.IsTrue(l.dpadX > 874.0f * 0.5f, "D-pad is on the right");
            t.IsTrue(l.dpadR * 2.0f >= 44.0f, "D-pad disc is a >= 44pt hit target");
            t.IsTrue(l.stickZoneX < l.dpadX - l.dpadR * 1.15f, "stick zone ends left of the D-pad disc");
            // The D-pad hit disc is drawn clear of every other button: no
            // touch on a drawn button falls inside the disc.
            for (const Button &b : l.buttons)
            {
                if (isDpad(b.mask))
                    continue;
                const float dx = b.x - l.dpadX;
                const float dy = b.y - l.dpadY;
                t.IsTrue(std::sqrt(dx * dx + dy * dy) > l.dpadR * 1.15f + b.r, b.label);
            }
            // Face buttons did not move: still the I26 diamond.
            const Button *tri = nullptr, *cro = nullptr, *squ = nullptr, *cir = nullptr;
            for (const Button &b : l.buttons)
            {
                if (b.mask == kTriangle) tri = &b;
                if (b.mask == kCross) cro = &b;
                if (b.mask == kSquare) squ = &b;
                if (b.mask == kCircle) cir = &b;
            }
            t.IsTrue(std::fabs((cro->y - tri->y) - 2.0f * 0.13f * 402.0f) < 1e-3f, "cross/triangle spacing");
            t.IsTrue(std::fabs((cir->x - squ->x) - 2.0f * 0.13f * 402.0f) < 1e-3f, "circle/square spacing");
            t.IsTrue(std::fabs((tri->x + cro->x) * 0.5f - (874.0f - 0.205f * 402.0f)) < 1e-3f, "face column x"); });

        tc.Run("I34 layout: 1.5x stick, 80% D-pad below the face cluster, all sizes", [](TestCase &t)
               {
            const float sizes[][2] = {
                {874.0f, 402.0f},  // iPhone 16 Pro landscape
                {956.0f, 440.0f},  // iPhone 16 Pro Max landscape
                {1180.0f, 820.0f}, // iPad Air 11" landscape
            };
            for (const auto &s : sizes)
            {
                const float w = s[0], h = s[1];
                const Layout l = makeLayout(w, h);
                const auto tag = std::to_string(static_cast<int>(w)) + "x" + std::to_string(static_cast<int>(h)) + " ";
                // Stick: 1.5x the I32 radius, rest on the left in the zone,
                // rest disc on screen and clear of L1/L2/SELECT.
                t.IsTrue(std::fabs(l.stickR - 0.15f * h) < 1e-3f, tag + "stickR = 0.15u");
                t.IsTrue(l.stickRestX < w * 0.5f && l.stickRestX < l.stickZoneX, tag + "stick rest left, in zone");
                t.IsTrue(l.stickRestX - l.stickR >= 0.0f && l.stickRestY - l.stickR >= 0.0f &&
                             l.stickRestY + l.stickR <= h,
                         tag + "stick rest disc on screen");
                // Face-cluster bbox (drawn circles) and the D-pad's 80%.
                float x0 = w, x1 = 0.0f, y0 = h, y1 = 0.0f;
                for (const Button &b : l.buttons)
                {
                    if (b.mask == kTriangle || b.mask == kCross || b.mask == kSquare || b.mask == kCircle)
                    {
                        x0 = std::min(x0, b.x - b.r);
                        x1 = std::max(x1, b.x + b.r);
                        y0 = std::min(y0, b.y - b.r);
                        y1 = std::max(y1, b.y + b.r);
                    }
                }
                t.IsTrue(std::fabs((x1 - x0) - 0.40f * h) < 1e-3f, tag + "face bbox width = 0.40u");
                t.IsTrue(std::fabs((y1 - y0) - 0.40f * h) < 1e-3f, tag + "face bbox height = 0.40u");
                t.IsTrue(std::fabs(2.0f * l.dpadR - 0.8f * (x1 - x0)) < 1e-3f, tag + "D-pad = 80% of face width");
                t.IsTrue(std::fabs(2.0f * l.dpadR - 0.8f * (y1 - y0)) < 1e-3f, tag + "D-pad = 80% of face height");
                // D-pad below the face centre, right of screen centre, fully on screen.
                const float faceCy = (y0 + y1) * 0.5f;
                t.IsTrue(l.dpadY > faceCy, tag + "D-pad below the face cluster");
                t.IsTrue(l.dpadX > w * 0.5f, tag + "D-pad right of screen centre");
                t.IsTrue(l.dpadX - l.dpadR >= 0.0f && l.dpadX + l.dpadR <= w && l.dpadY - l.dpadR >= 0.0f &&
                             l.dpadY + l.dpadR <= h,
                         tag + "D-pad disc on screen");
                t.IsTrue(l.stickZoneX < l.dpadX - l.dpadR * 1.15f, tag + "stick zone ends left of the D-pad disc");
                // No drawn/hit overlap: D-pad vs every button (both ways),
                // buttons pairwise (both ways), stick rest vs all drawn.
                auto clear = [](float d, float need) { return d > need; };
                for (const Button &b : l.buttons)
                {
                    if (isDpad(b.mask))
                        continue;
                    const float d = std::hypot(b.x - l.dpadX, b.y - l.dpadY);
                    t.IsTrue(clear(d, l.dpadR * 1.15f + b.r), tag + "D-pad hit vs " + b.label);
                    t.IsTrue(clear(d, l.dpadR + 1.2f * b.r), tag + "D-pad drawn vs " + b.label + " hit");
                    const float ds = std::hypot(b.x - l.stickRestX, b.y - l.stickRestY);
                    t.IsTrue(clear(ds, l.stickR + b.r), tag + "stick rest vs " + b.label);
                }
                t.IsTrue(clear(std::hypot(l.stickRestX - l.dpadX, l.stickRestY - l.dpadY), l.stickR + l.dpadR),
                         tag + "stick rest vs D-pad");
                for (size_t i = 0; i < l.buttons.size(); ++i)
                {
                    if (isDpad(l.buttons[i].mask))
                        continue;
                    for (size_t j = i + 1; j < l.buttons.size(); ++j)
                    {
                        if (isDpad(l.buttons[j].mask))
                            continue;
                        const Button &a = l.buttons[i];
                        const Button &b = l.buttons[j];
                        const float d = std::hypot(a.x - b.x, a.y - b.y);
                        t.IsTrue(clear(d, 1.2f * a.r + b.r) && clear(d, a.r + 1.2f * b.r),
                                 tag + std::string(a.label) + " vs " + b.label);
                    }
                }
                // Every button's own centre still presses exactly itself.
                for (const Button &b : l.buttons)
                {
                    const float x = b.x, y = b.y;
                    t.Equals(static_cast<uint32_t>(pressedMask(l, &x, &y, 1)), static_cast<uint32_t>(b.mask),
                             tag + b.label);
                }
                const float cx = w * 0.5f, cyy = h * 0.5f;
                t.Equals(static_cast<uint32_t>(pressedMask(l, &cx, &cyy, 1)), 0u, tag + "picture centre presses nothing");
            } });

        tc.Run("stick: touch offset maps to left-stick bytes", [](TestCase &t)
               {
            const Layout l = makeLayout(874.0f, 402.0f);
            StickState st;
            uint8_t lx = 0, ly = 0;
            auto drive = [&](float x, float y)
            {
                StickVec v = updateStick(st, l, &x, &y, 1);
                stickBytes(v, lx, ly);
                return v;
            };
            StickVec v = drive(l.stickRestX, l.stickRestY);
            t.IsTrue(v.active, "touch-down anchors the stick");
            t.Equals(lx, static_cast<uint8_t>(0x80), "lx centred on touch-down");
            t.Equals(ly, static_cast<uint8_t>(0x80), "ly centred on touch-down");
            drive(l.stickRestX + l.stickR, l.stickRestY);
            t.Equals(lx, static_cast<uint8_t>(0xFF), "full right -> 0xFF");
            t.Equals(ly, static_cast<uint8_t>(0x80), "ly centred on full right");
            drive(l.stickRestX - l.stickR, l.stickRestY);
            t.Equals(lx, static_cast<uint8_t>(0x01), "full left -> 0x01");
            drive(l.stickRestX, l.stickRestY + l.stickR);
            t.Equals(ly, static_cast<uint8_t>(0xFF), "full down -> 0xFF");
            drive(l.stickRestX, l.stickRestY - l.stickR);
            t.Equals(ly, static_cast<uint8_t>(0x01), "full up -> 0x01");
            drive(l.stickRestX + 0.05f * l.stickR, l.stickRestY);
            t.Equals(lx, static_cast<uint8_t>(0x80), "5% deflection is inside the dead zone");
            drive(l.stickRestX + 0.11f * l.stickR, l.stickRestY);
            t.Equals(lx, static_cast<uint8_t>(142), "11% deflection leaves the dead zone");
            drive(l.stickRestX + 1.5f * l.stickR, l.stickRestY);
            t.Equals(lx, static_cast<uint8_t>(0xFF), "over-drag clamps to 0xFF");
            drive(l.stickRestX + l.stickR, l.stickRestY + l.stickR);
            t.Equals(lx, static_cast<uint8_t>(218), "diagonal clamps to the unit circle (lx)");
            t.Equals(ly, static_cast<uint8_t>(218), "diagonal clamps to the unit circle (ly)");
            v = updateStick(st, l, nullptr, nullptr, 0);
            stickBytes(v, lx, ly);
            t.IsFalse(v.active, "release deactivates");
            t.Equals(lx, static_cast<uint8_t>(0x80), "lx recentres on release");
            t.Equals(ly, static_cast<uint8_t>(0x80), "ly recentres on release"); });

        tc.Run("stick: track, re-anchor, button exclusion", [](TestCase &t)
               {
            const Layout l = makeLayout(874.0f, 402.0f);
            StickState st;
            float x = l.stickRestX, y = l.stickRestY;
            StickVec v = updateStick(st, l, &x, &y, 1);
            t.IsTrue(v.active && v.x == 0.0f && v.y == 0.0f, "anchored and centred");
            // A second finger far away does not steal the stick.
            float xs[2] = {l.stickRestX + 0.6f * l.stickR, 400.0f};
            float ys[2] = {l.stickRestY, 100.0f};
            v = updateStick(st, l, xs, ys, 2);
            t.IsTrue(v.active && v.x > 0.4f && v.x < 0.8f && v.y == 0.0f, "nearest touch drives");
            // The finger jumps away: re-anchor, centred until it moves.
            x = 400.0f;
            y = 100.0f;
            v = updateStick(st, l, &x, &y, 1);
            t.IsTrue(v.active && v.ax == 400.0f && v.ay == 100.0f, "jump re-anchors");
            t.IsTrue(v.x == 0.0f && v.y == 0.0f, "centred after re-anchor");
            // A touch on a button is never a stick touch (stick + button).
            const Button *l1 = nullptr;
            for (const Button &b : l.buttons)
            {
                if (b.mask == kL1)
                    l1 = &b;
            }
            StickState st2;
            float bx[2] = {l1->x, l.stickRestX + 0.6f * l.stickR};
            float by[2] = {l1->y, l.stickRestY};
            t.Equals(static_cast<uint32_t>(pressedMask(l, bx, by, 2)), static_cast<uint32_t>(kL1), "L1 still presses");
            v = updateStick(st2, l, bx, by, 2);
            t.IsTrue(v.active && v.ax == bx[1] && v.ay == by[1], "stick anchors at the non-button touch"); });

        tc.Run("stick bytes reach the pad backend", [](TestCase &t)
               {
            PSPadBackend backend;
            uint8_t data[32]{};
            liveStick().store(static_cast<uint16_t>(0xE0u | (0x20u << 8)));
            t.IsTrue(backend.readState(0, 0, data, sizeof(data)), "readState ok");
            t.Equals(data[6], static_cast<uint8_t>(0xE0), "lx from the virtual stick");
            t.Equals(data[7], static_cast<uint8_t>(0x20), "ly from the virtual stick");
            liveStick().store(kStickNoOverride);
            t.IsTrue(backend.readState(0, 0, data, sizeof(data)), "readState ok");
            t.Equals(data[6], static_cast<uint8_t>(0x80), "lx centred with no override");
            t.Equals(data[7], static_cast<uint8_t>(0x80), "ly centred with no override"); });

        tc.Run("dev test stick parses", [](TestCase &t)
               {
            float lx = 0.0f, ly = 0.0f;
            t.IsTrue(parseTestStick("0.75,-0.5", lx, ly), "valid parses");
            t.IsTrue(std::fabs(lx - 0.75f) < 1e-6f && std::fabs(ly + 0.5f) < 1e-6f, "values exact");
            StickVec v;
            v.x = lx;
            v.y = ly;
            uint8_t bx = 0, by = 0;
            stickBytes(v, bx, by);
            t.Equals(bx, static_cast<uint8_t>(0xDF), "0.75 -> 0xDF");
            t.Equals(by, static_cast<uint8_t>(0x40), "-0.5 -> 0x40");
            t.IsTrue(parseTestStick("2,-3", lx, ly), "out of range parses");
            t.IsTrue(lx == 1.0f && ly == -1.0f, "clamped to +-1");
            t.IsFalse(parseTestStick(nullptr, lx, ly), "null rejected");
            t.IsFalse(parseTestStick("", lx, ly), "empty rejected");
            t.IsFalse(parseTestStick("bogus", lx, ly), "text rejected");
            t.IsFalse(parseTestStick("0.5,", lx, ly), "half rejected"); });

        tc.Run("VT1: stick finger sliding over Select/L2 presses nothing", [](TestCase &t)
               {
            const Layout l = makeLayout(874.0f, 402.0f);
            const Button *sel = nullptr, *l2 = nullptr;
            for (const Button &b : l.buttons)
            {
                if (b.mask == kSelect) sel = &b;
                if (b.mask == kL2) l2 = &b;
            }
            PadState st;
            // Touch down on empty stick-zone ground (rest spot), slide down
            // over Select's centre, then up over L2's centre, same finger.
            TouchPoint p{7, l.stickRestX, l.stickRestY};
            PadFrame f = updatePad(st, l, &p, 1);
            t.IsTrue(f.stick.active, "stick anchors at touch-down");
            t.Equals(static_cast<uint32_t>(f.pressed), 0u, "touch-down presses nothing");
            p.x = sel->x; p.y = sel->y;
            f = updatePad(st, l, &p, 1);
            t.Equals(static_cast<uint32_t>(f.pressed), 0u, "slide over Select presses nothing");
            t.IsTrue(f.stick.active, "stick stays owned over Select");
            p.x = l2->x; p.y = l2->y;
            f = updatePad(st, l, &p, 1);
            t.Equals(static_cast<uint32_t>(f.pressed), 0u, "slide over L2 presses nothing");
            t.IsTrue(f.stick.active, "stick stays owned over L2");
            f = updatePad(st, l, nullptr, 0);
            t.IsFalse(f.stick.active, "lift releases the stick");
            t.Equals(static_cast<uint32_t>(f.pressed), 0u, "lift presses nothing"); });

        tc.Run("VT1: touch-down on Select/L1 presses, sticks until lift", [](TestCase &t)
               {
            const Layout l = makeLayout(874.0f, 402.0f);
            const Button *sel = nullptr, *l1 = nullptr;
            for (const Button &b : l.buttons)
            {
                if (b.mask == kSelect) sel = &b;
                if (b.mask == kL1) l1 = &b;
            }
            PadState st;
            TouchPoint p{3, sel->x, sel->y};
            PadFrame f = updatePad(st, l, &p, 1);
            t.Equals(static_cast<uint32_t>(f.pressed), static_cast<uint32_t>(kSelect), "Select touch-down presses");
            t.IsFalse(f.stick.active, "Select touch never owns the stick");
            // Slide off onto empty ground: Select stays (sticky until lift).
            p.x = l.stickRestX; p.y = l.stickRestY;
            f = updatePad(st, l, &p, 1);
            t.Equals(static_cast<uint32_t>(f.pressed), static_cast<uint32_t>(kSelect), "Select stays past slide-off");
            t.IsFalse(f.stick.active, "owner touch never becomes the stick");
            f = updatePad(st, l, nullptr, 0);
            t.Equals(static_cast<uint32_t>(f.pressed), 0u, "lift releases Select");
            // L1 the same.
            TouchPoint q{4, l1->x, l1->y};
            f = updatePad(st, l, &q, 1);
            t.Equals(static_cast<uint32_t>(f.pressed), static_cast<uint32_t>(kL1), "L1 touch-down presses");
            q.x = l.stickRestX; q.y = l.stickRestY;
            f = updatePad(st, l, &q, 1);
            t.Equals(static_cast<uint32_t>(f.pressed), static_cast<uint32_t>(kL1), "L1 stays past slide-off"); });

        tc.Run("VT1: stick anchors at the left edge and beside buttons", [](TestCase &t)
               {
            const Layout l = makeLayout(874.0f, 402.0f);
            const Button *sel = nullptr;
            for (const Button &b : l.buttons)
            {
                if (b.mask == kSelect) sel = &b;
            }
            // Left edge.
            {
                PadState st;
                TouchPoint p{11, 2.0f, l.stickRestY};
                PadFrame f = updatePad(st, l, &p, 1);
                t.IsTrue(f.stick.active, "left-edge touch owns the stick");
                t.IsTrue(f.stick.ax == 2.0f && f.stick.ay == l.stickRestY, "anchor is the touch-down point");
                t.Equals(static_cast<uint32_t>(f.pressed), 0u, "left-edge anchor presses nothing");
            }
            // Just outside Select's drawn disc (1.0x + 2pt): stick, not Select.
            {
                PadState st;
                TouchPoint p{12, sel->x + sel->r + 2.0f, sel->y};
                PadFrame f = updatePad(st, l, &p, 1);
                t.IsTrue(f.stick.active, "ring touch owns the stick");
                t.Equals(static_cast<uint32_t>(f.pressed), 0u, "ring touch presses nothing");
            }
            // Just inside Select's drawn disc (1.0x - 2pt): Select, not stick.
            {
                PadState st;
                TouchPoint p{13, sel->x + sel->r - 2.0f, sel->y};
                PadFrame f = updatePad(st, l, &p, 1);
                t.IsFalse(f.stick.active, "disc touch does not own the stick");
                t.Equals(static_cast<uint32_t>(f.pressed), static_cast<uint32_t>(kSelect), "disc touch presses Select");
            } });

        tc.Run("VT1: follow mode past stickR never goes dead", [](TestCase &t)
               {
            const Layout l = makeLayout(874.0f, 402.0f);
            PadState st;
            TouchPoint p{21, l.stickRestX, l.stickRestY};
            PadFrame f = updatePad(st, l, &p, 1);
            t.IsTrue(f.stick.active && f.stick.x == 0.0f && f.stick.y == 0.0f, "centred at touch-down");
            // Drag to 3x stickR straight down: full deflection, anchor followed.
            p.y = l.stickRestY + 3.0f * l.stickR;
            f = updatePad(st, l, &p, 1);
            t.IsTrue(f.stick.active, "still owned past stickR (no re-anchor reset)");
            t.IsTrue(std::fabs(f.stick.x) < 1e-5f && std::fabs(f.stick.y - 1.0f) < 1e-5f,
                     "full down past stickR");
            t.IsTrue(std::fabs(f.stick.ay - (p.y - l.stickR)) < 1e-3f, "anchor followed to one radius above the finger");
            t.Equals(static_cast<uint32_t>(f.pressed), 0u, "far drag presses nothing");
            // Keep dragging further: still full, anchor keeps sliding.
            p.y += l.stickR;
            const float ay0 = f.stick.ay;
            f = updatePad(st, l, &p, 1);
            t.IsTrue(std::fabs(f.stick.y - 1.0f) < 1e-5f, "still full after more drag");
            t.IsTrue(f.stick.ay > ay0, "anchor slid further");
            // Drag back to the anchor: recentres, still owned.
            p.y = f.stick.ay;
            p.x = f.stick.ax;
            f = updatePad(st, l, &p, 1);
            t.IsTrue(f.stick.active, "still owned at the anchor");
            t.IsTrue(f.stick.x == 0.0f && f.stick.y == 0.0f, "centred at the anchor"); });

        tc.Run("VT1: two fingers stick + L1, second stick finger inert", [](TestCase &t)
               {
            const Layout l = makeLayout(874.0f, 402.0f);
            const Button *l1 = nullptr;
            for (const Button &b : l.buttons)
            {
                if (b.mask == kL1) l1 = &b;
            }
            PadState st;
            TouchPoint ts[2] = {{31, l.stickRestX, l.stickRestY}, {32, l1->x, l1->y}};
            PadFrame f = updatePad(st, l, ts, 2);
            t.IsTrue(f.stick.active, "stick finger owns the stick");
            t.Equals(static_cast<uint32_t>(f.pressed), static_cast<uint32_t>(kL1), "L1 finger presses L1");
            // Stick finger deflects right; L1 holds.
            ts[0].x = l.stickRestX + 0.6f * l.stickR;
            f = updatePad(st, l, ts, 2);
            t.IsTrue(f.stick.x > 0.4f && f.stick.x < 0.8f, "stick deflects with L1 down");
            t.Equals(static_cast<uint32_t>(f.pressed), static_cast<uint32_t>(kL1), "L1 holds during stick drag");
            // L1 lifts; stick keeps driving.
            f = updatePad(st, l, ts, 1);
            t.Equals(static_cast<uint32_t>(f.pressed), 0u, "L1 lift releases L1");
            t.IsTrue(f.stick.active, "stick survives L1 lift");
            // A second stick-zone finger while the stick is owned: inert.
            TouchPoint ts2[2] = {ts[0], {33, 100.0f, 100.0f}};
            f = updatePad(st, l, ts2, 2);
            t.IsTrue(f.stick.active && f.stick.ax == st.stick.ax, "second finger does not steal the anchor");
            t.Equals(static_cast<uint32_t>(f.pressed), 0u, "second finger presses nothing"); });

        tc.Run("VT1: face slide-between and D-pad slide stay in cluster", [](TestCase &t)
               {
            const Layout l = makeLayout(874.0f, 402.0f);
            const Button *cro = nullptr, *cir = nullptr;
            for (const Button &b : l.buttons)
            {
                if (b.mask == kCross) cro = &b;
                if (b.mask == kCircle) cir = &b;
            }
            // Cross -> slide to circle: press follows within the face cluster.
            {
                PadState st;
                TouchPoint p{41, cro->x, cro->y};
                PadFrame f = updatePad(st, l, &p, 1);
                t.Equals(static_cast<uint32_t>(f.pressed), static_cast<uint32_t>(kCross), "cross touch-down");
                p.x = cir->x; p.y = cir->y;
                f = updatePad(st, l, &p, 1);
                t.Equals(static_cast<uint32_t>(f.pressed), static_cast<uint32_t>(kCircle), "slide to circle");
                // Slide off the cluster: nothing, but still owned (slides back on).
                p.x = l.stickZoneX + 10.0f; p.y = 10.0f;
                f = updatePad(st, l, &p, 1);
                t.Equals(static_cast<uint32_t>(f.pressed), 0u, "slide off the face cluster releases");
                p.x = cro->x; p.y = cro->y;
                f = updatePad(st, l, &p, 1);
                t.Equals(static_cast<uint32_t>(f.pressed), static_cast<uint32_t>(kCross), "slide back re-presses");
            }
            // D-pad up -> slide to right: directions follow the angle.
            {
                PadState st;
                TouchPoint p{42, l.dpadX, l.dpadY - l.dpadR * 0.6f};
                PadFrame f = updatePad(st, l, &p, 1);
                t.Equals(static_cast<uint32_t>(f.pressed), static_cast<uint32_t>(kUp), "dpad up touch-down");
                p.x = l.dpadX + l.dpadR * 0.6f; p.y = l.dpadY;
                f = updatePad(st, l, &p, 1);
                t.Equals(static_cast<uint32_t>(f.pressed), static_cast<uint32_t>(kRight), "dpad slide to right");
                // Slide to the dead zone: nothing.
                p.x = l.dpadX; p.y = l.dpadY;
                f = updatePad(st, l, &p, 1);
                t.Equals(static_cast<uint32_t>(f.pressed), 0u, "dpad dead zone");
            } });

        tc.Run("VT1: synthetic IDs parse and drive a slide path", [](TestCase &t)
               {
            const auto v = parseTestTouches("100:0.1:0.5:1:7,101:0.1:0.6:1:7,102:0.1:0.7:1:7,bad,200:0.8:0.5:5");
            t.Equals(v.size(), static_cast<size_t>(4), "3 slide items + 1 auto-id (bad skipped)");
            t.IsTrue(v[0].id == 7 && v[1].id == 7 && v[2].id == 7, "explicit IDs kept");
            t.IsTrue(v[3].id == -1, "4-field item is auto-id");
            TouchPoint ts[8];
            int n = activeTestTouchesWithIds(v, 101, 874.0f, 402.0f, ts, 0, 8);
            t.Equals(n, 1, "one touch at tick 101");
            t.IsTrue(ts[0].id == kTestTouchIdBase + 7, "synthetic ID base + 7");
            // The slide path through updatePad: one stick finger, no presses.
            const Layout l = makeLayout(874.0f, 402.0f);
            PadState st;
            for (uint64_t tick = 100; tick <= 102; ++tick)
            {
                n = activeTestTouchesWithIds(v, tick, 874.0f, 402.0f, ts, 0, 8);
                PadFrame f = updatePad(st, l, ts, n);
                t.IsTrue(f.stick.active, "slide owns the stick");
                t.Equals(static_cast<uint32_t>(f.pressed), 0u, "slide presses nothing");
            }
            t.IsTrue(st.stickId == kTestTouchIdBase + 7, "same owner across the slide");
            // Auto-id items overlapping stay distinct.
            const auto w = parseTestTouches("300:0.1:0.5:5,300:0.8:0.5:5");
            n = activeTestTouchesWithIds(w, 302, 874.0f, 402.0f, ts, 0, 8);
            t.Equals(n, 2, "two overlapping auto-id touches");
            t.IsTrue(ts[0].id != ts[1].id, "auto IDs distinct"); });

        tc.Run("VT3: Brad's two lost iPhone touches give L1/L2", [](TestCase &t)
               {
            // iPhone 16 Pro Max landscape: the layout the app builds.
            const Layout l = makeLayout(956.0f, 440.0f);
            const float band = shoulderBandBottom(l);
            t.IsTrue(std::fabs(band - (0.10f * 440.0f + 0.07f * 440.0f + 0.5f * 0.07f * 440.0f)) < 1e-3f,
                     "band = L1/L2 bottom edge + half a radius (90.2pt)");
            // Ride 5: 137.3 s at (0.071, 0.164) held 556 ms, 193.5 s at
            // (0.081, 0.096) held 624 ms. Both produced nothing (VT1: the
            // first became the stick, since no stick finger was down).
            {
                PadState st;
                TouchPoint p{51, 0.0711f * 956.0f, 0.1636f * 440.0f};
                PadFrame f = updatePad(st, l, &p, 1);
                t.Equals(static_cast<uint32_t>(f.pressed), static_cast<uint32_t>(kL2), "137.3 s touch gives L2");
                t.IsFalse(f.stick.active, "137.3 s touch never owns the stick");
            }
            {
                PadState st;
                TouchPoint p{52, 0.0812f * 956.0f, 0.0955f * 440.0f};
                PadFrame f = updatePad(st, l, &p, 1);
                t.Equals(static_cast<uint32_t>(f.pressed), static_cast<uint32_t>(kL2), "193.5 s touch gives L2");
                t.IsFalse(f.stick.active, "193.5 s touch never owns the stick");
            }
            // The nearby registered L1 at (0.152, 0.104) still gives L1.
            {
                PadState st;
                TouchPoint p{53, 0.152f * 956.0f, 0.104f * 440.0f};
                PadFrame f = updatePad(st, l, &p, 1);
                t.Equals(static_cast<uint32_t>(f.pressed), static_cast<uint32_t>(kL1), "registered L1 still L1");
            }
            // Knob off (PS2X_VPAD_SHOULDER_BAND=0): today exactly — both
            // lost touches anchor the stick and press nothing.
            {
                PadState st;
                TouchPoint p{54, 0.0711f * 956.0f, 0.1636f * 440.0f};
                PadFrame f = updatePad(st, l, &p, 1, false);
                t.IsTrue(f.stick.active, "knob off: 137.3 s touch owns the stick (today)");
                t.Equals(static_cast<uint32_t>(f.pressed), 0u, "knob off: 137.3 s touch presses nothing");
            }
            {
                PadState st;
                TouchPoint p{55, 0.0812f * 956.0f, 0.0955f * 440.0f};
                PadFrame f = updatePad(st, l, &p, 1, false);
                t.IsTrue(f.stick.active, "knob off: 193.5 s touch owns the stick (today)");
                t.Equals(static_cast<uint32_t>(f.pressed), 0u, "knob off: 193.5 s touch presses nothing");
            } });

        tc.Run("VT3: stick starts below the band; second finger below band inert", [](TestCase &t)
               {
            const Layout l = makeLayout(956.0f, 440.0f);
            const float band = shoulderBandBottom(l);
            PadState st;
            // Centre-left stick touch still gives the stick.
            TouchPoint s{61, l.stickRestX, l.stickRestY};
            PadFrame f = updatePad(st, l, &s, 1);
            t.IsTrue(f.stick.active, "centre-left touch owns the stick");
            t.Equals(static_cast<uint32_t>(f.pressed), 0u, "stick touch-down presses nothing");
            // A second finger in the stick zone, below the band (VT1 rule).
            TouchPoint ts[2] = {s, {62, 0.35f * 956.0f, 0.60f * 440.0f}};
            t.IsTrue(ts[1].y > band, "second finger is below the band");
            f = updatePad(st, l, ts, 2);
            t.IsTrue(f.stick.active && f.stick.ax == st.stick.ax, "second finger does not steal the anchor");
            t.Equals(static_cast<uint32_t>(f.pressed), 0u, "second finger presses nothing");
            // Just below the band, off every disc: still the stick.
            {
                PadState st2;
                TouchPoint p{63, 0.19f * 956.0f, band + 2.0f};
                PadFrame g = updatePad(st2, l, &p, 1);
                t.IsTrue(g.stick.active, "touch just below the band owns the stick");
                t.Equals(static_cast<uint32_t>(g.pressed), 0u, "touch just below the band presses nothing");
            }
            // Same x, just above the band: the nearest shoulder (L1 here).
            {
                PadState st3;
                TouchPoint p{64, 0.19f * 956.0f, band - 2.0f};
                PadFrame g = updatePad(st3, l, &p, 1);
                t.Equals(static_cast<uint32_t>(g.pressed), static_cast<uint32_t>(kL1), "touch just above the band gives L1");
                t.IsFalse(g.stick.active, "band touch never owns the stick");
            } });

        tc.Run("VT3: slide-on turns an inert touch into its Button", [](TestCase &t)
               {
            const Layout l = makeLayout(956.0f, 440.0f);
            const Button *l1 = nullptr, *r1 = nullptr;
            for (const Button &b : l.buttons)
            {
                if (b.mask == kL1) l1 = &b;
                if (b.mask == kR1) r1 = &b;
            }
            // Left: stick owned, second finger below the band slides onto L1.
            {
                PadState st;
                TouchPoint ts[2] = {{71, l.stickRestX, l.stickRestY}, {72, 0.35f * 956.0f, 0.60f * 440.0f}};
                PadFrame f = updatePad(st, l, ts, 2);
                t.Equals(static_cast<uint32_t>(f.pressed), 0u, "second finger starts inert");
                ts[1].x = l1->x; ts[1].y = l1->y;
                f = updatePad(st, l, ts, 2);
                t.Equals(static_cast<uint32_t>(f.pressed), static_cast<uint32_t>(kL1), "slide onto L1 presses L1");
                t.IsTrue(f.stick.active, "stick survives the slide-on");
                // Slide back off: Button stays until lift (VT1 sticky rule).
                ts[1].x = 0.35f * 956.0f; ts[1].y = 0.60f * 440.0f;
                f = updatePad(st, l, ts, 2);
                t.Equals(static_cast<uint32_t>(f.pressed), static_cast<uint32_t>(kL1), "slid-on L1 stays past slide-off");
                f = updatePad(st, l, ts, 1);
                t.Equals(static_cast<uint32_t>(f.pressed), 0u, "lift releases the slid-on L1");
                t.IsTrue(f.stick.active, "stick survives the L1 lift");
            }
            // Right: an inert touch sliding onto R1 gives R1 until lift.
            // (700, 100) is right-side ground: clear of the D-pad disc and
            // every 1.2x button disc on the 956x440 layout.
            {
                PadState st;
                TouchPoint p{73, 700.0f, 100.0f};
                PadFrame f = updatePad(st, l, &p, 1);
                t.Equals(static_cast<uint32_t>(f.pressed), 0u, "off-disc right touch is inert");
                p.x = r1->x; p.y = r1->y;
                f = updatePad(st, l, &p, 1);
                t.Equals(static_cast<uint32_t>(f.pressed), static_cast<uint32_t>(kR1), "slide onto R1 presses R1");
                f = updatePad(st, l, nullptr, 0);
                t.Equals(static_cast<uint32_t>(f.pressed), 0u, "lift releases the slid-on R1");
            }
            // Knob off: no slide-on (today exactly).
            {
                PadState st;
                TouchPoint p{74, 700.0f, 100.0f};
                PadFrame f = updatePad(st, l, &p, 1, false);
                p.x = r1->x; p.y = r1->y;
                f = updatePad(st, l, &p, 1, false);
                t.Equals(static_cast<uint32_t>(f.pressed), 0u, "knob off: slide onto R1 stays inert");
            } });

        tc.Run("qsr1: menu dot + save/load/race row hit test", [](TestCase &t)
               {
            const float ws[2] = {874.0f, 1180.0f}, hs[2] = {402.0f, 820.0f};
            for (int k = 0; k < 2; ++k)
            {
                const float w = ws[k], h = hs[k];
                const Qsr1MenuGeom g = qsr1MenuLayout(w, h);
                t.Equals(qsr1MenuHit(g, false, g.dotX, g.dotY), 1, "dot centre hits collapsed");
                t.Equals(qsr1MenuHit(g, false, g.rowX[0], g.rowY), 0, "row misses while collapsed");
                t.Equals(qsr1MenuHit(g, true, g.rowX[0], g.rowY), 2, "save");
                t.Equals(qsr1MenuHit(g, true, g.rowX[1], g.rowY), 3, "load");
                t.Equals(qsr1MenuHit(g, true, g.rowX[2], g.rowY), 4, "race");
                t.Equals(qsr1MenuHit(g, true, w * 0.5f, h * 0.5f), 0, "screen centre misses");
                // Menu zones press no guest button (hit-test runs first anyway;
                // this keeps the guest controls fully reachable around the menu).
                const Layout l = makeLayout(w, h);
                const float mx[4] = {g.dotX, g.rowX[0], g.rowX[1], g.rowX[2]};
                const float my[4] = {g.dotY, g.rowY, g.rowY, g.rowY};
                t.Equals(static_cast<uint32_t>(pressedMask(l, mx, my, 4)), 0u, "menu zones are guest-inert");
                // All menu discs inside the window.
                t.IsTrue(g.dotX - g.dotR >= 0.0f && g.dotX + g.dotR <= w && g.dotY - g.dotR >= 0.0f, "dot inside");
                for (int i = 0; i < 3; ++i)
                    t.IsTrue(g.rowX[i] - g.rowR >= 0.0f && g.rowX[i] + g.rowR <= w && g.rowY - g.rowR >= 0.0f,
                             "row inside");
            } });

        tc.Run("qsr1: slot dir sits next to the card, never under elfDirectory", [](TestCase &t)
               {
            t.Equals(qsr1SlotDirFor("/docs", "/run/mc0", "/pinned/cd"), "/docs/states", "ios docs wins");
            t.Equals(qsr1SlotDirFor(nullptr, "/run/mc0", "/pinned/cd"), "/run/states", "card parent");
            t.Equals(qsr1SlotDirFor("", "/run/mc0", "/pinned/cd"), "/run/states", "empty docs = unset");
            t.Equals(qsr1SlotDirFor(nullptr, "", "/pinned/cd"), "/pinned/cd/states", "no card: elf fallback");
            t.Equals(qsr1SlotDirFor(nullptr, "mc0", "/pinned/cd"), "/pinned/cd/states",
                     "parentless card: elf fallback");
        }); });
}
