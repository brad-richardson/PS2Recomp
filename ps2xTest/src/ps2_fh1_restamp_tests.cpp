#include "MiniTest.h"
#include "ps2_fh1_restamp.h"

#include <cstdint>
#include <string>

namespace
{
    namespace R = ps2_fh1_restamp;

    // Active-mode stamp after the guest restamps at update u (clockPreHook shifts it +601).
    constexpr uint32_t activeRestamp(uint32_t u) { return u + 601u; }
    // Active updates until the next fire.
    constexpr int32_t activeRemaining(uint32_t upd, uint32_t stamp) { return R::stockRemaining(upd, stamp); }
}

void register_ps2_fh1_restamp_tests()
{
    MiniTest::Case("Ps2Fh1Restamp", [](TestCase &tc)
    {
        tc.Run("entry at stock elapsed 0/300/600 doubles the remaining time", [](TestCase &t)
        {
            const uint32_t u = 100000u;
            for (uint32_t e : {0u, 300u, 600u})
            {
                const uint32_t s = R::enter(u, u - e);
                t.Equals(activeRemaining(u, s), static_cast<int32_t>(2u * (601u - e)), "active remaining = 2 x stock remaining");
            }
        });

        tc.Run("entry then exit with no active update is exact (both rules)", [](TestCase &t)
        {
            const uint32_t u = 100000u;
            for (uint32_t e : {0u, 1u, 300u, 600u, 601u})
            {
                const uint32_t s = R::enter(u, u - e);
                t.Equals(R::stockRemaining(u, R::exitSigned(u, s)), static_cast<int32_t>(601u - e), "signed round trip");
            }
            // The clamped rule is exact only when the active stamp is not in the future (e = 601 at entry).
            t.Equals(R::stockRemaining(u, R::exitClamped(u, R::enter(u, u - 601u))), 0, "clamped round trip at e=601");
        });

        tc.Run("FCR1 example: enter at 0, exit after 120 active updates -> 541 stock", [](TestCase &t)
        {
            const uint32_t u = 5000u;
            const uint32_t s = R::enter(u, u); // stamp = u + 601
            t.Equals(static_cast<int32_t>((u + 120u) - s), -481, "signed elapsed at exit");
            t.Equals(R::stockRemaining(u + 120u, R::exitSigned(u + 120u, s)), 541, "clocksign keeps 541");
            t.Equals(R::stockRemaining(u + 120u, R::exitClamped(u + 120u, s)), 300, "FH5 clamp leaves 300 (241 early)");
        });

        tc.Run("exit on both parities rounds the half tick up", [](TestCase &t)
        {
            const uint32_t u = 5000u;
            const uint32_t s = R::enter(u, u - 100u); // 501 stock remaining -> 1002 active
            for (uint32_t k : {0u, 1u, 2u, 119u, 120u, 121u, 1001u})
            {
                const int32_t ra = 1002 - static_cast<int32_t>(k);
                t.Equals(activeRemaining(u + k, s), ra, "active remaining");
                t.Equals(R::stockRemaining(u + k, R::exitSigned(u + k, s)), (ra + 1) / 2, "ceil(R/2) stock");
            }
        });

        tc.Run("restamp while active, then exit", [](TestCase &t)
        {
            const uint32_t fire = 70000u;
            const uint32_t s = activeRestamp(fire); // 1202 active updates to the next fire
            for (uint32_t k : {0u, 1u, 480u, 601u, 602u, 1201u})
            {
                const int32_t ra = 1202 - static_cast<int32_t>(k);
                t.Equals(R::stockRemaining(fire + k, R::exitSigned(fire + k, s)), (ra + 1) / 2, "signed exit");
            }
            // Clamped: anything inside the first 601 active updates keeps only 300.
            t.Equals(R::stockRemaining(fire + 1u, R::exitClamped(fire + 1u, s)), 300, "clamp bug after a restamp");
        });

        tc.Run("update counter wrap", [](TestCase &t)
        {
            const uint32_t u = 0xffffff00u; // wraps 256 updates later
            const uint32_t s = R::enter(u, u - 10u);
            t.Equals(activeRemaining(u, s), 1182, "entry across wrap");
            const uint32_t x = u + 400u; // = 0x90 after the wrap
            t.Equals(R::stockRemaining(x, R::exitSigned(x, s)), (1182 - 400 + 1) / 2, "exit after the wrap");
            const uint32_t s2 = R::enter(5u, 0xfffffff0u); // stamp before the wrap, elapsed 21
            t.Equals(activeRemaining(5u, s2), 2 * (601 - 21), "elapsed spans the wrap");
        });

        tc.Run("bounds: overdue and far-future stamps", [](TestCase &t)
        {
            const uint32_t u = 9000u;
            t.Equals(R::stockRemaining(u, R::exitSigned(u, u - 700u)), 0, "overdue exits due now");
            t.Equals(R::stockRemaining(u, R::exitSigned(u, u + 900u)), 601, "future beyond one period caps at 601");
            t.Equals(activeRemaining(u, R::enter(u, u + 50u)), 1202, "future stamp at entry counts as 0 elapsed");
        });
    });
}
