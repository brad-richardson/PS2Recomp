#include "MiniTest.h"
#include "ps2_fh1_full120.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

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
        tc.Run("FH33 steering post-hook is classified only while active", [](TestCase &t)
        {
            HookConfig c; c.mode = Mode::Events; c.main = kFixGround2; c.guestActive = true;
            t.IsTrue(hookTableHit(buildHookInterest(c), 0u, 0x113e80u), "active steering hook");
            c.guestActive = false;
            t.IsFalse(hookTableHit(buildHookInterest(c), 0u, 0x113e80u), "inactive steering untouched");
            c.guestActive = true; c.main = kFixAll;
            t.IsFalse(hookTableHit(buildHookInterest(c), 0u, 0x113e80u), "default steering untouched");
        });

        tc.Run("FH30 envfilt is opt-in and its table follows active events", [](TestCase &t)
        {
            t.IsTrue((kFixAll & kFixEnvFilt) == 0u, "not in all");
            const FixMasks f = parseFix("all,envfilt");
            t.IsTrue(f.ok, "parses");
            t.Equals(f.main, kFixAll | kFixEnvFilt, "adds only envfilt");
            t.Equals(parseFix("all,envfilt,-envfilt").main, kFixAll, "opt-out");
            HookConfig c;
            c.mode = Mode::Events;
            c.main = kFixEnvFilt;
            c.guestActive = true;
            t.IsTrue(hookTableHit(buildHookInterest(c), kEnvFiltSite, kEnvFiltUpdate), "active hit");
            c.guestActive = false;
            t.IsFalse(hookTableHit(buildHookInterest(c), kEnvFiltSite, kEnvFiltUpdate), "inactive miss");
        });

        tc.Run("FH30 squared-gain half input composes to stock retention", [](TestCase &t)
        {
            for (const float f : {0.00001f, 0.01f, 0.08f, 0.3f, 0.8f, 0.999f})
            {
                const double h = envFiltHalfInput(f);
                const double halfRetention = 1.0 - h * h;
                t.IsTrue(std::fabs(halfRetention * halfRetention - (1.0 - double(f)*f)) < 1e-7,
                         "pair retains stock gap");
            }
            // Boundary/copy branches and invalid inputs retain their exact bits.
            for (const uint32_t b : {0u, 0x80000000u, 0xbf800000u, 0x3f800000u,
                                     0x40000000u, 0x7f800000u, 0x7fc00000u})
                t.Equals(floatToBits(envFiltHalfInput(bitsToFloat(b))), b, "preserves non-response input");
        });

        tc.Run("FH30 pre-hook converts only the audited caller", [](TestCase &t)
        {
            R5900Context ctx{};
            ctx.f[12] = 0.08f;
            envFiltPreHook(&ctx, 0x2c0990u, kEnvFiltUpdate);
            t.Equals(floatToBits(ctx.f[12]), floatToBits(0.08f), "other interpolation untouched");
            envFiltPreHook(&ctx, kEnvFiltSite, 0x123456u);
            t.Equals(floatToBits(ctx.f[12]), floatToBits(0.08f), "other target untouched");
            envFiltPreHook(&ctx, kEnvFiltSite, kEnvFiltUpdate);
            t.IsTrue(std::fabs(ctx.f[12] - 0.056613925f) < 1e-7f, "live input converted");
            envFiltPreHook(nullptr, kEnvFiltSite, kEnvFiltUpdate);
        });

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

        tc.Run("pid hold: zero gains in, history and idx rolled back out", [](TestCase &t)
        {
            // FH29: the second update of a pair runs 0x162c78 with zero gains
            // (out = O[p]) and its slot + idx write is undone afterwards.
            std::vector<uint8_t> ram(PS2_RAM_SIZE, 0u);
            const uint32_t shot = 0x15812d0u;
            const float kp = 0.0113945f, ki = 0.0f, kd = 0.00127851f;
            wr32(ram.data(), shot + kPidIdx, 2u);
            for (uint32_t s = 0; s < 5; ++s)
                for (int h = 0; h < 3; ++h)
                    wr32(ram.data(), shot + kPidHist[h] + s * 4u, floatToBits(0.1f * (h + 1) + 0.01f * s));
            wr32(ram.data(), shot + kPidGains[0], floatToBits(kp));
            wr32(ram.data(), shot + kPidGains[1], floatToBits(ki));
            wr32(ram.data(), shot + kPidGains[2], floatToBits(kd));
            const std::vector<uint8_t> before = ram;
            uint32_t saved[7] = {};
            t.IsTrue(pidHoldArm(ram.data(), shot, saved), "arms");
            uint32_t g = 1u;
            for (uint32_t off : kPidGains)
            {
                rd32(ram.data(), shot + off, g);
                t.Equals(g, 0u, "gain zeroed for the held call");
            }
            // The held call: out = O[p] + 0, writes E/O/R at idx 2, idx -> 3.
            uint32_t prevOut = 0u;
            rd32(ram.data(), shot + kPidHist[1] + 1u * 4u, prevOut);
            wr32(ram.data(), shot + kPidHist[0] + 8u, floatToBits(0.77f));
            wr32(ram.data(), shot + kPidHist[1] + 8u, prevOut);
            wr32(ram.data(), shot + kPidHist[2] + 8u, floatToBits(0.77f - bitsToFloat(prevOut)));
            wr32(ram.data(), shot + kPidIdx, 3u);
            pidHoldRelease(ram.data(), shot, saved, true);
            t.IsTrue(ram == before, "returned call: state exactly as before (gains, slot, idx)");
        });

        tc.Run("pid hold: a suspended call gets its gains back and keeps its writes", [](TestCase &t)
        {
            std::vector<uint8_t> ram(PS2_RAM_SIZE, 0u);
            const uint32_t shot = 0x15812d0u;
            wr32(ram.data(), shot + kPidIdx, 4u);
            wr32(ram.data(), shot + kPidGains[0], floatToBits(0.0113945f));
            wr32(ram.data(), shot + kPidGains[2], floatToBits(0.00127851f));
            uint32_t saved[7] = {};
            t.IsTrue(pidHoldArm(ram.data(), shot, saved), "arms");
            wr32(ram.data(), shot + kPidIdx, 0u); // the resumed call commits (4 -> 0)
            pidHoldRelease(ram.data(), shot, saved, false);
            uint32_t v = 0u;
            rd32(ram.data(), shot + kPidGains[0], v);
            t.Equals(v, floatToBits(0.0113945f), "Kp back");
            rd32(ram.data(), shot + kPidGains[2], v);
            t.Equals(v, floatToBits(0.00127851f), "Kd back");
            rd32(ram.data(), shot + kPidIdx, v);
            t.Equals(v, 0u, "suspended call's idx write kept");
        });

        tc.Run("pid hold refuses a bad ring index", [](TestCase &t)
        {
            std::vector<uint8_t> ram(PS2_RAM_SIZE, 0u);
            const uint32_t shot = 0x15812d0u;
            wr32(ram.data(), shot + kPidIdx, 5u);
            wr32(ram.data(), shot + kPidGains[0], floatToBits(0.5f));
            uint32_t saved[7] = {};
            t.IsFalse(pidHoldArm(ram.data(), shot, saved), "idx 5 refused");
            uint32_t v = 0u;
            rd32(ram.data(), shot + kPidGains[0], v);
            t.Equals(v, floatToBits(0.5f), "gains untouched on refusal");
            t.IsFalse(pidHoldArm(ram.data(), PS2_RAM_SIZE, saved), "out-of-RAM shot refused");
        });

        tc.Run("pid: two 120 updates with the hold equal one stock recurrence step", [](TestCase &t)
        {
            // Model of the guest recurrence (0x162df0-0x162efc) on the ring.
            struct Pid
            {
                float E[5], O[5], R[5], kp, ki, kd;
                uint32_t idx;
                float step(float e)
                {
                    const uint32_t p = (idx + 4u) % 5u;
                    float sum = 0.0f;
                    for (float r : R)
                        sum += r;
                    const float out = O[p] + kp * (e - O[p]) + ki * sum + kd * (E[p] - e);
                    E[idx] = e;
                    O[idx] = out;
                    R[idx] = e - out;
                    idx = (idx + 1u) % 5u;
                    return out;
                }
            };
            Pid stock{{0.1f, 0.2f, 0.3f, 0.4f, 0.5f}, {0.01f, 0.02f, 0.03f, 0.04f, 0.05f}, {}, 0.0113945f, 0.001f, 0.00127851f, 3u};
            Pid held = stock;
            const float e = 0.51f;
            const float outStock = stock.step(e);
            const float outCommit = held.step(e); // first update of the pair
            // Second update: zero gains, then roll back slot + idx.
            Pid snap = held;
            held.kp = held.ki = held.kd = 0.0f;
            const float outHeld = held.step(0.53f);
            t.IsTrue(outHeld == outCommit, "held call reproduces the committed output");
            held = snap; // release: gains, slot and idx back
            t.IsTrue(outCommit == outStock, "commit equals the stock step");
            t.IsTrue(std::memcmp(&held, &stock, sizeof(Pid)) == 0, "pair leaves the stock state");
        });

        tc.Run("hook interest lists the FH28 targets only when on", [](TestCase &t)
        {
            HookConfig c;
            c.mode = Mode::Events;
            c.main = kFixAll;
            c.guestActive = true;
            t.IsFalse(hookTableHit(buildHookInterest(c), 0x000001u, 0x1635f8u), "jcam out of all");
            t.IsFalse(hookTableHit(buildHookInterest(c), 0x000001u, 0x162998u), "c2cap out of all");
            t.IsFalse(hookTableHit(buildHookInterest(c), 0x000001u, 0x162c78u), "pid out of all");
            c.main |= static_cast<uint64_t>(kFixJcam | kFixPose | kFixPid | kFixC2Cap | kFixSpawn);
            const HookInterest hi = buildHookInterest(c);
            t.IsTrue(hookTableHit(hi, 0x000001u, 0x1635f8u), "jcam tgt hits");
            t.IsTrue(hookTableHit(hi, 0x000001u, 0x162c78u), "pid tgt hits (FH29)");
            t.IsTrue(hookTableHit(hi, 0x000001u, 0x162998u), "c2cap tgt hits");
            t.IsTrue(hookTableHit(hi, 0x3171b4u, 0x000001u), "parity src hits");
            c.guestActive = false;
            const HookInterest idle = buildHookInterest(c);
            t.IsFalse(hookTableHit(idle, 0x000001u, 0x1635f8u), "jcam out while inactive");
            t.IsFalse(hookTableHit(idle, 0x000001u, 0x162998u), "c2cap out while inactive");
            t.IsFalse(hookTableHit(idle, 0x000001u, 0x162c78u), "pid out while inactive");
        });
    });
}
