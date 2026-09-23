// E36 DEV-ONLY per-program VU1 trace tests (PS2X_VU1_TRACE).
#include "MiniTest.h"
#include "ps2_vu1_trace.h"
#include "runtime/gs/ps2_gif_arbiter.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_vu1.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace
{
    constexpr uint32_t kVuUpperNop = 0x000002FFu;

    struct Vu1Fixture
    {
        PS2Memory mem;
        GS gs;
        uint8_t *code = nullptr;
        uint8_t *data = nullptr;

        bool initialize()
        {
            if (!mem.initialize())
                return false;
            gs.init(mem.getGSVRAM(), static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &mem.gs());
            code = mem.getVU1Code();
            data = mem.getVU1Data();
            std::memset(code, 0, PS2_VU1_CODE_SIZE);
            std::memset(data, 0, PS2_VU1_DATA_SIZE);
            return code != nullptr && data != nullptr;
        }
    };

    void writeVuPair(uint8_t *code, uint32_t pc, uint32_t lower, uint32_t upper)
    {
        std::memcpy(code + pc, &lower, sizeof(lower));
        std::memcpy(code + pc + sizeof(lower), &upper, sizeof(upper));
    }

    std::string traceTmpPath(const char *name)
    {
        return (std::filesystem::temp_directory_path() / name).string();
    }

    std::string readWholeFile(const std::string &path)
    {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream out;
        out << in.rdbuf();
        return out.str();
    }

    bool contains(const std::string &haystack, const std::string &needle)
    {
        return haystack.find(needle) != std::string::npos;
    }

    size_t countOccurrences(const std::string &haystack, const std::string &needle)
    {
        size_t count = 0u;
        size_t pos = 0u;
        while ((pos = haystack.find(needle, pos)) != std::string::npos)
        {
            ++count;
            pos += needle.size();
        }
        return count;
    }

    // B -1: unconditional self-loop (target = pc+8-8 = pc).
    constexpr uint32_t kLowerBSelf = (0x20u << 25) | 0x7FFu;
    constexpr uint32_t kVuUpperNopEbit = 0x400002FFu;

    uint32_t makeVuLowerSpecial(uint8_t specialOp, uint8_t is)
    {
        return (0x40u << 25) |
               (static_cast<uint32_t>(is & 0x1Fu) << 11) |
               (static_cast<uint32_t>(specialOp & 0x7Cu) << 4) |
               static_cast<uint32_t>(specialOp & 0x3u) |
               0x3Cu;
    }
    // IADDIU vi01,vi01,1.
    constexpr uint32_t kLowerIaddiu = (0x08u << 25) | (1u << 16) | (1u << 11) | 1u;
    // IBNE vi01,vi02,-3 (target = pc+8-24). One NOP sits between the
    // counter increment and the branch: the interpreter models the VI
    // write→branch hazard, so an immediately-following IBNE would read
    // the stale counter and never take.
    constexpr uint32_t kLowerIbne = (0x29u << 25) | (1u << 16) | (2u << 11) | 0x7FDu;
}

