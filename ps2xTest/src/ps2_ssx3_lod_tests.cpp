#include "MiniTest.h"
#include "ps2_ssx3_lod.h"

// LOD1: PS2X_SSX3_LOD_SCALE parsing (off / accepted range / refusals).
void register_ps2_ssx3_lod_tests()
{
    MiniTest::Case("Ps2Ssx3Lod", [](TestCase &tc)
                   {
        tc.Run("unset, empty, 0, off and 1 mean off", [](TestCase &t)
               {
            using ps2_ssx3_lod::parseScale;
            t.IsTrue(parseScale(nullptr) == 0.0f, "unset");
            t.IsTrue(parseScale("") == 0.0f, "empty");
            t.IsTrue(parseScale("0") == 0.0f, "0");
            t.IsTrue(parseScale("off") == 0.0f, "off");
            t.IsTrue(parseScale("1") == 0.0f, "1 is stock"); });
        tc.Run("accepts 0.25..8", [](TestCase &t)
               {
            using ps2_ssx3_lod::parseScale;
            t.IsTrue(parseScale("1.5") == 1.5f, "1.5");
            t.IsTrue(parseScale("2") == 2.0f, "2");
            t.IsTrue(parseScale("0.25") == 0.25f, "0.25");
            t.IsTrue(parseScale("8") == 8.0f, "8"); });
        tc.Run("refuses out-of-range and non-numbers", [](TestCase &t)
               {
            using ps2_ssx3_lod::parseScale;
            t.IsTrue(parseScale("0.2") < 0.0f, "0.2");
            t.IsTrue(parseScale("9") < 0.0f, "9");
            t.IsTrue(parseScale("-2") < 0.0f, "-2");
            t.IsTrue(parseScale("2x") < 0.0f, "2x");
            t.IsTrue(parseScale("nan") < 0.0f, "nan");
            t.IsTrue(parseScale("inf") < 0.0f, "inf"); });
        tc.Run("TLS1: Tricky value wins on Tricky courses, global elsewhere", [](TestCase &t)
               {
            using ps2_ssx3_lod::selectScale;
            t.IsTrue(selectScale(0.0f, 0.0f, false) == 0.0f, "both off on stock");
            t.IsTrue(selectScale(0.0f, 0.0f, true) == 0.0f, "both off on tricky");
            t.IsTrue(selectScale(0.9f, 0.0f, false) == 0.9f, "global on stock");
            t.IsTrue(selectScale(0.9f, 0.0f, true) == 0.9f, "global covers tricky too");
            t.IsTrue(selectScale(0.0f, 0.8f, true) == 0.8f, "tricky-only applies on tricky");
            t.IsTrue(selectScale(0.0f, 0.8f, false) == 0.0f, "tricky-only leaves stock alone");
            t.IsTrue(selectScale(0.9f, 0.8f, true) == 0.8f, "tricky wins on tricky");
            t.IsTrue(selectScale(0.9f, 0.8f, false) == 0.9f, "global wins on stock"); }); });
}
