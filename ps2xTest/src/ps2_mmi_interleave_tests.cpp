#include "MiniTest.h"
#include "ps2_runtime_macros.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

namespace
{
    using Lanes = std::array<uint16_t, 8>;

    __m128i load(const Lanes &lanes)
    {
        __m128i value;
        std::memcpy(&value, lanes.data(), sizeof(value));
        return value;
    }

    Lanes lanesOf(__m128i value)
    {
        Lanes lanes{};
        std::memcpy(lanes.data(), &value, sizeof(value));
        return lanes;
    }

    void checkLanes(TestCase &t, const Lanes &actual, const Lanes &expected)
    {
        for (size_t lane = 0; lane < expected.size(); ++lane)
            t.Equals(actual[lane], expected[lane], "halfword lane " + std::to_string(lane));
    }
}

void register_ps2_mmi_interleave_tests()
{
    MiniTest::Case("Ps2MmiInterleave", [](TestCase &tc)
    {
        const __m128i rs = load({0x1100, 0x1101, 0x1102, 0x1103, 0x1104, 0x1105, 0x1106, 0x1107});
        const __m128i rt = load({0x2200, 0x2201, 0x2202, 0x2203, 0x2204, 0x2205, 0x2206, 0x2207});
        const __m128i zero = _mm_setzero_si128();

        tc.Run("PINTEH interleaves even halfwords from Rt and Rs", [=](TestCase &t)
        {
            checkLanes(t, lanesOf(PS2_PINTEH(rs, rt)),
                       {0x2200, 0x1100, 0x2202, 0x1102, 0x2204, 0x1104, 0x2206, 0x1106});
        });
        tc.Run("PINTH interleaves low Rt and high Rs halfwords", [=](TestCase &t)
        {
            checkLanes(t, lanesOf(PS2_PINTH(rs, rt)),
                       {0x2200, 0x1104, 0x2201, 0x1105, 0x2202, 0x1106, 0x2203, 0x1107});
        });
        tc.Run("PINTEH with zero Rs", [=](TestCase &t)
        {
            checkLanes(t, lanesOf(PS2_PINTEH(zero, rt)),
                       {0x2200, 0, 0x2202, 0, 0x2204, 0, 0x2206, 0});
        });
        tc.Run("PINTH with zero Rs", [=](TestCase &t)
        {
            checkLanes(t, lanesOf(PS2_PINTH(zero, rt)),
                       {0x2200, 0, 0x2201, 0, 0x2202, 0, 0x2203, 0});
        });
        tc.Run("PINTEH aliases both sources and destination", [=](TestCase &t)
        {
            __m128i alias = rs;
            alias = PS2_PINTEH(alias, alias);
            checkLanes(t, lanesOf(alias), {0x1100, 0x1100, 0x1102, 0x1102, 0x1104, 0x1104, 0x1106, 0x1106});
        });
        tc.Run("PINTH aliases both sources and destination", [=](TestCase &t)
        {
            __m128i alias = rs;
            alias = PS2_PINTH(alias, alias);
            checkLanes(t, lanesOf(alias), {0x1100, 0x1104, 0x1101, 0x1105, 0x1102, 0x1106, 0x1103, 0x1107});
        });
    });
}
