#include "MiniTest.h"
#include "ps2_virtual_pad.h"
#include "runtime/ps2_pad.h"

#include <cstdint>

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
            t.Equals(static_cast<uint32_t>(btns | kCross | kStart), 0xFFFFu, "nothing else pressed"); }); });
}
