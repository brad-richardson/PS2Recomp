#include "MiniTest.h"
#include "ps2_perf_log.h"

#include <cstdint>
#include <string>
#include <vector>

namespace
{
// PL2: AThermal stubs. Never dereference the manager (the fakes below are
// non-null but invalid); each getter returns a fixed status.
void *stubThermalAcquire()
{
    return reinterpret_cast<void *>(static_cast<uintptr_t>(0xA7));
}
void stubThermalRelease(void *)
{
}
int32_t stubThermalTwo(void *)
{
    return 2;
}
int32_t stubThermalZero(void *)
{
    return 0;
}
int32_t stubThermalSix(void *)
{
    return 6;
}
int32_t stubThermalHigh(void *)
{
    return 7;
}
int32_t stubThermalNeg(void *)
{
    return -1;
}
void *fakeThermalManager()
{
    return reinterpret_cast<void *>(static_cast<uintptr_t>(0x1CE));
}
} // namespace

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
                     "android golden line"); });

        tc.Run("thermalStatusWith passes the manager status through", [](TestCase &t)
               {
            ps2x::perflog::ThermalFns full;
            full.acquireManager = stubThermalAcquire;
            full.releaseManager = stubThermalRelease;
            full.getStatus = stubThermalTwo;
            t.Equals(ps2x::perflog::thermalStatusWith(full, fakeThermalManager()), 2, "mid-range passes");
            full.getStatus = stubThermalZero;
            t.Equals(ps2x::perflog::thermalStatusWith(full, fakeThermalManager()), 0, "NONE passes");
            full.getStatus = stubThermalSix;
            t.Equals(ps2x::perflog::thermalStatusWith(full, fakeThermalManager()), 6, "SHUTDOWN passes"); });

        tc.Run("thermalStatusWith reads -1 on any missing symbol", [](TestCase &t)
               {
            // The stub getter would return 2 (valid), so -1 proves it never ran.
            ps2x::perflog::ThermalFns full;
            full.acquireManager = stubThermalAcquire;
            full.getStatus = stubThermalTwo;
            full.releaseManager = stubThermalRelease;
            const ps2x::perflog::ThermalFns none;
            t.Equals(ps2x::perflog::thermalStatusWith(none, fakeThermalManager()), -1, "all missing");
            t.Equals(ps2x::perflog::thermalStatusWith(none, nullptr), -1, "all missing, null manager");
            ps2x::perflog::ThermalFns noAcquire = full;
            noAcquire.acquireManager = nullptr;
            t.Equals(ps2x::perflog::thermalStatusWith(noAcquire, fakeThermalManager()), -1,
                     "acquire missing, getter never called");
            ps2x::perflog::ThermalFns noGet = full;
            noGet.getStatus = nullptr;
            t.Equals(ps2x::perflog::thermalStatusWith(noGet, fakeThermalManager()), -1, "getter missing");
            ps2x::perflog::ThermalFns noRelease = full;
            noRelease.releaseManager = nullptr;
            t.Equals(ps2x::perflog::thermalStatusWith(noRelease, fakeThermalManager()), -1,
                     "release missing, getter never called"); });

        tc.Run("thermalStatusWith reads -1 on a null manager", [](TestCase &t)
               {
            ps2x::perflog::ThermalFns full;
            full.acquireManager = stubThermalAcquire;
            full.getStatus = stubThermalTwo;
            full.releaseManager = stubThermalRelease;
            t.Equals(ps2x::perflog::thermalStatusWith(full, nullptr), -1,
                     "null manager, getter never called"); });

        tc.Run("thermalStatusWith clamps outside 0..6 to -1", [](TestCase &t)
               {
            ps2x::perflog::ThermalFns full;
            full.acquireManager = stubThermalAcquire;
            full.releaseManager = stubThermalRelease;
            full.getStatus = stubThermalHigh;
            t.Equals(ps2x::perflog::thermalStatusWith(full, fakeThermalManager()), -1, "7 reads na");
            full.getStatus = stubThermalNeg;
            t.Equals(ps2x::perflog::thermalStatusWith(full, fakeThermalManager()), -1, "-1 reads na"); });

