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

// TK55: an upright gem basis (Mesa rid 540's authored rows), w lanes marked.
void fakeMatrix(std::vector<uint8_t> &ram, uint32_t at, const float r0[3], const float r1[3], const float r2[3])
{
    for (int k = 0; k < 3; ++k)
    {
        wf(ram, at + 0x10u + 4u * k, r0[k]);
        wf(ram, at + 0x20u + 4u * k, r1[k]);
        wf(ram, at + 0x30u + 4u * k, r2[k]);
    }
    w32(ram, at + 0x1cu, 0x11111111u);
    w32(ram, at + 0x2cu, 0x22222222u);
    w32(ram, at + 0x3cu, 0x33333333u);
}

double dot3(const float a[3], const float b[3])
{
    return double(a[0]) * b[0] + double(a[1]) * b[1] + double(a[2]) * b[2];
}

bool near(double a, double b, double eps = 1e-5)
{
    return std::fabs(a - b) <= eps;
}

const float kGemR0[3] = {-0.15126082f, -0.98849386f, 0.0f};
const float kGemR1[3] = {0.98849386f, -0.15126082f, 0.0f};
const float kGemR2[3] = {0.0f, 0.0f, 1.0f};

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
        }); });

MiniTest::Case("Ps2Ssx3TrickyGemsPoll", [](TestCase &tc)
               {
        tc.Run("pickup writes max and hides", [](TestCase &t)
               {
            std::vector<Gem> gems;
            t.IsTrue(parse2(gems), "table");
            std::vector<uint8_t> ram(kRamSize, 0);
            fakeRace(ram, 850, 0, 0, 100);
            fakeInstance(ram, kInst0, 1000, 0, 0);
            fakeInstance(ram, kInst1, 2000, 0, 0);
            State st;
            st.scanDone = true; // scan covered separately
            st.resolved[0][0] = kInst0;
            st.nres[0] = 1u;
            st.resolved[1][0] = kInst1;
            st.nres[1] = 1u;
            // Poll 1: prev invalid, no pickup. Poll 2: clock+1 (racing),
            // segment 850 -> 950 crosses gem0 (1000 r100)? No: ends at 950.
            ps2_tk45c::poll(st, gems, ram.data(), ram.size(), 1000, kR, true);
            t.IsTrue(rf(ram, kT + 0x1c4u) == 1.0f, "mult untouched");
            fakeRace(ram, 950, 0, 0, 101);
            ps2_tk45c::poll(st, gems, ram.data(), ram.size(), 1001, kR, true);
            t.IsTrue(rf(ram, kT + 0x1c4u) == 1.0f, "still outside");
            // Poll 3: 950 -> 1050 crosses gem0 -> pickup x2.
            fakeRace(ram, 1050, 0, 0, 102);
            ps2_tk45c::poll(st, gems, ram.data(), ram.size(), 1002, kR, true);
            t.IsTrue(rf(ram, kT + 0x1c4u) == 2.0f, "mult max-tracks to 2");
            t.IsTrue((r32(ram, kInst0 + 8u) & 0xffu) == 0x05u, "gem hidden");
            t.IsTrue((r32(ram, kInst1 + 8u) & 0xffu) != 0x05u, "other gem untouched");
            // Cross gem1 (x3): mult 2 -> 3.
            // (fakeRace resets the mult; restore the 2.0 to test max-up.)
            fakeRace(ram, 1950, 0, 0, 103);
            wf(ram, kT + 0x1c4u, 2.0f);
            ps2_tk45c::poll(st, gems, ram.data(), ram.size(), 1003, kR, true);
            t.IsTrue(rf(ram, kT + 0x1c4u) == 3.0f, "mult max-tracks to 3");
            // Cross gem2 (x2) with mult 3: hold, still collected + hidden.
            fakeRace(ram, 2950, 0, 0, 104);
            wf(ram, kT + 0x1c4u, 3.0f);
            fakeInstance(ram, 0x220000u, 3000, 0, 0);
            st.resolved[2][0] = 0x220000u;
            st.nres[2] = 1u;
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
                st.resolved[0][0] = kInst0;
                st.nres[0] = 1u;
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
            // New race (R change): bitmap resets, pendingClear fires, hidden
            // gems show again (world NOT reloaded: same instances).
            {
                std::vector<uint8_t> ram(kRamSize, 0);
                fakeRace(ram, 1050, 0, 0, 100);
                fakeInstance(ram, kInst0, 1000, 0, 0);
                State st;
                st.scanDone = true;
                st.resolved[0][0] = kInst0;
                st.nres[0] = 1u;
                ps2_tk45c::poll(st, gems, ram.data(), ram.size(), 1000, kR, true);
                // Simulate a stale multiplier + collected+hidden gem, then a new R.
                wf(ram, kT + 0x1c4u, 5.0f);
                st.collected[0] = 1u;
                w32(ram, kInst0 + 8u, 0x00010005u); // hidden world gem
                w32(ram, kB + 0x28u, kR + 0x1000u); // R reallocates
                w32(ram, kR + 0x1000u + 0x790u, kT);
                wf(ram, kR + 0x1000u + 0x110u, 1050);
                wf(ram, kR + 0x1000u + 0x114u, 0);
                wf(ram, kR + 0x1000u + 0x118u, 0);
                w32(ram, kB + 0x0cu, 101);
                ps2_tk45c::poll(st, gems, ram.data(), ram.size(), 1001, kR + 0x1000u, true);
                t.IsTrue(st.collected[0] == 0u, "bitmap resets per race");
                t.IsTrue(rf(ram, kT + 0x1c4u) == 1.0f, "stale clears on new race");
                t.IsTrue((r32(ram, kInst0 + 8u) & 0xffu) == 0x03u, "hidden gem reshown");
            }
            // Bank-clear: 3 -> 1, 1 untouched, NaN refused.
            {
                std::vector<uint8_t> ram(kRamSize, 0);
                wf(ram, kT + 0x1c4u, 3.0f);
                ps2_tk45c::onBankClear(ram.data(), ram.size(), 2000, kT, 100, 0x119b88u);
                t.IsTrue(rf(ram, kT + 0x1c4u) == 1.0f, "bank clears");
                ps2_tk45c::onBankClear(ram.data(), ram.size(), 2001, kT, 101, 0x119b88u);
                t.IsTrue(rf(ram, kT + 0x1c4u) == 1.0f, "clean no-op");
                wf(ram, kT + 0x1c4u, std::numeric_limits<float>::quiet_NaN());
                ps2_tk45c::onBankClear(ram.data(), ram.size(), 2002, kT, 102, 0x119b88u);
                t.IsTrue(std::isnan(rf(ram, kT + 0x1c4u)), "NaN refused");
                ps2_tk45c::onBankClear(ram.data(), ram.size(), 2003, 0u, 103, 0x119b88u);
                ps2_tk45c::onBankClear(ram.data(), ram.size(), 2004, kT + 1u, 104, 0x119b88u);
                t.IsTrue(true, "bad T survives");
                // Full-span guard: T ending past RAM refuses (no OOB write).
                wf(ram, kRamSize - 4u, 3.0f);
                ps2_tk45c::onBankClear(ram.data(), ram.size(), 2005, kRamSize - 4u, 105, 0x119b88u);
                t.IsTrue(rf(ram, kRamSize - 4u) == 3.0f, "short span refuses");
            }
            // Hide guard: 03/23 hide, 05 already, other lows refused.
            {
                std::vector<uint8_t> ram(kRamSize, 0);
                fakeRace(ram, 850, 0, 0, 100);
                fakeInstance(ram, kInst0, 1000, 0, 0);
                State st;
                st.scanDone = true;
                st.resolved[0][0] = kInst0;
                st.nres[0] = 1u;
                // 0x23 (lab alive): hides. (Three polls: seed, prev, cross.)
                ps2_tk45c::poll(st, gems, ram.data(), ram.size(), 1000, kR, true);
                fakeRace(ram, 950, 0, 0, 101);
                ps2_tk45c::poll(st, gems, ram.data(), ram.size(), 1001, kR, true);
                fakeRace(ram, 1050, 0, 0, 102);
                ps2_tk45c::poll(st, gems, ram.data(), ram.size(), 1002, kR, true);
                t.IsTrue((r32(ram, kInst0 + 8u) & 0xffu) == 0x05u, "0x23 hides");
                // Unknown low 0x13: refused (byte untouched), mult still tracks.
                w32(ram, kInst0 + 8u, 0x00010013u);
                st.collected[0] = 0u;
                st.prevValid = false;
                fakeRace(ram, 850, 0, 0, 103);
                ps2_tk45c::poll(st, gems, ram.data(), ram.size(), 1003, kR, true);
                fakeRace(ram, 880, 0, 0, 104); // still outside (880+100=980<1000)
                ps2_tk45c::poll(st, gems, ram.data(), ram.size(), 1004, kR, true);
                fakeRace(ram, 1050, 0, 0, 105);
                wf(ram, kT + 0x1c4u, 1.0f);
                ps2_tk45c::poll(st, gems, ram.data(), ram.size(), 1005, kR, true);
                t.IsTrue((r32(ram, kInst0 + 8u) & 0xffu) == 0x13u, "0x13 refused");
                t.IsTrue(rf(ram, kT + 0x1c4u) == 2.0f, "mult tracks despite refuse");
            }
        }); });

