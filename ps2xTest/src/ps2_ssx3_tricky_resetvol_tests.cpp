#include "MiniTest.h"
#include "ps2_ssx3_tricky_resetvol.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

void register_ps2_ssx3_tricky_resetvol_tests()
{
    MiniTest::Case("Ps2Ssx3TrickyResetvol", [](TestCase &tc)
    {
        tc.Run("rotated donor collision triangle and course gate", [](TestCase &t)
        {
            using namespace ps2_ssx3_tricky_resetvol;
            std::vector<Course> courses;
            std::string error;
            const char *data = "course ALOHA 14 0 0 0 1000 2000 3000 90 2\n"
                               "tri ALOHA 1273 15 20 30 15 30 30 15 20 40\n"
                               "tri ALOHA 1273 15 30 30 15 30 40 15 20 40\n";
            t.IsTrue(parseTableText(data, courses, error), "sidecar parses");
            t.IsTrue(courses.size() == 1u && courses[0].triangles.size() == 2u, "two triangles");
            // Donor (10,25,35) -> (20,25,35) crosses x=15 after a 90-degree course yaw.
            const double before[3] = {950, 2020, 3070}, after[3] = {950, 2040, 3070};
            double first[3], second[3];
            toDonor(courses[0], before, first);
            toDonor(courses[0], after, second);
            t.IsTrue(firstCrossing(courses[0], first, second) == 1273, "inverse placement crosses");
            const double away[3] = {950, 2020, 3110};
            toDonor(courses[0], away, first);
            t.IsTrue(firstCrossing(courses[0], first, second) == -1, "outside triangle excludes");
            std::vector<uint8_t> ram(0x2000u);
            const uint32_t r = 0x1000u;
            const uint32_t slot = 0u, mode = 7u;
            const float xyz[3] = {950, 2020, 3070};
            std::memcpy(ram.data() + r + 0x86cu, &slot, 4);
            std::memcpy(ram.data() + r + 0x880u, &mode, 4);
            std::memcpy(ram.data() + r + 0x110u, xyz, sizeof xyz);
            State state;
            t.IsTrue(poll(state, courses, ram.data(), ram.size(), false, 14, "ALOHA", r) == -1,
                     "stock gate");
            t.IsTrue(poll(state, courses, ram.data(), ram.size(), true, 14, "MEGA", r) == -1,
                     "archive gate");
            t.IsTrue(poll(state, courses, ram.data(), ram.size(), true, 14, "ALOHA", r) == -1,
                     "first sample arms sweep");
            const float crossing[3] = {950, 2040, 3070};
            std::memcpy(ram.data() + r + 0x110u, crossing, sizeof crossing);
            t.IsTrue(poll(state, courses, ram.data(), ram.size(), true, 14, "ALOHA", r) == 1273,
                     "crossing resets");
            t.IsTrue(poll(state, courses, ram.data(), ram.size(), true, 14, "ALOHA", r) == -1,
                     "held position does not repeat");
            std::memcpy(ram.data() + r + 0x110u, xyz, sizeof xyz);
            t.IsTrue(poll(state, courses, ram.data(), ram.size(), true, 14, "ALOHA", r) == 1273,
                     "reverse crossing resets");
            // A crossing while the course reset (control state 9) runs does not re-enter it.
            const uint32_t ctl = 0x1100u, resetting = 9u, riding = 0u;
            std::memcpy(ram.data() + r + 0x77cu, &ctl, 4);
            std::memcpy(ram.data() + ctl + 0xde4u, &resetting, 4);
            std::memcpy(ram.data() + r + 0x110u, crossing, sizeof crossing);
            t.IsTrue(poll(state, courses, ram.data(), ram.size(), true, 14, "ALOHA", r) == -1,
                     "crossing during the reset is ignored");
            std::memcpy(ram.data() + ctl + 0xde4u, &riding, 4);
            std::memcpy(ram.data() + r + 0x110u, xyz, sizeof xyz);
            t.IsTrue(poll(state, courses, ram.data(), ram.size(), true, 14, "ALOHA", r) == 1273,
                     "crossing after the reset resets");
            t.IsTrue(!parseTableText("tri ALOHA 1 0 0 0 1 1 1 2 2 2\n", courses, error),
                     "triangle without placement rejected");
        });
    });
}
