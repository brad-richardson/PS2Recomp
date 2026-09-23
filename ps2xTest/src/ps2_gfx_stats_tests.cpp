#include "MiniTest.h"
#include "ps2_gfx_stats.h"
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
#include <vector>

namespace
{
    constexpr uint32_t kVuUpperNop = 0x000002FFu;
    constexpr uint32_t kVuUpperNopEbit = 0x400002FFu;

    std::string statsTmpPath(const char *name)
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

    void writeVuPair(uint8_t *code, uint32_t pc, uint32_t lower, uint32_t upper)
    {
        std::memcpy(code + pc, &lower, sizeof(lower));
        std::memcpy(code + pc + sizeof(lower), &upper, sizeof(upper));
    }

    void appendU64(std::vector<uint8_t> &bytes, uint64_t value)
    {
        const uint8_t *src = reinterpret_cast<const uint8_t *>(&value);
        bytes.insert(bytes.end(), src, src + sizeof(value));
    }

    uint32_t makeVifCmd(uint8_t opcode, uint8_t num, uint16_t imm)
    {
        return (static_cast<uint32_t>(opcode) << 24) |
               (static_cast<uint32_t>(num) << 16) |
               static_cast<uint32_t>(imm);
    }

    void appendU32(std::vector<uint8_t> &bytes, uint32_t value)
    {
        const uint8_t *src = reinterpret_cast<const uint8_t *>(&value);
        bytes.insert(bytes.end(), src, src + sizeof(value));
    }
}

