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
            t.IsTrue(dead[0] == "perf-20260927-160301.log", "oldest by name pruned"); });

        tc.Run("parseProcTaskStat reads comm, utime, stime", [](TestCase &t)
               {
            ps2x::perflog::ProcTaskCpu cpu;
            t.IsTrue(ps2x::perflog::parseProcTaskStat(
                         1234, "1234 (GameThread) S 1 1234 1234 0 -1 4194304 100 0 5 0 812 34 0 0 "
                               "20 0 1 0 12345 1000 200",
                         cpu),
                     "valid stat parses");
            t.Equals(cpu.tid, 1234L, "tid");
            t.IsTrue(cpu.comm == "GameThread", "comm");
            t.Equals(cpu.utime, static_cast<uint64_t>(812), "utime is field 14");
            t.Equals(cpu.stime, static_cast<uint64_t>(34), "stime is field 15");
            t.IsTrue(ps2x::perflog::parseProcTaskStat(
                         999, "999 (Binder:1234_1) S 1 999 999 0 -1 4194304 7 0 0 0 3 4 0 0 20 0 1 0 9 8 7\n",
                         cpu),
                     "binder-style comm parses");
            t.IsTrue(cpu.comm == "Binder:1234_1", "binder comm kept");
            t.Equals(cpu.utime, static_cast<uint64_t>(3), "binder utime");
            t.IsTrue(ps2x::perflog::parseProcTaskStat(
                         42, "42 (my (weird) name) R 1 42 42 0 -1 4194304 0 0 0 0 11 22",
                         cpu),
                     "parens in comm parse (last ')' wins)");
            t.IsTrue(cpu.comm == "my (weird) name", "comm with parens");
            t.Equals(cpu.utime, static_cast<uint64_t>(11), "weird utime");
            t.Equals(cpu.stime, static_cast<uint64_t>(22), "weird stime"); });

        tc.Run("parseProcTaskStat rejects malformed input", [](TestCase &t)
               {
            ps2x::perflog::ProcTaskCpu cpu;
            cpu.comm = "untouched";
            t.IsTrue(!ps2x::perflog::parseProcTaskStat(0, "1 (x) S 1 1 1 0 -1 0 0 0 0 0 1 2", cpu),
                     "tid 0 rejected");
            t.IsTrue(!ps2x::perflog::parseProcTaskStat(1, "no parens here", cpu), "no comm rejected");
            t.IsTrue(!ps2x::perflog::parseProcTaskStat(1, "1 () S 1 1 1 0 -1 0 0 0 0 0 1 2", cpu),
                     "empty comm rejected");
            t.IsTrue(!ps2x::perflog::parseProcTaskStat(1, "1 (x) S 1 1", cpu), "truncated rejected");
            t.IsTrue(!ps2x::perflog::parseProcTaskStat(1, "1 (x) S 1 1 1 0 -1 0 0 0 0 0 1x 2", cpu),
                     "non-numeric utime rejected");
            t.IsTrue(!ps2x::perflog::parseProcTaskStat(1, "1 (x) S 1 1 1 0 -1 0 0 0 0 0 1", cpu),
                     "missing stime rejected");
            t.IsTrue(cpu.comm == "untouched", "out untouched on failure"); });

        tc.Run("procTicksToMs scales by USER_HZ", [](TestCase &t)
               {
            t.Equals(ps2x::perflog::procTicksToMs(100, 100), 1000.0, "100 ticks at HZ=100");
            t.Equals(ps2x::perflog::procTicksToMs(250, 100), 2500.0, "250 ticks");
            t.Equals(ps2x::perflog::procTicksToMs(100, 0), 0.0, "HZ 0 reads 0"); });

        tc.Run("parseSysfsLong reads small ints strictly", [](TestCase &t)
               {
            long v = 0;
            t.IsTrue(ps2x::perflog::parseSysfsLong("41800\n", v), "millidegrees parse");
            t.Equals(v, 41800L, "temp value");
            t.IsTrue(ps2x::perflog::parseSysfsLong("  1\r\n", v), "whitespace tolerated");
            t.Equals(v, 1L, "online flag");
            t.IsTrue(ps2x::perflog::parseSysfsLong("-5\n", v), "signed parses");
            t.Equals(v, -5L, "negative value");
            t.IsTrue(!ps2x::perflog::parseSysfsLong("", v), "empty rejected");
            t.IsTrue(!ps2x::perflog::parseSysfsLong("abc\n", v), "text rejected");
            t.IsTrue(!ps2x::perflog::parseSysfsLong("12x\n", v), "trailing junk rejected");
            t.IsTrue(!ps2x::perflog::parseSysfsLong("1 2\n", v), "inner space rejected"); });

        tc.Run("formatAndroidDevice goldens, missing reads na", [](TestCase &t)
               {
            ps2x::perflog::AndroidDevice full;
            full.hasThermal = true;
            full.thermal = 2;
            full.hasPrimeC = true;
            full.primeC = 41.8;
            full.hasBatt = true;
            full.batt = 87;
            full.hasAc = true;
            full.ac = 1;
            t.Equals(ps2x::perflog::formatAndroidDevice(full),
                     std::string("thermal=2 prime=41.8 batt=87 ac=1"), "full device line");
            const ps2x::perflog::AndroidDevice none;
            t.Equals(ps2x::perflog::formatAndroidDevice(none),
                     std::string("thermal=na prime=na batt=na ac=na"), "all-na device line");
            ps2x::perflog::AndroidDevice partial;
            partial.hasBatt = true;
            partial.batt = 50;
            t.Equals(ps2x::perflog::formatAndroidDevice(partial),
                     std::string("thermal=na prime=na batt=50 ac=na"), "partial device line"); });

        tc.Run("formatLine carries an Android sample in v1 shape", [](TestCase &t)
               {
            ps2x::perflog::Sample s;
            s.wall = "2026-09-28T10:00:01Z";
            s.elapsedS = 61.0;
            s.tick = 3660;
            s.vsyncsPerS = 59.94;
            s.presents = 60;
            s.maxGapMs = 17.1;
            s.threadsAvailable = true;
            s.threads = {{"GameThread#1234", 980.5}, {"MTVU#1235", 120.0}, {"Audio#1240", 8.2}};
            s.device = "thermal=0 prime=38.4 batt=87 ac=1";
            t.Equals(ps2x::perflog::formatLine(s),
                     std::string("[perf] wall=2026-09-28T10:00:01Z t=61.0s tick=3660 vsyncs_per_s=59.94 "
                                 "presents=60 maxgap_ms=17.1 "
                                 "threads=\"GameThread#1234=980.5 MTVU#1235=120.0 Audio#1240=8.2\" "
                                 "device=\"thermal=0 prime=38.4 batt=87 ac=1\""),
                     "android golden line"); }); });
}
