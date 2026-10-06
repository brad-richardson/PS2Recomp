#include "MiniTest.h"
#include "ps2_session_telemetry.h"

#include <cstring>
#include <cmath>
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

        tc.Run("TLG1: replay TEL3 session edges", [](TestCase &t)
               {
            // session-20261005-213843-27149.log SHA256 e9523753691cf15e…
            // Full logged edge sequence; no interpolation is claimed as guest evidence.
            struct Row { double wall; uint64_t tick; const char *edge; unsigned n, event, mode, clock; bool reset, stopped; };
            const Row rows[] = {
                {27.3, 1620u, "start", 1u, 17u, 4u, 1u, false, false},
                {29.2, 1822u, "stop", 1u, 0u, 0u, 87u, false, false},
                {36.5, 2268u, "end", 1u, 0u, 0u, 0u, true, true},
                {53.1, 3276u, "start", 2u, 5u, 4u, 1u, false, false},
                {59.5, 4052u, "end", 2u, 0u, 0u, 0u, true, false},
                {226.6, 14218u, "start", 3u, 5u, 1u, 1u, false, false},
                {377.7, 32568u, "stop", 3u, 0u, 0u, 9161u, false, false},
                {383.9, 32946u, "end", 3u, 0u, 0u, 0u, true, true},
                {387.3, 33153u, "start", 4u, 5u, 1u, 1u, false, false},
                {416.4, 36693u, "end", 4u, 0u, 0u, 0u, true, false},
                {420.6, 36949u, "start", 5u, 5u, 1u, 1u, false, false},
                {571.1, 55225u, "stop", 5u, 0u, 0u, 9124u, false, false},
                {579.0, 55708u, "end", 5u, 0u, 0u, 0u, true, true},
                {582.0, 55892u, "start", 6u, 5u, 1u, 9124u, false, false},
                {582.1, 55893u, "end", 6u, 0u, 0u, 0u, true, false},
                {595.4, 56706u, "start", 7u, 8u, 4u, 1u, false, false},
                {601.1, 57393u, "end", 7u, 0u, 0u, 0u, true, false},
                {654.4, 60638u, "start", 8u, 8u, 2u, 1u, false, false},
                {690.0, 64940u, "stop", 8u, 0u, 0u, 2137u, false, false},
                {696.2, 65317u, "end", 8u, 0u, 0u, 0u, true, true},
                {699.2, 65498u, "start", 9u, 8u, 2u, 1u, false, false},
                {701.3, 65756u, "end", 9u, 0u, 0u, 0u, true, false},
                {705.3, 65996u, "start", 10u, 8u, 2u, 1u, false, false},
                {741.4, 70360u, "stop", 10u, 0u, 0u, 2168u, false, false},
                {746.6, 70677u, "end", 10u, 0u, 0u, 0u, true, true},
                {749.5, 70858u, "start", 11u, 8u, 2u, 1u, false, false},
                {751.1, 71054u, "end", 11u, 0u, 0u, 0u, true, false},
                {768.3, 72096u, "start", 12u, 11u, 4u, 1u, false, false},
                {774.3, 72827u, "end", 12u, 0u, 0u, 0u, true, false},
                {806.2, 74766u, "start", 13u, 11u, 3u, 1u, false, false},
                {892.7, 85266u, "stop", 13u, 0u, 0u, 5236u, false, false},
                {898.9, 85644u, "end", 13u, 0u, 0u, 0u, true, true},
                {901.9, 85824u, "start", 14u, 11u, 3u, 1u, false, false},
                {903.6, 86036u, "end", 14u, 0u, 0u, 0u, true, false},
                {907.6, 86280u, "start", 15u, 11u, 3u, 1u, false, false},
                {986.2, 95816u, "stop", 15u, 0u, 0u, 4754u, false, false},
                {992.9, 96221u, "end", 15u, 0u, 0u, 0u, true, true},
                {995.8, 96401u, "start", 16u, 11u, 3u, 1u, false, false},
                {998.5, 96719u, "end", 16u, 0u, 0u, 0u, true, false},
                {1012.8, 97591u, "start", 17u, 14u, 5u, 1u, false, false},
                {1102.1, 108431u, "stop", 17u, 0u, 0u, 5406u, false, false},
                {1104.7, 108587u, "end", 17u, 0u, 0u, 0u, true, true},
                {1105.5, 108636u, "start", 18u, 14u, 5u, 1u, false, false},
                {1293.4, 131464u, "stop", 18u, 0u, 0u, 11400u, false, false},
                {1297.1, 131688u, "end", 18u, 0u, 0u, 0u, true, true},
                {1297.2, 131696u, "start", 19u, 14u, 5u, 5u, false, false},
                {1302.5, 132321u, "end", 19u, 0u, 0u, 0u, true, false},
                {1376.6, 136828u, "start", 20u, 14u, 6u, 1u, false, false},
                {1572.5, 160628u, "stop", 20u, 0u, 0u, 11886u, false, false},
                {1575.9, 160840u, "end", 20u, 0u, 0u, 0u, true, true},
                {1576.1, 160848u, "start", 21u, 14u, 6u, 5u, false, false},
                {1580.4, 161356u, "end", 21u, 0u, 0u, 0u, true, false},
                {1694.8, 168316u, "start", 22u, 14u, 6u, 1u, false, false},
                {2128.0, 220990u, "stop", 22u, 0u, 0u, 26323u, false, false},
                {2358.7, 235017u, "end", 22u, 0u, 0u, 0u, false, true},
            };
            SegmentLabels labels;
            GuestRaceState unlogged; // TEL3 logged neither finished nor pause-menu presence.
            unsigned freeRideRuns = 0u;
            uint64_t count = 0u;
            for (const auto &row : rows)
            {
                if (std::strcmp(row.edge, "start") == 0)
                {
                    labels.start(row.tick, row.clock, unlogged);
                    if (row.n == 2u || row.n == 7u || row.n == 12u)
                    {
                        t.Equals(row.mode, 4u, "Brad-confirmed Free Ride mode");
                        t.IsTrue(std::strcmp(labels.end(row.tick + 30u).kind, "run") == 0,
                                 "Free Ride is a normal run, never pre/flyover");
                        ++freeRideRuns;
                    }
                }
                else if (std::strcmp(row.edge, "stop") == 0) labels.stop(unlogged);
                else
                {
                    const auto out = labels.end(row.tick);
                    t.IsTrue(std::strcmp(out.outcome, "unknown") == 0,
                             "historical log cannot prove finished versus aborted");
                    t.IsTrue(std::strcmp(out.kind, "pre") != 0, "no mode-based pre label");
                    if (row.n == 6u || row.n == 19u || row.n == 21u)
                        t.IsTrue(out.pending, "missing guest flags cannot exclude pause/finish for a blip");
                    ++count;
                }
            }
            t.Equals(freeRideRuns, 3u, "all confirmed campaign transport rides");
            t.Equals(count, uint64_t(22), "all logged segments replayed"); });

        tc.Run("TLG1: guest pause predicate, session chain, and mode table", [](TestCase &t)
               {
            std::vector<uint8_t> ram(8u * 1024u * 1024u, 0u);
            const auto put = [&](uint32_t addr, uint32_t word) { std::memcpy(ram.data() + addr, &word, 4u); };
            const uint32_t root = 0x600000u, app = 0x601000u, session = 0x602000u, ui = 0x603000u;
            const uint32_t tail1 = 0x604000u, tail2 = 0x604100u, node = 0x604200u;
            put(0x4a28a8u, root); put(root + 0x84u, app); put(app + 0x28u, session);
            put(app + 0x48u, ui);
            put(ui + 0x1cu, tail1); put(tail1, tail1); put(tail1 + 4u, tail1);
            put(ui + 0x38u, tail2); put(tail2, tail2); put(tail2 + 4u, tail2);
            auto g = readGuestRaceState(ram.data(), ram.size());
            t.IsTrue(g.session == session && g.finishedKnown && !g.finished, "session+0x610 readable zero");
            t.IsTrue(g.pauseKnown && !g.paused, "empty UI lists are not a pause");
            PauseEdges pause;
            t.IsTrue(pause.step(g) == nullptr, "adopt unpaused");
            // Guest 0x317618 hash for the literal at 0x46F818 (pause template).
            uint32_t hash = 0u;
            for (const char *c = "cOVTemplate_PauseMenu"; *c; ++c)
            {
                hash = (hash << 4) + uint32_t(*c);
                const uint32_t high = hash & 0xf0000000u;
                if (high) hash ^= (high >> 23) ^ high;
            }
            put(ui + 0x1cu, node); put(node, tail1); put(node + 4u, tail1); put(node + 0xcu, hash);
            g = readGuestRaceState(ram.data(), ram.size());
            t.IsTrue(g.pauseKnown && g.paused, "guest pause template in first list");
            t.IsTrue(std::strcmp(pause.step(g), "pause") == 0, "pause-menu rising edge");
            t.IsTrue(pause.step(g) == nullptr, "no repeated edge");
            put(ui + 0x1cu, tail1); put(ui + 0x38u, node); put(node, tail2); put(node + 4u, tail2);
            t.IsTrue(readGuestRaceState(ram.data(), ram.size()).paused, "guest searches second list too");
            put(ui + 0x38u, tail2);
            g = readGuestRaceState(ram.data(), ram.size());
            t.IsTrue(std::strcmp(pause.step(g), "resume") == 0, "pause template removed");
            put(session + 0x610u, 1u);
            g = readGuestRaceState(ram.data(), ram.size());
            t.IsTrue(g.finished && !g.paused, "finish is distinct from pause");
            put(ui + 0x1cu, node); put(node + 4u, node + 0x20u);
            put(node + 0x20u, node); put(node + 0x24u, node); put(node + 0xcu, 0u);
            t.IsFalse(readGuestRaceState(ram.data(), ram.size()).pauseKnown, "corrupt cycle is bounded/unknown");
            put(app + 0x48u, 0x007fffffu);
            t.IsFalse(readGuestRaceState(ram.data(), ram.size()).pauseKnown, "invalid UI pointer");
            put(app + 0x28u, 0x007ffffcu);
            t.IsFalse(readGuestRaceState(ram.data(), ram.size()).finishedKnown, "out of range finished word");
            std::memcpy(ram.data() + 0x43e7d0u + 4u * 60u, "Free Ride", 10u);
            t.IsTrue(readModeName(ram.data(), ram.size(), 4u) == "Free Ride", "mode name from guest table");
            t.IsTrue(readModeName(ram.data(), ram.size(), 255u) == "?", "invalid signed mode rejected"); });

        tc.Run("TLG1: guest lifecycle overrides short/nonzero clock guesses", [](TestCase &t)
               {
            const GuestRaceState live{0x602000u, true, false, true, false};
            auto fin = live; fin.finished = true;
            auto paused = live; paused.paused = true;
            SegmentLabels labels;
            labels.start(100u, 99u, live); labels.stop(fin);
            auto out = labels.end(101u);
            t.IsTrue(std::strcmp(out.kind, "run") == 0 && std::strcmp(out.outcome, "finished") == 0,
                     "guest finish explains a short nonzero-clock segment");
            labels.start(100u, 99u, live); labels.stop(paused);
            t.IsTrue(std::strcmp(labels.end(101u).kind, "run") == 0, "pause explains a short segment");
            labels.start(100u, 1u, live); labels.stop(live);
            t.IsTrue(std::strcmp(labels.end(200u).kind, "aborted") == 0, "unfinished stop");
            labels.start(100u, 99u, live);
            t.IsTrue(std::strcmp(labels.end(200u).kind, "post") == 0, "unexplained nonzero-clock segment");
            t.IsTrue(std::strcmp(labels.end(101u).kind, "glitch") == 0, "unexplained short segment"); });

        tc.Run("TLG1: interior windows, pause, rate transition and locked lost time", [](TestCase &t)
               {
            RunRates rates;
            rates.start(100);
            rates.stop(300); // true frozen clock tick, not grace-expiry tick
            rates.start(400);
            rates.stop(500); // rate flip
            rates.start(501);
            rates.stop(900);
            rates.windows = {{90, 210, 1.0, 121.7, 120.0}, // start edge
                             {150, 270, 1.0, 121.7, 120.0}, // interior, 1.7 missing ticks
                             {270, 390, 1.0, 121.7, 120.0}, // stop edge
                             {390, 450, 1.0, 60.85, 60.0}, // resume edge
                             {450, 570, 1.0, 121.7, 120.0}, // rate edge
                             {600, 720, 1.0, 121.7, 120.0}};
            auto out = rates.summary();
            t.Equals(out.count, uint64_t(2), "only wholly Running windows");
            t.IsTrue(out.mean == 120.0 && out.min == 120.0, "edge rate not included");
            t.IsTrue(std::abs(out.lostMs - 3400.0 / 121.7) < 0.00001, "deficit divided by locked rate");
            rates.windows = {{600, 720, 1.0, 0.0, 120.0}};
            t.IsTrue(rates.summary().lostMs == 0.0, "unknown lock rate contributes no claimed loss"); });

        tc.Run("error filter and budget", [](TestCase &t)
               {
            t.IsTrue(isErrorLine("[mtvu] FATAL: unit job threw"), "FATAL");
            t.IsTrue(isErrorLine("fh1-full120-refused PS2X_SSX3_FULL120=x"), "refused");
            t.IsTrue(isErrorLine("bad JALR target 0x0"), "JALR");
            t.IsFalse(isErrorLine("[perf] wall=x"), "perf line");
            t.IsFalse(isErrorLine("[tel] coverage write failed FATAL"), "own notes skipped");
            t.IsFalse(isErrorLine(""), "empty");
            t.IsFalse(isErrorLine(nullptr), "null");
            // ACH4: "refused" is a word, never a key=value stat name.
            t.IsFalse(isErrorLine("[present-vk] window-change stats vk_queued=0 vk_dropped=0 vk_skipped=0 "
                                  "vk_callbacks=0 vk_release_timeouts=0 vk_cb_timeouts=0 vk_fence_timeouts=0 "
                                  "vk_fence_errors=0 vk_eintr=0 vk_stale_cb=0 vk_absent_cb=0 vk_refused=0 "
                                  "vk_layers=1 vk_layers_detached=0 vk_layers_made=1 vk_layers_released=0 "
                                  "vk_bufs=0 vk_bufs_released=0 vk_fences_held=0 vk_tokens=0 "
                                  "vk_release_wait_ms_avg=0.000 vk_apply_ms_avg=0.000 "
                                  "vk_latch_interval_ms_avg=0.00 proc_fds=128"),
                       "present-vk window-change stats (vk_refused=0) is not an error");
            t.IsFalse(isErrorLine("[gs] periodic appends=3 refused=0 sunk=1"), "bare refused= key");
            t.IsTrue(isErrorLine("[ssx3-tricky] refused: no function at 0x100; nothing wrapped"), "refused:");
            t.IsTrue(isErrorLine("[cd-overlay] REFUSED: PS2X_CD_OVERLAY needs PS2X_CD_IMAGE"), "REFUSED:");
            t.IsTrue(isErrorLine("[savestate] quick-load refused: build changed"), "quick-load refused");
            t.IsTrue(isErrorLine("[mtvu] gif-stage refused"), "refused at end of line");
            t.IsTrue(isErrorLine("Refused"), "whole line");
            t.IsTrue(isErrorLine("libc: terminate called after throwing"), "terminate");
            t.IsTrue(isErrorLine("Fatal signal 6 (SIGABRT) Abort message"), "Abort");
            t.IsTrue(isErrorLine("Fatal signal 11 (SIGSEGV)"), "SIGSEGV");
            t.IsFalse(isErrorLine("unrefusedly fine"), "inside a word");
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
