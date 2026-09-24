#include "MiniTest.h"
#include "ps2_present_fallback.h"
#include "ps2_present_geometry.h"

#include <cmath>

// I26: the no-guest-frame fallback is black unless PS2X_FALLBACK_MAGENTA is set.
void register_ps2_present_fallback_tests()
{
    MiniTest::Case("Ps2PresentFallback", [](TestCase &tc)
                   {
        tc.Run("black by default, magenta only when the dev env is set", [](TestCase &t)
               {
            auto isBlack = [](ps2x::FallbackRgba c) { return c.r == 0u && c.g == 0u && c.b == 0u && c.a == 255u; };
            auto isMagenta = [](ps2x::FallbackRgba c) { return c.r == 255u && c.g == 0u && c.b == 255u && c.a == 255u; };
            t.IsTrue(isBlack(ps2x::fallbackFrameColor(nullptr)), "unset -> black");
            t.IsTrue(isBlack(ps2x::fallbackFrameColor("")), "empty -> black");
            t.IsTrue(isBlack(ps2x::fallbackFrameColor("0")), "0 -> black");
            t.IsTrue(isMagenta(ps2x::fallbackFrameColor("1")), "1 -> magenta"); }); });
}

// I26 (G46 §4): 4:3 display aspect by default, bilinear at non-integer scale.
void register_ps2_present_geometry_tests()
{
    using namespace ps2x::present;
    MiniTest::Case("Ps2PresentGeometry", [](TestCase &tc)
                   {
        tc.Run("4:3 by default, native keeps the pixel aspect", [](TestCase &t)
               {
            t.IsTrue(aspectFromEnv(nullptr) == Aspect::FourThree, "unset -> 4:3");
            t.IsTrue(aspectFromEnv("native") == Aspect::Native, "native");
            const Rect r = presentRect(2868.0f, 1320.0f, 512.0f, 448.0f, Aspect::FourThree);
            t.IsTrue(std::fabs(r.h - 1320.0f) < 0.01f && std::fabs(r.w - 1760.0f) < 0.01f, "iPhone landscape: 1760x1320");
            t.IsTrue(std::fabs(r.x - 554.0f) < 0.01f && std::fabs(r.y) < 0.01f, "centred");
            const Rect n = presentRect(2868.0f, 1320.0f, 512.0f, 448.0f, Aspect::Native);
            t.IsTrue(std::fabs(n.w / n.h - 512.0f / 448.0f) < 1e-4f, "native = 8:7");
            const Rect p = presentRect(800.0f, 1200.0f, 512.0f, 448.0f, Aspect::FourThree);
            t.IsTrue(std::fabs(p.w - 800.0f) < 0.01f && std::fabs(p.h - 600.0f) < 0.01f, "portrait: width-bound"); });

        tc.Run("bilinear unless both scales are whole; env forces", [](TestCase &t)
               {
            t.IsTrue(filterFromEnv(nullptr) == Filter::Auto, "unset -> auto");
            t.IsTrue(useBilinear(Filter::Auto, 2.946f, 2.946f), "x2.946 -> bilinear");
            t.IsFalse(useBilinear(Filter::Auto, 2.0f, 2.0f), "x2 -> point");
            t.IsTrue(useBilinear(Filter::Auto, 2.0f, 2.5f), "one axis fractional -> bilinear");
            t.IsFalse(useBilinear(filterFromEnv("point"), 2.946f, 2.946f), "forced point");
            t.IsTrue(useBilinear(filterFromEnv("bilinear"), 2.0f, 2.0f), "forced bilinear"); }); });
}
