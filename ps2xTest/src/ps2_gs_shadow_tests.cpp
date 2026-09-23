// G44 shadow adapter tests: env gating (default off), pure window helpers,
// and the GifArbiter shadow tap (order + path preserved). No Vulkan is ever
// initialized here: the test build links the shadow stub (no backend), and
// the arbiter tap is exercised directly.
#include "MiniTest.h"
#include "runtime/gs/ps2_gs_shadow.h"
#include "runtime/gs/ps2_gif_arbiter.h"

#include <cstdint>
#include <cstdlib>
#include <vector>

namespace
{

void clearShadowEnv()
{
    ::unsetenv("PS2X_GS_SHADOW");
    ::unsetenv("PS2X_GS_SHADOW_DIR");
    ::unsetenv("PS2X_GS_SHADOW_FROM");
    ::unsetenv("PS2X_GS_SHADOW_TO");
    ps2x_gs_shadow::resetForTest();
}

} // namespace

void register_ps2_gs_shadow_tests()
{
    MiniTest::Case("Ps2GsShadow", [](TestCase &tc)
    {
        tc.Run("disabled by default: entry points are no-ops", [](TestCase &t)
        {
            clearShadowEnv();
            t.IsTrue(!ps2x_gs_shadow::enabled(), "flag unset => shadow disabled");
            t.IsTrue(!ps2x_gs_shadow::routeGifViaArbiter(), "flag unset => fast paths stay on");
            const uint8_t pkt[16] = {0};
            ps2x_gs_shadow::onGifPacket(3u, pkt, sizeof(pkt));
            ps2x_gs_shadow::onWriteRegister(0x4Cu, 0x1234u);
            ps2x_gs_shadow::onReset();
            ps2x_gs_shadow::onPresentFrame(42u, pkt, 2u, 2u, nullptr);
            t.Equals(ps2x_gs_shadow::gifPacketsFed(), 0ull, "no packets fed while disabled");
            t.Equals(ps2x_gs_shadow::regWritesFed(), 0ull, "no reg writes fed while disabled");
            t.Equals(ps2x_gs_shadow::presentsSeen(), 0ull, "no presents seen while disabled");
            t.Equals(ps2x_gs_shadow::pairsWrote(), 0ull, "no pairs while disabled");
            clearShadowEnv();
        });

        tc.Run("env=parallel honors backend presence (two-key design)", [](TestCase &t)
        {
            clearShadowEnv();
            ::setenv("PS2X_GS_SHADOW", "parallel", 1);
#ifdef PS2X_HAS_PARALLEL_SHADOW
            // Backend linked (G44 build): env enables the gate. enabled() and
            // routeGifViaArbiter() never initialize Vulkan (lazy on first
            // feed), so this is safe in the suite.
            t.IsTrue(ps2x_gs_shadow::hasBackend(), "G44 build links the backend");
            t.IsTrue(ps2x_gs_shadow::enabled(), "backend + env => enabled");
            t.IsTrue(ps2x_gs_shadow::routeGifViaArbiter(), "enabled => route via arbiter");
#else
            // Stub build: the gate must stay off despite the env request.
            t.IsTrue(!ps2x_gs_shadow::hasBackend(), "stub build has no backend");
            t.IsTrue(!ps2x_gs_shadow::enabled(), "no backend => disabled despite env");
            t.IsTrue(!ps2x_gs_shadow::routeGifViaArbiter(), "no backend => fast paths stay on");
#endif
            clearShadowEnv();
            t.IsTrue(!ps2x_gs_shadow::enabled(), "env cleared => disabled again");
        });

        tc.Run("parseModeParallel matches exactly", [](TestCase &t)
        {
            t.IsTrue(ps2x_gs_shadow::parseModeParallel("parallel"), "exact match enables");
            t.IsTrue(!ps2x_gs_shadow::parseModeParallel(nullptr), "null does not enable");
            t.IsTrue(!ps2x_gs_shadow::parseModeParallel(""), "empty does not enable");
            t.IsTrue(!ps2x_gs_shadow::parseModeParallel("Parallel"), "case differs => off");
            t.IsTrue(!ps2x_gs_shadow::parseModeParallel("parallel "), "trailing space => off");
            t.IsTrue(!ps2x_gs_shadow::parseModeParallel("off"), "off => off");
        });

        tc.Run("parseU64 falls back on garbage", [](TestCase &t)
        {
            t.Equals(ps2x_gs_shadow::parseU64("800", 0ull), 800ull, "plain value");
            t.Equals(ps2x_gs_shadow::parseU64(nullptr, 7ull), 7ull, "null => default");
            t.Equals(ps2x_gs_shadow::parseU64("", 7ull), 7ull, "empty => default");
            t.Equals(ps2x_gs_shadow::parseU64("abc", 7ull), 7ull, "garbage => default");
        });

        tc.Run("tickEligible window and cap edges", [](TestCase &t)
        {
            using ps2x_gs_shadow::tickEligible;
            t.IsTrue(tickEligible(800u, 800u, 1200u, 0u, 200u), "from is inclusive");
            t.IsTrue(tickEligible(1199u, 800u, 1200u, 199u, 200u), "to is exclusive, cap open");
            t.IsTrue(!tickEligible(799u, 800u, 1200u, 0u, 200u), "before from => no");
            t.IsTrue(!tickEligible(1200u, 800u, 1200u, 0u, 200u), "at to => no");
            t.IsTrue(!tickEligible(900u, 800u, 1200u, 200u, 200u), "cap reached => no");
            t.IsTrue(!tickEligible(900u, 800u, 1200u, 0u, 0u), "zero cap => no");
        });

        tc.Run("arbiter shadow tap preserves path and drain order", [](TestCase &t)
        {
            std::vector<uint8_t> cpuOrder;
            std::vector<uint8_t> shadowOrder;
            std::vector<uint32_t> shadowPaths;
            GifArbiter arbiter([&](const uint8_t *data, uint32_t sizeBytes)
            {
                if (data && sizeBytes > 0u)
                    cpuOrder.push_back(data[0]);
            });
            arbiter.setShadowPacketFn([&](GifPathId path, const uint8_t *data, uint32_t sizeBytes)
            {
                if (data && sizeBytes > 0u)
                {
                    shadowPaths.push_back(static_cast<uint32_t>(path));
                    shadowOrder.push_back(data[0]);
                }
            });

            const std::vector<uint8_t> p1(16u, 0x11u);
            const std::vector<uint8_t> p2(16u, 0x22u);
            const std::vector<uint8_t> p3(16u, 0x33u);
            arbiter.submit(GifPathId::Path3, p3.data(), static_cast<uint32_t>(p3.size()));
            arbiter.submit(GifPathId::Path2, p2.data(), static_cast<uint32_t>(p2.size()));
            arbiter.submit(GifPathId::Path1, p1.data(), static_cast<uint32_t>(p1.size()));
            arbiter.drain();

            t.Equals(cpuOrder, shadowOrder, "shadow sees the same bytes in the same order");
            t.Equals(shadowPaths.size(), static_cast<size_t>(3u), "three shadow observations");
            if (shadowPaths.size() == 3u)
            {
                t.Equals(shadowPaths[0], 1u, "first drained is PATH1");
                t.Equals(shadowPaths[1], 2u, "second drained is PATH2");
                t.Equals(shadowPaths[2], 3u, "third drained is PATH3");
            }
        });

        tc.Run("arbiter without shadow fn behaves as before", [](TestCase &t)
        {
            uint32_t calls = 0u;
            GifArbiter arbiter([&](const uint8_t *data, uint32_t sizeBytes)
            {
                (void)data;
                (void)sizeBytes;
                ++calls;
            });
            const std::vector<uint8_t> p(16u, 0xAAu);
            arbiter.submit(GifPathId::Path3, p.data(), static_cast<uint32_t>(p.size()));
            arbiter.drain();
            t.Equals(calls, 1u, "drain still delivers with no shadow fn set");
            t.IsTrue(arbiter.empty(), "queue drains fully");
        });
    });
}
