#include "MiniTest.h"
#include "ps2_perf_log.h"

#include <string>
#include <vector>

// IP3: PS2X_PERF_LOG pure parts — knob parse, v1 line format, ring-cap plan.
void register_ps2_perf_log_tests()
{
    MiniTest::Case("Ps2PerfLog", [](TestCase &tc)
                   {
        tc.Run("enabledFromEnv accepts only exact \"1\"", [](TestCase &t)
               {
            t.IsTrue(ps2x::perflog::enabledFromEnv("1"), "1 on");
            t.IsTrue(!ps2x::perflog::enabledFromEnv("0"), "0 off");
            t.IsTrue(!ps2x::perflog::enabledFromEnv(""), "empty off");
            t.IsTrue(!ps2x::perflog::enabledFromEnv(nullptr), "null off");
            t.IsTrue(!ps2x::perflog::enabledFromEnv("10"), "10 off");
            t.IsTrue(!ps2x::perflog::enabledFromEnv("true"), "true off"); });

        tc.Run("formatLine golden, thread names sanitized", [](TestCase &t)
               {
            ps2x::perflog::Sample s;
            s.wall = "2026-09-27T16:03:01Z";
            s.elapsedS = 12.0;
            s.tick = 12345;
            s.vsyncsPerS = 59.91;
            s.presents = 60;
            s.maxGapMs = 18.2;
            s.threadsAvailable = true;
            s.threads = {{"t#0", 812.4}, {"Game", 733.0}, {"my thread", 5.0}};
            s.device = "thermal=1 lpm=0 batt=80 chg=1";
            t.Equals(ps2x::perflog::formatLine(s),
                     std::string("[perf] wall=2026-09-27T16:03:01Z t=12.0s tick=12345 vsyncs_per_s=59.91 "
                                 "presents=60 maxgap_ms=18.2 threads=\"t#0=812.4 Game=733.0 my_thread=5.0\" "
                                 "device=\"thermal=1 lpm=0 batt=80 chg=1\""),
                     "golden line"); });

        tc.Run("formatLine unavailable channels read na", [](TestCase &t)
               {
            ps2x::perflog::Sample s;
            s.wall = "2026-09-27T16:03:01Z";
            s.elapsedS = 1.0;
            s.tick = 60;
            s.vsyncsPerS = 0.0;
            s.presents = 0;
            s.maxGapMs = -1.0;
            s.threadsAvailable = false;
            s.device = "na";
            t.Equals(ps2x::perflog::formatLine(s),
                     std::string("[perf] wall=2026-09-27T16:03:01Z t=1.0s tick=60 vsyncs_per_s=0.00 "
                                 "presents=0 maxgap_ms=-1.0 threads=\"na\" device=\"na\""),
                     "na line"); });

        tc.Run("planPrune keeps the newest 5 by name", [](TestCase &t)
               {
            const std::vector<ps2x::perflog::FileEntry> entries{
                {"perf-20260927-160301.log", 100}, {"perf-20260927-160302.log", 100},
                {"perf-20260927-160303.log", 100}, {"perf-20260927-160304.log", 100},
                {"perf-20260927-160305.log", 100}, {"perf-20260927-160305-2.log", 100}};
            const auto dead = ps2x::perflog::planPrune(entries, 5u * 1024u * 1024u, 5u);
            t.Equals(dead.size(), static_cast<size_t>(1), "one pruned");
            t.IsTrue(dead[0] == "perf-20260927-160301.log", "oldest pruned"); });

        tc.Run("planPrune enforces the byte cap oldest-first", [](TestCase &t)
               {
            const std::vector<ps2x::perflog::FileEntry> entries{
                {"perf-20260927-160301.log", 10}, {"perf-20260927-160302.log", 10},
                {"perf-20260927-160303.log", 10}};
            const auto dead = ps2x::perflog::planPrune(entries, 25u, 5u);
            t.Equals(dead.size(), static_cast<size_t>(1), "one pruned");
            t.IsTrue(dead[0] == "perf-20260927-160301.log", "oldest pruned");
            const auto none = ps2x::perflog::planPrune(entries, 30u, 5u);
            t.IsTrue(none.empty(), "at cap keeps all"); });

        tc.Run("planPrune sorts unordered input", [](TestCase &t)
               {
            const std::vector<ps2x::perflog::FileEntry> entries{
                {"perf-20260927-160303.log", 10}, {"perf-20260927-160301.log", 10},
                {"perf-20260927-160302.log", 10}};
            const auto dead = ps2x::perflog::planPrune(entries, 25u, 5u);
            t.Equals(dead.size(), static_cast<size_t>(1), "one pruned");
            t.IsTrue(dead[0] == "perf-20260927-160301.log", "oldest by name pruned"); }); });
}
