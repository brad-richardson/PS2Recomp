#include "MiniTest.h"
#include "ps2_mtvu.h"
#include "ps2_vif_unpack_fast.h"
#include "runtime/ps2_memory.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    uint32_t makeVifCmd(uint8_t opcode, uint8_t num, uint16_t imm)
    {
        return (static_cast<uint32_t>(opcode) << 24) | (static_cast<uint32_t>(num) << 16) |
               static_cast<uint32_t>(imm);
    }

    void appendU32(std::vector<uint8_t> &dst, uint32_t value)
    {
        const size_t pos = dst.size();
        dst.resize(pos + sizeof(uint32_t));
        std::memcpy(dst.data() + pos, &value, sizeof(uint32_t));
    }

    // xorshift32: deterministic fuzz stream.
    struct Rng
    {
        uint32_t x;
        explicit Rng(uint32_t seed) : x(seed ? seed : 0x9E3779B9u) {}
        uint32_t next()
        {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            return x;
        }
        uint32_t below(uint32_t n) { return next() % n; }
    };

    // VIF UNPACK size formula (mirrors both interpreter loops).
    uint32_t unpackTotalBytes(uint32_t vn, uint32_t vl, uint8_t num, uint32_t cl, uint32_t wl)
    {
        const uint32_t write = (num == 0u) ? 256u : num;
        uint32_t src = write;
        if (cl < wl)
        {
            const uint32_t full = write / wl;
            uint32_t rem = write % wl;
            if (rem > cl)
                rem = cl;
            src = full * cl + rem;
        }
        uint32_t bpv;
        if (vl == 3u && vn == 3u)
        {
            bpv = 2u;
        }
        else
        {
            const uint32_t bits = (vl == 0u) ? 32u : ((vl == 1u) ? 16u : ((vl == 2u) ? 8u : 16u));
            bpv = ((vn + 1u) * bits + 7u) / 8u;
        }
        return (src * bpv + 3u) & ~3u;
    }

    struct Outcome
    {
        std::vector<uint8_t> vu1;
        VIFRegisters regs{};
        uint64_t hits = 0u;
        std::vector<uint64_t> mscal; // staged MSCAL digests (ordering check)
    };

    bool sameBytes(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b)
    {
        return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
    }

    bool sameRegs(const VIFRegisters &a, const VIFRegisters &b)
    {
        return std::memcmp(&a, &b, sizeof(VIFRegisters)) == 0;
    }

    // One inline (Impl-loop) run of a raw VIF1 buffer with preset registers.
    Outcome runInline(PS2Memory &mem, const std::vector<uint8_t> &buf, const VIFRegisters &regs,
                      const std::vector<uint8_t> &prefill, bool fast)
    {
        ps2_mtvu::setModeForTest(ps2_mtvu::Mode::Off);
        mem.setVifUnpackFastForTest(fast);
        mem.vif1_regs = regs;
        std::memcpy(mem.getVU1Data(), prefill.data(), PS2_VU1_DATA_SIZE);
        ps2_vif_unpack_fast::resetFastUnpackHits();
        mem.processVIF1Data(buf.data(), static_cast<uint32_t>(buf.size()));
        Outcome out;
        out.vu1.assign(mem.getVU1Data(), mem.getVU1Data() + PS2_VU1_DATA_SIZE);
        out.regs = mem.vif1_regs;
        out.hits = ps2_vif_unpack_fast::fastUnpackHits();
        return out;
    }

    // One staged-loop run: the buffer parses on the MTVU-VIF thread, records
    // apply on the MTVU thread (vpl2Run pattern, stages already started).
    Outcome runStaged(PS2Memory &mem, const std::vector<uint8_t> &buf, const VIFRegisters &regs,
                      const std::vector<uint8_t> &prefill, bool fast, std::vector<uint64_t> &mscalLog)
    {
        mem.setVifUnpackFastForTest(fast);
        mem.vif1_regs = regs;
        std::memcpy(mem.getVU1Data(), prefill.data(), PS2_VU1_DATA_SIZE);
        mscalLog.clear();
        ps2_vif_unpack_fast::resetFastUnpackHits();
        ps2_mtvu::submit(
            [&mem, &buf]() { mem.processVIF1Data(buf.data(), static_cast<uint32_t>(buf.size())); },
            buf.size(), 0u, true);
        ps2_mtvu::syncAll();
        Outcome out;
        out.vu1.assign(mem.getVU1Data(), mem.getVU1Data() + PS2_VU1_DATA_SIZE);
        out.regs = mem.vif1_regs;
        out.hits = ps2_vif_unpack_fast::fastUnpackHits();
        out.mscal = mscalLog;
        return out;
    }

    void checkIdentical(TestCase &t, const Outcome &off, const Outcome &on, const std::string &what)
    {
        t.IsTrue(sameBytes(off.vu1, on.vu1), what + ": VU1 data differs knob off vs on");
        t.IsTrue(sameRegs(off.regs, on.regs), what + ": VIF1 regs differ knob off vs on");
        t.IsTrue(off.mscal == on.mscal, what + ": MSCAL sequence differs knob off vs on");
    }

    // Build a single-UNPACK buffer: preNops NOPs (alignment control), the
    // UNPACK command, payloadBytes random bytes, plus trailingWords past the
    // payload (V3 w-reads). NOP trailing keeps exact hit counts (random words
    // would decode as extra commands); random trailing is for identical-only
    // cases.
    std::vector<uint8_t> singleUnpack(uint8_t opcode, uint8_t num, uint16_t imm, uint32_t preNops,
                                      uint32_t payloadBytes, uint32_t trailingWords, Rng &rng,
                                      bool nopTrail = false)
    {
        std::vector<uint8_t> buf;
        for (uint32_t i = 0u; i < preNops; ++i)
            appendU32(buf, makeVifCmd(0x00u, 0u, 0u));
        appendU32(buf, makeVifCmd(opcode, num, imm));
        for (uint32_t i = 0u; i < payloadBytes; ++i)
            buf.push_back(static_cast<uint8_t>(rng.next()));
        for (uint32_t i = 0u; i < trailingWords; ++i)
            appendU32(buf, nopTrail ? 0u : rng.next());
        return buf;
    }

    VIFRegisters baseRegs()
    {
        VIFRegisters r{};
        r.cycle = 0x0101u; // cl=1, wl=1
        return r;
    }

    // True when the bulk path (not fast) takes an enumerated command: bulk
    // wins first for V4-32 at cl == wl with an all-data (or ignored) mask.
    bool bulkWins(uint32_t vn, uint32_t vl, uint8_t opcode, uint32_t cl, uint32_t wl, uint32_t mask)
    {
        if (vn != 3u || vl != 0u || cl != wl)
            return false;
        if ((opcode & 0x10u) == 0u)
            return true;
        for (uint32_t c = 0u; c < wl; ++c)
        {
            const uint32_t maskCycle = (c > 3u) ? 3u : c;
            for (uint32_t f = 0u; f < 4u; ++f)
                if (((mask >> (((maskCycle * 4u) + f) * 2u)) & 0x3u) != 0u)
                    return false;
        }
        return true;
    }
} // namespace

