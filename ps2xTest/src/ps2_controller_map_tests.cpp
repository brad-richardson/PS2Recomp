#include "MiniTest.h"
#include "ps2_controller_map.h"
#include "ps2_pad_latch.h"

#include <cstdint>

// ctrl-harden-1: controller -> pad-port assignment, the iOS overlay-hide
// name rule, and per-port latch isolation (Brad 10-17: one controller drove
// both riders because port 2 peeked port 1's live mask). The connect /
// disconnect / reconnect sequences below drive the pure assignment plus a
// Latch pair the way the render thread (publish) and the pad backend
// (consume per port) use the two shared latches.
void register_ps2_controller_map_tests()
{
    using namespace ps2x::ctrlmap;
    using ps2x::padlatch::Latch;
    MiniTest::Case("Ps2ControllerMap", [](TestCase &tc)
                   {
        tc.Run("no controller: both ports silent", [](TestCase &t)
               {
            const bool avail[4] = {false, false, false, false};
            t.Equals(pad1Slot(avail, 4), -1, "pad1 none");
            t.Equals(pad2Slot(avail, 4), -1, "pad2 none"); });

        tc.Run("one controller drives pad 1, pad 2 silent", [](TestCase &t)
               {
            const bool avail[4] = {true, false, false, false};
            t.Equals(pad1Slot(avail, 4), 0, "pad1 slot 0");
            t.Equals(pad2Slot(avail, 4), -1, "pad2 silent"); });

        tc.Run("one controller off slot 0 still takes pad 1", [](TestCase &t)
               {
            const bool avail[4] = {false, false, true, false};
            t.Equals(pad1Slot(avail, 4), 2, "pad1 slot 2");
            t.Equals(pad2Slot(avail, 4), -1, "pad2 silent"); });

        tc.Run("two controllers: first is pad 1, second is pad 2", [](TestCase &t)
               {
            const bool avail[4] = {true, false, true, false};
            t.Equals(pad1Slot(avail, 4), 0, "pad1 first");
            t.Equals(pad2Slot(avail, 4), 2, "pad2 second"); });

        tc.Run("disconnect of pad 1 promotes the survivor", [](TestCase &t)
               {
            // Slots 0+2 connected, then slot 0 (pad 1) disconnects: slot 2
            // promotes to pad 1 immediately and pad 2 goes silent.
            const bool before[4] = {true, false, true, false};
            t.Equals(pad1Slot(before, 4), 0, "before: pad1 slot 0");
            t.Equals(pad2Slot(before, 4), 2, "before: pad2 slot 2");
            const bool after[4] = {false, false, true, false};
            t.Equals(pad1Slot(after, 4), 2, "after: survivor is pad 1");
            t.Equals(pad2Slot(after, 4), -1, "after: pad2 silent"); });

        tc.Run("reconnect takes pad 1 again immediately", [](TestCase &t)
               {
            const bool gone[4] = {false, false, false, false};
            t.Equals(pad1Slot(gone, 4), -1, "gone: pad1 none");
            // Same controller back on another slot: pad 1 at once.
            const bool back[4] = {false, true, false, false};
            t.Equals(pad1Slot(back, 4), 1, "back: pad1 slot 1");
            t.Equals(pad2Slot(back, 4), -1, "back: pad2 silent"); });

        tc.Run("null availability reads silent", [](TestCase &t)
               {
            t.Equals(pad1Slot(nullptr, 4), -1, "pad1 null");
            t.Equals(pad2Slot(nullptr, 4), -1, "pad2 null"); });

        tc.Run("simulator fake never hides the overlay alone", [](TestCase &t)
               {
            t.Equals(hidesOverlay(nullptr) ? 1u : 0u, 0u, "null");
            t.Equals(hidesOverlay("") ? 1u : 0u, 0u, "empty");
            t.Equals(hidesOverlay("Gamepad") ? 1u : 0u, 0u, "sim fake");
            t.Equals(hidesOverlay("Xbox Wireless Controller") ? 1u : 0u, 1u, "xbox");
            t.Equals(hidesOverlay("DualSense Wireless Controller") ? 1u : 0u, 1u, "ps5");
            t.Equals(hidesOverlay("Backbone One") ? 1u : 0u, 1u, "mfi"); });

        tc.Run("ports stay isolated: pad 1 press never reaches pad 2", [](TestCase &t)
               {
            // Brad 10-17: with one controller, pad 2 must get no input.
            Latch l0, l1;
            l0.noteSample(0x4000u); // cross on pad 1's controller
            l1.noteSample(0u);      // nothing on pad 2
            t.Equals(static_cast<uint32_t>(l0.consumeRead()), 0x4000u, "pad1 shows cross");
            t.Equals(static_cast<uint32_t>(l1.consumeRead()), 0u, "pad2 silent");
            l0.noteSample(0u);
            t.Equals(static_cast<uint32_t>(l0.consumeRead()), 0u, "pad1 release");
            t.Equals(static_cast<uint32_t>(l1.consumeRead()), 0u, "pad2 still silent"); });

        tc.Run("two controllers drive only their own rider", [](TestCase &t)
               {
            Latch l0, l1;
            l0.noteSample(0x4000u); // P1 cross
            l1.noteSample(0x2000u); // P2 circle
            t.Equals(static_cast<uint32_t>(l0.consumeRead()), 0x4000u, "pad1 cross only");
            t.Equals(static_cast<uint32_t>(l1.consumeRead()), 0x2000u, "pad2 circle only"); });

        tc.Run("disconnect releases cleanly with no stuck buttons", [](TestCase &t)
               {
            // Held buttons at disconnect: the render thread publishes zero
            // (sampler reads no slot), and each port's next guest read shows
            // the release, then idles — nothing sticks.
            Latch l0, l1;
            l0.noteSample(0x4000u);
            l1.noteSample(0x2000u);
            t.Equals(static_cast<uint32_t>(l0.consumeRead()), 0x4000u, "p1 down");
            t.Equals(static_cast<uint32_t>(l1.consumeRead()), 0x2000u, "p2 down");
            l0.noteSample(0u); // disconnect: physical gone
            l1.noteSample(0u);
            t.Equals(static_cast<uint32_t>(l0.consumeRead()), 0u, "p1 released");
            t.Equals(static_cast<uint32_t>(l1.consumeRead()), 0u, "p2 released");
            t.Equals(static_cast<uint32_t>(l0.consumeRead()), 0u, "p1 idles");
            t.Equals(static_cast<uint32_t>(l1.consumeRead()), 0u, "p2 idles"); });

        tc.Run("a tap between reads shows exactly once per port", [](TestCase &t)
               {
            Latch l1;
            l1.noteSample(0x1000u);
            l1.noteSample(0u);
            t.Equals(static_cast<uint32_t>(l1.consumeRead()), 0x1000u, "tap shows down");
            t.Equals(static_cast<uint32_t>(l1.consumeRead()), 0u, "then up");
            t.Equals(static_cast<uint32_t>(l1.consumeRead()), 0u, "stays up"); }); });
}
