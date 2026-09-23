#include "MiniTest.h"
#include "ps2_mpg_src_trace.h"
#include "runtime/ps2_memory.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace
{
    std::string srcTmpPath(const char *name)
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

    uint32_t srcMakeVifCmd(uint8_t opcode, uint8_t num, uint16_t imm)
    {
        return (static_cast<uint32_t>(opcode) << 24) |
               (static_cast<uint32_t>(num) << 16) |
               static_cast<uint32_t>(imm);
    }

    uint64_t srcMakeDmaTag(uint16_t qwc, uint8_t id, uint32_t addr)
    {
        return static_cast<uint64_t>(qwc) |
               (static_cast<uint64_t>(id & 0x7u) << 28) |
               (static_cast<uint64_t>(addr & 0x7FFFFFFFu) << 32);
    }

    void srcWriteDmaTag(uint8_t *rdram, uint32_t tagAddr, uint64_t tagLo)
    {
        std::memset(rdram + tagAddr, 0, 16);
        std::memcpy(rdram + tagAddr, &tagLo, sizeof(tagLo));
    }

    void srcSetReg(R5900Context &ctx, int reg, uint32_t value)
    {
        ctx.r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(value));
    }

    size_t countLines(const std::string &text)
    {
        size_t n = 0u;
        for (char c : text)
        {
            if (c == '\n')
                ++n;
        }
        return n;
    }
}

