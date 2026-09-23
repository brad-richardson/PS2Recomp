// E37 DEV-ONLY VU1 entry trace tests (PS2X_VU1_ENTRY_TRACE).
#include "MiniTest.h"
#include "ps2_vu1_entry_trace.h"
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

    // IADDIU vi3,vi0,7.
    constexpr uint32_t kLowerIaddiuVi3 = (0x08u << 25) | (3u << 16) | (0u << 11) | 7u;
    // B +129: from 0x8, target = 0x10 + 129*8 = 0x418.
    constexpr uint32_t kLowerBToLoop = (0x20u << 25) | 129u;
    // B -1: self-loop.
    constexpr uint32_t kLowerBSelf = (0x20u << 25) | 0x7FFu;
}

void register_ps2_vu1_entry_trace_tests()
{
    MiniTest::Case("Ps2Vu1EntryTrace", [](TestCase &tc)
    {
        tc.Run("trace is off by default", [](TestCase &t)
        {
            ps2_vu1_entry_trace::clearForTest();
            t.IsTrue(!ps2_vu1_entry_trace::enabled(), "trace must be disabled with nothing configured");
            // Disabled taps must be safe to call from any site.
            ps2_vu1_entry_trace::noteVsync(1u);
            ps2_vu1_entry_trace::appendVif("vif NOP num=0 addr=- fmt=- usn=- mask=00000000 cl=1 wl=1 data=-");
            uint8_t dummy[16]{};
            ps2_vu1_entry_trace::noteMscalEntry(0x40u, false, dummy, sizeof(dummy));
            t.IsTrue(!ps2_vu1_entry_trace::takeArm(0x40u), "nothing may arm while off");
            ps2_vu1_entry_trace::finishEntry(0u, {}, 0u, 0u);
            t.IsTrue(!ps2_vu1_entry_trace::enabled(), "disabled taps must not arm the module");
        });

        tc.Run("PCS list parses hex and decimal", [](TestCase &t)
        {
            std::vector<uint32_t> pcs;
            t.IsTrue(ps2_vu1_entry_trace::parsePcsForTest("0x40,0x10", pcs), "hex list must parse");
            t.Equals(pcs.size(), 2u, "two items expected");
            t.Equals(pcs[0], 0x40u, "first item is 0x40");
            t.Equals(pcs[1], 0x10u, "second item is 0x10");
            t.IsTrue(ps2_vu1_entry_trace::parsePcsForTest("64, 16", pcs), "decimal list must parse");
            t.Equals(pcs[0], 64u, "decimal 64");
            t.Equals(pcs[1], 16u, "decimal 16");
            t.IsTrue(!ps2_vu1_entry_trace::parsePcsForTest("0x40,zz", pcs), "bad items must fail");
            t.IsTrue(!ps2_vu1_entry_trace::parsePcsForTest("", pcs), "empty list must fail");
        });

        tc.Run("first in-window MSCAL freezes vumem and the packet log", [](TestCase &t)
        {
            const std::string tmp = traceTmpPath("ps2x-vu1-entry-e37-freeze.txt");
            std::remove(tmp.c_str());
            t.IsTrue(ps2_vu1_entry_trace::configureForTest(tmp.c_str(), {0x40u, 0x10u}, 1300u),
                     "test config should install");

            uint8_t data[ps2_vu1_entry_trace::kVuMemWords * 4u]{};
            const uint32_t row5[4] = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
            std::memcpy(data + 5u * 16u, row5, sizeof(row5));

            // Before the vsync gate: no arm, packet log keeps flowing.
            ps2_vu1_entry_trace::noteVsync(1299u);
            ps2_vu1_entry_trace::appendVif("vif PRE-GATE");
            ps2_vu1_entry_trace::noteMscalEntry(0x40u, false, data, sizeof(data));
            t.IsTrue(!ps2_vu1_entry_trace::takeArm(0x40u), "pre-gate MSCAL must not arm");

            // In-window MSCAL freezes the log (which the pre-gate MSCAL reset).
            ps2_vu1_entry_trace::noteVsync(1300u);
            ps2_vu1_entry_trace::appendVif("vif IN-WINDOW-1");
            ps2_vu1_entry_trace::appendVif("vif IN-WINDOW-2");
            ps2_vu1_entry_trace::noteMscalEntry(0x40u, false, data, sizeof(data));
            t.IsTrue(ps2_vu1_entry_trace::takeArm(0x40u), "first in-window MSCAL must arm");
            t.IsTrue(!ps2_vu1_entry_trace::takeArm(0x40u), "the arm is one-shot");

            // A repeat MSCAL for the same PC never arms again.
            ps2_vu1_entry_trace::noteVsync(1301u);
            ps2_vu1_entry_trace::appendVif("vif REPEAT");
            ps2_vu1_entry_trace::noteMscalEntry(0x40u, false, data, sizeof(data));
            t.IsTrue(!ps2_vu1_entry_trace::takeArm(0x40u), "repeats must not re-arm");

            // An unlisted PC never arms.
            ps2_vu1_entry_trace::noteMscalEntry(0x50u, false, data, sizeof(data));
            t.IsTrue(!ps2_vu1_entry_trace::takeArm(0x50u), "unlisted PCs must not arm");

            ps2_vu1_entry_trace::finishEntry(0u, {"pair pc=0x40 up=000002ff lo=10030007 IADDIU vi3,vi0,7 | up NOP | vi3:0000->0007"},
                                             1u, 0u);
            const std::string text = readWholeFile(tmp);
            t.IsTrue(contains(text, "entry startPC=0x40 vsync=1300"), "header names PC and vsync");
            t.IsTrue(contains(text, "vumem 5 11111111 22222222 33333333 44444444"),
                     "vumem carries the MSCAL-entry snapshot");
            t.Equals(countOccurrences(text, "vumem "), 1024u, "vumem covers all 1024 rows");
            t.IsTrue(contains(text, "vif IN-WINDOW-1") && contains(text, "vif IN-WINDOW-2"),
                     "frozen packet log is written");
            t.IsTrue(!contains(text, "PRE-GATE") && !contains(text, "REPEAT"),
                     "only the triggering packet is frozen");
            t.IsTrue(contains(text, "endentry startPC=0x40 pairs=1 arrivals=0"),
                     "trailer closes the block");

            ps2_vu1_entry_trace::clearForTest();
            std::remove(tmp.c_str());
        });

        tc.Run("pair stream stops after the loop head plus 3 iterations", [](TestCase &t)
        {
            const std::string tmp = traceTmpPath("ps2x-vu1-entry-e37-pairs.txt");
            std::remove(tmp.c_str());
            t.IsTrue(ps2_vu1_entry_trace::configureForTest(tmp.c_str(), {0u}, 0u),
                     "test config should install");

            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");
            writeVuPair(fx.code, 0x0u, kLowerIaddiuVi3, kVuUpperNop);
            writeVuPair(fx.code, 0x8u, kLowerBToLoop, kVuUpperNop);
            writeVuPair(fx.code, 0x10u, 0u, kVuUpperNop);
            writeVuPair(fx.code, 0x418u, kLowerBSelf, kVuUpperNop);
            writeVuPair(fx.code, 0x420u, 0u, kVuUpperNop);

            ps2_vu1_entry_trace::noteVsync(7u);
            ps2_vu1_entry_trace::appendVif("vif STCYCL num=0 addr=- fmt=- usn=- mask=00000000 cl=1 wl=1 data=-");
            ps2_vu1_entry_trace::noteMscalEntry(0u, false, fx.data, PS2_VU1_DATA_SIZE);
            VU1Interpreter vu;
            vu.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE,
                       fx.gs, &fx.mem, 0u, 0u, 0u, 1000u);

            const std::string text = readWholeFile(tmp);
            t.IsTrue(contains(text, "entry startPC=0x0 vsync=7"), "block opens for the armed PC");
            t.IsTrue(contains(text, "pair pc=0x0") && contains(text, "IADDIU vi3,vi0,7") &&
                         contains(text, "vi3:0000->0007"),
                     "the setup pair records its VI write");
            t.IsTrue(contains(text, "B 129"), "the setup branch disassembles");
            t.Equals(countOccurrences(text, "pair pc=0x418"), 4u,
                     "the loop head is recorded 4 times (first arrival + 3 iterations)");
            t.IsTrue(contains(text, "endentry startPC=0x0 pairs=10 arrivals=4"),
                     "setup (3) + 3 loop passes (6) + 4th arrival (1) = 10 pairs");

            ps2_vu1_entry_trace::clearForTest();
            std::remove(tmp.c_str());
        });

        tc.Run("E50 all-mode captures each new startPC once with regs and a pair cap", [](TestCase &t)
        {
            const std::string tmp = traceTmpPath("ps2x-vu1-entry-e50-all.txt");
            std::remove(tmp.c_str());
            t.IsTrue(ps2_vu1_entry_trace::configureForTest(tmp.c_str(), {0u}, 5u),
                     "test config should install");
            ps2_vu1_entry_trace::setModeForTest(true, 2u, 100000u);

            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");
            writeVuPair(fx.code, 0x0u, kLowerIaddiuVi3, kVuUpperNop);
            writeVuPair(fx.code, 0x8u, kLowerBToLoop, kVuUpperNop);
            writeVuPair(fx.code, 0x10u, 0u, kVuUpperNop);
            writeVuPair(fx.code, 0x418u, kLowerBSelf, kVuUpperNop);
            writeVuPair(fx.code, 0x420u, 0u, kVuUpperNop);

            // Before the gate nothing registers.
            ps2_vu1_entry_trace::noteVsync(4u);
            ps2_vu1_entry_trace::noteMscalEntry(0x0u, false, fx.data, PS2_VU1_DATA_SIZE);
            t.IsTrue(!ps2_vu1_entry_trace::takeArm(0x0u), "pre-gate MSCAL must not arm");

            ps2_vu1_entry_trace::noteVsync(5u);
            ps2_vu1_entry_trace::noteMscalEntry(0x0u, false, fx.data, PS2_VU1_DATA_SIZE);
            VU1Interpreter vu;
            vu.state().vf[7][0] = 2.5f;
            vu.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE,
                       fx.gs, &fx.mem, 0u, 0u, 0u, 1000u);
            // A repeat of the same PC does not arm; a new PC does.
            ps2_vu1_entry_trace::noteMscalEntry(0x0u, false, fx.data, PS2_VU1_DATA_SIZE);
            t.IsTrue(!ps2_vu1_entry_trace::takeArm(0x0u), "repeat PC must not re-arm");
            ps2_vu1_entry_trace::noteMscalEntry(0x10u, false, fx.data, PS2_VU1_DATA_SIZE);
            t.IsTrue(ps2_vu1_entry_trace::takeArm(0x10u), "a new PC arms in all-mode");
            ps2_vu1_entry_trace::finishEntry(ps2_vu1_entry_trace::armIndex(0x10u), {}, 0u, 0u);

            const std::string text = readWholeFile(tmp);
            t.IsTrue(contains(text, "entry startPC=0x0 vsync=5"), "first PC block opens");
            t.IsTrue(contains(text, "entry startPC=0x10 vsync=5"), "second PC block opens");
            t.IsTrue(contains(text, "reg vf0 00000000 00000000 00000000 3f800000 | 0 0 0 1"),
                     "VF0 is dumped at entry");
            t.IsTrue(contains(text, "reg vf7 40200000 00000000 00000000 00000000 | 2.5 0 0 0"),
                     "VF registers are dumped as hex and float");
            t.IsTrue(contains(text, "reg vi0 00000000 0"), "VI registers are dumped");
            t.IsTrue(contains(text, "endentry startPC=0x0 pairs=2 arrivals=0"),
                     "the pair stream stops at MAXPAIRS");
            t.Equals(countOccurrences(text, "pair pc="), 2u, "only MAXPAIRS pair lines are kept");

            ps2_vu1_entry_trace::clearForTest();
            std::remove(tmp.c_str());
        });
    });
}