MiniTest::Case("Ps2Ssx3TrickyGemsSpin", [](TestCase &tc)
               {
        tc.Run("spin rows: rotation about the own axis, history-free", [](TestCase &t)
               {
            float o0[3], o1[3];
            t.IsTrue(ps2_tk45c::spinRows(kGemR0, kGemR1, kGemR2, 0u, o0, o1), "upright accepted");
            t.IsTrue(near(o0[0], 1) && near(o0[1], 0) && near(o1[0], 0) && near(o1[1], 1) && o0[2] == 0.0f &&
                         o1[2] == 0.0f,
                     "phase 0 = world X/Y");
            t.IsTrue(ps2_tk45c::spinRows(kGemR0, kGemR1, kGemR2, 90u, o0, o1), "90 accepted");
            t.IsTrue(near(o0[0], 0) && near(o0[1], 1) && near(o1[0], -1) && near(o1[1], 0), "90 deg turns X to Y");
            for (uint32_t deg : {3u, 177u, 357u})
            {
                t.IsTrue(ps2_tk45c::spinRows(kGemR0, kGemR1, kGemR2, deg, o0, o1), "accepted");
                t.IsTrue(near(dot3(o0, o0), 1) && near(dot3(o1, o1), 1) && near(dot3(o0, o1), 0), "orthonormal");
                t.IsTrue(near(dot3(o0, kGemR2), 0) && near(dot3(o1, kGemR2), 0), "axis kept");
                const float cz = o0[0] * o1[1] - o0[1] * o1[0];
                t.IsTrue(near(cz, 1), "right-handed kept");
            }
            // History-free: spinning an already-spun basis to the same angle
            // lands on the same rows (savestate load mid-spin), and the same
            // input gives the same bytes.
            float s0[3], s1[3], q0[3], q1[3], p0[3], p1[3];
            t.IsTrue(ps2_tk45c::spinRows(kGemR0, kGemR1, kGemR2, 213u, s0, s1), "spun once");
            t.IsTrue(ps2_tk45c::spinRows(s0, s1, kGemR2, 60u, q0, q1), "from spun");
            t.IsTrue(ps2_tk45c::spinRows(kGemR0, kGemR1, kGemR2, 60u, p0, p1), "from authored");
            bool same = true;
            for (int k = 0; k < 3; ++k)
                same = same && q0[k] == p0[k] && q1[k] == p1[k];
            t.IsTrue(same, "angle is absolute");
            float d0[3], d1[3];
            ps2_tk45c::spinRows(kGemR0, kGemR1, kGemR2, 60u, d0, d1);
            t.IsTrue(std::memcmp(d0, p0, 12) == 0 && std::memcmp(d1, p1, 12) == 0, "deterministic bytes");
            // Byte-exact history independence: 1184 sequential clock steps
            // (each spun from the previous write) equal the direct result.
            float h0[3], h1[3];
            std::memcpy(h0, kGemR0, 12);
            std::memcpy(h1, kGemR1, 12);
            int seqDiffs = 0;
            for (uint32_t clk = 16u; clk < 1200u; ++clk)
            {
                const uint32_t deg = (clk % 120u) * 3u;
                float n0[3], n1[3], e0[3], e1[3];
                ps2_tk45c::spinRows(h0, h1, kGemR2, deg, n0, n1);
                std::memcpy(h0, n0, 12);
                std::memcpy(h1, n1, 12);
                ps2_tk45c::spinRows(kGemR0, kGemR1, kGemR2, deg, e0, e1);
                seqDiffs += (std::memcmp(n0, e0, 12) != 0 || std::memcmp(n1, e1, 12) != 0) ? 1 : 0;
            }
            t.IsTrue(seqDiffs == 0, "sequential writes equal direct bytes");
            // Tilted, scaled and mirrored bases keep axis, scale and handedness.
            const float a2[3] = {0.0f, 1.2f, 1.6f}; // scale 2 axis (0, .6, .8)
            const float a0[3] = {2.0f, 0.0f, 0.0f};
            const float a1[3] = {0.0f, 1.6f, -1.2f}; // a0 x a1 = (0,2.4,3.2) ~ +a2
            t.IsTrue(ps2_tk45c::spinRows(a0, a1, a2, 45u, o0, o1), "tilted accepted");
            t.IsTrue(near(dot3(o0, o0), 4, 1e-4) && near(dot3(o1, o1), 4, 1e-4), "scale kept");
            t.IsTrue(near(dot3(o0, a2), 0, 1e-4) && near(dot3(o1, a2), 0, 1e-4), "tilted axis kept");
            const double hz = (double(o0[1]) * o1[2] - double(o0[2]) * o1[1]) * a2[0] +
                              (double(o0[2]) * o1[0] - double(o0[0]) * o1[2]) * a2[1] +
                              (double(o0[0]) * o1[1] - double(o0[1]) * o1[0]) * a2[2];
            t.IsTrue(hz > 0.0, "tilted handedness kept");
            const float m1[3] = {-kGemR1[0], -kGemR1[1], -kGemR1[2]};
            t.IsTrue(ps2_tk45c::spinRows(kGemR0, m1, kGemR2, 30u, o0, o1), "mirrored accepted");
            t.IsTrue(o0[0] * o1[1] - o0[1] * o1[0] < 0.0f, "mirror kept");
            // Fail closed.
            const float big[3] = {2.0f * kGemR1[0], 2.0f * kGemR1[1], 0.0f};
            t.IsTrue(!ps2_tk45c::spinRows(kGemR0, big, kGemR2, 30u, o0, o1), "non-uniform scale refused");
            const float shear[3] = {0.5f, 0.5f, 0.7f};
            t.IsTrue(!ps2_tk45c::spinRows(kGemR0, kGemR1, shear, 30u, o0, o1), "shear refused");
            const float zero[3] = {0, 0, 0};
            t.IsTrue(!ps2_tk45c::spinRows(zero, zero, kGemR2, 30u, o0, o1), "zero refused");
            float nan[3] = {kGemR0[0], kGemR0[1], 0.0f};
            nan[2] = std::numeric_limits<float>::quiet_NaN();
            t.IsTrue(!ps2_tk45c::spinRows(nan, kGemR1, kGemR2, 30u, o0, o1), "NaN refused");
        });
        tc.Run("poll spins live gems only; visual-only, Tricky-gated", [](TestCase &t)
               {
            std::vector<Gem> gems;
            t.IsTrue(parse2(gems), "table");
            auto setup = [&](std::vector<uint8_t> &ram, State &st) {
                fakeRace(ram, 500, 0, 0, 100);
                fakeInstance(ram, kInst0, 1000, 0, 0);
                fakeInstance(ram, kInst1, 2000, 0, 0);
                fakeMatrix(ram, kInst0, kGemR0, kGemR1, kGemR2);
                fakeMatrix(ram, kInst1, kGemR0, kGemR1, kGemR2);
                st.scanDone = true;
                st.resolved[0][0] = kInst0;
                st.nres[0] = 1u;
                st.resolved[1][0] = kInst1;
                st.nres[1] = 1u;
            };
            std::vector<uint8_t> ram(kRamSize, 0);
            State st;
            setup(ram, st);
            const std::vector<uint8_t> before = ram;
            ps2_tk45c::poll(st, gems, ram.data(), ram.size(), 1000, kR, true, true);
            fakeRace(ram, 600, 0, 0, 101);
            ps2_tk45c::poll(st, gems, ram.data(), ram.size(), 1001, kR, true, true);
            // clk 101 -> (101 % 120) * 3 = 303 deg.
            float e0[3], e1[3];
            ps2_tk45c::spinRows(kGemR0, kGemR1, kGemR2, 303u, e0, e1);
            t.IsTrue(std::memcmp(ram.data() + kInst0 + 0x10u, e0, 12) == 0 &&
                         std::memcmp(ram.data() + kInst0 + 0x20u, e1, 12) == 0,
                     "gem0 rows at clock angle");
            t.IsTrue(std::memcmp(ram.data() + kInst1 + 0x10u, e0, 12) == 0, "gem1 rows at clock angle");
            bool untouched = std::memcmp(ram.data() + kInst0 + 0x30u, before.data() + kInst0 + 0x30u, 0x1cu) == 0 &&
                             r32(ram, kInst0 + 0x1cu) == 0x11111111u && r32(ram, kInst0 + 0x2cu) == 0x22222222u &&
                             r32(ram, kInst0 + 8u) == 0x00210123u && rf(ram, kT + 0x1c4u) == 1.0f;
            t.IsTrue(untouched, "row2, translation, w lanes, flags, mult untouched");
            // Only rows 0/1 of the two instances differ from the start image.
            size_t diff = 0u;
            for (size_t k = 0u; k < ram.size(); ++k)
                if (ram[k] != before[k])
                {
                    const uint32_t a = static_cast<uint32_t>(k);
                    const bool rows = (a >= kInst0 + 0x10u && a < kInst0 + 0x2cu) ||
                                      (a >= kInst1 + 0x10u && a < kInst1 + 0x2cu);
                    const bool race = a >= kB && a < kB + 0x10u; // fakeRace clock + position
                    const bool pos = a >= kR + 0x110u && a < kR + 0x11cu;
                    if (!rows && !race && !pos)
                        ++diff;
                }
            t.IsTrue(diff == 0u, "no other bytes written");
            // Same clock again: zero writes.
            const std::vector<uint8_t> again = ram;
            ps2_tk45c::poll(st, gems, ram.data(), ram.size(), 1002, kR, true, true);
            t.IsTrue(ram == again, "same clock is a no-op");
            // Hidden (collected) gem is left alone; spin knob off writes nothing.
            {
                std::vector<uint8_t> r2(kRamSize, 0);
                State s2;
                setup(r2, s2);
                w32(r2, kInst0 + 8u, 0x00210105u);
                ps2_tk45c::poll(s2, gems, r2.data(), r2.size(), 1000, kR, true, true);
                fakeRace(r2, 600, 0, 0, 101);
                ps2_tk45c::poll(s2, gems, r2.data(), r2.size(), 1001, kR, true, true);
                t.IsTrue(std::memcmp(r2.data() + kInst0 + 0x10u, kGemR0, 12) == 0, "hidden gem not spun");
                t.IsTrue(std::memcmp(r2.data() + kInst1 + 0x10u, e0, 12) == 0, "live gem spun");
            }
            {
                std::vector<uint8_t> r3(kRamSize, 0);
                State s3;
                setup(r3, s3);
                const std::vector<uint8_t> b3 = r3;
                ps2_tk45c::poll(s3, gems, r3.data(), r3.size(), 1000, kR, true, false);
                t.IsTrue(std::memcmp(r3.data() + kInst0, b3.data() + kInst0, 0x50) == 0, "spin off: no writes");
                ps2_tk45c::poll(s3, gems, r3.data(), r3.size(), 1001, kR, false, true);
                t.IsTrue(std::memcmp(r3.data() + kInst0, b3.data() + kInst0, 0x50) == 0, "stock: no writes");
            }
            // Pickup is unchanged with spin on: crossing gem0 still collects x2 and hides.
            {
                std::vector<uint8_t> r4(kRamSize, 0);
                State s4;
                setup(r4, s4);
                fakeRace(r4, 850, 0, 0, 100);
                ps2_tk45c::poll(s4, gems, r4.data(), r4.size(), 1000, kR, true, true);
                fakeRace(r4, 950, 0, 0, 101);
                ps2_tk45c::poll(s4, gems, r4.data(), r4.size(), 1001, kR, true, true);
                fakeRace(r4, 1050, 0, 0, 102);
                ps2_tk45c::poll(s4, gems, r4.data(), r4.size(), 1002, kR, true, true);
                t.IsTrue(rf(r4, kT + 0x1c4u) == 2.0f && (r32(r4, kInst0 + 8u) & 0xffu) == 0x05u,
                         "pickup x2 + hide unchanged");
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
            t.IsTrue(st.nres[0] == 1u && st.resolved[0][0] == kInst0 && st.nres[1] == 1u &&
                         st.resolved[1][0] == kInst1 && st.nres[2] == 0u,
                     "two resolved, third absent");
            // Null link words are legit (ram1: all 25 first-misses had one).
            w32(ram, kInst0, 0u);
            w32(ram, kInst0 + 4u, 0u);
            State stn;
            for (int i = 0; i < 40 && !stn.scanDone; ++i)
                ps2_tk45c::scanChunk(stn, gems, ram.data(), ram.size(), 3000, 100);
            t.IsTrue(stn.nres[0] == 1u && stn.resolved[0][0] == kInst0, "null links resolve");
            // Stacked triple (GARI 2152): same translation 3x, all recorded.
            fakeInstance(ram, 0x220000u, 1000, 0, 0);
            fakeInstance(ram, 0x230000u, 1000, 0, 0);
            State st3;
            for (int i = 0; i < 40 && !st3.scanDone; ++i)
                ps2_tk45c::scanChunk(st3, gems, ram.data(), ram.size(), 3000, 100);
            t.IsTrue(st3.nres[0] == 3u && !st3.ambig[0], "triple multi-resolves");
            // A fifth struct sharing the position -> ambiguous -> hide off.
            fakeInstance(ram, 0x240000u, 1000, 0, 0);
            fakeInstance(ram, 0x250000u, 1000, 0, 0);
            State st2;
            for (int i = 0; i < 40 && !st2.scanDone; ++i)
                ps2_tk45c::scanChunk(st2, gems, ram.data(), ram.size(), 3000, 100);
            t.IsTrue(st2.nres[0] == 0u && st2.ambig[0] && st2.nres[1] == 1u, "5x fails closed");
        }); });
}
