#include "MiniTest.h"
#include "ps2_pad_latch.h"
#include "ps2_virtual_pad.h"
#include "runtime/ps2_pad.h"

#include <cstdint>
#include <cstdlib>

// IN2: the press latch between host input (render thread, host frame rate)
// and the guest's pad read (game thread, guest speed). Samples and reads
// are driven directly: the fake read clock.
void register_ps2_pad_latch_tests()
{
    using namespace ps2x::padlatch;
    using namespace ps2x::vpad;
    MiniTest::Case("Ps2PadLatch", [](TestCase &tc)
                   {
        tc.Run("a tap between two reads is seen exactly once", [](TestCase &t)
               {
            Latch l;
            t.Equals(static_cast<uint32_t>(l.consumeRead()), 0u, "idle reads up");
            l.noteSample(kCross);
            l.noteSample(0u);
            t.Equals(static_cast<uint32_t>(l.consumeRead()), static_cast<uint32_t>(kCross), "tap shows down");
            t.Equals(static_cast<uint32_t>(l.consumeRead()), 0u, "then up");
            t.Equals(static_cast<uint32_t>(l.consumeRead()), 0u, "stays up"); });

        tc.Run("a held button is continuous", [](TestCase &t)
               {
            Latch l;
            l.noteSample(kCross);
            t.Equals(static_cast<uint32_t>(l.consumeRead()), static_cast<uint32_t>(kCross), "read 1 down");
            t.Equals(static_cast<uint32_t>(l.consumeRead()), static_cast<uint32_t>(kCross), "read 2 down");
            t.Equals(static_cast<uint32_t>(l.consumeRead()), static_cast<uint32_t>(kCross), "read 3 down");
            l.noteSample(0u);
            t.Equals(static_cast<uint32_t>(l.consumeRead()), 0u, "release reads up"); });

        tc.Run("two quick taps between reads coalesce to one press", [](TestCase &t)
               {
            // Documented: sticky bits, not a queue. Replaying every tap
            // would stack stale presses at low guest speed; spam-tapping a
            // menu yields one press, not N.
            Latch l;
            l.noteSample(kCross);
            l.noteSample(0u);
            l.noteSample(kCross);
            l.noteSample(0u);
            t.Equals(static_cast<uint32_t>(l.consumeRead()), static_cast<uint32_t>(kCross), "one down");
            t.Equals(static_cast<uint32_t>(l.consumeRead()), 0u, "one up");
            t.Equals(static_cast<uint32_t>(l.consumeRead()), 0u, "no second press"); });

        tc.Run("a release reads as up before a new press shows", [](TestCase &t)
               {
            Latch l;
            t.Equals(static_cast<uint32_t>(l.consumeRead()), 0u, "idle reads up");
            l.noteSample(kCross);
            t.Equals(static_cast<uint32_t>(l.consumeRead()), static_cast<uint32_t>(kCross), "press shows");
            l.noteSample(0u);
            l.noteSample(kCross); // release + repress between reads
            t.Equals(static_cast<uint32_t>(l.consumeRead()), 0u, "release gets its up-read");
            t.Equals(static_cast<uint32_t>(l.consumeRead()), static_cast<uint32_t>(kCross), "repress shows next");
            l.noteSample(0u);
            t.Equals(static_cast<uint32_t>(l.consumeRead()), 0u, "release reads up"); });

        tc.Run("buttons latch independently", [](TestCase &t)
               {
            Latch l;
            l.noteSample(kCircle); // hold circle, tap cross
            t.Equals(static_cast<uint32_t>(l.consumeRead()), static_cast<uint32_t>(kCircle), "circle down");
            l.noteSample(static_cast<uint16_t>(kCircle | kCross));
            l.noteSample(kCircle);
            t.Equals(static_cast<uint32_t>(l.consumeRead()), static_cast<uint32_t>(kCircle | kCross), "tap seen alongside the hold");
            t.Equals(static_cast<uint32_t>(l.consumeRead()), static_cast<uint32_t>(kCircle), "hold continues, tap over");
            l.noteSample(0u);
            t.Equals(static_cast<uint32_t>(l.consumeRead()), 0u, "all up"); });

        tc.Run("no samples reads up; a button already held shows at once", [](TestCase &t)
               {
            Latch l;
            t.Equals(static_cast<uint32_t>(l.consumeRead()), 0u, "no samples: up");
            l.noteSample(kStart); // held since before the first sample
            t.Equals(static_cast<uint32_t>(l.consumeRead()), static_cast<uint32_t>(kStart), "held shows down"); });

        tc.Run("latch env switch defaults on", [](TestCase &t)
               {
            t.IsTrue(ps2x::padlatch::enabledFromEnv(nullptr), "unset = on");
            t.IsTrue(ps2x::padlatch::enabledFromEnv("1"), "1 = on");
            t.IsFalse(ps2x::padlatch::enabledFromEnv("0"), "0 = off"); });

        tc.Run("dev test taps parse and window on the wall clock", [](TestCase &t)
               {
            const auto v = parseTestTap("1000:cross:40,bad,2000:r3:60,1:2:3:0,3000:bogus:10,4000:up:0");
            t.Equals(v.size(), static_cast<size_t>(2), "2 valid items (bad text, bad button, hold 0 skipped)");
            t.Equals(static_cast<uint32_t>(activeTestTap(v, 999)), 0u, "before the tap");
            t.Equals(static_cast<uint32_t>(activeTestTap(v, 1000)), static_cast<uint32_t>(kCross), "tap starts inclusive");
            t.Equals(static_cast<uint32_t>(activeTestTap(v, 1039)), static_cast<uint32_t>(kCross), "tap holds");
            t.Equals(static_cast<uint32_t>(activeTestTap(v, 1040)), 0u, "tap ends exclusive");
            t.Equals(static_cast<uint32_t>(activeTestTap(v, 2000)), static_cast<uint32_t>(kR3), "r3 tap");
            t.Equals(static_cast<uint32_t>(activeTestTap(v, 2060)), 0u, "r3 tap over");
            const auto w = parseTestTap("100:cross:50,120:circle:50");
            t.Equals(static_cast<uint32_t>(activeTestTap(w, 130)), static_cast<uint32_t>(kCross | kCircle), "overlapping taps union"); });

        tc.Run("pad backend takes the latched mask (latch on)", [](TestCase &t)
               {
            unsetenv("PS2X_PAD_LATCH");
            sharedLatch().resetForTest();
            PSPadBackend backend;
            uint8_t data[32]{};
            auto buttons = [&]()
            {
                t.IsTrue(backend.readState(0, 0, data, sizeof(data)), "readState ok");
                return static_cast<uint16_t>(data[2] | (data[3] << 8));
            };
            sharedLatch().publish(static_cast<uint16_t>(kCross | kStart));
            t.Equals(static_cast<uint32_t>(buttons() & (kCross | kStart)), 0u, "cross+start active-low");
            t.Equals(static_cast<uint32_t>(buttons() | kCross | kStart), 0xFFFFu, "held: nothing else pressed");
            sharedLatch().publish(0u);
            t.Equals(static_cast<uint32_t>(buttons()), 0xFFFFu, "release reads up");
            // A tap published between two reads is seen exactly once.
            sharedLatch().publish(kCross);
            sharedLatch().publish(0u);
            t.Equals(static_cast<uint32_t>(~buttons() & 0xFFFFu), static_cast<uint32_t>(kCross), "tap seen once");
            t.Equals(static_cast<uint32_t>(buttons()), 0xFFFFu, "then up");
            sharedLatch().resetForTest(); });

        tc.Run("port 1 follows live without consuming the latch", [](TestCase &t)
               {
            unsetenv("PS2X_PAD_LATCH");
            sharedLatch().resetForTest();
            PSPadBackend backend;
            uint8_t data[32]{};
            sharedLatch().publish(kCross);
            t.IsTrue(backend.readState(1, 0, data, sizeof(data)), "port 1 readState ok");
            t.Equals(static_cast<uint32_t>(~static_cast<uint16_t>(data[2] | (data[3] << 8)) & 0xFFFFu),
                     static_cast<uint32_t>(kCross), "port 1 sees the held button");
            t.IsTrue(backend.readState(0, 0, data, sizeof(data)), "port 0 readState ok");
            t.Equals(static_cast<uint32_t>(~static_cast<uint16_t>(data[2] | (data[3] << 8)) & 0xFFFFu),
                     static_cast<uint32_t>(kCross), "port 0 still consumes the press");
            sharedLatch().resetForTest(); });

        tc.Run("pad backend keeps direct sampling with PS2X_PAD_LATCH=0", [](TestCase &t)
               {
            setenv("PS2X_PAD_LATCH", "0", 1);
            sharedLatch().resetForTest();
            PSPadBackend backend;
            uint8_t data[32]{};
            liveMask().store(static_cast<uint16_t>(kCross | kStart));
            t.IsTrue(backend.readState(0, 0, data, sizeof(data)), "readState ok");
            const uint16_t btns = static_cast<uint16_t>(data[2] | (data[3] << 8));
            liveMask().store(0u);
            unsetenv("PS2X_PAD_LATCH");
            t.Equals(static_cast<uint32_t>(btns & (kCross | kStart)), 0u, "cross+start active-low");
            t.Equals(static_cast<uint32_t>(btns | kCross | kStart), 0xFFFFu, "nothing else pressed");
            sharedLatch().resetForTest(); }); });
}
