#include "MiniTest.h"
#include "ps2_perf_log.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#include <thread>
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
                                 "device=\"thermal=1 lpm=0 batt=80 chg=1\" gpubusy_pct=na gpuclk=na"),
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
                                 "presents=0 maxgap_ms=-1.0 threads=\"na\" device=\"na\" "
                                 "gpubusy_pct=na gpuclk=na"),
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
                                 "device=\"thermal=0 prime=38.4 batt=87 ac=1\" gpubusy_pct=na gpuclk=na"),
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
        tc.Run("stageName covers all fourteen stages", [](TestCase &t)
               {
            t.IsTrue(std::string(ps2x::perflog::stageName(ps2x::perflog::Stage::EeBusy)) == "ee.busy", "ee.busy");
            t.IsTrue(std::string(ps2x::perflog::stageName(ps2x::perflog::Stage::EeCpu)) == "ee.cpu", "ee.cpu");
            t.IsTrue(std::string(ps2x::perflog::stageName(ps2x::perflog::Stage::EeWait)) == "ee.wait", "ee.wait");
            t.IsTrue(std::string(ps2x::perflog::stageName(ps2x::perflog::Stage::GsBusy)) == "gs.busy", "gs.busy");
            t.IsTrue(std::string(ps2x::perflog::stageName(ps2x::perflog::Stage::MtvuBusy)) == "mtvu.busy", "mtvu.busy");
            t.IsTrue(std::string(ps2x::perflog::stageName(ps2x::perflog::Stage::GpuBusy)) == "gpu.busy", "gpu.busy");
            t.IsTrue(std::string(ps2x::perflog::stageName(ps2x::perflog::Stage::GsBackBusy)) == "gsback.busy", "gsback.busy");
            t.IsTrue(std::string(ps2x::perflog::stageName(ps2x::perflog::Stage::MtvuGifBusy)) == "mtvugif.busy", "mtvugif.busy");
            t.IsTrue(std::string(ps2x::perflog::stageName(ps2x::perflog::Stage::MtvuVifBusy)) == "mtvuvif.busy", "mtvuvif.busy");
            t.IsTrue(std::string(ps2x::perflog::stageName(ps2x::perflog::Stage::EePace)) == "ee.pace", "ee.pace");
            t.IsTrue(std::string(ps2x::perflog::stageName(ps2x::perflog::Stage::EeEvent)) == "ee.event", "ee.event");
            t.IsTrue(std::string(ps2x::perflog::stageName(ps2x::perflog::Stage::EeEnq)) == "ee.enq", "ee.enq");
            t.IsTrue(std::string(ps2x::perflog::stageName(ps2x::perflog::Stage::EeMtvu)) == "ee.mtvu", "ee.mtvu");
            t.IsTrue(std::string(ps2x::perflog::stageName(ps2x::perflog::Stage::EeMtvuVb)) == "ee.mtvuvb", "ee.mtvuvb");
            t.Equals(ps2x::perflog::kStageCount, static_cast<size_t>(14), "fourteen stages"); });

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

        // IOSR1: per-reason MTVU window line (all reasons in order, the
        // unattributed "other" bucket, and the MQ2/MQ3 skip counts).
        tc.Run("formatMtvuLine golden with skips", [](TestCase &t)
                {
            const ps2x::perflog::MtvuReasonCell cells[] = {
                {"vblank", 8u, 28900000u},
                {"vu1mem", 0u, 0u},
                {"finishpoll", 12u, 8390000u},
            };
            t.Equals(ps2x::perflog::formatMtvuLine(9115u, cells, 3, 0u, 0u, 376u, 752u),
                     std::string("[perf-mtvu] tick=9115 vblank=8/28.900 vu1mem=0/0.000 "
                                 "finishpoll=12/8.390 other=0/0.000 "
                                 "finishee_skip=376 vif1statfree_skip=752"),
                     "golden mtvu line"); });

        tc.Run("formatMtvuLine all-zero window", [](TestCase &t)
                {
            const ps2x::perflog::MtvuReasonCell cells[] = {
                {"vblank", 0u, 0u},
            };
            t.Equals(ps2x::perflog::formatMtvuLine(100u, cells, 1, 0u, 0u, 0u, 0u),
                     std::string("[perf-mtvu] tick=100 vblank=0/0.000 other=0/0.000 "
                                 "finishee_skip=0 vif1statfree_skip=0"),
                     "zero mtvu line"); });

        // PT2 Part 2a/b: drain clamp, kgsl parsers, gpu line fields.
        tc.Run("clampDrainStart keeps the newest lap", [](TestCase &t)
               {
            t.Equals(ps2x::perflog::clampDrainStart(100, 160, 16384), static_cast<uint64_t>(100), "in-lap");
            t.Equals(ps2x::perflog::clampDrainStart(0, 0, 16384), static_cast<uint64_t>(0), "empty");
            t.Equals(ps2x::perflog::clampDrainStart(0, 20000, 16384), static_cast<uint64_t>(20000 - 16384),
                     "lapped drops the oldest");
            t.Equals(ps2x::perflog::clampDrainStart(5, 5 + 16384, 16384), static_cast<uint64_t>(5),
                     "exactly one lap keeps all"); });

        tc.Run("parseKgslBusy accepts counter pairs, rejects the rest", [](TestCase &t)
               {
            uint64_t b = 0, tot = 0;
            t.IsTrue(ps2x::perflog::parseKgslBusy("12345 33406\n", b, tot), "newline pair");
            t.Equals(b, static_cast<uint64_t>(12345), "busy");
            t.Equals(tot, static_cast<uint64_t>(33406), "total");
            t.IsTrue(ps2x::perflog::parseKgslBusy("  7\t9  ", b, tot), "whitespace pair");
            t.Equals(b, static_cast<uint64_t>(7), "busy ws");
            t.IsFalse(ps2x::perflog::parseKgslBusy("", b, tot), "empty");
            t.IsFalse(ps2x::perflog::parseKgslBusy("12345", b, tot), "single");
            t.IsFalse(ps2x::perflog::parseKgslBusy("a b", b, tot), "non-numeric");
            t.IsFalse(ps2x::perflog::parseKgslBusy("1 2 3", b, tot), "triple");
            t.IsFalse(ps2x::perflog::parseKgslBusy("1 -2", b, tot), "negative");
            t.IsFalse(ps2x::perflog::parseKgslBusy("18446744073709551616 0", b, tot), "overflow"); });

        tc.Run("parseKgslClk accepts a bare frequency", [](TestCase &t)
               {
            uint64_t hz = 0;
            t.IsTrue(ps2x::perflog::parseKgslClk("800000000\n", hz), "newline clk");
            t.Equals(hz, static_cast<uint64_t>(800000000), "hz");
            t.IsFalse(ps2x::perflog::parseKgslClk("", hz), "empty");
            t.IsFalse(ps2x::perflog::parseKgslClk("8x", hz), "trailing junk"); });

        tc.Run("kgsl pct is always instantaneous (read-reset windows)", [](TestCase &t)
               {
            double pct = 0.0;
            t.IsTrue(ps2x::perflog::kgslSamplePct(10, 100, pct), "usable");
            t.Equals(pct, 10.0, "pct");
            t.IsFalse(ps2x::perflog::kgslSamplePct(0, 0, pct), "zero total (idle)");
            // PT2-grav2 spike repro: consecutive read-reset windows where total
            // barely moves while busy jumps. A delta reads 7500%; the sample
            // reads the window's own 34.9%.
            t.IsTrue(ps2x::perflog::kgslSamplePct(350000, 1002000, pct), "spike window usable");
            t.IsTrue(pct <= 100.0, "spike window bounded");
            t.Equals(pct, 100.0 * 350000.0 / 1002000.0, "spike window instant"); });

        tc.Run("formatLine carries gpu values when sampled", [](TestCase &t)
               {
            ps2x::perflog::Sample s;
            s.wall = "2026-09-28T12:00:01Z";
            s.elapsedS = 61.0;
            s.tick = 3660;
            s.vsyncsPerS = 59.94;
            s.presents = 60;
            s.maxGapMs = 17.1;
            s.threadsAvailable = false;
            s.device = "na";
            s.kgslBusy = "43.2";
            s.kgslClk = "800000000";
            t.Equals(ps2x::perflog::formatLine(s),
                      std::string("[perf] wall=2026-09-28T12:00:01Z t=61.0s tick=3660 vsyncs_per_s=59.94 "
                                  "presents=60 maxgap_ms=17.1 threads=\"na\" device=\"na\" "
                                  "gpubusy_pct=43.2 gpuclk=800000000"),
                      "gpu line"); });

        // PT3: the timer flush reads stage rings from a second thread while
        // the stage owners write: a concurrent drain loses/duplicates nothing.
        tc.Run("StageRing second-thread drain is exact while writing", [](TestCase &t)
               {
            ps2x::perflog::StageRing r;
            constexpr uint64_t kN = 8000; // under one lap: nothing overwritten
            std::atomic<uint64_t> written{0};
            std::vector<uint64_t> seen;
            seen.reserve(kN);
            std::thread reader([&] {
                uint64_t consumed = 0;
                while (written.load(std::memory_order_acquire) < kN)
                {
                    const uint64_t head = r.head();
                    consumed = ps2x::perflog::clampDrainStart(consumed, head, ps2x::perflog::StageRing::kCap);
                    for (uint64_t i = consumed; i != head; ++i)
                        seen.push_back(ps2x::perflog::StageRing::decode(r.slotAt(i)).tick);
                    consumed = head;
                }
                const uint64_t head = r.head();
                const uint64_t start =
                    ps2x::perflog::clampDrainStart(consumed, head, ps2x::perflog::StageRing::kCap);
                for (uint64_t i = start; i != head; ++i)
                    seen.push_back(ps2x::perflog::StageRing::decode(r.slotAt(i)).tick);
            });
            for (uint64_t i = 1; i <= kN; ++i)
            {
                r.push(static_cast<uint32_t>(i), 1.0f);
                written.store(i, std::memory_order_release);
            }
            reader.join();
            t.Equals(seen.size(), static_cast<size_t>(kN), "every push drained once");
            std::vector<bool> have(kN + 1, false);
            bool bad = false;
            for (uint64_t tick : seen)
            {
                if (tick < 1 || tick > kN || have[tick])
                    bad = true;
                else
                    have[tick] = true;
            }
            t.IsTrue(!bad, "no lost or duplicated entries");
            bool all = true;
            for (uint64_t i = 1; i <= kN; ++i)
                all = all && have[i];
            t.IsTrue(all, "ticks 1..N all present"); });

        // PL3: deltas key by stable ID, not enumeration position: a churned
        // snapshot (reordered, one thread gone, one new) keeps the survivors'
        // deltas and starts the newcomer at zero marked new.
        tc.Run("diffStableCpu survives enumeration churn", [](TestCase &t)
               {
            using ps2x::perflog::StableThreadSample;
            const std::vector<StableThreadSample> prev{{101, "GameThread", 1000.0, false},
                                                       {202, "MTVU", 500.0, false},
                                                       {303, "MTVU", 10.0, false}};
            const std::vector<StableThreadSample> cur{{303, "MTVU", 30.0, false},
                                                      {101, "GameThread", 1100.0, false},
                                                      {404, "MTVU", 7.0, false}};
            const auto deltas = ps2x::perflog::diffStableCpu(prev, true, cur);
            t.Equals(deltas.size(), static_cast<size_t>(3), "one delta per live thread");
            t.Equals(deltas[0].label, std::string("MTVU#303"), "stable label, not position");
            t.Equals(deltas[0].ms, 20.0, "survivor delta kept across reorder");
            t.IsTrue(!deltas[0].isNew, "survivor not new");
            t.Equals(deltas[1].ms, 100.0, "game delta kept");
            t.Equals(deltas[2].ms, 0.0, "newcomer starts at zero");
            t.IsTrue(deltas[2].isNew, "newcomer marked new");
            t.Equals(deltas[2].label, std::string("MTVU#404"), "newcomer keeps full label"); });

        tc.Run("diffStableCpu first window and renames read new", [](TestCase &t)
               {
            using ps2x::perflog::StableThreadSample;
            const std::vector<StableThreadSample> cur{{101, "GameThread", 1000.0, true}};
            const auto first = ps2x::perflog::diffStableCpu({}, false, cur);
            t.Equals(first.size(), static_cast<size_t>(1), "one delta");
            t.IsTrue(first[0].isNew, "everything is new on the first window");
            t.Equals(first[0].ms, 0.0, "first-window delta is zero");
            t.IsTrue(first[0].isMain, "main flag carried");
            // Same ID, different name (ID reuse after death): never reuse the
            // old delta.
            const std::vector<StableThreadSample> prev{{101, "GameThread", 1000.0, false}};
            const std::vector<StableThreadSample> renamed{{101, "Worker", 5000.0, false}};
            const auto second = ps2x::perflog::diffStableCpu(prev, true, renamed);
            t.IsTrue(second[0].isNew, "renamed ID reads new");
            t.Equals(second[0].ms, 0.0, "renamed delta is zero, not 4000"); });

        tc.Run("roleOf names the runtime roles, main wins", [](TestCase &t)
               {
            using ps2x::perflog::roleOf;
            t.Equals(roleOf("GameThread", false), std::string("game"), "game");
            t.Equals(roleOf("MTVU", false), std::string("mtvu"), "mtvu");
            t.Equals(roleOf("MTVU-VIF", false), std::string("vif"), "vif");
            t.Equals(roleOf("MTVU-GIF", false), std::string("gif"), "gif");
            t.Equals(roleOf("GsWorker", false), std::string("gs"), "gs");
            t.Equals(roleOf("Audio", false), std::string("audio"), "audio");
            t.Equals(roleOf("SDLTimer", false), std::string("other"), "unknown to other");
            t.Equals(roleOf("GameThread", true), std::string("main"), "main wins over name");
            t.Equals(roleOf("t", true), std::string("main"), "unnamed main"); });

        tc.Run("sumRoles folds duplicate names, formatCpuLine golden", [](TestCase &t)
               {
            using ps2x::perflog::StableThreadDelta;
            // The old bug: idle MTVU#11 overwrote active MTVU#8. Both share
            // the mtvu role now, so the sum keeps the active thread's CPU.
            const std::vector<StableThreadDelta> deltas{
                {"GameThread#101", 100.0, false, "GameThread", false},
                {"MTVU#202", 794.3, false, "MTVU", false},
                {"MTVU#303", 0.0, false, "MTVU", false},
                {"MTVU-GIF#404", 50.0, false, "MTVU-GIF", false},
                {"t#505", 12.5, true, "t", true}};
            const auto sums = ps2x::perflog::sumRoles(deltas);
            t.Equals(sums.game, 100.0, "game");
            t.Equals(sums.mtvu, 794.3, "both MTVUs summed, active kept");
            t.Equals(sums.gif, 50.0, "gif");
            t.Equals(sums.main, 12.5, "main");
            t.Equals(sums.vif, 0.0, "vif absent is zero");
            t.IsTrue(std::fabs(sums.total() - 956.8) < 1e-9, "total matches the thread cells");
            t.Equals(ps2x::perflog::formatCpuLine(32823, sums),
                      std::string("[perf-cpu] tick=32823 game=100.0 mtvu=794.3 vif=0.0 gif=50.0 gs=0.0 "
                                  "main=12.5 audio=0.0 other=0.0 total=956.8"),
                      "cpu golden"); });

        tc.Run("formatLine marks new threads with a star", [](TestCase &t)
               {
            ps2x::perflog::Sample s;
            s.wall = "2026-10-03T00:00:01Z";
            s.elapsedS = 2.0;
            s.tick = 120;
            s.vsyncsPerS = 60.0;
            s.presents = 60;
            s.maxGapMs = 17.0;
            s.threadsAvailable = true;
            s.threads = {{"GameThread#101", 100.0, false}, {"MTVU#404", 0.0, true}};
            s.device = "na";
            t.Equals(ps2x::perflog::formatLine(s),
                      std::string("[perf] wall=2026-10-03T00:00:01Z t=2.0s tick=120 vsyncs_per_s=60.00 "
                                  "presents=60 maxgap_ms=17.0 threads=\"GameThread#101=100.0 MTVU#404=0.0*\" "
                                  "device=\"na\" gpubusy_pct=na gpuclk=na"),
                      "star line"); });

        tc.Run("formatPresentLine golden, empty ages read -1", [](TestCase &t)
               {
            ps2x::perflog::PresentStats st;
            st.vblanks = 120;
            st.gsVsyncs = 120;
            st.latches = 60;
            st.latchAvgMs = 0.11;
            st.latchMaxMs = 0.42;
            st.presents = 60;
            st.uframes = 58;
            st.udup = 2;
            st.ageP50Ms = 9.31;
            st.ageMaxMs = 18.74;
            st.sfLatched = 119;
            st.sfLatchShortPct = 98.3;
            st.gsEmpty = 3;
            t.Equals(ps2x::perflog::formatPresentLine(3681, st),
                      std::string("[perf-present] tick=3681 vblanks=120 gs_vsyncs=120 latches=60 "
                                  "latch_ms_avg=0.11 latch_ms_max=0.42 presents=60 uframes=58 udup=2 "
                                  "frame_age_ms_p50=9.31 frame_age_ms_max=18.74 latched=119 latch_short=98.3% "
                                  "gsempty=3"),
                      "present golden");
            ps2x::perflog::PresentStats empty;
            t.Equals(ps2x::perflog::formatPresentLine(60, empty),
                      std::string("[perf-present] tick=60 vblanks=0 gs_vsyncs=0 latches=0 latch_ms_avg=0.00 "
                                  "latch_ms_max=0.00 presents=0 uframes=0 udup=0 frame_age_ms_p50=-1.00 "
                                  "frame_age_ms_max=-1.00 latched=0 latch_short=-1.0% gsempty=0"),
                      "empty ages"); });

        tc.Run("ageP50 is the nearest rank, empty is -1", [](TestCase &t)
               {
            t.Equals(ps2x::perflog::ageP50({}), -1.0, "empty is -1");
            t.Equals(ps2x::perflog::ageP50({9.0}), 9.0, "single");
            t.Equals(ps2x::perflog::ageP50({30.0, 10.0, 20.0}), 20.0, "median of three"); });
    });
}
