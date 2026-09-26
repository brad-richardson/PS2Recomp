#include "MiniTest.h"
#include "ps2_mtvu.h"
#include "ps2_vu1_engine.h"
#include "runtime/ps2_memory.h"
#include "runtime/gs/ps2_gif_arbiter.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    uint32_t vifCmd(uint8_t opcode, uint8_t num, uint16_t imm)
    {
        return (static_cast<uint32_t>(opcode) << 24) | (static_cast<uint32_t>(num) << 16) | imm;
    }

    void put32(std::vector<uint8_t> &v, uint32_t x)
    {
        const uint8_t *b = reinterpret_cast<const uint8_t *>(&x);
        v.insert(v.end(), b, b + 4);
    }

    void putAd(std::vector<uint8_t> &v, bool eop, uint64_t value)
    {
        const uint64_t q[4] = {1ull | (eop ? (1ull << 15) : 0ull) | (1ull << 60), 0xEull, value, 0x42ull};
        const uint8_t *b = reinterpret_cast<const uint8_t *>(q);
        v.insert(v.end(), b, b + sizeof(q));
    }

    uint64_t qw(const uint8_t *data, uint32_t qwIndex)
    {
        uint64_t w = 0;
        std::memcpy(&w, data + (qwIndex & 0x3FFu) * 16u, sizeof(w));
        return w;
    }
}

