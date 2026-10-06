#include "MiniTest.h"
#include "ps2_session_telemetry.h"

#include <cstring>
#include <string>
#include <vector>

using namespace ps2x::tel;

void register_ps2_session_telemetry_tests()
{
    MiniTest::Case("Ps2SessionTelemetry", [](TestCase &tc)
                   {
        tc.Run("race tracker: start, stop, resume, reset end", [](TestCase &t)
               {
            RaceTracker r;
            t.Equals(r.step(100, 0x800000u, true, 0u), static_cast<uint32_t>(kEdgeNone), "first sight adopts");
            t.Equals(r.step(101, 0x800000u, true, 0u), static_cast<uint32_t>(kEdgeNone), "frozen at gate");
            t.Equals(r.step(200, 0x800000u, true, 1u), static_cast<uint32_t>(kEdgeStart), "GO starts");
            t.Equals(r.startTick, static_cast<uint64_t>(200u), "start tick");
            for (uint64_t k = 201; k < 400; ++k)
                t.Equals(r.step(k, 0x800000u, true, static_cast<uint32_t>(k - 199u)),
                         static_cast<uint32_t>(kEdgeNone), "running");
            // Clock last changed at 399 (value 200); freeze it.
            uint32_t e = kEdgeNone;
            uint64_t k = 400;
            for (; k < 400 + RaceTracker::kStopTicks + 2u && e == kEdgeNone; ++k)
                e = r.step(k, 0x800000u, true, 200u);
            t.Equals(e, static_cast<uint32_t>(kEdgeStop), "freeze stops");
            t.Equals(r.stopClock, 200u, "stop clock");
            t.Equals(r.stopTick, static_cast<uint64_t>(399u), "stop at last change");
            t.Equals(r.step(k + 10, 0x800000u, true, 201u), static_cast<uint32_t>(kEdgeResume), "unpause resumes");
            t.Equals(r.step(k + 11, 0x800000u, true, 0u), static_cast<uint32_t>(kEdgeEnd), "restart resets");
            t.IsTrue(std::strcmp(r.endReason, "reset") == 0, "reset reason");
            t.Equals(r.step(k + 12, 0x800000u, true, 0u), static_cast<uint32_t>(kEdgeNone), "idle at 0");
            t.Equals(r.step(k + 50, 0x800000u, true, 1u), static_cast<uint32_t>(kEdgeStart), "next GO");
            t.Equals(r.step(k + 51, 0x900000u, true, 5u), static_cast<uint32_t>(kEdgeEnd), "object change ends");
            t.IsTrue(std::strcmp(r.endReason, "object") == 0, "object reason");
            t.Equals(r.step(k + 52, 0x900000u, true, 6u), static_cast<uint32_t>(kEdgeStart), "new object runs");
            t.Equals(r.step(k + 53, 0u, false, 0u), static_cast<uint32_t>(kEdgeEnd), "object gone ends");
            t.IsTrue(std::strcmp(r.endReason, "gone") == 0, "gone reason");
            t.Equals(r.step(k + 54, 0u, false, 0u), static_cast<uint32_t>(kEdgeNone), "stays idle"); });

        tc.Run("race tracker: menus never start (frozen or backwards clock)", [](TestCase &t)
               {
            RaceTracker r;
            uint32_t edges = 0u;
            for (uint64_t k = 0; k < 500; ++k)
                edges |= r.step(k, 0x700000u, true, 77u);
            edges |= r.step(500, 0x700000u, true, 3u); // backwards while idle
            t.Equals(edges, static_cast<uint32_t>(kEdgeNone), "no edges");
            t.IsTrue(r.st == RaceTracker::Idle, "idle"); });

        tc.Run("error filter and budget", [](TestCase &t)
               {
            t.IsTrue(isErrorLine("[mtvu] FATAL: unit job threw"), "FATAL");
            t.IsTrue(isErrorLine("fh1-full120-refused PS2X_SSX3_FULL120=x"), "refused");
            t.IsTrue(isErrorLine("bad JALR target 0x0"), "JALR");
            t.IsFalse(isErrorLine("[perf] wall=x"), "perf line");
            t.IsFalse(isErrorLine("[tel] coverage write failed FATAL"), "own notes skipped");
            t.IsFalse(isErrorLine(""), "empty");
            t.IsFalse(isErrorLine(nullptr), "null");
            ErrorBudget b;
            uint32_t n = 0u;
            t.IsTrue(b.admit("FATAL tick=1", n) && n == 1u, "first");
            t.IsTrue(b.admit("FATAL tick=22", n) && n == 2u, "digits fold to one key");
            t.IsTrue(b.admit("FATAL tick=333", n) && n == 3u, "third");
            t.IsFalse(b.admit("FATAL tick=4444", n), "fourth suppressed");
            t.Equals(n, 4u, "still counted");
            t.Equals(b.suppressed, static_cast<uint64_t>(1u), "suppressed count");
            for (int i = 0; i < 400; ++i)
            {
                char line[32];
                std::snprintf(line, sizeof(line), "JALR key %c%c", 'a' + i % 26, 'a' + (i / 26) % 26);
                b.admit(line, n);
            }
            t.Equals(b.printed, ErrorBudget::kTotal, "total cap"); });

        tc.Run("hitch budget: burst then one per refill", [](TestCase &t)
               {
            HitchBudget h;
            int printed = 0;
            for (int i = 0; i < 100; ++i)
                printed += h.admit(10.0 + i * 0.01) ? 1 : 0;
            t.Equals(printed, static_cast<int>(HitchBudget::kBurst), "burst");
            t.IsTrue(h.dropped == 90u, "dropped counted");
            t.IsFalse(h.admit(11.0 + HitchBudget::kRefillS * 0.5), "half refill not enough");
            t.IsTrue(h.admit(11.0 + HitchBudget::kRefillS * 1.1), "one token after refill");
            // An hour of 1 Hz hitches stays near 60 + burst lines.
            HitchBudget hh;
            int hour = 0;
            for (int s = 0; s < 3600; ++s)
                hour += hh.admit(static_cast<double>(s)) ? 1 : 0;
            t.IsTrue(hour <= 71 && hour >= 65, "hourly cap"); });

        tc.Run("hitch stages: waits are not the top stage", [](TestCase &t)
               {
            const StageMax st[] = {{"ee.busy", 12.5}, {"ee.wait", 60.0}, {"gs.busy", 55.25}, {"gpu", -1.0},
                                   {"ee.mtvu", 80.0}, {"mtvu.busy", 4.9}};
            std::string top;
            const std::string s = formatHitchStages(st, 5, top);
            t.IsTrue(top == "gs.busy", "top work stage");
            t.IsTrue(s == "ee.busy=12.5 ee.wait=60.0 gs.busy=55.2 ee.mtvu=80.0" ||
                         s == "ee.busy=12.5 ee.wait=60.0 gs.busy=55.3 ee.mtvu=80.0",
                     "cells skip unfired and sub-5 ms stages"); });

        tc.Run("coverage format: hits, overrides, CT1 sets, end marker", [](TestCase &t)
               {
            std::vector<uint8_t> bytes(16, 0u);
            bytes[0] = 1u;
            bytes[5] = 1u;
            CoverageMeta m;
            m.session = "s1";
            m.build = "abc";
            m.envHash = 0x1234u;
            m.reason = "periodic";
            m.tick = 99u;
            m.wall = "2026-10-05T00:00:00Z";
            m.uptimeS = 60u;
            const std::string out = formatCoverage(m, 0x100008u, bytes.data(), 16u, {0x100008u, 0x10000cu},
                                                   {{0x200000u, 2u}}, {{0x7cu, 1u}},
                                                   {{(0x80000001ull << 32u) | 3u, 4u}});
            const std::string want = "# ps2x-coverage v1\n"
                                     "session=s1 build=abc env_hash=0000000000001234\n"
                                     "table base=0x100008 slots=16\n"
                                     "dump reason=periodic tick=99 wall=2026-10-05T00:00:00Z uptime_s=60\n"
                                     "hits=2\n"
                                     "h 100008\n"
                                     "h 10001c\n"
                                     "ov 100008 ran=1\n"
                                     "ov 10000c ran=0\n"
                                     "missing 200000 hits=2\n"
                                     "syscall 7c hits=1\n"
                                     "rpc 80000001 3 hits=4\n"
                                     "end\n";
            t.IsTrue(out == want, "exact text"); });

        tc.Run("coverPc maps PCs onto slots and ignores out-of-range", [](TestCase &t)
               {
            // The suite never calls init(); exercise the hot path on a local map.
            static std::atomic<uint8_t> map[8];
            for (auto &b : map)
                b.store(0u);
            auto *saved = g_covBytes.load();
            const uint32_t sb = g_covBase, ss = g_covSlots;
            g_covBase = 0x100000u;
            g_covSlots = 8u;
            g_covBytes.store(map);
            coverPc(0x100000u);
            coverPc(0x10001cu);
            coverPc(0x100020u); // past the end
            coverPc(0x0ffffcu); // below base
            coverSlot(3u);
            coverSlot(8u); // out of range
            g_covBytes.store(saved);
            g_covBase = sb;
            g_covSlots = ss;
            t.Equals(static_cast<int>(map[0].load()), 1, "slot 0");
            t.Equals(static_cast<int>(map[7].load()), 1, "slot 7");
            t.Equals(static_cast<int>(map[3].load()), 1, "slot 3");
            int total = 0;
            for (auto &b : map)
                total += b.load();
            t.Equals(total, 3, "nothing else"); });

        tc.Run("guestStr bounds and sanitizes", [](TestCase &t)
               {
            uint8_t ram[16] = {'S', 'n', 'o', 'w', '"', 1, 0, 'x'};
            t.IsTrue(guestStr(ram, sizeof(ram), 0u, 32u) == "Snow??", "stops at NUL, sanitizes");
            t.IsTrue(guestStr(ram, sizeof(ram), 14u, 8u).size() <= 2u, "clamped at ram end");
            t.IsTrue(guestStr(ram, sizeof(ram), 100u, 8u).empty(), "out of range"); }); });
}
