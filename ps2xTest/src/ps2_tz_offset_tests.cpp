#include "MiniTest.h"

// LX1c: unit-test the OSD timezone source directly. Common.h contributes
// declarations plus inline/static helpers only, so including it gives this
// TU its own static copies; no runtime state is touched.
#include "../../ps2xRuntime/src/lib/Kernel/Syscalls/Common.h"

#include <cstdlib>
#include <ctime>

namespace
{
void clearTzEnv()
{
    ::unsetenv("PS2X_DETERMINISTIC");
}
} // namespace

void register_ps2_tz_offset_tests()
{
    MiniTest::Case("Ps2TzOffset", [](TestCase &tc)
    {
        tc.Run("deterministic pins the offset to 0", [](TestCase &t)
        {
            ::setenv("PS2X_DETERMINISTIC", "1", 1);
            t.Equals(getTimezoneOffsetMinutes(), 0, "pinned 0 under PS2X_DETERMINISTIC=1");
            clearTzEnv();
        });

        tc.Run("non-deterministic returns the DST-aware host offset", [](TestCase &t)
        {
            clearTzEnv();
            std::time_t now = std::time(nullptr);
            std::tm local{};
#if defined(_WIN32)
            // No tm_gmtoff on MSVC (legacy mktime path kept): sanity range only.
            const int got = getTimezoneOffsetMinutes();
            t.IsTrue(got >= -1200 && got <= 1200, "offset within plausible range");
#else
            t.IsTrue(localtime_r(&now, &local) != nullptr, "localtime_r works");
            const int expect = static_cast<int>(local.tm_gmtoff / 60);
            t.Equals(getTimezoneOffsetMinutes(), expect, "matches tm_gmtoff (DST-aware)");
#endif
            clearTzEnv();
        });
    });
}