void register_ps2_vu1_engine_tests()
{
    MiniTest::Case("PS2VU1Engine", [](TestCase &tc)
    {
        tc.Run("VP2 reorder buffer commits runs and actions in stream order", [](TestCase &t)
        {
            ps2_vu1_engine::ReorderBuffer rob;
            std::vector<int> order;
            std::vector<bool> done(8, false);
            rob.pushRun(1);
            rob.pushAction([&] { order.push_back(100); });
            rob.pushRun(2);
            rob.pushAction([&] { order.push_back(200); });
            rob.pushAction([&] { order.push_back(201); });
            rob.pushRun(3);
            auto isDone = [&](uint64_t id) { return static_cast<bool>(done[id]); };
            auto commit = [&](uint64_t id) { order.push_back(static_cast<int>(id)); };

            done[2] = true; // finishes out of order: nothing may retire past run 1
            t.Equals(rob.commitReady(isDone, commit), static_cast<size_t>(0u), "head run unfinished: nothing retires");
            done[1] = true;
            t.Equals(rob.commitReady(isDone, commit), static_cast<size_t>(5u), "runs 1, 2 and their actions retire");
            t.Equals(rob.size(), static_cast<size_t>(1u), "run 3 stays queued");
            // An action that queues another action (a released PATH3 flush) keeps order.
            rob.pushAction([&] { order.push_back(300); rob.pushAction([&] { order.push_back(301); }); });
            done[3] = true;
            rob.commitReady(isDone, commit);
            const std::vector<int> want{1, 100, 2, 200, 201, 3, 300, 301};
            t.IsTrue(order == want, "commit order equals push order");
            t.IsTrue(rob.empty(), "buffer drains");
        });

        tc.Run("VP2 write-back keeps words VIF wrote after the snapshot", [](TestCase &t)
        {
            std::vector<uint8_t> canonical(ps2_vu1_engine::kDataBytes), snap(ps2_vu1_engine::kDataBytes);
            for (uint32_t i = 0; i < ps2_vu1_engine::kDataBytes; ++i)
                canonical[i] = static_cast<uint8_t>(i * 7u);
            snap = canonical; // dispatch
            std::vector<uint8_t> result = snap;
            ps2_vu1_engine::WawMask waw{};
            // The run writes words 0..7, 100, 4095 (last) and one full 64-word block.
            auto runWrite = [&](uint32_t word, uint32_t value) { std::memcpy(result.data() + word * 4u, &value, 4u); };
            for (uint32_t w = 0; w < 8u; ++w)
                runWrite(w, 0xAA000000u | w);
            runWrite(100, 0xAA000064u);
            runWrite(4095, 0xAA000FFFu);
            for (uint32_t w = 640; w < 704u; ++w)
                runWrite(w, 0xBB000000u | w);
            // VIF writes after the dispatch: lanes 2,3 of qword 1 (words 6,7), word 101
            // (the run did not write it), and all of block 640..703 but word 700.
            auto vifWrite = [&](uint32_t word, uint32_t value) { std::memcpy(canonical.data() + word * 4u, &value, 4u); };
            vifWrite(6, 0xCC000006u);
            vifWrite(7, 0xCC000007u);
            ps2_vu1_engine::markQwordLanes(waw.data(), 1u, 0xCu);
            vifWrite(101, 0xCC000065u);
            ps2_vu1_engine::markWord(waw.data(), 101u);
            for (uint32_t w = 640; w < 704u; ++w)
                if (w != 700u)
                {
                    vifWrite(w, 0xCC000000u | w);
                    ps2_vu1_engine::markWord(waw.data(), w);
                }
            // Serial reference: the run's writes land first, then VIF's.
            std::vector<uint8_t> serial = result;
            std::memcpy(serial.data() + 6u * 4u, canonical.data() + 6u * 4u, 8u);
            std::memcpy(serial.data() + 101u * 4u, canonical.data() + 101u * 4u, 4u);
            for (uint32_t w = 640; w < 704u; ++w)
                if (w != 700u)
                    std::memcpy(serial.data() + w * 4u, canonical.data() + w * 4u, 4u);
            ps2_vu1_engine::mergeWriteBack(canonical.data(), result.data(), waw.data());
            t.IsTrue(canonical == serial, "merged memory equals the serial order");
            uint32_t w5 = 0, w6 = 0, w700 = 0;
            std::memcpy(&w5, canonical.data() + 20u, 4u);
            std::memcpy(&w6, canonical.data() + 24u, 4u);
            std::memcpy(&w700, canonical.data() + 2800u, 4u);
            t.Equals(w5, 0xAA000005u, "an unmasked run write lands");
            t.Equals(w6, 0xCC000006u, "a later VIF lane write wins");
            t.Equals(w700, 0xBB0002BCu, "the one unmasked word of a masked block takes the run's value");
        });

        tc.Run("VP2 engine W=1 gives the synchronous GIF stream, VU1 memory and code", [](TestCase &t)
        {
            // The MT1 scenario shape with a stand-in VU1 program that has the
            // dependencies VP1 found: it reads what the previous run wrote
            // (RAW through the commit), writes words the next VIF window
            // overwrites (WAW) or half-overwrites through a write-protect mask,
            // reads code memory that MPG changes, and XGKICKs a packet built
            // from all of it. DIRECT and MSKPATH3 windows follow each MSCAL.
            struct Outcome
            {
                std::vector<std::pair<int, std::vector<uint8_t>>> packets;
                std::vector<uint8_t> vu1Data;
                std::vector<uint8_t> vu1Code;
                std::vector<uint64_t> eeReads;
                uint32_t row0 = 0;
                bool masked = false;
                size_t runs = 0;
                uint64_t committed = 0;
            };
            auto run = [](ps2_mtvu::Mode mode, int workers, uint32_t spinUs, uint32_t jitterUs) -> Outcome
            {
                ps2_mtvu::setModeForTest(mode, false, 0u);
                ps2_vu1_engine::Engine &eng = ps2_vu1_engine::engine();
                eng.setWorkersForTest(workers, spinUs, jitterUs);
                const uint64_t committed0 = eng.committedForTest();
                Outcome out;
                PS2Memory mem;
                if (!mem.initialize())
                    return out;
                int lastPath = -1;
                GifArbiter arbiter([&](const uint8_t *data, uint32_t size)
                                   { out.packets.emplace_back(lastPath, std::vector<uint8_t>(data, data + size)); });
                arbiter.setPacketListener([&](GifPathId path, uint32_t) { lastPath = static_cast<int>(path); });
                mem.setGifArbiter(&arbiter);
                auto program = [&mem](uint8_t *vu, uint32_t startPC, uint32_t top, uint32_t itop)
                {
                    uint64_t sum = startPC ^ (static_cast<uint64_t>(itop) << 40);
                    for (uint32_t q = 0; q < 4u; ++q)
                        sum = sum * 1000003ull + qw(vu, top + q);
                    sum = sum * 1000003ull + qw(vu, 0x3F0u);         // previous run's output
                    sum = sum * 1000003ull + qw(vu, 0x100u);         // VIF-overwritten after the last run
                    sum = sum * 1000003ull + qw(vu, 0x101u) + qw(vu, 0x101u) * 3u + (qw(vu, 0x101u) >> 32);
                    uint64_t code = 0;
                    std::memcpy(&code, mem.getVU1Code() + (startPC & 0x3FF8u), sizeof(code));
                    sum = sum * 1000003ull + code;
                    const uint64_t chain[2] = {sum, sum ^ 0x5555u};
                    std::memcpy(vu + 0x3F0u * 16u, chain, sizeof(chain)); // RAW for the next run
                    const uint64_t full[2] = {sum + 1u, sum + 2u};
                    std::memcpy(vu + 0x100u * 16u, full, sizeof(full));   // VIF overwrites all of it
                    std::memcpy(vu + 0x101u * 16u, full, sizeof(full));   // VIF overwrites lanes 0-1
                    const uint64_t pkt[4] = {1ull | (1ull << 15) | (1ull << 60), 0xEull, sum, 0x42ull};
                    if (!ps2_vu1_engine::captureXgkick(reinterpret_cast<const uint8_t *>(pkt), sizeof(pkt)))
                        mem.submitGifPacket(GifPathId::Path1, reinterpret_cast<const uint8_t *>(pkt), sizeof(pkt));
                };
                eng.setCanonical(mem.getVU1Data());
                eng.setRunFn([&](const ps2_vu1_engine::RunJob &job, uint8_t *data)
                             { program(data, job.startPC, job.top, job.itop); });
                eng.setPath1Fn([&](const uint8_t *data, uint32_t size)
                               { mem.submitGifPacket(GifPathId::Path1, data, size); });
                mem.setVu1MscalCallback([&](uint32_t startPC, uint32_t top, uint32_t itop)
                {
                    ++out.runs;
                    if (eng.sequencing())
                    {
                        ps2_vu1_engine::RunJob job;
                        job.startPC = startPC;
                        job.top = top;
                        job.itop = itop;
                        eng.dispatch(job);
                        return;
                    }
                    program(mem.getVU1Data(), startPC, top, itop);
                });
                uint8_t *rdram = mem.getRDRAM();
                constexpr uint32_t kVif1 = 0x10009000u, kGif = 0x1000A000u;
                constexpr uint32_t kSrc = 0x00100000u, kGifSrc = 0x00180000u;
                for (uint32_t i = 0; i < 80u; ++i)
                {
                    std::vector<uint8_t> v;
                    put32(v, vifCmd(0x01u, 0u, 0x0404u));              // STCYCL 4/4
                    put32(v, vifCmd(0x03u, 0u, (i * 8u) & 0x3F0u));    // BASE
                    put32(v, vifCmd(0x02u, 0u, 0x40u));                // OFFSET
                    put32(v, vifCmd(0x6Cu, 4u, 0x8000u));              // UNPACK V4-32, 4 qw, +TOPS
                    for (uint32_t w = 0; w < 16u; ++w)
                        put32(v, i * 131u + w * 7u);
                    if (i % 3u == 0u)
                    {
                        put32(v, vifCmd(0x4Au, 1u, static_cast<uint16_t>((i & 7u) * 2u))); // MPG 1 insn at startPC
                        put32(v, 0xC0DE0000u | i);
                        put32(v, 0x0BAD0000u | (i * 3u));
                    }
                    put32(v, vifCmd(0x14u, 0u, static_cast<uint16_t>((i & 7u) * 2u))); // MSCAL
                    // Two runs per kick on odd i (runs back to back: the second waits for the first).
                    if (i & 1u)
                        put32(v, vifCmd(0x14u, 0u, static_cast<uint16_t>(((i + 3u) & 7u) * 2u)));
                    put32(v, vifCmd(0x6Cu, 1u, 0x0100u));              // UNPACK V4-32 to qw 0x100 (WAW, all lanes)
                    for (uint32_t w = 0; w < 4u; ++w)
                        put32(v, 0xF1000000u | (i << 8) | w);
                    put32(v, vifCmd(0x20u, 0u, 0u));                   // STMASK: lanes 2,3 write-protected
                    put32(v, 0x000000F0u);
                    put32(v, vifCmd(0x7Cu, 1u, 0x0101u));              // UNPACK V4-32 masked to qw 0x101
                    for (uint32_t w = 0; w < 4u; ++w)
                        put32(v, 0xF2000000u | (i << 8) | w);
                    put32(v, vifCmd(0x06u, 0u, 0x0000u));              // MSKPATH3 0 (window)
                    put32(v, vifCmd(0x06u, 0u, 0x8000u));              // MSKPATH3 1
                    while (v.size() % 16u != 12u)
                        put32(v, 0u);                                  // NOP pad
                    put32(v, vifCmd(0x50u, 0u, 2u));                   // DIRECT 2 qw
                    putAd(v, true, 0x1000u + i);
                    while (v.size() % 16u)
                        put32(v, 0u);
                    std::memcpy(rdram + kSrc, v.data(), v.size());
                    mem.writeIORegister(kVif1 + 0x10u, kSrc);
                    mem.writeIORegister(kVif1 + 0x20u, static_cast<uint32_t>(v.size() / 16u));
                    mem.writeIORegister(kVif1 + 0x00u, 0x101u);
                    std::memset(rdram + kSrc, 0xCD, v.size());
                    if (i % 5u == 0u)
                    {
                        std::vector<uint8_t> g;
                        for (uint32_t k = 0; k < 3u; ++k)
                            putAd(g, true, (static_cast<uint64_t>(i) << 8) | k);
                        std::memcpy(rdram + kGifSrc, g.data(), g.size());
                        mem.writeIORegister(kGif + 0x10u, kGifSrc);
                        mem.writeIORegister(kGif + 0x20u, static_cast<uint32_t>(g.size() / 16u));
                        mem.writeIORegister(kGif + 0x00u, 0x100u);
                        std::memset(rdram + kGifSrc, 0xEE, g.size());
                    }
                    if (i % 6u == 2u)
                    {
                        const uint32_t fifo[4] = {vifCmd(0x06u, 0u, 0x0000u), vifCmd(0x06u, 0u, 0x8000u), 0u, 0u};
                        __m128i q;
                        std::memcpy(&q, fifo, sizeof(q));
                        mem.write128(0x10005000u, q); // VIF1 FIFO: an unmask window
                    }
                    if (i % 3u == 0u)
                        mem.write32(0x1100C000u + ((i * 48u) & 0x3FF0u), 0xA5000000u | i); // VU1 data (sync)
                    if (i % 4u == 1u)
                        out.eeReads.push_back(mem.read32(0x1100C000u + 0x3F00u)); // the run chain word (sync)
                }
                ps2_mtvu::syncAll();
                out.vu1Data.assign(mem.getVU1Data(), mem.getVU1Data() + PS2_VU1_DATA_SIZE);
                out.vu1Code.assign(mem.getVU1Code(), mem.getVU1Code() + PS2_VU1_CODE_SIZE);
                out.row0 = mem.vif1_regs.row[0];
                out.masked = mem.isPath3Masked();
                out.committed = eng.committedForTest() - committed0;
                ps2_mtvu::setModeForTest(ps2_mtvu::Mode::Off);
                eng.setWorkersForTest(0);
                eng.setRunFn({});
                eng.setPath1Fn({});
                mem.setGifArbiter(nullptr);
                return out;
            };
            const Outcome base = run(ps2_mtvu::Mode::Off, 0, 50u, 0u);
            const Outcome mt = run(ps2_mtvu::Mode::Threaded, 0, 50u, 0u);
            const Outcome w1 = run(ps2_mtvu::Mode::Threaded, 1, 50u, 0u);
            const Outcome w1cv = run(ps2_mtvu::Mode::Threaded, 1, 0u, 0u);
            const Outcome w1jit = run(ps2_mtvu::Mode::Threaded, 1, 50u, 300u);
            const Outcome syncW1 = run(ps2_mtvu::Mode::Off, 1, 50u, 0u); // engine idle off the unit thread
            t.Equals(base.runs, static_cast<size_t>(120u), "every MSCAL runs");
            t.IsTrue(base.packets.size() > 200u, "scenario should produce a long GIF stream");
            t.Equals(w1.committed, static_cast<uint64_t>(120u), "W=1 commits every run through the engine");
            t.Equals(w1jit.committed, static_cast<uint64_t>(120u), "jitter run commits every run");
            t.Equals(mt.committed, static_cast<uint64_t>(0u), "W=0 never uses the engine");
            t.Equals(syncW1.committed, static_cast<uint64_t>(0u), "synchronous mode never uses the engine");
            // PS2X_VP2_TEST_REPEAT=N: N more W=1 runs (spin, condvar, 20 us jitter) as a race stress.
            std::vector<Outcome> extra;
            if (const char *rep = std::getenv("PS2X_VP2_TEST_REPEAT"))
                for (long r = std::strtol(rep, nullptr, 10); r > 0; --r)
                    extra.push_back(run(ps2_mtvu::Mode::Threaded, 1, (r % 3 == 0) ? 0u : 50u, (r % 3 == 2) ? 20u : 0u));
            std::vector<const Outcome *> all{&mt, &w1, &w1cv, &w1jit, &syncW1};
            for (const Outcome &o : extra)
                all.push_back(&o);
            for (const Outcome *o : all)
            {
                t.IsTrue(o->packets == base.packets, "GIF packet sequence (path + bytes) is identical");
                t.IsTrue(o->vu1Data == base.vu1Data, "VU1 data memory is identical");
                t.IsTrue(o->vu1Code == base.vu1Code, "VU1 code memory is identical");
                t.IsTrue(o->eeReads == base.eeReads, "every EE read-back is identical");
                t.Equals(o->row0, base.row0, "VIF1 ROW is identical");
                t.Equals(o->masked, base.masked, "PATH3 mask state is identical");
            }
        });
    });
}
