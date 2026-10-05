#include "MiniTest.h"

#include "ps2_ssx3_tricky_gems.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace
{

using ps2_tk45c::Gem;
using ps2_tk45c::State;

constexpr uint32_t kRamSize = 0x2000000u;
constexpr uint32_t kG = 0x100000u, kA = 0x110000u, kB = 0x120000u, kR = 0x130000u, kT = 0x140000u;
constexpr uint32_t kInst0 = 0x200000u, kInst1 = 0x210000u;

void w32(std::vector<uint8_t> &ram, uint32_t addr, uint32_t v)
{
    std::memcpy(ram.data() + (addr & 0x01ffffffu), &v, 4);
}

void wf(std::vector<uint8_t> &ram, uint32_t addr, float v)
{
    std::memcpy(ram.data() + (addr & 0x01ffffffu), &v, 4);
}

uint32_t r32(const std::vector<uint8_t> &ram, uint32_t addr)
{
    uint32_t v = 0u;
    std::memcpy(&v, ram.data() + (addr & 0x01ffffffu), 4);
    return v;
}

float rf(const std::vector<uint8_t> &ram, uint32_t addr)
{
    float v = 0;
    std::memcpy(&v, ram.data() + (addr & 0x01ffffffu), 4);
    return v;
}

// Fake race: chain root -> g/a/b/r, B+0xc clock, R+0x110 pos, R+0x790 -> T.
void fakeRace(std::vector<uint8_t> &ram, float px, float py, float pz, uint32_t clock)
{
    w32(ram, 0x4a28a8u, kG);
    w32(ram, kG + 0x84u, kA);
    w32(ram, kA + 0x0cu, kB);
    w32(ram, kB + 0x28u, kR);
    w32(ram, kB + 0x0cu, clock);
    wf(ram, kR + 0x110u, px);
    wf(ram, kR + 0x114u, py);
    wf(ram, kR + 0x118u, pz);
    w32(ram, kR + 0x790u, kT);
    wf(ram, kT + 0x1c4u, 1.0f);
    wf(ram, kT + 0x14u, 0.0f);
    wf(ram, kT + 0x18u, 1.0f);
}

void fakeInstance(std::vector<uint8_t> &ram, uint32_t at, float x, float y, float z)
{
    w32(ram, at, 0x180000u); // link words: mapped, nonzero
    w32(ram, at + 4u, 0x190000u);
    w32(ram, at + 8u, 0x00210123u); // alive marker (lab)
    wf(ram, at + 0x40u, x);
    wf(ram, at + 0x44u, y);
    wf(ram, at + 0x48u, z);
}

const char *kTable2 = "# synthetic three-gem table\n"
                      "TEST 10 1000 0 0 100 2 Gem_Yellow_Test\n"
                      "TEST 20 2000 0 0 100 3 Gem_Orange_Test\n"
                      "TEST 30 3000 0 0 100 2 Gem_Yellow_Test2\n";

bool parse2(std::vector<Gem> &gems)
{
    std::string err;
    return ps2_tk45c::parseTableText(kTable2, gems, err);
}

} // namespace

void register_ps2_ssx3_tricky_gems_tests();

