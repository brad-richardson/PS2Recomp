#include "MiniTest.h"
#include "ps2_fh1_full120.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace
{
float bitsToFloat(uint32_t b)
{
    float f = 0.0f;
    std::memcpy(&f, &b, 4);
    return f;
}

uint32_t floatToBits(float f)
{
    uint32_t b = 0u;
    std::memcpy(&b, &f, 4);
    return b;
}
} // namespace

void register_ps2_fh1_fh28_tests()
{
    using namespace ps2_fh1;
    MiniTest::Case("Ps2Fh1Fh28", [](TestCase &tc)
    {
        tc.Run("pool overrides only fire for their group", [](TestCase &t)
        {
            const uint64_t all28 =
                static_cast<uint64_t>(kFixJcam | kFixPose | kFixPid | kFixC2Cap | kFixSpawn);
            // Each override fires only with its own bit.
            t.IsTrue(fh28PoolOverride(0x49f628u, static_cast<uint64_t>(kFixSpawn)), "spawn word");
            t.IsFalse(fh28PoolOverride(0x49f628u, kFixAll), "spawn word needs spawn");
            t.IsTrue(fh28PoolOverride(0x49c5fcu, static_cast<uint64_t>(kFixC2Cap)), "c2cap word");
            t.IsFalse(fh28PoolOverride(0x49c5fcu, kFixAll), "c2cap word needs c2cap");
            t.IsFalse(fh28PoolOverride(0x49c624u, all28), "pid span stays patched");
            t.IsFalse(fh28PoolOverride(0x49c628u, all28), "pid base stays patched");
            // Nothing else is overridden, even with every FH28 bit on.
            t.IsFalse(fh28PoolOverride(0x49c650u, all28), "unpatched jump clock untouched");
            t.IsFalse(fh28PoolOverride(0x49bb04u, all28), "pose word untouched");
            t.IsFalse(fh28PoolOverride(0x49be9cu, all28), "rider word untouched");
            t.IsFalse(fh28PoolOverride(0x49f628u, 0u), "empty mask overrides nothing");
        });

        tc.Run("jcam retention correction composes to d per pair", [](TestCase &t)
        {
            // FXT1 T1b: d = 0.9649, offsets decay by exactly d per stock tick.
            // The guest multiplies by d; the hook sees the product and divides
            // by sqrt(d), so each corrected update retains sqrt(d) and the
            // pair recovers d.
            const float d = 0.9649f, pre = -9.582f;
            const uint32_t postBits = floatToBits(pre * d); // the guest multiply
            const float got = bitsToFloat(jcamRetainCorrect(postBits, floatToBits(d)));
            t.IsTrue(std::fabs(got - pre * std::sqrt(d)) <= 1e-5f, "one update retains sqrt(d)");
            const float step = got / pre;
            t.IsTrue(std::fabs(step * step - d) <= 1e-6f, "pair composes to d");
        });

        tc.Run("jcam retention correction guards bad d", [](TestCase &t)
        {
            const uint32_t offBits = floatToBits(-9.582f);
            t.Equals(jcamRetainCorrect(offBits, 0u), offBits, "d = 0 leaves the offset");
            t.Equals(jcamRetainCorrect(offBits, 0x80000000u), offBits, "d = -0 leaves the offset");
            t.Equals(jcamRetainCorrect(offBits, 0xbf800000u), offBits, "negative d leaves the offset");
            t.Equals(jcamRetainCorrect(offBits, 0x7fc00000u), offBits, "NaN d leaves the offset");
            t.Equals(jcamRetainCorrect(offBits, 0x7f800000u), offBits, "Inf d leaves the offset");
            t.Equals(jcamRetainCorrect(offBits, 0x40000000u), offBits, "d = 2 leaves the offset");
            t.Equals(jcamRetainCorrect(0u, floatToBits(0.9649f)), 0u, "zero offset stays zero");
        });

        tc.Run("pose halving is an exact power-of-two scale", [](TestCase &t)
        {
            // The pool table halves 1/30 (0x3d088889) to 1/60 (kSixtieth);
            // assert the assumption the table bakes in.
            t.Equals(static_cast<uint32_t>(0x3d088889u) >> 0u, 0x3d088889u, "stock bits");
            t.Equals(kSixtieth, 0x3c888889u, "replacement bits");
            t.IsTrue(bitsToFloat(kSixtieth) == bitsToFloat(0x3d088889u) * 0.5f, "exact half");
        });

        tc.Run("hook interest lists the FH28 targets only when on", [](TestCase &t)
        {
            HookConfig c;
            c.mode = Mode::Events;
            c.main = kFixAll;
            c.guestActive = true;
            t.IsFalse(hookTableHit(buildHookInterest(c), 0x000001u, 0x1635f8u), "jcam out of all");
            t.IsFalse(hookTableHit(buildHookInterest(c), 0x000001u, 0x162998u), "c2cap out of all");
            c.main |= static_cast<uint64_t>(kFixJcam | kFixPose | kFixPid | kFixC2Cap | kFixSpawn);
            const HookInterest hi = buildHookInterest(c);
            t.IsTrue(hookTableHit(hi, 0x000001u, 0x1635f8u), "jcam tgt hits");
            t.IsFalse(hookTableHit(hi, 0x000001u, 0x162c78u), "pid installs no hook (inert)");
            t.IsTrue(hookTableHit(hi, 0x000001u, 0x162998u), "c2cap tgt hits");
            t.IsTrue(hookTableHit(hi, 0x3171b4u, 0x000001u), "parity src hits");
            c.guestActive = false;
            const HookInterest idle = buildHookInterest(c);
            t.IsFalse(hookTableHit(idle, 0x000001u, 0x1635f8u), "jcam out while inactive");
            t.IsFalse(hookTableHit(idle, 0x000001u, 0x162998u), "c2cap out while inactive");
        });
    });
}
