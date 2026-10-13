#include "MiniTest.h"
#include "ps2_fh1_full120.h"

#include <cstdint>
#include <cstring>
#include <vector>

// REV1 item 1 (RV27 F1 / RV30 #2,#3 / RV32 F3): the FLP3 stuntchg latch
// (g_stuntchgHigh/Idle/Delay) is host-only state. It must not survive an
// events flip or a savestate load (load side: ps2_fh1_savestate_tests), and
// a pending rise delay must not survive an aborted (quiet) rise.

namespace
{
    using namespace ps2_fh1;

    constexpr uint32_t kR = 0x20000u; // rider R; charges at +0x2a4/+0x2b0

    uint32_t fbits(float f)
    {
        uint32_t bits = 0u;
        std::memcpy(&bits, &f, 4);
        return bits;
    }

    // Player-1 rider chain for stuntchgPlayerR: [0x4a28a8]=g, [g+0x84]=a,
    // [a+0x0c]=b, [b+0x28]=R.
    void plantPlayerChain(uint8_t *ram)
    {
        wr32(ram, 0x4a28a8u, 0x30000u);
        wr32(ram, 0x30000u + 0x84u, 0x31000u);
        wr32(ram, 0x31000u + 0x0cu, 0x32000u);
        wr32(ram, 0x32000u + 0x28u, kR);
    }

    // One 0x1211f8 return: pre-call charges in g_post.saved, post-call RAM.
    void stuntReturn(uint8_t *ram, float savedSpin, float savedFlip, float spin, float flip)
    {
        g_post.obj = kR;
        g_post.saved[0] = fbits(savedSpin);
        g_post.saved[1] = fbits(savedFlip);
        wr32(ram, kR + 0x2a4u, fbits(spin));
        wr32(ram, kR + 0x2b0u, fbits(flip));
        stuntchgOnReturn(ram);
    }

    uint32_t charge(const uint8_t *ram, uint32_t off)
    {
        uint32_t bits = 0u;
        rd32(ram, kR + off, bits);
        return bits;
    }

    void dirtyLatch()
    {
        for (int i = 0; i < 2; ++i)
        {
            g_stuntchgHigh[i] = true;
            g_stuntchgIdle[i] = 3;
            g_stuntchgDelay[i] = 2;
        }
    }

    bool latchClear()
    {
        for (int i = 0; i < 2; ++i)
            if (g_stuntchgHigh[i] || g_stuntchgIdle[i] != 0 || g_stuntchgDelay[i] != 0)
                return false;
        return true;
    }
} // namespace

void register_ps2_fh1_stuntchg_tests()
{
    MiniTest::Case("Ps2Fh1Stuntchg", [](TestCase &tc)
    {
        tc.Run("REV1: quiet return clears a pending rise delay (aborted rise)", [](TestCase &t)
        {
            std::vector<uint8_t> ram(PS2_RAM_SIZE);
            plantPlayerChain(ram.data());
            dirtyLatch();
            g_stuntchgDelay[0] = 0;
            g_stuntchgDelay[1] = 0;
            // Post-reset fresh spin rise with the latch high: held at ~0,
            // delay armed at kStuntChgDelay[spin] - 1 = 1, latch consumed.
            stuntReturn(ram.data(), 0.0f, 0.0f, 0.02f, 0.0f);
            t.Equals(charge(ram.data(), 0x2a4u), fbits(0.0f), "fresh rise held at 0");
            t.Equals(g_stuntchgDelay[0], 1, "spin delay armed (2 halves total)");
            t.IsFalse(g_stuntchgHigh[0], "latch consumed on arm");
            // The player neutralises: a quiet (~0, unmoved) return must drop
            // the pending delay instead of billing it to the next rise.
            stuntReturn(ram.data(), 0.0f, 0.0f, 0.0f, 0.0f);
            t.Equals(g_stuntchgDelay[0], 0, "quiet return clears the pending delay");
            // The next rise is unrelated (no high latch): plain halving.
            stuntReturn(ram.data(), 0.0f, 0.0f, 0.02f, 0.0f);
            t.Equals(charge(ram.data(), 0x2a4u), fbits(0.01f), "next rise halved, not held");
            dirtyLatch();
        });

        tc.Run("REV1: moved returns still consume the delay one half at a time", [](TestCase &t)
        {
            std::vector<uint8_t> ram(PS2_RAM_SIZE);
            plantPlayerChain(ram.data());
            for (int i = 0; i < 2; ++i)
            {
                g_stuntchgHigh[i] = false;
                g_stuntchgIdle[i] = 0;
                g_stuntchgDelay[i] = 0;
            }
            g_stuntchgDelay[1] = 3; // flip delay remainder
            stuntReturn(ram.data(), 0.0f, 0.0f, 0.0f, 0.02f);
            t.Equals(charge(ram.data(), 0x2b0u), fbits(0.0f), "delayed flip rise held at 0");
            t.Equals(g_stuntchgDelay[1], 2, "one delayed half consumed");
            dirtyLatch();
        });

        tc.Run("REV1: guestFlip resets the latch in both directions", [](TestCase &t)
        {
            std::vector<uint8_t> ram(PS2_RAM_SIZE);
            constexpr uint32_t manager = 0x1000u;
            wr32(ram.data(), kMgrPtr, manager);
            wr32(ram.data(), manager + 0x10u, 60u);
            wr32(ram.data(), manager + 0x14u, kSixtieth);
            wr32(ram.data(), manager + 0x24u, 0x3f800000u);
            dirtyLatch();
            guestFlip(ram.data(), 1u, true);
            t.IsTrue(latchClear(), "enter flip clears high/idle/delay");
            uint32_t rate = 0u;
            rd32(ram.data(), manager + 0x10u, rate);
            t.Equals(rate, 120u, "manager converted on enter");
            dirtyLatch();
            guestFlip(ram.data(), 2u, false);
            t.IsTrue(latchClear(), "exit flip clears high/idle/delay");
            rd32(ram.data(), manager + 0x10u, rate);
            t.Equals(rate, 60u, "manager restored on exit");
        });
    });
}
