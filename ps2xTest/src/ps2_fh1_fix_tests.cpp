#include "MiniTest.h"
#include "ps2_fh1_fix.h"
#include "ps2_fh1_full120.h"
#include "ps2_fh1_ground2.h"
#include "ps2_fh1_normcap.h"
#include "ps2_fh1_input2.h"
#include "ps2_fh1_fh35.h"
#include <cmath>
#include <algorithm>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

void register_ps2_fh1_fix_tests()
{
    using namespace ps2_fh1;
    MiniTest::Case("Ps2Fh1Fix", [](TestCase &tc)
    {
        tc.Run("INP2 every declared FIX group has a unique bit in its mask", [](TestCase &t)
        {
            // Read the declarations rather than maintain a second group list:
            // future additions must participate, including internal kFixRamp.
            // The two masks deliberately have independent bit namespaces.
            std::ifstream file("ps2xRuntime/include/ps2_fh1_fix.h");
            t.IsTrue(file.is_open(), "run the suite from the fork worktree root");
            if (!file.is_open()) return;
            std::ostringstream buffer;
            buffer << file.rdbuf();
            const std::string source = buffer.str();
            const std::regex member(R"(^\s*(kFix[A-Za-z0-9_]+)\b)");
            const std::regex definition(R"(^\s*(kFix[A-Za-z0-9_]+)\s*=\s*1(ull|u)\s*<<\s*([0-9]+)\s*,)");
            auto check = [&](const char *declaration, unsigned width)
            {
                const size_t at = source.find(declaration);
                t.IsTrue(at != std::string::npos, "FIX enum declaration found");
                if (at == std::string::npos) return;
                const size_t begin = source.find('{', at), end = source.find('}', begin);
                t.IsTrue(begin != std::string::npos && end != std::string::npos, "FIX enum body found");
                if (begin == std::string::npos || end == std::string::npos) return;
                std::istringstream body(source.substr(begin+1, end-begin-1));
                std::set<unsigned> seen;
                std::string line;
                while (std::getline(body, line))
                {
                    std::smatch name, value;
                    if (!std::regex_search(line, name, member)) continue;
                    const std::string group = name[1].str();
                    const bool explicitBit = std::regex_search(line, value, definition);
                    t.IsTrue(explicitBit, group + " declares one explicit bit");
                    if (!explicitBit) continue;
                    const unsigned bit = static_cast<unsigned>(std::stoul(value[3].str()));
                    t.IsTrue(bit < width, group + " fits its mask");
                    t.IsTrue(bit < 32u || value[2].str() == "ull", group + " uses a wide shift when required");
                    t.IsTrue(seen.insert(bit).second, group + " does not alias another FIX group");
                }
                t.IsTrue(!seen.empty(), "all FIX declarations scanned");
            };
            check("enum Fix : uint64_t", 64u);
            check("enum : uint32_t", 32u);
        });
        tc.Run("INP2 input opts are independent of jcam2 life2 and ground2", [](TestCase &t)
        {
            const uint64_t existing = kFixJcam2 | kFixLife2 | kFixGround2;
            t.IsTrue(((kFixInput2 | kFixInputChain) & existing) == 0u, "new bits do not alias current-tip opts");
            t.IsTrue((kFixInput2 & kFixInputChain) == 0u, "counter and chaining bits differ");
            t.Equals(parseFix("jcam2,life2,ground2,input2,inputchain,-input2,-inputchain").main,
                     existing, "removing input opts preserves the other selections");
        });
        tc.Run("INP2 four-stock-frame guard and 24/12 repeats", [](TestCase &t)
        {
            t.IsTrue((kFixAll & kFixInput2) != 0u, "input2 in all (PRM1, Brad 10-11)");
            t.Equals(parseFix("all,input2,-input2").main, kFixAll & ~static_cast<uint64_t>(kFixInput2), "input2 parses and opts out");
            for (unsigned step : {1u, 2u})
            {
                Input2Record r{}; r.guard = 3u;
                input2Step(r, 1.f, step);
                t.Equals(r.press, 1u, "idle press immediate");
                t.Equals(r.repeat, 1u, "initial repeat pulse");
                for (unsigned half = step; half < 8u; half += step)
                {
                    input2Step(r, 0.f, step);
                    t.Equals(r.level, 1u, "digital hold lasts four stock frames");
                    t.Equals(r.press | r.release, 0u, "edge cleared each guest update");
                }
                input2Step(r, 0.f, step);
                t.Equals(r.release, 1u, "release exactly at four stock frames");
                r = {}; r.guard = 3u;
                for (unsigned half = 0u; half <= 100u; half += step)
                {
                    input2Step(r, 1.f, step);
                    t.Equals(r.repeat, (half == 0u || (half >= 48u && (half-48u)%24u == 0u)) ? 1u : 0u,
                             "repeat stock deadlines");
                }
            }
        });
        tc.Run("INP2 event flips and saved records preserve half updates", [](TestCase &t)
        {
            Input2Record r{}; r.guard = 3u;
            input2Step(r, 1.f, 2u); // stock press
            input2Step(r, 0.f, 1u); // first active half
            Input2Record restored = r; // persistent state is exactly the guest record
            for (unsigned n = 0u; n < 7u; ++n)
            {
                input2Step(r, 0.f, 1u); input2Step(restored, 0.f, 1u);
                t.Equals(r.guard, restored.guard, "reload exact remaining guard");
                t.Equals(r.level, n == 6u ? 0u : 1u, "flip keeps eight-half-frame interval");
            }
            r = {}; r.guard = 3u;
            input2Step(r, 1.f, 1u);
            input2Step(r, 0.f, 1u); // remaining seven halves
            for (unsigned n = 0; n < 3; ++n) input2Step(r, 0.f, 2u);
            t.Equals(r.level, 1u, "exit keeps odd half remaining");
            input2Step(r, 0.f, 2u);
            t.Equals(r.level, 0u, "exit rounds to next available stock update");
            Input2Record legacy{}; legacy.guard = 1u; legacy.level = 1u; legacy.countdown = 10u;
            input2Step(legacy, 1.f, 1u);
            t.Equals(legacy.countdown, 19u, "legacy remaining repeat converts to half units");
            t.Equals(legacy.guard, kInput2Tag | 5u, "legacy guard converts once");
        });
        tc.Run("INP3 phase parking survives a guest-memory checkpoint", [](TestCase &t)
        {
            t.IsTrue((kFixAll & kFixInputChain) == 0u, "prototype excluded from all");
            t.Equals(parseFix("all,inputchain,-inputchain").main, kFixAll, "separate chain opt-in");
            for (uint32_t phase = 0; phase <= 3; ++phase)
            {
                const uint32_t saved = inputChainPark(phase);
                t.IsTrue(saved > 3u, "no active branch while parked");
                t.Equals(inputChainRestore(saved), phase, "phase restored exactly");
                t.Equals(inputChainPark(saved), saved, "parking idempotent after resume");
                t.Equals(inputChainRestore(phase), phase, "ordinary phase unchanged");
            }
        });

        tc.Run("INP5 neutral admission excludes pending rotation and chaining", [](TestCase &t)
        {
            std::array<uint32_t, 10> angles{};
            t.IsTrue(inputChainNeutral(3u, angles), "neutral idle may run on either half");
            for (uint32_t phase : {0u, 1u, 2u, kInputChainHold | 3u})
                t.IsTrue(!inputChainNeutral(phase, angles), "active or parked phase stays held");
            angles.fill(0x80000000u);
            t.IsTrue(inputChainNeutral(3u, angles), "negative zero is neutral");
            for (unsigned i = 0; i < angles.size(); ++i)
            {
                angles[i] = 0x3f800000u;
                t.IsTrue(!inputChainNeutral(3u, angles), "each target/current/offset/sequence accumulator prevents admission");
                angles[i] = 0x80000000u;
            }
            // INP5 chain-6212: all normalized angle words are zero after
            // publication, but the guest sequence accumulator retains motion.
            angles.fill(0u);
            angles[8] = 0x40561151u; // S+30 at raw6323
            t.IsTrue(!inputChainNeutral(3u, angles), "completed spin is not initial idle");
            angles[8] = 0u; // original guest reset/new-target store
            t.IsTrue(inputChainNeutral(3u, angles), "guest reset restores initial idle");
        });

        tc.Run("FH33 ground2 is opt-in and squares to the bounded stock map", [](TestCase &t)
        {
            t.IsTrue((kFixAll & kFixGround2) == 0u, "excluded from all");
            t.Equals(parseFix("all,ground2").main, kFixAll | kFixGround2, "opt-in");
            t.Equals(parseFix("all,ground2,-ground2").main, kFixAll, "opt-out");
            for (double multiplier : {1.0, 0.5, 2.0})
            {
                const double a = .2*multiplier, b = (.1/60)*multiplier, c = (14.0/60)*multiplier;
                double maxError = 0;
                for (unsigned i = 0; i <= 40000; ++i)
                {
                    const double g = i/10000.0;
                    const double h = ground2Residual(g,a,b,c);
                    const double stock = std::max(0.0,g-std::clamp(a*g,b,c));
                    maxError = std::max(maxError, std::abs(ground2Residual(h,a,b,c)-stock));
                    if (!(h >= 0 && h <= g)) { t.IsTrue(false, "half step contracts"); return; }
                }
                t.IsTrue(maxError < 1e-12, "two half steps equal stock across floor, proportional, cap and transitions");
            }
            t.IsTrue(std::abs(ground2Residual(.06,.2,.1/60,14.0/60)/.06-std::sqrt(.8)) < 1e-14,
                     "proportional retention sqrt(.8)");
        });
        tc.Run("GRD1 ground is opt-in and all minus ground is unchanged", [](TestCase &t)
        {
            t.IsTrue((kFixAll & kFixGround) == 0u, "ground excluded from all");
            t.Equals(parseFix("all,-ground").main, kFixAll, "old play layer remains equivalent");
            t.Equals(parseFix("all,ground").main, kFixAll | kFixGround, "ground remains selectable");
            t.Equals(parseFix("all,ground,-ground").main, kFixAll, "opt-in can be removed");
            t.IsTrue((kFixAll & kFixGround2) == 0u, "ground2 remains separate");
        });

        tc.Run("JMP3 query is opt-in at bit 50", [](TestCase &t)
        {
            t.Equals(static_cast<uint64_t>(kFixQuery), 1ull << 50, "query uses bit 50");
            t.IsTrue((kFixAll & kFixQuery) == 0u, "excluded from all");
            t.Equals(parseFix("all,query").main, kFixAll | kFixQuery, "opt-in");
            t.Equals(parseFix("all,query,-query").main, kFixAll, "opt-out");
            t.Equals(parseFix("query").main, static_cast<uint64_t>(kFixQuery), "query alone");
        });

        tc.Run("JMP4 query hold selects the owner slot", [](TestCase &t)
        {
            uint32_t ps[kQueryHoldSlots] = {};
            t.Equals(queryHoldSelect(ps, kQueryHoldSlots, 0x144a130u), 0, "first owner takes slot 0");
            ps[0] = 0x144a130u;
            t.Equals(queryHoldSelect(ps, kQueryHoldSlots, 0x144a130u), 0, "owner wins its slot");
            t.Equals(queryHoldSelect(ps, kQueryHoldSlots, 0x144b200u), 1, "second owner takes slot 1");
            for (uint32_t i = 1u; i < kQueryHoldSlots; ++i)
                ps[i] = 0x1450000u + i;
            t.Equals(queryHoldSelect(ps, kQueryHoldSlots, 0x144a130u), 0, "owner still wins when full");
            t.Equals(queryHoldSelect(ps, kQueryHoldSlots, 0x1460000u), -1, "full table misses");
            t.Equals(kQueryHoldStart + 4u * kQueryHoldWords, 0x14cu, "hold covers sp+0xC0..0x14C");
        });

        tc.Run("JDR2 shadowpub is opt-in at bit 53", [](TestCase &t)
        {
            t.Equals(static_cast<uint64_t>(kFixShadowPub), 1ull << 53, "shadowpub uses bit 53");
            t.IsTrue((kFixAll & kFixShadowPub) == 0u, "excluded from all");
            t.Equals(parseFix("all,query,shadowpub").main, kFixAll | kFixQuery | kFixShadowPub, "opt-in");
            t.Equals(parseFix("all,query,shadowpub,-shadowpub").main, kFixAll | kFixQuery, "opt-out");
        });

        tc.Run("JDR2 shadowpub publishes only after two even samples with the live frame held", [](TestCase &t)
        {
            PubSlot<2> s;
            const uint32_t a[2] = {0x3f800000u, 0x40000000u}; // 1, 2
            const uint32_t b[2] = {0x40000000u, 0x40400000u}; // 2, 3
            pubSample(s, a, 10u);
            t.IsTrue(!pubReady(s, a, 11u), "one sample is not enough");
            pubSample(s, b, 12u);
            pubSample(s, b, 12u); // a second call in the same update only refreshes
            t.IsTrue(pubReady(s, b, 13u), "odd update after two consecutive even samples publishes");
            t.IsTrue(!pubReady(s, a, 13u), "a moved live frame never publishes");
            t.IsTrue(!pubReady(s, b, 15u), "a stale sample never publishes");
            t.Equals(pubHalfStep(s.cur[0], s.prev[0]), 2.5f, "held + half the last even step");
            pubSample(s, a, 16u);
            t.IsTrue(!pubReady(s, a, 17u), "samples 4 updates apart never publish");
            t.Equals(kShadowPubOffs[6], 0xaacu, "ground-plane words follow the frame");
        });

        tc.Run("JMP6 vnret is opt-in at bit 52", [](TestCase &t)
        {
            t.Equals(static_cast<uint64_t>(kFixVnRet), 1ull << 52, "vnret uses bit 52");
            t.IsTrue((kFixAll & kFixVnRet) == 0u, "excluded from all");
            t.Equals(parseFix("all,vnret").main, kFixAll | kFixVnRet, "opt-in");
            t.Equals(parseFix("all,vnret,-vnret").main, kFixAll, "opt-out");
            t.Equals(parseFix("vnret").main, static_cast<uint64_t>(kFixVnRet), "vnret alone");
        });

        tc.Run("NRM1 normcap is opt-in at bit 56 and pairs to the bounded stock map", [](TestCase &t)
        {
            t.Equals(static_cast<uint64_t>(kFixNormcap), 1ull << 56, "normcap uses bit 56");
            t.IsTrue((kFixAll & kFixNormcap) == 0u, "excluded from all");
            t.Equals(parseFix("all,query,vnret,normcap").main, kFixAll | kFixQuery | kFixVnRet | kFixNormcap, "opt-in");
            t.Equals(parseFix("all,query,vnret,normcap,-normcap").main, kFixAll | kFixQuery | kFixVnRet, "opt-out");
            t.Equals(parseFix("normcap").main, static_cast<uint64_t>(kFixNormcap), "normcap alone");
            const double kh = std::sqrt(0.6) - 1.0;
            auto pair = [&](const double w[3], const double n[3], double out[3], bool &binds) {
                double h1[3], used1 = 0.0, e1 = 0.0, mag1 = 0.0;
                if (!normcapFirst(w, n, kh, h1, used1, e1, mag1))
                    return false;
                return normcapSecond(h1, n, kh, used1, e1, mag1, out, binds);
            };
            // Captured bound rows: A+100 takeoff/lip, A+100 landing, B late recontact.
            const double rows[4][6] = {
                {158.023941, 1609.033936, 358.280823, -0.05790357, -0.14487608, 0.98775405},
                {-516.690308, 1908.668457, 290.626251, -0.45353130, 0.02129498, 0.89098603},
                {-544.894531, 1892.244263, 191.545029, -0.30095994, -0.06139826, 0.95165825},
                {835.014099, 177.890167, 1362.358521, -0.54401886, -0.28040093, 0.79083425},
            };
            for (const auto &row : rows)
            {
                const double *w = row, *n = row + 3;
                double h2[3], m[3];
                bool binds = false;
                t.IsTrue(pair(w, n, h2, binds), "pair computes");
                t.IsTrue(binds, "captured row binds");
                t.IsTrue(normcapStock(w, n, -0.4, m), "stock computes");
                for (int i = 0; i < 3; ++i)
                    t.IsTrue(std::abs(h2[i] - m[i]) <= 1e-3 * std::max(std::abs(m[i]), 1.0),
                             "bound pair matches stock within 1e-3");
            }
            // Unbound pair: the hook skips the write (guest value stands).
            {
                const double w[3] = {100.0, 2100.0, 300.0};
                const double n[3] = {-0.3, -0.1, 0.9486833};
                double h2[3];
                bool binds = true;
                t.IsTrue(pair(w, n, h2, binds), "unbound pair computes");
                t.IsTrue(!binds, "unbound pair reports no bind");
            }
            // Sweep from deep saturation through the bind boundary to upward.
            double worst = 0.0;
            for (int i = 0; i <= 4000; ++i)
            {
                const double w[3] = {100.0 + i * 0.5, -1500.0 - i * 0.25, 300.0};
                const double n[3] = {-0.3, -0.1, 0.9486833};
                double h2[3], m[3];
                bool binds = false;
                if (!pair(w, n, h2, binds) || !normcapStock(w, n, -0.4, m))
                {
                    t.IsTrue(false, "sweep computes");
                    return;
                }
                for (int c = 0; c < 3; ++c)
                    worst = std::max(worst, std::abs(h2[c] - m[c]) / std::max(std::abs(m[c]), 1.0));
            }
            t.IsTrue(worst < 1e-3, "pair matches stock within 1e-3 across the boundary");
        });

        tc.Run("SEL2 sel5b is opt-in at bit 58 and needs sel5", [](TestCase &t)
        {
            t.Equals(static_cast<uint64_t>(kFixSel5b), 1ull << 58, "sel5b uses bit 58");
            t.IsTrue((kFixAll & kFixSel5b) == 0u, "excluded from all");
            t.Equals(parseFix("all,sel5,sel5b").main, kFixAll | kFixSel5 | kFixSel5b, "opt-in");
            t.Equals(parseFix("all,sel5,sel5b,-sel5b").main, kFixAll | kFixSel5, "opt-out");
            // The charge word flips only with sel5 alongside: sel5b alone is stripped.
            std::vector<uint8_t> ram(PS2_RAM_SIZE, 0u);
            const uint32_t a = 0x1000u;
            auto poke = [&](uint32_t addr, uint32_t v) {
                std::memcpy(ram.data() + (addr & 0x1FFFFFFFu), &v, 4);
            };
            auto peek = [&](uint32_t addr) {
                uint32_t v = 0u;
                std::memcpy(&v, ram.data() + (addr & 0x1FFFFFFFu), 4);
                return v;
            };
            auto stock = [&]() {
                poke(a + 0x10u, 60u);
                poke(a + 0x14u, kSixtieth);
                poke(a + 0x24u, 0x3f800000u);
                poke(0x49beecu, kSixtieth);
                poke(0x49bb90u, kSixtieth);
            };
            stock();
            FixMasks solo;
            solo.main = static_cast<uint64_t>(kFixSel5b);
            applyWords(ram.data(), a, true, solo);
            t.Equals(peek(0x49bb90u), kSixtieth, "sel5b alone leaves the charge word stock");
            stock();
            FixMasks both;
            both.main = static_cast<uint64_t>(kFixSel5) | static_cast<uint64_t>(kFixSel5b);
            applyWords(ram.data(), a, true, both);
            t.Equals(peek(0x49bb90u), kHundredTwentieth, "sel5+sel5b flips the charge word");
            t.Equals(peek(0x49beecu), kHundredTwentieth, "sel5 flips with it");
            applyWords(ram.data(), a, false, both);
            t.Equals(peek(0x49bb90u), kSixtieth, "exit restores stock");
        });

        tc.Run("KNC5 bit 51 stays reserved and parses (JMP5 springhold removed)", [](TestCase &t)
        {
            t.Equals(static_cast<uint64_t>(kFixSpringHold), 1ull << 51, "bit 51 reserved");
            t.IsTrue((kFixAll & kFixSpringHold) == 0u, "excluded from all");
            t.Equals(parseFix("all,springhold").main, kFixAll | kFixSpringHold, "old envs keep parsing");
            t.Equals(parseFix("all,springhold,-springhold").main, kFixAll, "opt-out");
            t.Equals(parseFix("springhold").main, static_cast<uint64_t>(kFixSpringHold), "springhold alone");
        });

        tc.Run("all includes stick2 and clocksign (FH26, Brad 10-02) and both masks", [](TestCase &t)
        {
            const FixMasks f = parseFix("all");
            t.IsTrue(f.ok, "parses");
            t.Equals(f.main, kFixAll, "main = kFixAll");
            t.Equals(f.fh12, kFix12All, "fh12 = kFix12All");
            t.IsTrue((f.main & kFixStick2) != 0u, "stick2 in all");
            t.IsTrue((f.main & kFixClockSign) != 0u, "clocksign in all");
            t.IsTrue((f.main & kFixLift) == 0u, "lift stays opt-in");
            t.IsTrue((f.main & kFixRamp) == 0u, "ramp is internal");
        });

        tc.Run("minus items opt out, left to right", [](TestCase &t)
        {
            const FixMasks a = parseFix("all,-stick2");
            t.IsTrue(a.ok, "parses");
            t.Equals(a.main, kFixAll & ~static_cast<uint64_t>(kFixStick2), "all minus stick2");
            t.Equals(a.fh12, kFix12All, "fh12 untouched");
            const FixMasks b = parseFix("all,-stick2,-clocksign");
            t.Equals(b.main, kFixAll & ~static_cast<uint64_t>(kFixStick2 | kFixClockSign), "pre-FH26 all");
            const FixMasks c = parseFix("all,-particles");
            t.Equals(c.fh12, kFix12All & ~kFix12Particles, "fh12 opt-out");
            t.Equals(c.main, kFixAll, "main untouched");
            const FixMasks d = parseFix("-stick2,all");
            t.Equals(d.main, kFixAll, "order matters: a later all re-adds");
            const FixMasks e = parseFix("all,-all,stick");
            t.Equals(e.main, static_cast<uint64_t>(kFixStick), "-all clears");
            t.Equals(e.fh12, 0u, "-all clears fh12");
        });

        tc.Run("single items, lift opt-in, empty items", [](TestCase &t)
        {
            t.Equals(parseFix("stick2").main, static_cast<uint64_t>(kFixStick2), "stick2 alone");
            t.Equals(parseFix("all,lift").main, kFixAll | kFixLift, "all plus lift");
            t.Equals(parseFix("spin,clock").fh12, static_cast<uint32_t>(kFix12Spin), "fh12 item");
            t.Equals(parseFix("spin,clock").main, static_cast<uint64_t>(kFixClock), "main item");
            t.IsTrue(parseFix("all,").ok, "trailing comma");
        });

        tc.Run("FH27 bonusflip and loops2 are in all (DEF1, Brad 10-03)", [](TestCase &t)
        {
            t.IsTrue((kFixAll & kFixBonusFlip) != 0u, "bonusflip in all");
            t.IsTrue((kFix12All & kFix12Loops2) != 0u, "loops2 in all");
            const FixMasks f = parseFix("all,-bonusflip,-loops2");
            t.IsTrue(f.ok, "parses");
            t.Equals(f.main, kFixAll & ~static_cast<uint64_t>(kFixBonusFlip), "main drops bonusflip");
            t.Equals(f.fh12, kFix12All & ~kFix12Loops2, "fh12 drops loops2");
        });

        tc.Run("FLK2 flare is opt-in", [](TestCase &t)
        {
            t.IsTrue((kFix12All & kFix12Flare) == 0u, "flare not in all");
            const FixMasks f = parseFix("all,flare");
            t.IsTrue(f.ok, "parses");
            t.Equals(f.main, kFixAll, "main untouched");
            t.Equals(f.fh12, kFix12All | kFix12Flare, "fh12 adds flare");
            t.Equals(parseFix("all,flare,-flare").fh12, kFix12All, "opt-out");
        });

        tc.Run("FH28 jcam/pid stay opt-in; pose/c2cap/spawn in all (PRM1, Brad 10-11)", [](TestCase &t)
        {
            const uint64_t out = static_cast<uint64_t>(kFixJcam | kFixPid);
            const uint64_t in = static_cast<uint64_t>(kFixPose | kFixC2Cap | kFixSpawn);
            t.IsTrue((kFixAll & out) == 0u, "jcam/pid stay opt-in");
            t.IsTrue((kFixAll & in) == in, "pose/c2cap/spawn in all");
            const FixMasks f = parseFix("all,-ground,jcam,pose,pid,c2cap,spawn");
            t.IsTrue(f.ok, "parses");
            t.Equals(f.main, kFixAll | out, "all minus ground plus jcam/pid");
            t.Equals(f.fh12, kFix12All, "fh12 untouched");
            t.Equals(parseFix("jcam").main, static_cast<uint64_t>(kFixJcam), "jcam alone");
            t.Equals(parseFix("pose").main, static_cast<uint64_t>(kFixPose), "pose alone");
            t.Equals(parseFix("pid").main, static_cast<uint64_t>(kFixPid), "pid alone");
            t.Equals(parseFix("c2cap").main, static_cast<uint64_t>(kFixC2Cap), "c2cap alone");
            t.Equals(parseFix("spawn").main, static_cast<uint64_t>(kFixSpawn), "spawn alone");
        });

        tc.Run("FH35 trails corrects only direct half-dt callers", [](TestCase &t)
        {
            static_assert(sizeof(FixMasks{}.main) == sizeof(uint64_t), "main FIX mask must retain bits above 47");
            t.Equals(static_cast<uint64_t>(kFixTrails), 1ull << 48, "trails uses bit 48");
            t.Equals(static_cast<uint64_t>(kFixChase2), 1ull << 49, "chase2 uses bit 49");
            t.IsTrue(((kFixTrails|kFixChase2) & ((1ull << 46)|(1ull << 47)))==0u,"INP2 bits reserved");
            t.IsTrue((kFixAll & kFixTrails)==0u,"trails stays opt-in");
            t.IsTrue((kFixAll & kFixChase2)!=0u,"chase2 in all (PRM1, Brad 10-11)");
            t.Equals(parseFix("all,trails,chase2").main,kFixAll|kFixTrails,"adds trails (chase2 already in)");
            t.Equals(parseFix("all,trails,chase2,-trails,-chase2").main,kFixAll & ~static_cast<uint64_t>(kFixChase2),"removes both");
            const float half=fh35Float(0x3c088889u), stock=fh35Float(0x3c888889u);
            for (uint32_t pc : {0x345e4cu,0x345fdcu,0x34600cu,0x346028u})
            {
                t.Equals(fh35Bits(directTrailDt(pc,half)),fh35Bits(stock),"direct stock dt");
                t.Equals(fh35Bits(directTrailDt(pc,stock)),fh35Bits(stock),"already stock unchanged");
                t.Equals(fh35Bits(directTrailDt(pc,0.02f)),fh35Bits(0.02f),"custom dt unchanged");
            }
            for (uint32_t pc : {0x345b7cu,0x345b8cu,0x3718d0u,0x2e1520u,0u})
                t.Equals(fh35Bits(directTrailDt(pc,half)),fh35Bits(half),"control unchanged");
        });
        tc.Run("FH35 chase2 recovers stock retention with exact endpoints", [](TestCase &t)
        {
            for (bool b : {false,true})
            {
                const float k0=fh35Float(b?0x3f055b3fu:0x3f6c0535u), k1=fh35Float(b?0x3f72dce8u:0x3f7c217au);
                const float r0=fh35Float(b?0x3e8aefe0u:0x3f59999au), r1=fh35Float(b?0x3f666666u:0x3f7851ecu);
                float out=-1;
                t.IsTrue(!chase2Retention(k0,k1,0,out) && out==-1,"low endpoint untouched");
                t.IsTrue(!chase2Retention(k0,k1,1,out) && out==-1,"high endpoint untouched");
                for (unsigned i=1;i<1000;++i)
                {
                    const float x=i/1000.0f;
                    t.IsTrue(chase2Retention(k0,k1,x,out),"interior selected");
                    const double expected=double(r0)+(double(r1)-r0)*x;
                    t.IsTrue(std::fabs(double(out)*out-expected)<2e-7,"pair recovers stock lerp");
                }
            }
            float out=42;
            t.IsTrue(!chase2Retention(.8f,.9f,.5f,out) && out==42,"custom endpoints untouched");
            t.Equals(chase2T(.375f,.125f,.625f),.5f,"same midpoint geometry");
            t.Equals(chase2T(-.375f,.125f,.625f),.5f,"absolute direction");
            t.Equals(chase2T(.05f,.1f,.6f),0.0f,"low clamp");
            t.Equals(chase2T(.7f,.1f,.6f),1.0f,"high clamp");
        });

        tc.Run("unknown items refuse", [](TestCase &t)
        {
            const FixMasks f = parseFix("all,stik2");
            t.IsTrue(!f.ok, "refused");
            t.Equals(f.bad, std::string("stik2"), "names the item");
            t.IsTrue(!parseFix("-nope").ok, "unknown opt-out refused");
        });
    });
}
