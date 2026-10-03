#include "MiniTest.h"
#include "ps2_adpf.h"

#include <cstdint>
#include <string>

// AD1: PS2X_ADPF pure parts — knob parse, thread names.
void register_ps2_adpf_tests()
{
    MiniTest::Case("Ps2Adpf", [](TestCase &tc)
                   {
        tc.Run("enabledFromEnv accepts only exact \"1\"", [](TestCase &t)
               {
            t.IsTrue(ps2x::adpf::enabledFromEnv("1"), "1 on");
            t.IsTrue(!ps2x::adpf::enabledFromEnv("0"), "0 off");
            t.IsTrue(!ps2x::adpf::enabledFromEnv(""), "empty off");
            t.IsTrue(!ps2x::adpf::enabledFromEnv(nullptr), "null off");
            t.IsTrue(!ps2x::adpf::enabledFromEnv("10"), "10 off"); });

        tc.Run("reportMode is Busy (the critical override is deleted)", [](TestCase &t)
               {
            using ps2x::adpf::ReportMode;
            t.IsTrue(ps2x::adpf::reportMode() == ReportMode::Busy, "always busy"); });

        tc.Run("threadName covers all four sessions", [](TestCase &t)
               {
            t.Equals(std::string(ps2x::adpf::threadName(ps2x::adpf::Thread::Game)), std::string("game"), "game");
            t.Equals(std::string(ps2x::adpf::threadName(ps2x::adpf::Thread::Mtvu)), std::string("mtvu"), "mtvu");
            t.Equals(std::string(ps2x::adpf::threadName(ps2x::adpf::Thread::GsWorker)), std::string("gsw"), "gsw");
            t.Equals(std::string(ps2x::adpf::threadName(ps2x::adpf::Thread::GsBack)), std::string("gsb"), "gsb");
            t.Equals(ps2x::adpf::kThreadCount, static_cast<size_t>(4), "four sessions"); });

        tc.Run("shouldReport skips zero-duration reports", [](TestCase &t)
                {
            t.IsTrue(!ps2x::adpf::shouldReport(0u), "zero skipped");
            t.IsTrue(ps2x::adpf::shouldReport(1u), "1ns reported");
            t.IsTrue(ps2x::adpf::shouldReport(6000000u), "6ms reported"); });

#if !defined(__ANDROID__)
        tc.Run("header compiles out off Android", [](TestCase &t)
               {
            t.IsFalse(ps2x::adpf::enabled(), "stub disabled");
            ps2x::adpf::noteThread(ps2x::adpf::Thread::Game);
            ps2x::adpf::report(ps2x::adpf::Thread::Game, 1234u);
            t.IsTrue(true, "stub calls return"); });
#endif
    });
}