void register_ps2_vu1_trace_tests()
{
    MiniTest::Case("Ps2Vu1Trace", [](TestCase &tc)
    {
        tc.Run("trace is off by default", [](TestCase &t)
        {
            ps2_vu1_trace::clearForTest();
            t.IsTrue(!ps2_vu1_trace::enabled(), "trace must be disabled with nothing configured");
            // Disabled taps must be safe to call from any site.
            ps2_vu1_trace::noteVsync(1u);
            ps2_vu1_trace::noteMscal(false, 0u, 0u, 0u, 0u, 0u, 0u, 0u, false);
            ps2_vu1_trace::MscalContext ctx;
            t.IsTrue(!ps2_vu1_trace::consumeContext(ctx), "no context may surface while off");
            t.IsTrue(!ps2_vu1_trace::armFor(0u), "nothing may arm while off");
            ps2_vu1_trace::noteCensus(0u, 65536u, 0u);
            t.IsTrue(!ps2_vu1_trace::emitDetail(0u, "detail\n"), "nothing may emit while off");
            t.IsTrue(!ps2_vu1_trace::enabled(), "disabled taps must not arm the module");
        });

        tc.Run("stuck B-loop emits census and a detail block", [](TestCase &t)
        {
            const std::string tmp = traceTmpPath("ps2x-vu1-trace-e36-bloop.txt");
            std::remove(tmp.c_str());
            t.IsTrue(ps2_vu1_trace::configureForTest(tmp.c_str()), "test config should install");

            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");
            writeVuPair(fx.code, 0u, kLowerBSelf, kVuUpperNop);
            writeVuPair(fx.code, 8u, 0u, kVuUpperNop);

            ps2_vu1_trace::noteVsync(5u);
            ps2_vu1_trace::noteMscal(false, 0u, 3u, 9u, 1u, 2u, 3u, 9u, false);
            VU1Interpreter vu;
            vu.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE,
                       fx.gs, &fx.mem, 0u, 3u, 9u, 32u);

            const std::string text = readWholeFile(tmp);
            t.IsTrue(contains(text, "census vsync=5 startPC=0x0 cycles=32 xgkick=0"),
                     "an exhausted program must leave a census line");
            t.IsTrue(contains(text, "detail startPC=0x0 top=3 itop=9 base=1 ofst=2 tops=3 itops=9 dbf=0 ctx=mscal"),
                     "the detail block must carry the MSCAL snapshot");
            t.IsTrue(contains(text, "cycles=32 xgkick=0"), "the detail block must carry run totals");
            t.IsTrue(contains(text, "hist 0x0=16 0x8=16"), "the histogram must show the 2-pair loop");
            t.IsTrue(contains(text, "taken 0x0=16"), "every B issues taken");
            t.IsTrue(contains(text, "branch pc=0x0 op=B") && contains(text, "target=0x0") &&
                         contains(text, "taken=16 visits=16"),
                     "the hottest backward branch must be the self-loop B");
            t.IsTrue(contains(text, "body 0x0 lo=0x400007ff") && contains(text, "B -1"),
                     "the loop body must disassemble the branch");

            // A repeat of the same startPC stays census-only.
            vu.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE,
                       fx.gs, &fx.mem, 0u, 3u, 9u, 32u);
            const std::string text2 = readWholeFile(tmp);
            t.Equals(countOccurrences(text2, "detail startPC="), 1u,
                     "one startPC earns one detail block");
            t.Equals(countOccurrences(text2, "census vsync=5"), 2u,
                     "every exhausted run still leaves a census line");

            ps2_vu1_trace::clearForTest();
            std::remove(tmp.c_str());
        });

        tc.Run("IBNE loop names operands and end-of-slice VI", [](TestCase &t)
        {
            const std::string tmp = traceTmpPath("ps2x-vu1-trace-e36-ibne.txt");
            std::remove(tmp.c_str());
            t.IsTrue(ps2_vu1_trace::configureForTest(tmp.c_str()), "test config should install");

            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");
            writeVuPair(fx.code, 0u, kLowerIaddiu, kVuUpperNop);
            writeVuPair(fx.code, 8u, 0u, kVuUpperNop);
            writeVuPair(fx.code, 16u, kLowerIbne, kVuUpperNop);
            writeVuPair(fx.code, 24u, 0u, kVuUpperNop);

            ps2_vu1_trace::noteVsync(6u);
            VU1Interpreter vu;
            vu.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE,
                       fx.gs, &fx.mem, 0u, 0u, 0u, 40u);

            const std::string text = readWholeFile(tmp);
            t.IsTrue(contains(text, "hist 0x0=10 0x8=10 0x10=10 0x18=10"),
                     "the histogram must show the 4-pair counted loop");
            t.IsTrue(contains(text, "branch pc=0x10 op=IBNE is=2 it=1 imm=-3 target=0x0 taken=10 visits=10 vi_is=0 vi_it=10"),
                     "the branch line must name the tested registers and their end values");
            t.IsTrue(contains(text, "ctx=none"), "no MSCAL note means no context");
            t.IsTrue(contains(text, "IADDIU vi1,vi1,1"), "the counter increment must disassemble");

            ps2_vu1_trace::clearForTest();
            std::remove(tmp.c_str());
        });

        tc.Run("census xgkick counts each program's own kicks", [](TestCase &t)
        {
            const std::string tmp = traceTmpPath("ps2x-vu1-trace-e36-ownkicks.txt");
            std::remove(tmp.c_str());
            t.IsTrue(ps2_vu1_trace::configureForTest(tmp.c_str()), "test config should install");

            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");
            uint32_t kicked = 0u;
            fx.mem.setGifPacketCallback([&](const uint8_t *, uint32_t)
            {
                ++kicked;
            });
            // IMAGE-mode tag, NLOOP=1, at qword 0: one XGKICK delivers it.
            const uint64_t tag = 1u | (1ull << 15) | (2ull << 58);
            std::memcpy(fx.data, &tag, sizeof(tag));
            writeVuPair(fx.code, 0u, kLowerBSelf, kVuUpperNop);
            writeVuPair(fx.code, 8u, 0u, kVuUpperNop);
            writeVuPair(fx.code, 64u, makeVuLowerSpecial(0x6Cu, 0u), kVuUpperNop);
            writeVuPair(fx.code, 72u, 0u, kVuUpperNopEbit);

            ps2_vu1_trace::noteVsync(11u);
            VU1Interpreter vu;
            vu.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE,
                       fx.gs, &fx.mem, 0u, 0u, 0u, 32u);
            vu.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE,
                       fx.gs, &fx.mem, 64u, 0u, 0u, 256u);
            vu.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE,
                       fx.gs, &fx.mem, 0u, 0u, 0u, 32u);

            t.Equals(kicked, 1u, "the healthy program must issue exactly one XGKICK");
            const std::string text = readWholeFile(tmp);
            t.Equals(countOccurrences(text, "census vsync=11"), 2u,
                     "only the two exhausted runs leave census lines");
            t.Equals(countOccurrences(text, "census vsync=11 startPC=0x0 cycles=32 xgkick=0"), 2u,
                     "each census line carries its own program's count, not a stale one");

            ps2_vu1_trace::clearForTest();
            std::remove(tmp.c_str());
        });

        tc.Run("detail blocks stop at 40 distinct startPCs", [](TestCase &t)
        {
            const std::string tmp = traceTmpPath("ps2x-vu1-trace-e36-cap.txt");
            std::remove(tmp.c_str());
            t.IsTrue(ps2_vu1_trace::configureForTest(tmp.c_str()), "test config should install");

            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");
            for (uint32_t i = 0u; i < 41u; ++i)
            {
                writeVuPair(fx.code, i * 16u, kLowerBSelf, kVuUpperNop);
                writeVuPair(fx.code, i * 16u + 8u, 0u, kVuUpperNop);
            }

            ps2_vu1_trace::noteVsync(9u);
            VU1Interpreter vu;
            for (uint32_t i = 0u; i < 41u; ++i)
            {
                const uint32_t pc = i * 16u;
                ps2_vu1_trace::noteMscal(false, pc, 0u, 0u, 0u, 0u, 0u, 0u, false);
                vu.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE,
                           fx.gs, &fx.mem, pc, 0u, 0u, 32u);
            }

            const std::string text = readWholeFile(tmp);
            t.Equals(countOccurrences(text, "detail startPC="), 40u,
                     "detail blocks cap at 40 distinct startPCs");
            t.Equals(countOccurrences(text, "census vsync=9"), 41u,
                     "the census still covers every exhausted program");

            ps2_vu1_trace::clearForTest();
            std::remove(tmp.c_str());
        });
    });
}
