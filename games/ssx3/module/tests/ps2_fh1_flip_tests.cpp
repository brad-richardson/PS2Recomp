#include "MiniTest.h"
#include "ps2_fh1_full120.h"

#include <cstdint>
#include <vector>

// full120 slice 2: guestFlip's enter path converts the manager word table,
// which lives in the module now — so this Run lives with the module. (The
// FLP3 latch Runs stay shared: the latch itself is runtime state.)

namespace
{
    using namespace ps2_fh1;

    // Twin of the stuntchg latch helpers (ps2_fh1_stuntchg_tests.cpp): each
    // TU keeps its own copy; both touch the same runtime latch state.
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

void register_ps2_fh1_flip_tests()
{
    MiniTest::Case("Ps2Fh1Flip", [](TestCase &tc)
    {
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

        tc.Run("online-stock-2: lsGateWhy mirrors the 0x237950 lockstep gate", [](TestCase &t)
        {
            std::vector<uint8_t> ram(PS2_RAM_SIZE);
            constexpr uint32_t ng = 0x2000u, in = 0x3000u;
            wr32(ram.data(), kNetGamePtr, ng);
            wr32(ram.data(), ng + 0x70u, 13u);
            wr32(ram.data(), in + 0x10u, 2u);
            for (uint32_t i = 0u; i < 2u; ++i)
            {
                wr32(ram.data(), in + 0x14u + 0x18u * i + 4u, 5u);
                wr32(ram.data(), in + 0x14u + 0x18u * i + 8u, 7u);
            }
            t.Equals(lsGateWhy(ram.data(), in), 1u, "state 13 closes");
            wr32(ram.data(), ng + 0x70u, 14u);
            t.Equals(lsGateWhy(ram.data(), in), 0u, "state 14, both queues hold input: open");
            wr32(ram.data(), ng + 0x3f8u, 1u);
            t.Equals(lsGateWhy(ram.data(), in), 2u, "pause hold closes");
            wr32(ram.data(), ng + 0x3f8u, 0u);
            wr32(ram.data(), ng + 0x3fcu, 3u);
            t.Equals(lsGateWhy(ram.data(), in), 3u, "countdown hold closes");
            wr32(ram.data(), ng + 0x3fcu, 0xffffffffu);
            t.Equals(lsGateWhy(ram.data(), in), 0u, "negative countdown is open");
            wr32(ram.data(), in + 0x14u + 0x18u + 8u, 5u);
            t.Equals(lsGateWhy(ram.data(), in), 5u, "player 1 queue empty closes");
            wr32(ram.data(), in + 0x10u, 0u);
            t.Equals(lsGateWhy(ram.data(), in), 0u, "no players: open");
        });

        tc.Run("online-stock-2: input-delay trim and paused-local neutral input", [](TestCase &t)
        {
            std::vector<uint8_t> ram(PS2_RAM_SIZE);
            constexpr uint32_t in = 0x3000u, base0 = 0x4000u, base1 = 0x8000u;
            wr32(ram.data(), in, 12u);       // [in+0] delay target
            wr32(ram.data(), in + 4u, 16u);  // ring size
            wr32(ram.data(), in + 8u, 4u);   // N
            wr32(ram.data(), in + 0x10u, 2u);
            const uint32_t recs[2] = {in + 0x14u, in + 0x2cu};
            wr32(ram.data(), recs[0], base0);
            wr32(ram.data(), recs[0] + 4u, 3u);
            wr32(ram.data(), recs[0] + 8u, 5u);
            wr32(ram.data(), recs[0] + 0xcu, 0x1234u); // local pad
            wr32(ram.data(), recs[1], base1);
            wr32(ram.data(), recs[1] + 4u, 3u);
            wr32(ram.data(), recs[1] + 8u, 5u);
            wr32(ram.data(), recs[1] + 0xcu, 0u);      // remote
            for (uint32_t k = 3u; k < 5u; ++k)
            {
                wr32(ram.data(), base0 + k * 100u, 24u);
                wr32(ram.data(), base0 + k * 100u + 8u, 0x3f800000u);
                wr32(ram.data(), base1 + k * 100u + 8u, 0x3f800000u);
            }
            stock120TrimDelay(ram.data(), in);
            uint32_t v = 0u;
            rd32(ram.data(), in, v);
            t.Equals(v, 4u, "delay trimmed to N");
            stock120NeutralLocal(ram.data(), in);
            rd32(ram.data(), base0 + 3u * 100u + 8u, v);
            t.Equals(v, 0u, "local unread input zeroed");
            rd32(ram.data(), base0 + 3u * 100u, v);
            t.Equals(v, 24u, "count word kept");
            rd32(ram.data(), base1 + 3u * 100u + 8u, v);
            t.Equals(v, 0x3f800000u, "remote input untouched");
        });
    });
}