void register_ps2_vif_unpack_tests()
{
    MiniTest::Case("PS2VifUnpack", [](TestCase &tc)
    {
        tc.Run("VUP1 eligibility accepts covered shapes, rejects the rest", [](TestCase &t)
        {
            using ps2_vif_unpack_fast::eligible;
            // Covered: the heavy-window shapes at wl=1, mode 0.
            const uint32_t covered[][2] = {{0u, 2u}, {0u, 3u}, {0u, 4u}, {1u, 2u}, {1u, 3u},
                                           {1u, 4u}, {2u, 2u}, {2u, 4u}, {3u, 4u}};
            for (const auto &[vl, comp] : covered)
            {
                t.IsTrue(eligible(vl, comp, false, 0u, 0u, 1u), "unmasked covered shape eligible");
                t.IsTrue(eligible(vl, comp, true, 0u, 0x40404040u, 1u), "mask 0x40 eligible");
                t.IsTrue(eligible(vl, comp, true, 0u, 0x50505050u, 1u), "mask 0x50 eligible");
                t.IsTrue(eligible(vl, comp, true, 0u, 0x00000000u, 1u), "all-data mask eligible");
                t.IsTrue(eligible(vl, comp, true, 0u, 0x55555555u, 1u), "all-row mask eligible");
                // High mask rows are ignored at wl == 1 (col/protect up there is fine).
                t.IsTrue(eligible(vl, comp, true, 0u, 0xFFFFFF40u, 1u), "high mask rows ignored");
                // Rejections.
                t.IsFalse(eligible(vl, comp, false, 0u, 0u, 2u), "wl != 1 rejected");
                t.IsFalse(eligible(vl, comp, false, 0u, 0u, 4u), "wl=4 rejected");
                t.IsFalse(eligible(vl, comp, false, 1u, 0u, 1u), "mode 1 rejected");
                t.IsFalse(eligible(vl, comp, false, 2u, 0u, 1u), "mode 2 rejected");
                t.IsFalse(eligible(vl, comp, false, 3u, 0u, 1u), "mode 3 rejected");
                t.IsFalse(eligible(vl, comp, true, 0u, 0x00000002u, 1u), "col lane rejected");
                t.IsFalse(eligible(vl, comp, true, 0u, 0x00000003u, 1u), "protect lane rejected");
                t.IsFalse(eligible(vl, comp, true, 0u, 0x00000080u, 1u), "w col rejected");
                t.IsFalse(eligible(vl, comp, true, 0u, 0x000000C0u, 1u), "w protect rejected");
            }
            // Uncovered shapes never eligible, even unmasked at wl=1/mode 0.
            t.IsFalse(eligible(0u, 1u, false, 0u, 0u, 1u), "S-32 stays generic");
            t.IsFalse(eligible(1u, 1u, false, 0u, 0u, 1u), "S-16 stays generic");
            t.IsFalse(eligible(2u, 1u, false, 0u, 0u, 1u), "S-8 stays generic");
            t.IsFalse(eligible(2u, 3u, false, 0u, 0u, 1u), "V3-8 stays generic");
            t.IsFalse(eligible(3u, 1u, false, 0u, 0u, 1u), "vl3/vn0 stays generic");
            t.IsFalse(eligible(3u, 2u, false, 0u, 0u, 1u), "vl3/vn1 stays generic");
            t.IsFalse(eligible(3u, 3u, false, 0u, 0u, 1u), "vl3/vn2 stays generic");
        });

        tc.Run("VUP1 enumerated shapes identical, fast path engaged (inline)", [](TestCase &t)
        {
            PS2Memory memOff, memOn;
            t.IsTrue(memOff.initialize(), "memOff initialize");
            t.IsTrue(memOn.initialize(), "memOn initialize");
            // Covered opcodes: (vn,vl) for the 9 shapes x mask bit.
            struct Shape
            {
                uint8_t opcode;
                uint32_t vn, vl;
            };
            std::vector<Shape> shapes;
            const uint32_t pairs[][2] = {{1u, 0u}, {2u, 0u}, {3u, 0u}, {1u, 1u}, {2u, 1u},
                                         {3u, 1u}, {1u, 2u}, {3u, 2u}, {3u, 3u}};
            for (const auto &[vn, vl] : pairs)
                for (uint32_t m = 0u; m < 2u; ++m)
                    shapes.push_back({static_cast<uint8_t>(0x60u | (m ? 0x10u : 0u) | (vn << 2) | vl),
                                      vn, vl});
            const uint32_t masks[] = {0x00000000u, 0x40404040u, 0x50505050u, 0x55555555u, 0xFFFFFF40u,
                                      0x00000044u};
            const uint32_t cls[] = {1u, 3u};
            uint32_t cases = 0u;
            for (const Shape &s : shapes)
            {
                for (uint32_t mask : masks)
                {
                    for (uint32_t cl : cls)
                    {
                        for (uint32_t usgn = 0u; usgn < 2u; ++usgn)
                        {
                            Rng r2(0x10000u + cases * 2654435761u);
                            VIFRegisters regs = baseRegs();
                            regs.cycle = (1u << 8) | cl;
                            regs.mask = mask;
                            for (int i = 0; i < 4; ++i)
                                regs.row[i] = r2.next();
                            const uint8_t num = 17u;
                            const uint16_t imm =
                                static_cast<uint16_t>(0x10u | (usgn ? 0x4000u : 0u));
                            const uint32_t total = unpackTotalBytes(s.vn, s.vl, num, cl, 1u);
                            std::vector<uint8_t> buf =
                                singleUnpack(s.opcode, num, imm, 0u, total, 4u, r2, true);
                            std::vector<uint8_t> prefill(PS2_VU1_DATA_SIZE);
                            for (auto &b : prefill)
                                b = static_cast<uint8_t>(r2.next());
                            Outcome off = runInline(memOff, buf, regs, prefill, false);
                            Outcome on = runInline(memOn, buf, regs, prefill, true);
                            const std::string what = "op=0x" + std::to_string(s.opcode) +
                                                           " cl=" + std::to_string(cl) + " mask=0x" +
                                                           std::to_string(mask) + " usgn=" +
                                                           std::to_string(usgn);
                            checkIdentical(t, off, on, what);
                            // Bulk wins over fast for V4-32 at cl == wl with
                            // an all-data (or ignored) mask; every other
                            // enumerated case must engage fast exactly once.
                            const bool bulk = bulkWins(s.vn, s.vl, s.opcode, cl, 1u, mask);
                            t.Equals(on.hits, bulk ? 0u : 1u, what + ": fast-path engagement");
                            t.Equals(off.hits, 0u, what + ": knob off never engages");
                            ++cases;
                        }
                    }
                }
            }
            t.Equals(cases, static_cast<uint32_t>(shapes.size() * 6u * 2u * 2u), "case count");
        });

        tc.Run("VUP1 num/addr/align sweeps identical (inline)", [](TestCase &t)
        {
            PS2Memory memOff, memOn;
            t.IsTrue(memOff.initialize(), "memOff initialize");
            t.IsTrue(memOn.initialize(), "memOn initialize");
            struct Shape
            {
                uint8_t opcode;
                uint32_t vn, vl, mask;
            };
            const Shape shapes[] = {{0x79u, 2u, 1u, 0x40404040u}, // masked V3-16
                                    {0x6Fu, 3u, 3u, 0u},          // V4-5
                                    {0x74u, 1u, 0u, 0x50505050u}}; // masked V2-32
            const uint8_t nums[] = {1u, 2u, 5u, 64u, 255u, 0u};
            const uint16_t addrs[] = {0u, 1u, 7u, 0x3F0u, 0x3FDu, 0x3FFu};
            for (const Shape &s : shapes)
            {
                for (uint8_t num : nums)
                {
                    for (uint16_t addr : addrs)
                    {
                        for (uint32_t pre = 0u; pre < 4u; ++pre)
                        {
                            Rng r2(0x20000u + (num * 131u + addr) * 17u + pre + s.opcode);
                            VIFRegisters regs = baseRegs();
                            regs.mask = s.mask;
                            for (int i = 0; i < 4; ++i)
                                regs.row[i] = r2.next();
                            const uint32_t total = unpackTotalBytes(s.vn, s.vl, num, 1u, 1u);
                            // No trailing words on half the cases: V3 w-reads
                            // at the buffer end must match (w = 0).
                            const uint32_t trailing = ((num + pre) & 1u) ? 4u : 0u;
                            std::vector<uint8_t> buf =
                                singleUnpack(s.opcode, num, addr, pre, total, trailing, r2, true);
                            std::vector<uint8_t> prefill(PS2_VU1_DATA_SIZE);
                            for (auto &b : prefill)
                                b = static_cast<uint8_t>(r2.next());
                            Outcome off = runInline(memOff, buf, regs, prefill, false);
                            Outcome on = runInline(memOn, buf, regs, prefill, true);
                            const std::string what =
                                "sweep op=" + std::to_string(s.opcode) + " num=" +
                                std::to_string(num) + " addr=" + std::to_string(addr) + " pre=" +
                                std::to_string(pre) + " hits=" + std::to_string(on.hits);
                            checkIdentical(t, off, on, what);
                            t.Equals(on.hits, 1u, what + ": fast engaged");
                        }
                    }
                }
            }
        });

        tc.Run("VUP1 ineligible commands stay generic and identical (inline)", [](TestCase &t)
        {
            PS2Memory memOff, memOn;
            t.IsTrue(memOff.initialize(), "memOff initialize");
            t.IsTrue(memOn.initialize(), "memOn initialize");
            // (opcode, vn, vl, cl, wl, mode, mask): S splats, V3-8, strided,
            // add modes, col/protect masks.
            struct Case
            {
                uint8_t opcode;
                uint32_t vn, vl, cl, wl, mode, mask;
            };
            const Case cases[] = {
                {0x60u, 0u, 0u, 1u, 1u, 0u, 0u},          // S-32
                {0x70u, 0u, 0u, 1u, 1u, 0u, 0x40404040u}, // masked S-32
                {0x6Bu, 2u, 3u, 1u, 1u, 0u, 0u},          // V3-5 (unhandled)
                {0x7Bu, 2u, 3u, 1u, 1u, 0u, 0x40404040u}, // masked V3-5
                {0x6Au, 2u, 2u, 1u, 1u, 0u, 0u},          // V3-8
                {0x79u, 2u, 1u, 1u, 2u, 0u, 0x40404040u}, // wl=2 strided
                {0x79u, 2u, 1u, 4u, 4u, 0u, 0x40404040u}, // wl=4
                {0x79u, 2u, 1u, 1u, 1u, 1u, 0x40404040u}, // mode 1 (add)
                {0x79u, 2u, 1u, 1u, 1u, 2u, 0x40404040u}, // mode 2 (accumulate)
                {0x79u, 2u, 1u, 1u, 1u, 0u, 0x000000C0u}, // w protect
                {0x79u, 2u, 1u, 1u, 1u, 0u, 0x00000002u}, // x col
                {0x6Fu, 3u, 3u, 1u, 1u, 1u, 0u},          // V4-5 mode 1
            };
            for (const Case &c : cases)
            {
                Rng r2(0x30000u + c.opcode * 7919u + c.wl * 131u + c.mode);
                VIFRegisters regs{};
                regs.cycle = (c.wl << 8) | c.cl;
                regs.mode = c.mode;
                regs.mask = c.mask;
                for (int i = 0; i < 4; ++i)
                {
                    regs.row[i] = r2.next();
                    regs.col[i] = r2.next();
                }
                const uint8_t num = 9u;
                const uint32_t total =
                    unpackTotalBytes(c.vn, c.vl, num, c.cl ? c.cl : 1u, c.wl ? c.wl : 1u);
                std::vector<uint8_t> buf = singleUnpack(c.opcode, num, 0x20u, 1u, total, 2u, r2, true);
                std::vector<uint8_t> prefill(PS2_VU1_DATA_SIZE);
                for (auto &b : prefill)
                    b = static_cast<uint8_t>(r2.next());
                Outcome off = runInline(memOff, buf, regs, prefill, false);
                Outcome on = runInline(memOn, buf, regs, prefill, true);
                checkIdentical(t, off, on, "ineligible");
                t.Equals(on.hits, 0u, "ineligible: fast never engages");
            }
        });

        tc.Run("VUP1 truncation and partial QWs identical (inline)", [](TestCase &t)
        {
            PS2Memory memOff, memOn;
            t.IsTrue(memOff.initialize(), "memOff initialize");
            t.IsTrue(memOn.initialize(), "memOn initialize");
            // Eligible shapes with short buffers: both sides skip the command.
            const uint8_t opcodes[] = {0x79u, 0x75u, 0x78u, 0x74u, 0x6Fu, 0x6Du, 0x66u, 0x7Cu};
            const uint32_t vns[] = {2u, 1u, 2u, 1u, 3u, 3u, 1u, 3u};
            const uint32_t vls[] = {1u, 1u, 0u, 0u, 3u, 1u, 2u, 0u};
            for (size_t i = 0; i < sizeof(opcodes); ++i)
            {
                Rng r2(0x40000u + opcodes[i] * 104729u);
                VIFRegisters regs = baseRegs();
                regs.mask = 0x40404040u;
                for (int k = 0; k < 4; ++k)
                    regs.row[k] = r2.next();
                const uint8_t num = 48u;
                const uint32_t total = unpackTotalBytes(vns[i], vls[i], num, 1u, 1u);
                for (uint32_t cut = 1u; cut <= 3u; ++cut)
                {
                    // Buffer ends partway through the payload.
                    const uint32_t payloadBytes =
                        total > cut * 4u + 3u ? total - cut * 4u + (r2.below(3u) + 1u) : 1u;
                    std::vector<uint8_t> buf =
                        singleUnpack(opcodes[i], num, 0x30u, 0u, payloadBytes, 0u, r2);
                    std::vector<uint8_t> prefill(PS2_VU1_DATA_SIZE, 0xA5u);
                    Outcome off = runInline(memOff, buf, regs, prefill, false);
                    Outcome on = runInline(memOn, buf, regs, prefill, true);
                    checkIdentical(t, off, on, "truncated");
                    t.Equals(on.hits, 0u, "truncated: fast never engages");
                    t.IsTrue(sameBytes(on.vu1, prefill), "truncated: VU1 untouched");
                }
            }
        });

        tc.Run("VUP1 randomized differential, exact engagement (inline)", [](TestCase &t)
        {
            PS2Memory memOff, memOn;
            t.IsTrue(memOff.initialize(), "memOff initialize");
            t.IsTrue(memOn.initialize(), "memOn initialize");
            // One random UNPACK per seed over every vn/vl, mask bit, num,
            // TOPS/usgn, cl/wl (0 tests the fixup), mode (incl >3: the impl
            // masks &3), full 32-bit mask, row/col, alignment, and exact /
            // NOP-trailing / truncated payloads. The expected hit count
            // mirrors the implementation gate exactly (bulk wins first).
            for (uint32_t seed = 1u; seed <= 500u; ++seed)
            {
                Rng r2(0x50000u + seed * 2246822519u);
                const uint32_t vn = r2.below(4u), vl = r2.below(4u);
                const bool m = r2.below(2u) != 0u;
                const uint8_t opcode =
                    static_cast<uint8_t>(0x60u | (m ? 0x10u : 0u) | (vn << 2) | vl);
                const uint8_t num =
                    (r2.below(10u) == 0u) ? 0u : static_cast<uint8_t>(1u + r2.below(80u));
                uint16_t addr = static_cast<uint16_t>(r2.below(0x400u));
                if (r2.below(3u) == 0u)
                    addr = static_cast<uint16_t>(0x3F0u + r2.below(16u));
                uint16_t imm = addr;
                if (r2.below(2u))
                    imm |= 0x4000u;
                if (r2.below(3u) == 0u)
                    imm |= 0x8000u;
                const uint32_t cl = r2.below(6u), wl = r2.below(5u);
                const uint32_t clFix = cl ? cl : 1u, wlFix = wl ? wl : 1u;
                const uint32_t mode = r2.below(8u);
                const uint32_t mask = r2.next();
                VIFRegisters regs{};
                regs.cycle = (wl << 8) | cl;
                regs.mode = mode;
                regs.mask = mask;
                regs.tops = r2.below(0x400u);
                for (int i = 0; i < 4; ++i)
                {
                    regs.row[i] = r2.next();
                    regs.col[i] = r2.next();
                }
                const uint32_t preNops = r2.below(6u);
                const uint32_t total = unpackTotalBytes(vn, vl, num, clFix, wlFix);
                const uint32_t pick = r2.below(4u);
                uint32_t payloadBytes = total, trailing = 0u;
                if (pick == 1u)
                    trailing = r2.below(5u);
                else if (pick >= 2u)
                    payloadBytes = r2.below(total + 1u);
                std::vector<uint8_t> buf =
                    singleUnpack(opcode, num, imm, preNops, payloadBytes, trailing, r2, true);
                std::vector<uint8_t> prefill(PS2_VU1_DATA_SIZE);
                for (auto &b : prefill)
                    b = static_cast<uint8_t>(r2.next());
                const uint32_t posPayload = preNops * 4u + 4u;
                const bool fit = total > 0u && posPayload + total <= buf.size();
                const bool bulk =
                    fit && (mode & 3u) == 0u && bulkWins(vn, vl, opcode, clFix, wlFix, mask);
                const bool fast =
                    fit && !bulk &&
                    ps2_vif_unpack_fast::eligible(vl, vn + 1u, m, mode & 3u, mask, wlFix);
                const uint64_t expect = fast ? 1u : 0u;
                Outcome off = runInline(memOff, buf, regs, prefill, false);
                Outcome on = runInline(memOn, buf, regs, prefill, true);
                const std::string what = "fuzz seed " + std::to_string(seed) + " op=" +
                                         std::to_string(opcode) + " cl=" + std::to_string(cl) +
                                         " wl=" + std::to_string(wl);
                checkIdentical(t, off, on, what);
                t.Equals(on.hits, expect, what + ": exact engagement");
                t.Equals(off.hits, 0u, what + ": knob off never engages");
            }
        });

        tc.Run("VUP1 modal control-command script identical (inline + staged)", [](TestCase &t)
        {
            // Fixed script with known modal evolution (STMASK/STROW consume
            // payload words): 4 eligible UNPACKs (one strided), one mode-1 and
            // one wl=2 generic. Same buffer runs inline and staged.
            Rng r2(0x80000u);
            std::vector<uint8_t> buf;
            auto unpack = [&](uint8_t op, uint8_t n, uint16_t imm, uint32_t cl, uint32_t wl)
            {
                appendU32(buf, makeVifCmd(op, n, imm));
                const uint32_t total = unpackTotalBytes((op >> 2) & 3u, op & 3u, n, cl, wl);
                for (uint32_t i = 0u; i < total; ++i)
                    buf.push_back(static_cast<uint8_t>(r2.next()));
            };
            unpack(0x79u, 8u, 0x20u, 1u, 1u); // eligible (mask 0x40) -> hit
            appendU32(buf, makeVifCmd(0x20u, 0u, 0u));
            appendU32(buf, 0x50505050u); // STMASK + mask word
            appendU32(buf, makeVifCmd(0x30u, 0u, 0u));
            for (int k = 0; k < 4; ++k)
                appendU32(buf, r2.next()); // STROW + row
            unpack(0x75u, 8u, 0x40u, 1u, 1u); // eligible (row0 0x50) -> hit
            appendU32(buf, makeVifCmd(0x01u, 0u, 0x0103u)); // STCYCL cl=3
            unpack(0x78u, 8u, 0x60u, 3u, 1u); // eligible stride -> hit
            appendU32(buf, makeVifCmd(0x05u, 0u, 1u)); // STMOD 1
            unpack(0x79u, 8u, 0x80u, 3u, 1u); // mode 1 -> generic
            appendU32(buf, makeVifCmd(0x05u, 0u, 0u)); // STMOD 0
            unpack(0x6Fu, 8u, 0xA0u, 3u, 1u); // V4-5 -> hit
            appendU32(buf, makeVifCmd(0x01u, 0u, 0x0201u)); // STCYCL wl=2
            unpack(0x79u, 8u, 0xC0u, 1u, 2u); // wl=2 -> generic

            VIFRegisters regs = baseRegs();
            regs.mask = 0x40404040u;
            for (int i = 0; i < 4; ++i)
                regs.row[i] = r2.next();
            std::vector<uint8_t> prefill(PS2_VU1_DATA_SIZE);
            for (auto &b : prefill)
                b = static_cast<uint8_t>(r2.next());

            PS2Memory memOff, memOn;
            t.IsTrue(memOff.initialize(), "memOff initialize");
            t.IsTrue(memOn.initialize(), "memOn initialize");
            Outcome offInline = runInline(memOff, buf, regs, prefill, false);
            Outcome onInline = runInline(memOn, buf, regs, prefill, true);
            checkIdentical(t, offInline, onInline, "script inline");
            t.Equals(onInline.hits, 4u, "script inline: 4 fast hits");

            std::vector<uint64_t> logOff, logOn;
            ps2_mtvu::setModeForTest(ps2_mtvu::Mode::Threaded, false, 0u);
            ps2_mtvu::startGifStage([&memOff](ps2_mtvu::GifOp &op) { memOff.execGifStageOp(op); });
            ps2_mtvu::startVifStage(&PS2Memory::execVifStageRec, &memOff);
            Outcome offStaged = runStaged(memOff, buf, regs, prefill, false, logOff);
            ps2_mtvu::stopVifStage();
            ps2_mtvu::stopGifStage();
            ps2_mtvu::startGifStage([&memOn](ps2_mtvu::GifOp &op) { memOn.execGifStageOp(op); });
            ps2_mtvu::startVifStage(&PS2Memory::execVifStageRec, &memOn);
            Outcome onStaged = runStaged(memOn, buf, regs, prefill, true, logOn);
            ps2_mtvu::stopVifStage();
            ps2_mtvu::stopGifStage();
            ps2_mtvu::setModeForTest(ps2_mtvu::Mode::Off);
            checkIdentical(t, offStaged, onStaged, "script staged");
            t.Equals(onStaged.hits, 4u, "script staged: 4 fast hits");
            checkIdentical(t, offInline, offStaged, "script inline vs staged");
        });

        tc.Run("VUP1 env knob wires the constructor", [](TestCase &t)
        {
            // The device path reads the env (no test setter exists there).
            ::setenv("PS2X_VIF_UNPACK_FAST", "1", 1);
            PS2Memory memOn;
            t.IsTrue(memOn.initialize(), "memOn initialize");
            ::unsetenv("PS2X_VIF_UNPACK_FAST");
            PS2Memory memOff;
            t.IsTrue(memOff.initialize(), "memOff initialize");

            Rng r2(0x70000u);
            VIFRegisters regs = baseRegs();
            regs.mask = 0x40404040u;
            for (int i = 0; i < 4; ++i)
                regs.row[i] = r2.next();
            std::vector<uint8_t> buf = singleUnpack(0x79u, 8u, 0x20u, 0u,
                                                    unpackTotalBytes(2u, 1u, 8u, 1u, 1u), 2u, r2,
                                                    true);
            std::vector<uint8_t> prefill(PS2_VU1_DATA_SIZE, 0x5Au);
            // No setter calls: the constructor's env read decides.
            ps2_mtvu::setModeForTest(ps2_mtvu::Mode::Off);
            memOn.vif1_regs = regs;
            std::memcpy(memOn.getVU1Data(), prefill.data(), PS2_VU1_DATA_SIZE);
            ps2_vif_unpack_fast::resetFastUnpackHits();
            memOn.processVIF1Data(buf.data(), static_cast<uint32_t>(buf.size()));
            t.Equals(ps2_vif_unpack_fast::fastUnpackHits(), 1u, "env=1 engages fast");
            std::vector<uint8_t> vuOn(memOn.getVU1Data(), memOn.getVU1Data() + PS2_VU1_DATA_SIZE);

            memOff.vif1_regs = regs;
            std::memcpy(memOff.getVU1Data(), prefill.data(), PS2_VU1_DATA_SIZE);
            ps2_vif_unpack_fast::resetFastUnpackHits();
            memOff.processVIF1Data(buf.data(), static_cast<uint32_t>(buf.size()));
            t.Equals(ps2_vif_unpack_fast::fastUnpackHits(), 0u, "env unset stays generic");
            std::vector<uint8_t> vuOff(memOff.getVU1Data(), memOff.getVU1Data() + PS2_VU1_DATA_SIZE);
            t.IsTrue(sameBytes(vuOff, vuOn), "env-wired fast matches generic");
        });

        tc.Run("VUP1 enumerated shapes identical (staged)", [](TestCase &t)
        {
            struct Case
            {
                std::vector<uint8_t> buf;
                VIFRegisters regs;
                std::vector<uint8_t> prefill;
                uint64_t expectHits;
            };
            std::vector<Case> cases;
            const uint8_t opcodes[] = {0x79u, 0x75u, 0x78u, 0x74u, 0x6Fu, 0x6Du,
                                       0x66u, 0x6Cu, 0x7Cu, 0x60u};
            const uint32_t vns[] = {2u, 1u, 2u, 1u, 3u, 3u, 1u, 3u, 3u, 0u};
            const uint32_t vls[] = {1u, 1u, 0u, 0u, 3u, 1u, 2u, 0u, 0u, 0u};
            for (size_t i = 0; i < sizeof(opcodes); ++i)
            {
                Rng r2(0x60000u + opcodes[i] * 32609u);
                VIFRegisters regs = baseRegs();
                regs.mask = (opcodes[i] & 0x10u) ? 0x50505050u : 0x40404040u;
                for (int k = 0; k < 4; ++k)
                    regs.row[k] = r2.next();
                const uint8_t num = 23u;
                const uint32_t cl = (i & 1u) ? 3u : 1u;
                regs.cycle = (1u << 8) | cl;
                const uint32_t total = unpackTotalBytes(vns[i], vls[i], num, cl, 1u);
                std::vector<uint8_t> buf =
                    singleUnpack(opcodes[i], num, 0x3F0u, 0u, total, 2u, r2, true);
                uint64_t expect = (ps2_vif_unpack_fast::eligible(vls[i], vns[i] + 1u,
                                                                 (opcodes[i] & 0x10u) != 0u, 0u,
                                                                 regs.mask, 1u) &&
                                   !bulkWins(vns[i], vls[i], opcodes[i], cl, 1u, regs.mask))
                                      ? 1u
                                      : 0u;
                // MSCAL sandwich on the first shape: record order around fast records.
                if (i == 0u)
                {
                    appendU32(buf, makeVifCmd(0x14u, 0u, 0x0100u));
                    std::vector<uint8_t> tail =
                        singleUnpack(opcodes[i], 4u, 0x10u, 0u,
                                     unpackTotalBytes(vns[i], vls[i], 4u, cl, 1u), 0u, r2);
                    buf.insert(buf.end(), tail.begin(), tail.end());
                    expect = 2u;
                }
                std::vector<uint8_t> prefill(PS2_VU1_DATA_SIZE);
                for (auto &b : prefill)
                    b = static_cast<uint8_t>(r2.next());
                cases.push_back({buf, regs, prefill, expect});
            }
            PS2Memory memOff, memOn;
            t.IsTrue(memOff.initialize(), "memOff initialize");
            t.IsTrue(memOn.initialize(), "memOn initialize");
            std::vector<uint64_t> logOff, logOn;
            auto digestAtMscal = [](PS2Memory &m, std::vector<uint64_t> &log)
            {
                m.setVu1MscalCallback(
                    [&m, &log](uint32_t pc, uint32_t top, uint32_t itop)
                    {
                        uint64_t h = pc * 3u + top * 5u + itop * 7u;
                        const uint8_t *vu = m.getVU1Data();
                        for (size_t i = 0; i < PS2_VU1_DATA_SIZE; i += 64u)
                            h = h * 31u + vu[i];
                        log.push_back(h);
                    });
            };
            digestAtMscal(memOff, logOff);
            digestAtMscal(memOn, logOn);
            // Off arm: stages serve memOff.
            ps2_mtvu::setModeForTest(ps2_mtvu::Mode::Threaded, false, 0u);
            ps2_mtvu::startGifStage([&memOff](ps2_mtvu::GifOp &op) { memOff.execGifStageOp(op); });
            ps2_mtvu::startVifStage(&PS2Memory::execVifStageRec, &memOff);
            std::vector<Outcome> offs;
            for (auto &c : cases)
                offs.push_back(runStaged(memOff, c.buf, c.regs, c.prefill, false, logOff));
            ps2_mtvu::stopVifStage();
            ps2_mtvu::stopGifStage();
            // On arm: restart the stages on memOn.
            ps2_mtvu::startGifStage([&memOn](ps2_mtvu::GifOp &op) { memOn.execGifStageOp(op); });
            ps2_mtvu::startVifStage(&PS2Memory::execVifStageRec, &memOn);
            std::vector<Outcome> ons;
            for (auto &c : cases)
                ons.push_back(runStaged(memOn, c.buf, c.regs, c.prefill, true, logOn));
            ps2_mtvu::stopVifStage();
            ps2_mtvu::stopGifStage();
            ps2_mtvu::setModeForTest(ps2_mtvu::Mode::Off);
            for (size_t i = 0; i < cases.size(); ++i)
            {
                checkIdentical(t, offs[i], ons[i], "staged shape " + std::to_string(i));
                t.Equals(ons[i].hits, cases[i].expectHits, "staged engagement " + std::to_string(i));
            }
        });
    });
}