#include "MiniTest.h"
#include "ps2_thread_affinity.h"

#include <string>
#include <vector>

// N11: PS2X_GAME_THREAD_CPUS list parser (pure part; the sched_setaffinity
// call lives in ps2_runtime.cpp's game thread start).
void register_ps2_thread_affinity_tests()
{
    MiniTest::Case("Ps2ThreadAffinity", [](TestCase &tc)
                   {
        tc.Run("parses comma-separated cpu lists", [](TestCase &t)
               {
            t.IsTrue(ps2x::parseCpuList("6,7") == std::vector<int>{6, 7}, "6,7");
            t.IsTrue(ps2x::parseCpuList("7") == std::vector<int>{7}, "single");
            t.IsTrue(ps2x::parseCpuList(" 6 , 7 ") == std::vector<int>{6, 7}, "whitespace"); });

        tc.Run("skips empty, non-numeric and negative items", [](TestCase &t)
               {
            t.IsTrue(ps2x::parseCpuList("").empty(), "empty");
            t.IsTrue(ps2x::parseCpuList("   ").empty(), "blank");
            t.IsTrue(ps2x::parseCpuList("a,6,,x") == std::vector<int>{6}, "garbage skipped");
            t.IsTrue(ps2x::parseCpuList("-1,6") == std::vector<int>{6}, "negative skipped");
            t.IsTrue(ps2x::parseCpuList("6,9999999") == std::vector<int>{6}, "absurd dropped"); }); });
}
