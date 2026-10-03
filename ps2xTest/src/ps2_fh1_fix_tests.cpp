#include "MiniTest.h"
#include "ps2_fh1_fix.h"

#include <cstdint>
#include <string>

void register_ps2_fh1_fix_tests()
{
    using namespace ps2_fh1;
    MiniTest::Case("Ps2Fh1Fix", [](TestCase &tc)
    {
        tc.Run("all includes stick2 and clocksign (FH26, Brad 10-02) and both masks", [](TestCase &t)
        {
            const FixMasks f = parseFix("all");
            t.IsTrue(f.ok, "parses");
            t.Equals(f.main, kFixAll, "main = kFixAll");
            t.Equals(f.fh12, kFix12All, "fh12 = kFix12All");
            t.IsTrue((f.main & kFixStick2) != 0u, "stick2 in all");
            t.IsTrue((f.main & kFixClockSign) != 0u, "clocksign in all");
            t.IsTrue((f.main & kFixLift) == 0u, "lift stays opt-in");
            t.IsTrue((f.main & kFixRamp) == 0u, "ramp is internal");
        });

        tc.Run("minus items opt out, left to right", [](TestCase &t)
        {
            const FixMasks a = parseFix("all,-stick2");
            t.IsTrue(a.ok, "parses");
            t.Equals(a.main, kFixAll & ~static_cast<uint64_t>(kFixStick2), "all minus stick2");
            t.Equals(a.fh12, kFix12All, "fh12 untouched");
            const FixMasks b = parseFix("all,-stick2,-clocksign");
            t.Equals(b.main, kFixAll & ~static_cast<uint64_t>(kFixStick2 | kFixClockSign), "pre-FH26 all");
            const FixMasks c = parseFix("all,-particles");
            t.Equals(c.fh12, kFix12All & ~kFix12Particles, "fh12 opt-out");
            t.Equals(c.main, kFixAll, "main untouched");
            const FixMasks d = parseFix("-stick2,all");
            t.Equals(d.main, kFixAll, "order matters: a later all re-adds");
            const FixMasks e = parseFix("all,-all,stick");
            t.Equals(e.main, static_cast<uint64_t>(kFixStick), "-all clears");
            t.Equals(e.fh12, 0u, "-all clears fh12");
        });

        tc.Run("single items, lift opt-in, empty items", [](TestCase &t)
        {
            t.Equals(parseFix("stick2").main, static_cast<uint64_t>(kFixStick2), "stick2 alone");
            t.Equals(parseFix("all,lift").main, kFixAll | kFixLift, "all plus lift");
            t.Equals(parseFix("spin,clock").fh12, static_cast<uint32_t>(kFix12Spin), "fh12 item");
            t.Equals(parseFix("spin,clock").main, static_cast<uint64_t>(kFixClock), "main item");
            t.IsTrue(parseFix("all,").ok, "trailing comma");
        });

        tc.Run("FH27 bonusflip and loops2 are opt-in", [](TestCase &t)
        {
            t.IsTrue((kFixAll & kFixBonusFlip) == 0u, "bonusflip not in all");
            t.IsTrue((kFix12All & kFix12Loops2) == 0u, "loops2 not in all");
            const FixMasks f = parseFix("all,bonusflip,loops2");
            t.IsTrue(f.ok, "parses");
            t.Equals(f.main, kFixAll | kFixBonusFlip, "main adds bonusflip");
            t.Equals(f.fh12, kFix12All | kFix12Loops2, "fh12 adds loops2");
        });

        tc.Run("unknown items refuse", [](TestCase &t)
        {
            const FixMasks f = parseFix("all,stik2");
            t.IsTrue(!f.ok, "refused");
            t.Equals(f.bad, std::string("stik2"), "names the item");
            t.IsTrue(!parseFix("-nope").ok, "unknown opt-out refused");
        });
    });
}