void register_ps2_mpg_src_trace_tests()
{
    MiniTest::Case("Ps2MpgSrcTrace", [](TestCase &tc)
    {
        tc.Run("trace is off by default", [](TestCase &t)
        {
            ps2_mpg_src_trace::clearForTest();
            t.IsTrue(!ps2_mpg_src_trace::enabled(), "trace must be disabled with no file configured");
            t.IsTrue(!ps2_mpg_src_trace::writeArmed(), "no watches may be armed when disabled");
            ps2_mpg_src_trace::noteMpgsrc(0u, 0x1000u, 3u, 1u, 0x435bf8u, 0u, 0u);
            t.IsTrue(!ps2_mpg_src_trace::writeArmed(), "disabled taps must not arm watches");
            R5900Context ctx{};
            ps2_mpg_src_trace::noteStoreCtx(nullptr, &ctx, 0x1004u, 4u, 1u, 0u, "sub_dead");
            t.IsTrue(!ps2_mpg_src_trace::enabled(), "disabled store taps must stay off");
        });

        tc.Run("mpgsrc line format arms tag word watch", [](TestCase &t)
        {
            const std::string tmp = srcTmpPath("ps2x-mpg-src-format.txt");
            std::remove(tmp.c_str());
            t.IsTrue(ps2_mpg_src_trace::configureForTest(tmp.c_str()), "test config should install");
            t.IsTrue(!ps2_mpg_src_trace::writeArmed(), "no watches before the first mpgsrc");

            ps2_mpg_src_trace::noteMpgsrc(1234u, 0x00110000u, 3u, 8u, 0x00435bf8u,
                                          0x00000000u, 0x4a000000u);
            t.IsTrue(ps2_mpg_src_trace::writeArmed(), "mpgsrc must arm the tag_at+4 watch");

            ps2_mpg_src_trace::clearForTest();
            const std::string text = readWholeFile(tmp);
            char expected[256];
            std::snprintf(expected, sizeof(expected),
                          "mpgsrc vsync=1234 tag_at=0x00110000 id=3 qwc=8 addr=0x00435bf8 tte_vif=000000004a000000\n");
            t.Equals(text, std::string(expected), "mpgsrc line must match the brief format exactly");
            std::remove(tmp.c_str());
        });

        tc.Run("tagwrite carries pc ra fn and computing regs", [](TestCase &t)
        {
            const std::string tmp = srcTmpPath("ps2x-mpg-src-tagwrite.txt");
            std::remove(tmp.c_str());
            t.IsTrue(ps2_mpg_src_trace::configureForTest(tmp.c_str()), "test config should install");

            constexpr uint32_t kTag = 0x00200000u;
            ps2_mpg_src_trace::noteMpgsrc(1500u, kTag, 4u, 2u, 0x004349b8u, 0u, 0u);

            R5900Context ctx{};
            ctx.pc = 0x002A1B40u;
            srcSetReg(ctx, 31, 0x002A1C00u); // ra
            srcSetReg(ctx, 4, 0x00434000u);  // a0: microcode base
            srcSetReg(ctx, 5, 0x00000009u);  // a1: index
            srcSetReg(ctx, 6, 0x00000800u);  // a2: stride
            srcSetReg(ctx, 7, 0x00000001u);  // a3
            srcSetReg(ctx, 2, 0x00434900u);  // v0
            srcSetReg(ctx, 3, 0x000000B8u);  // v1
            for (int r = 8; r <= 15; ++r)
                srcSetReg(ctx, r, 0x100u + static_cast<uint32_t>(r));
            srcSetReg(ctx, 24, 0x118u);
            srcSetReg(ctx, 25, 0x119u);

            // Non-overlapping store first: must stay silent.
            ps2_mpg_src_trace::noteStoreCtx(nullptr, &ctx, kTag + 0x100u, 4u,
                                             0xDEADBEEFu, 0u, "sub_002A1B40_0x2a1b40");
            // Overlapping store on the watched word.
            ps2_mpg_src_trace::noteStoreCtx(nullptr, &ctx, kTag + 4u, 4u,
                                             0x004349B8u, 0u, "sub_002A1B40_0x2a1b40");

            ps2_mpg_src_trace::clearForTest();
            const std::string text = readWholeFile(tmp);
            t.Equals(countLines(text), static_cast<size_t>(2u), "one mpgsrc plus one tagwrite line expected");
            t.IsTrue(text.find("tagwrite vsync=0 addr=0x00200004 value=0x004349b8 "
                               "pc=0x002a1b40 ra=0x002a1c00 fn=sub_002A1B40_0x2a1b40") != std::string::npos,
                     "tagwrite must carry addr value pc ra fn");
            t.IsTrue(text.find("a0=0x00434000 a1=0x00000009 a2=0x00000800 a3=0x00000001") != std::string::npos,
                     "tagwrite must carry a0-a3 at the store");
            t.IsTrue(text.find("v0=0x00434900 v1=0x000000b8") != std::string::npos,
                     "tagwrite must carry v0-v1 at the store");
            t.IsTrue(text.find("t8=0x00000118 t9=0x00000119") != std::string::npos,
                     "tagwrite must carry t0-t9 at the store");
            std::remove(tmp.c_str());
        });

        tc.Run("wide overlapping store extracts the watched word", [](TestCase &t)
        {
            const std::string tmp = srcTmpPath("ps2x-mpg-src-wide.txt");
            std::remove(tmp.c_str());
            t.IsTrue(ps2_mpg_src_trace::configureForTest(tmp.c_str()), "test config should install");

            constexpr uint32_t kTag = 0x00300000u;
            ps2_mpg_src_trace::noteMpgsrc(1500u, kTag, 3u, 2u, 0x00435bf8u, 0u, 0u);

            R5900Context ctx{};
            ctx.pc = 0x00300010u;
            // 8-byte store starting 4 bytes before the watched word: the
            // watched word is the upper half of valueLo.
            ps2_mpg_src_trace::noteStoreCtx(nullptr, &ctx, kTag, 8u,
                                             0xAABBCCDD11223344ull, 0u, "sub_wide");
            // Adjacent store just past the watched word: must stay silent.
            ps2_mpg_src_trace::noteStoreCtx(nullptr, &ctx, kTag + 8u, 4u,
                                             0x12345678u, 0u, "sub_wide");

            ps2_mpg_src_trace::clearForTest();
            const std::string text = readWholeFile(tmp);
            t.Equals(countLines(text), static_cast<size_t>(2u), "one mpgsrc plus one tagwrite line expected");
            t.IsTrue(text.find("addr=0x00300004 value=0xaabbccdd") != std::string::npos,
                     "wide store must log the overlapped watched word");
            std::remove(tmp.c_str());
        });

        tc.Run("from/to window filters both line kinds", [](TestCase &t)
        {
            const std::string tmp = srcTmpPath("ps2x-mpg-src-window.txt");
            std::remove(tmp.c_str());
            t.IsTrue(ps2_mpg_src_trace::configureForTest(tmp.c_str(), 1000u, 1400u),
                     "test config should install");

            // Out-of-window mpgsrc: no line, no watch armed.
            ps2_mpg_src_trace::noteMpgsrc(500u, 0x00400000u, 3u, 1u, 0x00435bf8u, 0u, 0u);
            t.IsTrue(!ps2_mpg_src_trace::writeArmed(), "out-of-window mpgsrc must not arm");
            // In-window mpgsrc: arms.
            ps2_mpg_src_trace::noteMpgsrc(1200u, 0x00401000u, 3u, 1u, 0x00435bf8u, 0u, 0u);
            t.IsTrue(ps2_mpg_src_trace::writeArmed(), "in-window mpgsrc must arm");

            R5900Context ctx{};
            ps2_mpg_src_trace::noteStore(500u, 0x00401004u, 4u, 7u, 0u, 1u, 2u, "sub_x", &ctx);
            ps2_mpg_src_trace::noteStore(1200u, 0x00401004u, 4u, 7u, 0u, 1u, 2u, "sub_x", &ctx);

            ps2_mpg_src_trace::clearForTest();
            const std::string text = readWholeFile(tmp);
            t.IsTrue(text.find("vsync=500") == std::string::npos, "vsync 500 must fall outside 1000..1400");
            t.IsTrue(text.find("mpgsrc vsync=1200") != std::string::npos, "in-window mpgsrc must log");
            t.IsTrue(text.find("tagwrite vsync=1200") != std::string::npos, "in-window tagwrite must log");
            std::remove(tmp.c_str());
        });

        tc.Run("line cap stops the file at 2000", [](TestCase &t)
        {
            const std::string tmp = srcTmpPath("ps2x-mpg-src-cap.txt");
            std::remove(tmp.c_str());
            t.IsTrue(ps2_mpg_src_trace::configureForTest(tmp.c_str()), "test config should install");
            for (uint32_t i = 0u; i < 2005u; ++i)
            {
                ps2_mpg_src_trace::noteMpgsrc(static_cast<uint64_t>(i),
                                              0x00500000u + i * 16u, 3u, 1u, 0x00435bf8u, 0u, 0u);
            }
            ps2_mpg_src_trace::clearForTest();
            const std::string text = readWholeFile(tmp);
            t.Equals(countLines(text), static_cast<size_t>(2000u), "file must stop at the 2000-line cap");
            std::remove(tmp.c_str());
        });

        tc.Run("walker logs in-range REF tag with tte bytes", [](TestCase &t)
        {
            const std::string tmp = srcTmpPath("ps2x-mpg-src-walker.txt");
            std::remove(tmp.c_str());
            t.IsTrue(ps2_mpg_src_trace::configureForTest(tmp.c_str()), "test config should install");

            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            mem.gs_regs.vsyncTick.store(1200u, std::memory_order_relaxed);

            constexpr uint32_t kVif1Ch = 0x10009000u;
            constexpr uint32_t kTag = 0x00027600u;
            constexpr uint32_t kMicro = 0x00435bf8u;

            uint8_t *rdram = mem.getRDRAM();
            srcWriteDmaTag(rdram, kTag, srcMakeDmaTag(1u, 3u, kMicro));
            const uint32_t nopCmd = srcMakeVifCmd(0x00u, 0u, 0u);
            const uint32_t mpgCmd = srcMakeVifCmd(0x4Au, 2u, 0u);
            std::memcpy(rdram + kTag + 8u, &nopCmd, sizeof(nopCmd));
            std::memcpy(rdram + kTag + 12u, &mpgCmd, sizeof(mpgCmd));
            for (uint32_t i = 0u; i < 16u; ++i)
            {
                rdram[kMicro + i] = static_cast<uint8_t>(0xA0u + i);
            }

            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x30u, kTag), "write VIF1 TADR should succeed");
            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x00u, 0x144u), "write VIF1 CHCR STR|CHAIN|TTE should succeed");

            t.IsTrue(ps2_mpg_src_trace::writeArmed(), "in-range REF must arm its tag word watch");

            ps2_mpg_src_trace::clearForTest();
            const std::string text = readWholeFile(tmp);
            char expected[256];
            std::snprintf(expected, sizeof(expected),
                          "mpgsrc vsync=1200 tag_at=0x00027600 id=3 qwc=1 addr=0x00435bf8 tte_vif=000000004a020000\n");
            t.Equals(text, std::string(expected), "walker must log the in-range REF tag exactly once");
            std::remove(tmp.c_str());
        });

        tc.Run("walker scan path keys on payload MPG", [](TestCase &t)
        {
            constexpr uint32_t kVif1Ch = 0x10009000u;
            constexpr uint32_t kTag = 0x00027C00u;
            constexpr uint32_t kPayload = 0x00027D00u;

            // Out-of-range addr with an MPG-led payload: must log.
            {
                const std::string tmp = srcTmpPath("ps2x-mpg-src-scan-hit.txt");
                std::remove(tmp.c_str());
                t.IsTrue(ps2_mpg_src_trace::configureForTest(tmp.c_str()), "test config should install");

                PS2Memory mem;
                t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

                uint8_t *rdram = mem.getRDRAM();
                srcWriteDmaTag(rdram, kTag, srcMakeDmaTag(1u, 3u, kPayload));
                const uint32_t mpgCmd = srcMakeVifCmd(0x4Au, 2u, 0u);
                std::memcpy(rdram + kPayload, &mpgCmd, sizeof(mpgCmd));
                std::memset(rdram + kPayload + 4u, 0x5A, 12u);

                t.IsTrue(mem.writeIORegister(kVif1Ch + 0x30u, kTag), "write VIF1 TADR should succeed");
                t.IsTrue(mem.writeIORegister(kVif1Ch + 0x00u, 0x144u), "write VIF1 CHCR should succeed");

                ps2_mpg_src_trace::clearForTest();
                const std::string text = readWholeFile(tmp);
                t.IsTrue(text.find("tag_at=0x00027c00 id=3 qwc=1 addr=0x00027d00") != std::string::npos,
                         "MPG-led out-of-range payload must log");
                std::remove(tmp.c_str());
            }

            // Out-of-range addr with NOP-only payload: must stay silent.
            {
                const std::string tmp = srcTmpPath("ps2x-mpg-src-scan-miss.txt");
                std::remove(tmp.c_str());
                t.IsTrue(ps2_mpg_src_trace::configureForTest(tmp.c_str()), "test config should install");

                PS2Memory mem;
                t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

                uint8_t *rdram = mem.getRDRAM();
                srcWriteDmaTag(rdram, kTag, srcMakeDmaTag(1u, 3u, kPayload));
                const uint32_t nopCmd = srcMakeVifCmd(0x00u, 0u, 0u);
                for (uint32_t i = 0u; i < 4u; ++i)
                {
                    std::memcpy(rdram + kPayload + i * 4u, &nopCmd, sizeof(nopCmd));
                }

                t.IsTrue(mem.writeIORegister(kVif1Ch + 0x30u, kTag), "write VIF1 TADR should succeed");
                t.IsTrue(mem.writeIORegister(kVif1Ch + 0x00u, 0x144u), "write VIF1 CHCR should succeed");

                t.IsTrue(!ps2_mpg_src_trace::writeArmed(), "NOP-only payload must not arm");
                ps2_mpg_src_trace::clearForTest();
                const std::string text = readWholeFile(tmp);
                t.Equals(text, std::string(), "NOP-only out-of-range payload must stay silent");
                std::remove(tmp.c_str());
            }
        });
    });
}