#if defined(__linux__) || defined(__APPLE__)
        tc.Run("perfThermalStatusForTest reads -1 where libandroid is absent", [](TestCase &t)
               {
            // Desktop: no libandroid.so, so the cached sampler degrades to na.
            // (This host-suite binary never runs on Android.)
            t.Equals(ps2x::perflog::perfThermalStatusForTest(), -1, "desktop thermal na"); });
#endif

        tc.Run("primeZoneRank prefers cpu-1-1-1, then siblings", [](TestCase &t)
               {
            t.Equals(ps2x::perflog::primeZoneRank("cpu-1-1-1"), 0, "prime first");
            t.Equals(ps2x::perflog::primeZoneRank("cpu-1-1-0"), 1, "sibling 1");
            t.Equals(ps2x::perflog::primeZoneRank("cpu-1-0-1"), 2, "sibling 2");
            t.Equals(ps2x::perflog::primeZoneRank("cpu-1-0-0"), 3, "sibling 3");
            t.Equals(ps2x::perflog::primeZoneRank("cpuss-1-0"), 4, "cluster agg 0");
            t.Equals(ps2x::perflog::primeZoneRank("cpuss-1-1"), 5, "cluster agg 1"); });

        tc.Run("primeZoneRank rejects non-prime zones", [](TestCase &t)
               {
            t.Equals(ps2x::perflog::primeZoneRank("cpu-0-0-0"), -1, "little cluster");
            t.Equals(ps2x::perflog::primeZoneRank("gpuss-0"), -1, "gpu");
            t.Equals(ps2x::perflog::primeZoneRank("aoss-0"), -1, "aoss");
            t.Equals(ps2x::perflog::primeZoneRank("vbat"), -1, "battery");
            t.Equals(ps2x::perflog::primeZoneRank(""), -1, "empty");
            t.Equals(ps2x::perflog::primeZoneRank("cpu-1-1-1 "), -1, "untrimmed (caller trims)"); });

        // PT2: per-stage tail rings, nearest-rank stats, hist buckets, lines.
        tc.Run("stageName covers all six stages", [](TestCase &t)
               {
            t.IsTrue(std::string(ps2x::perflog::stageName(ps2x::perflog::Stage::EeBusy)) == "ee.busy", "ee.busy");
            t.IsTrue(std::string(ps2x::perflog::stageName(ps2x::perflog::Stage::EeCpu)) == "ee.cpu", "ee.cpu");
            t.IsTrue(std::string(ps2x::perflog::stageName(ps2x::perflog::Stage::EeWait)) == "ee.wait", "ee.wait");
            t.IsTrue(std::string(ps2x::perflog::stageName(ps2x::perflog::Stage::GsBusy)) == "gs.busy", "gs.busy");
            t.IsTrue(std::string(ps2x::perflog::stageName(ps2x::perflog::Stage::MtvuBusy)) == "mtvu.busy", "mtvu.busy");
            t.IsTrue(std::string(ps2x::perflog::stageName(ps2x::perflog::Stage::GpuBusy)) == "gpu.busy", "gpu.busy");
            t.Equals(ps2x::perflog::kStageCount, static_cast<size_t>(6), "six stages"); });

        tc.Run("StageRing packs tick+usec and wraps to the newest lap", [](TestCase &t)
               {
            ps2x::perflog::StageRing r;
            t.Equals(r.head(), static_cast<uint64_t>(0), "head starts 0");
            r.push(1234u, 8.125f);
            r.push(1235u, 0.0f);
            r.push(1236u, -1.0f);
            t.Equals(r.head(), static_cast<uint64_t>(3), "head counts pushes");
            const auto a = ps2x::perflog::StageRing::decode(r.slotAt(0));
            t.Equals(a.tick, static_cast<uint64_t>(1234), "tick survives");
            t.Equals(static_cast<double>(a.ms), 8.125, "ms exact to 1 us");
            t.Equals(static_cast<double>(ps2x::perflog::StageRing::decode(r.slotAt(1)).ms), 0.0, "zero stays zero");
            t.Equals(static_cast<double>(ps2x::perflog::StageRing::decode(r.slotAt(2)).ms), 0.0, "negative clamps to 0");
            for (uint64_t i = 3; i < ps2x::perflog::StageRing::kCap + 3; ++i)
                r.push(static_cast<uint32_t>(10000 + i), 1.0f);
            t.Equals(r.head(), static_cast<uint64_t>(ps2x::perflog::StageRing::kCap + 3), "head past the lap");
            const auto oldest =
                ps2x::perflog::StageRing::decode(r.slotAt(ps2x::perflog::StageRing::kCap + 3 - ps2x::perflog::StageRing::kCap));
            t.Equals(oldest.tick, static_cast<uint64_t>(10003), "oldest slot is the newest lap");
            const auto newest = ps2x::perflog::StageRing::decode(r.slotAt(ps2x::perflog::StageRing::kCap + 2));
            t.Equals(newest.tick, static_cast<uint64_t>(10000 + ps2x::perflog::StageRing::kCap + 2), "newest slot decodes"); });

        tc.Run("summarizeStage nearest-rank stats over 1..60", [](TestCase &t)
               {
            std::vector<ps2x::perflog::StageEntry> v;
            for (uint64_t i = 1; i <= 60; ++i)
                v.push_back({1000 + i, static_cast<float>(i)});
            const auto st = ps2x::perflog::summarizeStage(v);
            t.Equals(st.n, static_cast<uint64_t>(60), "n");
            t.Equals(st.tick0, static_cast<uint64_t>(1001), "tick0");
            t.Equals(st.tick1, static_cast<uint64_t>(1060), "tick1");
            t.Equals(st.mean, 30.5, "mean");
            t.Equals(st.p50, 30.0, "p50 rank ceil(30)-1");
            t.Equals(st.p95, 57.0, "p95 rank ceil(57)-1");
            t.Equals(st.p99, 60.0, "p99 rank ceil(59.4)-1");
            t.Equals(st.max, 60.0, "max");
            uint64_t histSum = 0;
            for (uint64_t c : st.hist)
                histSum += c;
            t.Equals(histSum, static_cast<uint64_t>(60), "hist counts all"); });

        tc.Run("summarizeStage of no entries reads -1", [](TestCase &t)
               {
            const auto st = ps2x::perflog::summarizeStage({});
            t.Equals(st.n, static_cast<uint64_t>(0), "n 0");
            t.Equals(st.mean, -1.0, "mean -1");
            t.Equals(st.p99, -1.0, "p99 -1");
            t.Equals(st.max, -1.0, "max -1"); });

        tc.Run("histBucket edges and overflow", [](TestCase &t)
               {
            t.Equals(ps2x::perflog::histBucket(0.0), static_cast<size_t>(0), "0 -> 0");
            t.Equals(ps2x::perflog::histBucket(0.249), static_cast<size_t>(0), "0.249 -> 0");
            t.Equals(ps2x::perflog::histBucket(0.25), static_cast<size_t>(1), "0.25 -> 1");
            t.Equals(ps2x::perflog::histBucket(19.999), static_cast<size_t>(79), "19.999 -> 79");
            t.Equals(ps2x::perflog::histBucket(20.0), static_cast<size_t>(80), "20.0 overflows");
            t.Equals(ps2x::perflog::histBucket(123.0), static_cast<size_t>(80), "123 overflows");
            t.Equals(ps2x::perflog::histBucket(-2.0), static_cast<size_t>(0), "negative -> 0"); });

        tc.Run("formatStageLine golden with hist", [](TestCase &t)
               {
            const std::vector<ps2x::perflog::StageEntry> v{
                {100, 1.0f}, {101, 2.0f}, {102, 3.0f}, {103, 20.5f}};
            const auto st = ps2x::perflog::summarizeStage(v);
            t.Equals(ps2x::perflog::formatStageLine("ee.busy", st),
                     std::string("[perf-stage] tick0=100 tick1=103 stage=ee.busy n=4 mean=6.625 "
                                 "p50=2.000 p95=20.500 p99=20.500 max=20.500 hist=\"1.00:1 2.00:1 3.00:1 ovf:1\""),
                     "golden stage line"); });

        tc.Run("formatStageLine n=0 reads -1 with an empty hist", [](TestCase &t)
               {
            const auto st = ps2x::perflog::summarizeStage({});
            t.Equals(ps2x::perflog::formatStageLine("gs.busy", st),
                     std::string("[perf-stage] tick0=0 tick1=0 stage=gs.busy n=0 mean=-1.000 "
                                 "p50=-1.000 p95=-1.000 p99=-1.000 max=-1.000 hist=\"\""),
                     "empty stage line"); });
    });
}
