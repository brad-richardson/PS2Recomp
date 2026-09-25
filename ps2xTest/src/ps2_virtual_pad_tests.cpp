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

        tc.Run("env switch and pad backend union", [](TestCase &t)
               {
            t.IsTrue(enabledFromEnv(nullptr), "unset = on");
            t.IsTrue(enabledFromEnv("1"), "1 = on");
            t.IsFalse(enabledFromEnv("0"), "0 = off");
            PSPadBackend backend;
            uint8_t data[32]{};
            liveMask().store(kCross | kStart);
            t.IsTrue(backend.readState(0, 0, data, sizeof(data)), "readState ok");
            const uint16_t btns = static_cast<uint16_t>(data[2] | (data[3] << 8));
            liveMask().store(0u);
            t.Equals(static_cast<uint32_t>(btns & (kCross | kStart)), 0u, "cross+start active-low");
            t.Equals(static_cast<uint32_t>(btns | kCross | kStart), 0xFFFFu, "nothing else pressed"); });

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
            t.IsFalse(parseTestStick("0.5,", lx, ly), "half rejected"); }); });
}
