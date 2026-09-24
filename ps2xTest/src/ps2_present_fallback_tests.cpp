#include "MiniTest.h"
#include "ps2_present_fallback.h"

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