void register_ps2_ssx3_tricky_gems_tests()
{
    MiniTest::Case("Ps2Ssx3TrickyGemsSwept", [](TestCase &tc)
               {
        tc.Run("segment through sphere hits, miss misses", [](TestCase &t)
               {
            const float c[3] = {1000, 0, 0};
            const float a[3] = {900, 0, 0}, b[3] = {1100, 0, 0};
            t.IsTrue(ps2_tk45c::segSphereHit(a, b, c, 100), "through");
            const float m0[3] = {900, 500, 0}, m1[3] = {1100, 500, 0};
            t.IsTrue(!ps2_tk45c::segSphereHit(m0, m1, c, 100), "miss");
            const float i0[3] = {1000, 0, 0}, i1[3] = {1200, 0, 0};
            t.IsTrue(ps2_tk45c::segSphereHit(i0, i1, c, 100), "start inside");
            const float d[3] = {1000, 0, 0};
            t.IsTrue(ps2_tk45c::segSphereHit(d, d, c, 100), "degenerate inside");
            const float e[3] = {1300, 0, 0};
            t.IsTrue(!ps2_tk45c::segSphereHit(e, e, c, 100), "degenerate outside");
            const float n[3] = {900, 0, 0}, f[3] = {1100, 0, 0};
            float bad[3] = {1000, 0, 0};
            bad[0] = std::numeric_limits<float>::quiet_NaN();
            t.IsTrue(!ps2_tk45c::segSphereHit(n, f, bad, 100), "NaN guard");
            t.IsTrue(!ps2_tk45c::segSphereHit(n, f, c, 0), "zero radius");
        }); });

MiniTest::Case("Ps2Ssx3TrickyGemsParse", [](TestCase &tc)
               {
        tc.Run("good parses, bad refuses whole table", [](TestCase &t)
               {
            std::vector<Gem> g;
            std::string err;
            t.IsTrue(ps2_tk45c::parseTableText(kTable2, g, err), "good");
            t.IsTrue(g.size() == 3u, "three rows");
            t.IsTrue(g[0].rid == 10u && g[0].value == 2u && g[1].value == 3u, "fields");
            t.IsTrue(!ps2_tk45c::parseTableText("TEST 1 0 0 0 10 4 Gem_X\n", g, err) && g.empty(),
                     "bad value refuses");
            t.IsTrue(!ps2_tk45c::parseTableText("TEST 1 0 0 0 0 2 Gem_X\n", g, err) && g.empty(),
                     "bad radius refuses");
            t.IsTrue(!ps2_tk45c::parseTableText("TEST 1 0 0 0\n", g, err) && g.empty(), "truncated refuses");
            t.IsTrue(!ps2_tk45c::parseTableText("# only a comment\n", g, err) && g.empty(), "empty refuses");
            t.IsTrue(ps2_tk45c::maskForValue(2) == 1u && ps2_tk45c::maskForValue(3) == 2u &&
                         ps2_tk45c::maskForValue(5) == 4u,
                     "masks");
        }); });

MiniTest::Case("Ps2Ssx3TrickyGemsPoll", [](TestCase &tc)
               {
        tc.Run("pickup writes max, hides, queues with capture", [](TestCase &t)
               {
            std::vector<Gem> gems;
            t.IsTrue(parse2(gems), "table");
            std::vector<uint8_t> ram(kRamSize, 0);
            fakeRace(ram, 850, 0, 0, 100);
            fakeInstance(ram, kInst0, 1000, 0, 0);
            fakeInstance(ram, kInst1, 2000, 0, 0);
            State st;
            st.scanDone = true; // scan covered separately
            st.resolved[0] = kInst0;
            st.resolved[1] = kInst1;
            // Poll 1: prev invalid, no pickup. Poll 2: clock+1 (racing),
            // segment 850 -> 950 crosses gem0 (1000 r100)? No: ends at 950.
            ps2_tk45c::Callout o1 = ps2_tk45c::poll(st, gems, ram.data(), ram.size(), 1000, kR, true);
            t.IsTrue(!o1.fire, "no callout before pickup");
            t.IsTrue(rf(ram, kT + 0x1c4u) == 1.0f, "mult untouched");
            fakeRace(ram, 950, 0, 0, 101);
            ps2_tk45c::poll(st, gems, ram.data(), ram.size(), 1001, kR, true);
            t.IsTrue(rf(ram, kT + 0x1c4u) == 1.0f, "still outside");
            // Poll 3: 950 -> 1050 crosses gem0 -> pickup x2, no capture yet.
            fakeRace(ram, 1050, 0, 0, 102);
            ps2_tk45c::Callout o3 = ps2_tk45c::poll(st, gems, ram.data(), ram.size(), 1002, kR, true);
            t.IsTrue(rf(ram, kT + 0x1c4u) == 2.0f, "mult max-tracks to 2");
            t.IsTrue((r32(ram, kInst0 + 8u) & 0xffu) == 0x05u, "gem hidden");
            t.IsTrue((r32(ram, kInst1 + 8u) & 0xffu) != 0x05u, "other gem untouched");
            t.IsTrue(!o3.fire, "callout skipped without capture");
            // Capture, then cross gem1 (x3): mult 2 -> 3, callout fires.
            // (fakeRace resets the mult; restore the 2.0 to test max-up.)
            uint32_t s[8] = {1, 2, 3, 4, 5, 6, 7, 8};
            t.IsTrue(ps2_tk45c::captureIcons(st, kA, 0xbbbb0000u, s, 3.0f, 1002, ram.data(), ram.size()),
                     "capture validates");
            fakeRace(ram, 1950, 0, 0, 103);
            wf(ram, kT + 0x1c4u, 2.0f);
            ps2_tk45c::Callout o4 = ps2_tk45c::poll(st, gems, ram.data(), ram.size(), 1003, kR, true);
            t.IsTrue(rf(ram, kT + 0x1c4u) == 3.0f, "mult max-tracks to 3");
            t.IsTrue(o4.fire && o4.mask == 2u && o4.ladder == 3.0f, "callout x3");
            t.IsTrue(st.cap.a0 == kA, "capture kept");
            // Cross gem2 (x2) with mult 3: hold, still collected + hidden.
            fakeRace(ram, 2950, 0, 0, 104);
            wf(ram, kT + 0x1c4u, 3.0f);
            fakeInstance(ram, 0x220000u, 3000, 0, 0);
            st.resolved[2] = 0x220000u;
            ps2_tk45c::poll(st, gems, ram.data(), ram.size(), 1004, kR, true);
            fakeRace(ram, 3050, 0, 0, 105);
            wf(ram, kT + 0x1c4u, 3.0f);
            ps2_tk45c::poll(st, gems, ram.data(), ram.size(), 1005, kR, true);
            t.IsTrue(rf(ram, kT + 0x1c4u) == 3.0f, "mult holds at 3");
            t.IsTrue((r32(ram, 0x220000u + 8u) & 0xffu) == 0x05u, "third gem hidden");
        }); });

MiniTest::Case("Ps2Ssx3TrickyGemsGuards", [](TestCase &tc)
               {
        tc.Run("stock/ai/teleport/frozen/reset/bank paths", [](TestCase &t)
               {
            std::vector<Gem> gems;
            t.IsTrue(parse2(gems), "table");
            // Stock: zero writes (mult + instance bytes untouched).
            {
                std::vector<uint8_t> ram(kRamSize, 0);
                fakeRace(ram, 1050, 0, 0, 100);
                fakeInstance(ram, kInst0, 1000, 0, 0);
                State st;
                st.scanDone = true;
                st.resolved[0] = kInst0;
                ps2_tk45c::poll(st, gems, ram.data(), ram.size(), 1000, kR, false);
                ps2_tk45c::poll(st, gems, ram.data(), ram.size(), 1001, kR, false);
                t.IsTrue(rf(ram, kT + 0x1c4u) == 1.0f, "stock mult untouched");
                t.IsTrue((r32(ram, kInst0 + 8u) & 0xffu) != 0x05u, "stock hide untouched");
            }
            // AI pass (a0 != R): no pickup even inside the sphere.
            {
                std::vector<uint8_t> ram(kRamSize, 0);
                fakeRace(ram, 1050, 0, 0, 100);
                State st;
                st.scanDone = true;
                ps2_tk45c::poll(st, gems, ram.data(), ram.size(), 1000, 0x999999u, true);
                t.IsTrue(rf(ram, kT + 0x1c4u) == 1.0f, "ai pass ignored");
            }
            // Teleport: jump over the gem collects nothing, re-scan armed.
            {
                std::vector<uint8_t> ram(kRamSize, 0);
                fakeRace(ram, 500, 0, 0, 100);
                State st;
                st.scanDone = true;
                ps2_tk45c::poll(st, gems, ram.data(), ram.size(), 1000, kR, true);
                fakeRace(ram, 500, 0, 0, 101);
                ps2_tk45c::poll(st, gems, ram.data(), ram.size(), 1001, kR, true);
                fakeRace(ram, 5000, 0, 0, 102); // 4500-unit jump over gem0
                ps2_tk45c::poll(st, gems, ram.data(), ram.size(), 1002, kR, true);
                t.IsTrue(rf(ram, kT + 0x1c4u) == 1.0f, "teleport collects nothing");
                t.IsTrue(!st.scanDone && st.rehideArmed, "teleport re-arms scan");
            }
            // Frozen clock (menus/pause): no pickup.
            {
                std::vector<uint8_t> ram(kRamSize, 0);
                fakeRace(ram, 1050, 0, 0, 100);
                State st;
                st.scanDone = true;
                for (uint64_t i = 0u; i < 6u; ++i)
                    ps2_tk45c::poll(st, gems, ram.data(), ram.size(), 1000 + i, kR, true);
                t.IsTrue(rf(ram, kT + 0x1c4u) == 1.0f, "frozen clock blocks");
            }
            // New race (R change): bitmap resets, pendingClear fires.
            {
                std::vector<uint8_t> ram(kRamSize, 0);
                fakeRace(ram, 1050, 0, 0, 100);
                State st;
                st.scanDone = true;
                ps2_tk45c::poll(st, gems, ram.data(), ram.size(), 1000, kR, true);
                // Simulate a stale multiplier + collected gem, then a new R.
                wf(ram, kT + 0x1c4u, 5.0f);
                st.collected[0] = 1u;
                w32(ram, kB + 0x28u, kR + 0x1000u); // R reallocates
                w32(ram, kR + 0x1000u + 0x790u, kT);
                wf(ram, kR + 0x1000u + 0x110u, 1050);
                wf(ram, kR + 0x1000u + 0x114u, 0);
                wf(ram, kR + 0x1000u + 0x118u, 0);
                w32(ram, kB + 0x0cu, 101);
                ps2_tk45c::poll(st, gems, ram.data(), ram.size(), 1001, kR + 0x1000u, true);
                t.IsTrue(st.collected[0] == 0u, "bitmap resets per race");
                t.IsTrue(rf(ram, kT + 0x1c4u) == 1.0f, "stale clears on new race");
            }
            // Bank-clear: 3 -> 1, 1 untouched, NaN refused.
            {
                std::vector<uint8_t> ram(kRamSize, 0);
                wf(ram, kT + 0x1c4u, 3.0f);
                ps2_tk45c::onBankClear(ram.data(), ram.size(), 2000, kT, 100);
                t.IsTrue(rf(ram, kT + 0x1c4u) == 1.0f, "bank clears");
                ps2_tk45c::onBankClear(ram.data(), ram.size(), 2001, kT, 101);
                t.IsTrue(rf(ram, kT + 0x1c4u) == 1.0f, "clean no-op");
                wf(ram, kT + 0x1c4u, std::numeric_limits<float>::quiet_NaN());
                ps2_tk45c::onBankClear(ram.data(), ram.size(), 2002, kT, 102);
                t.IsTrue(std::isnan(rf(ram, kT + 0x1c4u)), "NaN refused");
                ps2_tk45c::onBankClear(ram.data(), ram.size(), 2003, 0u, 103);
                ps2_tk45c::onBankClear(ram.data(), ram.size(), 2004, kT + 1u, 104);
                t.IsTrue(true, "bad T survives");
                // Full-span guard: T ending past RAM refuses (no OOB write).
                wf(ram, kRamSize - 4u, 3.0f);
                ps2_tk45c::onBankClear(ram.data(), ram.size(), 2005, kRamSize - 4u, 105);
                t.IsTrue(rf(ram, kRamSize - 4u) == 3.0f, "short span refuses");
            }
            // Capture validation: null/unmapped a0 refused, null a1 allowed.
            {
                std::vector<uint8_t> ram(kRamSize, 0);
                uint32_t s[8] = {0};
                State st;
                t.IsTrue(!ps2_tk45c::captureIcons(st, 0u, 0u, s, 0.0f, 100, ram.data(), ram.size()) &&
                             !st.cap.ok,
                         "null a0 refused");
                t.IsTrue(!ps2_tk45c::captureIcons(st, 0xaaaa0000u, 0u, s, 0.0f, 101, ram.data(),
                                                 ram.size()) &&
                             !st.cap.ok,
                         "unmapped a0 refused");
                t.IsTrue(ps2_tk45c::captureIcons(st, kA, 0u, s, 0.0f, 102, ram.data(), ram.size()) &&
                             st.cap.ok && st.cap.a1 == 0u,
                         "null a1 allowed");
            }
        }); });

MiniTest::Case("Ps2Ssx3TrickyGemsScan", [](TestCase &tc)
               {
        tc.Run("position scan resolves, ambiguity fails closed", [](TestCase &t)
               {
            std::vector<Gem> gems;
            t.IsTrue(parse2(gems), "table");
            std::vector<uint8_t> ram(kRamSize, 0);
            fakeInstance(ram, kInst0, 1000, 0, 0);
            fakeInstance(ram, kInst1, 2000, 0, 0);
            State st;
            for (int i = 0; i < 40 && !st.scanDone; ++i)
                ps2_tk45c::scanChunk(st, gems, ram.data(), ram.size(), 3000, 100);
            t.IsTrue(st.scanDone, "scan completes");
            t.IsTrue(st.resolved[0] == kInst0 && st.resolved[1] == kInst1 && st.resolved[2] == 0u,
                     "two resolved, third absent");
            // A second struct sharing gem0's position -> ambiguous -> 0.
            fakeInstance(ram, 0x220000u, 1000, 0, 0);
            State st2;
            for (int i = 0; i < 40 && !st2.scanDone; ++i)
                ps2_tk45c::scanChunk(st2, gems, ram.data(), ram.size(), 3000, 100);
            t.IsTrue(st2.resolved[0] == 0u && st2.resolved[1] == kInst1, "ambiguity fails closed");
        }); });
}