void register_ps2_gfx_stats_tests()
{
    MiniTest::Case("Ps2GfxStats", [](TestCase &tc)
    {
        tc.Run("stats are off by default", [](TestCase &t)
        {
            ps2_gfx_stats::clearForTest();
            t.IsTrue(!ps2_gfx_stats::enabled(), "stats must be disabled with no file configured");
            // Disabled taps must be safe to call from any site.
            ps2_gfx_stats::noteMscal();
            ps2_gfx_stats::noteMscnt();
            ps2_gfx_stats::noteXgkick();
            ps2_gfx_stats::noteVuRun(10u, true);
            ps2_gfx_stats::noteGifPacket(GifPathId::Path1, 16u);
            ps2_gfx_stats::noteDraw(GifPathId::Path3, 3u, 0.0f, 1.0f, 0.0f, 1.0f, 0.0, 1.0, 0u, 3u);
            ps2_gfx_stats::noteVsync(1u);
            t.IsTrue(!ps2_gfx_stats::enabled(), "disabled taps must not arm the module");
        });

        tc.Run("per-vsync line carries the full census", [](TestCase &t)
        {
            const std::string tmp = statsTmpPath("ps2x-gfx-stats-e33-full.txt");
            std::remove(tmp.c_str());
            t.IsTrue(ps2_gfx_stats::configureForTest(tmp.c_str()), "test config should install");
            t.IsTrue(ps2_gfx_stats::enabled(), "stats must be enabled once configured");

            // The first noteVsync opens the window; earlier notes belong to
            // no window and are not counted.
            ps2_gfx_stats::noteVsync(7u);
            ps2_gfx_stats::noteMscal();
            ps2_gfx_stats::noteMscal();
            ps2_gfx_stats::noteMscal();
            ps2_gfx_stats::noteMscnt();
            ps2_gfx_stats::noteVuRun(100u, false);
            ps2_gfx_stats::noteVuRun(65536u, true);
            ps2_gfx_stats::noteXgkick();
            ps2_gfx_stats::noteXgkick();
            ps2_gfx_stats::noteGifPacket(GifPathId::Path1, 64u);
            ps2_gfx_stats::noteGifPacket(GifPathId::Path3, 128u);
            ps2_gfx_stats::noteDraw(GifPathId::Path1, 3u, 10.0f, 20.0f, 30.0f, 40.0f, 1.0, 5.0, 112u, 3u);
            ps2_gfx_stats::noteDraw(GifPathId::Path1, 3u, 0.0f, 30.0f, 0.0f, 50.0f, 0.0, 9.0, 112u, 6u);

            ps2_gfx_stats::noteVsync(8u);

            const std::string text = readWholeFile(tmp);
            const std::string expected =
                "vsync=7 mscal=3 mscnt=1 vu_cycles=65636 vu_exhausted=1 vu_maxcyc=65536 xgkick=2 "
                "p1_pkt=1 p1_bytes=64 p2_pkt=0 p2_bytes=0 p3_pkt=1 p3_bytes=128 "
                "d1_n=2 d1_vert=6 d1_box=0.00,30.00,0.00,50.00,0.00,9.00 "
                "d2_n=0 d2_vert=0 d2_box=none "
                "d3_n=0 d3_vert=0 d3_box=none "
                "top=112:3=1,112:6=1\n";
            t.Equals(text, expected, "the flushed window must carry every counter");

            ps2_gfx_stats::clearForTest();
            std::remove(tmp.c_str());
        });

        tc.Run("from/to window filters vsyncs", [](TestCase &t)
        {
            const std::string tmp = statsTmpPath("ps2x-gfx-stats-e33-window.txt");
            std::remove(tmp.c_str());
            t.IsTrue(ps2_gfx_stats::configureForTest(tmp.c_str(), 10u, 10u), "window config should install");

            ps2_gfx_stats::noteVsync(9u);
            ps2_gfx_stats::noteMscal();
            ps2_gfx_stats::noteVsync(10u);
            ps2_gfx_stats::noteMscnt();
            ps2_gfx_stats::noteVsync(11u);

            const std::string text = readWholeFile(tmp);
            t.IsTrue(text.compare(0, 9, "vsync=10 ") == 0, "only the in-window vsync is emitted");
            t.IsTrue(text.find("mscal=0") != std::string::npos, "window 9 counts stay out");
            t.IsTrue(text.find("mscnt=1") != std::string::npos, "window 10 counts are kept");
            t.IsTrue(text.find('\n') == text.size() - 1, "exactly one line is emitted");

            ps2_gfx_stats::clearForTest();
            std::remove(tmp.c_str());
        });

        tc.Run("vu1 budget exhaustion is counted, e-bit programs are not", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            GS gs;
            gs.init(mem.getGSVRAM(), static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &mem.gs());
            uint8_t *code = mem.getVU1Code();
            uint8_t *data = mem.getVU1Data();
            std::memset(code, 0, PS2_VU1_CODE_SIZE);
            std::memset(data, 0, PS2_VU1_DATA_SIZE);

            const std::string tmp = statsTmpPath("ps2x-gfx-stats-e33-budget.txt");
            std::remove(tmp.c_str());
            t.IsTrue(ps2_gfx_stats::configureForTest(tmp.c_str()), "test config should install");
            ps2_gfx_stats::noteVsync(20u);

            // Program A: 64 NOPs, budget 32 -> truncated, no E-bit.
            for (uint32_t i = 0; i < 64u; ++i)
            {
                writeVuPair(code, i * 8u, 0u, kVuUpperNop);
            }
            VU1Interpreter vuTruncated;
            vuTruncated.execute(code, PS2_VU1_CODE_SIZE, data, PS2_VU1_DATA_SIZE,
                                gs, &mem, 0u, 0u, 0u, 32u);
            ps2_gfx_stats::noteVsync(21u);

            // Program B: E-bit NOP at pc 0 -> ends on its own terms.
            writeVuPair(code, 0u, 0u, kVuUpperNopEbit);
            VU1Interpreter vuClean;
            vuClean.execute(code, PS2_VU1_CODE_SIZE, data, PS2_VU1_DATA_SIZE,
                            gs, &mem, 0u, 0u, 0u, 32u);
            ps2_gfx_stats::noteVsync(22u);

            const std::string text = readWholeFile(tmp);
            std::istringstream lines(text);
            std::string lineA, lineB;
            std::getline(lines, lineA);
            std::getline(lines, lineB);
            t.IsTrue(lineA.find("vsync=20 ") == 0, "first window is vsync 20");
            t.IsTrue(lineA.find("vu_exhausted=1") != std::string::npos,
                     "truncated program counts one exhausted budget");
            t.IsTrue(lineA.find("vu_cycles=32") != std::string::npos,
                     "truncated program consumes the whole budget");
            t.IsTrue(lineB.find("vsync=21 ") == 0, "second window is vsync 21");
            t.IsTrue(lineB.find("vu_exhausted=0") != std::string::npos,
                     "e-bit program exhausts nothing");

            ps2_gfx_stats::clearForTest();
            std::remove(tmp.c_str());
        });

        tc.Run("vif mscal and mscnt are counted", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            const std::string tmp = statsTmpPath("ps2x-gfx-stats-e33-vif.txt");
            std::remove(tmp.c_str());
            t.IsTrue(ps2_gfx_stats::configureForTest(tmp.c_str()), "test config should install");
            ps2_gfx_stats::noteVsync(40u);

            std::vector<uint8_t> mscal;
            appendU32(mscal, makeVifCmd(0x14u, 0u, 0u)); // MSCAL
            mem.processVIF1Data(mscal.data(), static_cast<uint32_t>(mscal.size()));
            std::vector<uint8_t> mscalf;
            appendU32(mscalf, makeVifCmd(0x15u, 0u, 0u)); // MSCALF
            mem.processVIF1Data(mscalf.data(), static_cast<uint32_t>(mscalf.size()));
            std::vector<uint8_t> mscnt;
            appendU32(mscnt, makeVifCmd(0x17u, 0u, 0u)); // MSCNT
            mem.processVIF1Data(mscnt.data(), static_cast<uint32_t>(mscnt.size()));
            ps2_gfx_stats::noteVsync(41u);

            const std::string text = readWholeFile(tmp);
            t.IsTrue(text.find("vsync=40 mscal=2 mscnt=1") != std::string::npos,
                     "mscal/mscalf/mscnt land in the window");

            ps2_gfx_stats::clearForTest();
            std::remove(tmp.c_str());
        });

        tc.Run("arbiter listener observes the path before processing", [](TestCase &t)
        {
            bool processed = false;
            GifArbiter arbiter([&](const uint8_t *data, uint32_t sizeBytes)
            {
                (void)data;
                (void)sizeBytes;
                processed = true;
            });
            std::vector<std::pair<GifPathId, uint32_t>> seen;
            arbiter.setPacketListener([&](GifPathId path, uint32_t sizeBytes)
            {
                seen.emplace_back(path, sizeBytes);
            });

            uint8_t bytes[32] = {};
            arbiter.submit(GifPathId::Path2, bytes, sizeof(bytes));
            arbiter.drain();

            t.IsTrue(processed, "process function still runs with a listener set");
            t.Equals(static_cast<uint32_t>(seen.size()), static_cast<uint32_t>(1),
                     "listener fires once per packet");
            t.IsTrue(seen[0].first == GifPathId::Path2, "listener observes the packet path");
            t.Equals(seen[0].second, static_cast<uint32_t>(32), "listener observes the packet size");
        });

        tc.Run("gs draw attributes to the noted gif path", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            GS gs;
            gs.init(mem.getGSVRAM(), static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &mem.gs());

            const std::string tmp = statsTmpPath("ps2x-gfx-stats-e33-draw.txt");
            std::remove(tmp.c_str());
            t.IsTrue(ps2_gfx_stats::configureForTest(tmp.c_str()), "test config should install");
            ps2_gfx_stats::noteVsync(30u);

            // PACKED sprite: one PRIM tag (PRIM resets the vertex queue,
            // so it is sent once), then one XYZ2 tag with two vertices.
            gs.noteGifPath(GifPathId::Path2);
            std::vector<uint8_t> pkt;
            appendU64(pkt, 1u | (1ull << 60));          // tag1: nloop=1, nreg=1, packed
            appendU64(pkt, 0x00ull);                    // regs: PRIM
            appendU64(pkt, 6u);                         // PRIM = sprite
            appendU64(pkt, 0u);                         // PRIM hi
            appendU64(pkt, 2u | (1ull << 15) | (1ull << 60)); // tag2: nloop=2, eop, nreg=1
            appendU64(pkt, 0x05ull);                    // regs: XYZ2
            const uint64_t xyz1Lo = (static_cast<uint64_t>(0x0200u) << 32) | 0x0100u; // (16.0, 32.0)
            const uint64_t xyz2Lo = (static_cast<uint64_t>(0x0400u) << 32) | 0x0300u; // (48.0, 64.0)
            appendU64(pkt, xyz1Lo);
            appendU64(pkt, 100u); // z
            appendU64(pkt, xyz2Lo);
            appendU64(pkt, 200u); // z
            gs.processGIFPacket(pkt.data(), static_cast<uint32_t>(pkt.size()));

            ps2_gfx_stats::noteVsync(31u);

            const std::string text = readWholeFile(tmp);
            t.IsTrue(text.find("d2_n=1 d2_vert=2 d2_box=16.00,48.00,32.00,64.00,100.00,200.00") != std::string::npos,
                     "the sprite draw lands on path 2 with its screen box");
            t.IsTrue(text.find("top=0:6=1") != std::string::npos,
                     "the draw reports its (TBP0, PRIM) tuple");
            t.IsTrue(text.find("d1_n=0") != std::string::npos, "path 1 stays empty");
            t.IsTrue(text.find("d3_n=0") != std::string::npos, "path 3 stays empty");

            t.IsTrue(text.find("d2_scr=1,0,0") != std::string::npos,
                     "the GS tap classifies the on-screen sprite (E50)");

            ps2_gfx_stats::clearForTest();
            std::remove(tmp.c_str());
        });

        tc.Run("E50 screen class against scissor and offset", [](TestCase &t)
        {
            // XYOFFSET 1792/1824 (raw 12.4), scissor 0..511 x 0..447.
            const uint16_t ofx = 0x7000u;
            const uint16_t ofy = 0x7200u;
            using ps2_gfx_stats::classifyScreen;
            t.Equals(classifyScreen(1800.0f, 1810.0f, 1830.0f, 1840.0f, ofx, ofy, 0, 511, 0, 447),
                     ps2_gfx_stats::kScrOn, "inside the viewport is on");
            t.Equals(classifyScreen(2270.0f, 2550.0f, 1670.0f, 1813.0f, ofx, ofy, 0, 511, 0, 447),
                     ps2_gfx_stats::kScrOff, "above the top edge is off");
            t.Equals(classifyScreen(2305.0f, 2400.0f, 1900.0f, 1950.0f, ofx, ofy, 0, 511, 0, 447),
                     ps2_gfx_stats::kScrOff, "right of the right edge is off");
            t.Equals(classifyScreen(1023.5f, 3071.5f, 1023.5f, 3071.5f, ofx, ofy, 0, 511, 0, 447),
                     ps2_gfx_stats::kScrStraddle, "a guard-band-wide triangle straddles");
            t.Equals(classifyScreen(2303.9f, 2303.9f, 2271.9f, 2271.9f, ofx, ofy, 0, 511, 0, 447),
                     ps2_gfx_stats::kScrOn, "the last pixel is on");
            t.Equals(classifyScreen(2304.0f, 2304.0f, 1900.0f, 1900.0f, ofx, ofy, 0, 511, 0, 447),
                     ps2_gfx_stats::kScrOff, "one past the last pixel is off");
        });

        tc.Run("E50 dN_scr and pcs fields are appended only when seen", [](TestCase &t)
        {
            const std::string tmp = statsTmpPath("ps2x-gfx-stats-e50-scr.txt");
            std::remove(tmp.c_str());
            t.IsTrue(ps2_gfx_stats::configureForTest(tmp.c_str()), "test config should install");

            ps2_gfx_stats::noteMscalPc(0x88u); // before any window: sets curPc, not counted
            ps2_gfx_stats::noteVsync(1u);
            ps2_gfx_stats::noteDraw(GifPathId::Path1, 3u, 0.0f, 1.0f, 0.0f, 1.0f, 0.0, 1.0, 0u, 4u);
            ps2_gfx_stats::noteDrawScreen(GifPathId::Path1, ps2_gfx_stats::kScrOff);
            ps2_gfx_stats::noteMscalPc(0x10u);
            ps2_gfx_stats::noteMscalPc(0x10u);
            ps2_gfx_stats::noteDraw(GifPathId::Path1, 3u, 0.0f, 1.0f, 0.0f, 1.0f, 0.0, 1.0, 0u, 4u);
            ps2_gfx_stats::noteDrawScreen(GifPathId::Path1, ps2_gfx_stats::kScrOn);
            ps2_gfx_stats::noteDraw(GifPathId::Path1, 3u, 0.0f, 1.0f, 0.0f, 1.0f, 0.0, 1.0, 0u, 4u);
            ps2_gfx_stats::noteDrawScreen(GifPathId::Path1, ps2_gfx_stats::kScrStraddle);
            ps2_gfx_stats::noteDraw(GifPathId::Path3, 2u, 0.0f, 1.0f, 0.0f, 1.0f, 0.0, 1.0, 0u, 6u);
            ps2_gfx_stats::noteDrawScreen(GifPathId::Path3, ps2_gfx_stats::kScrOn);
            ps2_gfx_stats::noteVsync(2u);
            // A window with no E50 notes keeps the pre-E50 line shape.
            ps2_gfx_stats::noteVsync(3u);

            const std::string text = readWholeFile(tmp);
            const size_t nl = text.find('\n');
            const std::string first = text.substr(0, nl);
            const std::string second = text.substr(nl + 1);
            t.IsTrue(first.find(" d1_scr=1,1,1 d3_scr=1,0,0 pcs=0x10:2:1/0/1;0x88:0:0/1/0") != std::string::npos,
                     "per-path classes and per-PC attribution are appended");
            t.IsTrue(second.find("scr=") == std::string::npos && second.find("pcs=") == std::string::npos,
                     "a window without E50 notes has no E50 fields");

            ps2_gfx_stats::clearForTest();
            std::remove(tmp.c_str());
        });
    });
}
