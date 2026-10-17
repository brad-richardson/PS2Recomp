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
    });
}
