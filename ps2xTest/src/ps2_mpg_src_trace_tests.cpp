#include "MiniTest.h"
#include "ps2_mpg_src_trace.h"
#include "ps2_runtime_macros.h"
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

    // READ-macro probe: exercises the Part-2 READ32 tap with the same
    // identifier shape as generated code (rdram/ctx/runtime in scope).
    uint32_t srcReadProbe32(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t addr)
    {
        (void)runtime;
        return READ32(addr);
    }

    // WRITE-macro probe for the Part-3 dmareg tap (real runtime: DMA
    // registers are special addresses, so Store32 must succeed).
    uint32_t srcDmaProbe32(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t addr, uint32_t value)
    {
        WRITE32(addr, value);
        return runtime->memory().readIORegister(addr);
    }

    // WRITE-macro probe for the Part-4 arena tap (plain RAM: the FAST
    // path serves it, so a null runtime is fine).
    uint32_t srcArenaProbe32(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t addr, uint32_t value)
    {
        WRITE32(addr, value);
        return READ32(addr);
    }

    void srcWriteMpgPayload(uint8_t *rdram, uint32_t payload, uint16_t imm)
    {
        const uint32_t mpgCmd = srcMakeVifCmd(0x4Au, 2u, imm);
        std::memcpy(rdram + payload, &mpgCmd, sizeof(mpgCmd));
        for (uint32_t i = 0u; i < 16u; ++i)
        {
            rdram[payload + 4u + i] = static_cast<uint8_t>(0xA0u + i);
        }
        std::memset(rdram + payload + 20u, 0x5A, 12u);
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
            // TTE MPG uses a nonzero slot so this stays a pure mpgsrc
            // test (a dest-0 TTE MPG would also emit an mpgpay line).
            const uint32_t nopCmd = srcMakeVifCmd(0x00u, 0u, 0u);
            const uint32_t mpgCmd = srcMakeVifCmd(0x4Au, 2u, 4u);
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
            // The same walk also emits the ctag dump (Part-4 shares the
            // master gate): per tag the ctag line precedes the mpgsrc line.
            char expected[512];
            std::snprintf(expected, sizeof(expected),
                          "ctag tag_at=0x00027600 id=3 qwc=1 addr=0x00435bf8 tte=000000004a020004\n"
                          "mpgsrc vsync=1200 tag_at=0x00027600 id=3 qwc=1 addr=0x00435bf8 tte_vif=000000004a020004\n"
                          "ctag tag_at=0x00027610 id=0 qwc=0 addr=0x00000000 tte=0000000000000000\n");
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
                // The ctag dump (Part-4) still logs the walked tags; only
                // mpgsrc/mpgpay must stay silent here.
                t.IsTrue(text.find("mpgsrc") == std::string::npos, "NOP-only payload must not log mpgsrc");
                t.IsTrue(text.find("mpgpay") == std::string::npos, "NOP-only payload must not log mpgpay");
                char expected[512];
                std::snprintf(expected, sizeof(expected),
                              "ctag tag_at=0x00027c00 id=3 qwc=1 addr=0x00027d00 tte=0000000000000000\n"
                              "ctag tag_at=0x00027c10 id=0 qwc=0 addr=0x00000000 tte=0000000000000000\n");
                t.Equals(text, std::string(expected), "only the ctag dump must appear");
                std::remove(tmp.c_str());
            }
        });

        tc.Run("read watch is off by default", [](TestCase &t)
        {
            ps2_mpg_src_trace::clearForTest();
            t.IsTrue(!ps2_mpg_src_trace::readArmed(), "read tap must be disarmed with no file configured");
            // The address pre-filter is stateless: regions match even when off.
            t.IsTrue(ps2_mpg_src_trace::isReadWatched(0x00435BF8u, 4u), "region head must pre-match");
            t.IsTrue(ps2_mpg_src_trace::isReadWatched(0x004349B8u, 16u), "second region must pre-match");
            t.IsTrue(!ps2_mpg_src_trace::isReadWatched(0x00100000u, 4u), "plain RAM must not pre-match");
            R5900Context ctx{};
            ps2_mpg_src_trace::noteRead(0u, 0x00435BF8u, 4u, 1u, 2u, "sub_dead", &ctx);
            t.IsTrue(!ps2_mpg_src_trace::readArmed(), "disabled read taps must stay off");
        });

        tc.Run("srcread line carries pc ra fn and s regs", [](TestCase &t)
        {
            const std::string tmp = srcTmpPath("ps2x-mpg-src-srcread.txt");
            std::remove(tmp.c_str());
            t.IsTrue(ps2_mpg_src_trace::configureForTest(tmp.c_str()), "test config should install");
            t.IsTrue(ps2_mpg_src_trace::readArmed(), "regions must be open after configure");

            R5900Context ctx{};
            ctx.pc = 0x003A55AAu;
            srcSetReg(ctx, 31, 0x003A5600u); // ra
            srcSetReg(ctx, 4, 0x00435BF0u);  // a0: source base
            srcSetReg(ctx, 5, 0x00000002u);  // a1: index
            srcSetReg(ctx, 6, 0x00000004u);  // a2
            srcSetReg(ctx, 7, 0x00000000u);  // a3
            srcSetReg(ctx, 2, 0xDEAD0001u);  // v0
            srcSetReg(ctx, 3, 0xDEAD0002u);  // v1
            for (int r = 8; r <= 15; ++r)
                srcSetReg(ctx, r, 0x200u + static_cast<uint32_t>(r));
            srcSetReg(ctx, 24, 0x218u);
            srcSetReg(ctx, 25, 0x219u);
            for (int r = 16; r <= 23; ++r)
                srcSetReg(ctx, r, 0x300u + static_cast<uint32_t>(r));

            // Unwatched address first: must stay silent.
            ps2_mpg_src_trace::noteRead(1100u, 0x00100000u, 4u, 1u, 2u, "sub_x", &ctx);
            ps2_mpg_src_trace::noteRead(1100u, 0x00435BF8u, 4u, 0x003A55AAu, 0x003A5600u,
                                        "sub_003A4000_0x3a4000", &ctx);

            ps2_mpg_src_trace::clearForTest();
            const std::string text = readWholeFile(tmp);
            t.Equals(countLines(text), static_cast<size_t>(1u), "one srcread line expected");
            t.IsTrue(text.find("srcread vsync=1100 addr=0x00435bf8 size=4 "
                               "pc=0x003a55aa ra=0x003a5600 fn=sub_003A4000_0x3a4000") != std::string::npos,
                     "srcread must carry addr size pc ra fn");
            t.IsTrue(text.find("a0=0x00435bf0 a1=0x00000002") != std::string::npos,
                     "srcread must carry the pointer-forming regs");
            t.IsTrue(text.find("s0=0x00000310 s1=0x00000311 s2=0x00000312 s3=0x00000313 "
                               "s4=0x00000314 s5=0x00000315 s6=0x00000316 s7=0x00000317") != std::string::npos,
                     "srcread must carry s0-s7 at the load");
            std::remove(tmp.c_str());
        });

        tc.Run("first 64 hits per address, windows do not consume", [](TestCase &t)
        {
            const std::string tmp = srcTmpPath("ps2x-mpg-src-readcap.txt");
            std::remove(tmp.c_str());
            t.IsTrue(ps2_mpg_src_trace::configureForTest(tmp.c_str(), 1000u, 1400u),
                     "test config should install");

            R5900Context ctx{};
            // Out-of-window reads must neither log nor consume hits.
            for (uint32_t i = 0u; i < 70u; ++i)
            {
                ps2_mpg_src_trace::noteRead(500u, 0x00435BF8u, 4u, 1u, 2u, "sub_x", &ctx);
            }
            // 70 in-window reads on region 0: 64 log, then silent.
            for (uint32_t i = 0u; i < 70u; ++i)
            {
                ps2_mpg_src_trace::noteRead(1200u, 0x00435BF8u, 4u, 1u, 2u, "sub_x", &ctx);
            }
            t.IsTrue(ps2_mpg_src_trace::readArmed(), "region 1 still open, tap must stay armed");
            // Region 1 is independent: 3 more reads log.
            for (uint32_t i = 0u; i < 3u; ++i)
            {
                ps2_mpg_src_trace::noteRead(1200u, 0x004349B8u, 4u, 1u, 2u, "sub_x", &ctx);
            }

            ps2_mpg_src_trace::clearForTest();
            const std::string text = readWholeFile(tmp);
            t.Equals(countLines(text), static_cast<size_t>(67u), "64 + 3 srcread lines expected");
            t.IsTrue(text.find("vsync=500") == std::string::npos, "out-of-window reads must stay silent");
            std::remove(tmp.c_str());
        });

        tc.Run("READ32 macro tap logs with the host function name", [](TestCase &t)
        {
            const std::string tmp = srcTmpPath("ps2x-mpg-src-readmacro.txt");
            std::remove(tmp.c_str());
            t.IsTrue(ps2_mpg_src_trace::configureForTest(tmp.c_str()), "test config should install");

            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            uint8_t *rdram = mem.getRDRAM();
            const uint32_t marker = 0xA55A00FFu;
            std::memcpy(rdram + 0x00435BFCu, &marker, sizeof(marker));

            R5900Context ctxStruct{};
            R5900Context *ctx = &ctxStruct;
            ctxStruct.pc = 0x00123456u;
            PS2Runtime *runtime = nullptr;
            const uint32_t got = srcReadProbe32(rdram, ctx, runtime, 0x00435BFCu);
            t.Equals(got, marker, "macro probe must return the RAM word");

            ps2_mpg_src_trace::clearForTest();
            const std::string text = readWholeFile(tmp);
            t.Equals(countLines(text), static_cast<size_t>(1u), "one srcread line expected");
            t.IsTrue(text.find("srcread vsync=0 addr=0x00435bfc size=4 "
                               "pc=0x00123456 ra=0x00000000 fn=srcReadProbe32") != std::string::npos,
                     "macro tap must log addr size pc ra and the host function name");
            std::remove(tmp.c_str());
        });

        tc.Run("chain mpgpay attributes dest-0 payload to EE source", [](TestCase &t)
        {
            const std::string tmp = srcTmpPath("ps2x-mpg-src-mpgpay-chain.txt");
            std::remove(tmp.c_str());
            t.IsTrue(ps2_mpg_src_trace::configureForTest(tmp.c_str()), "test config should install");

            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            mem.gs_regs.vsyncTick.store(1310u, std::memory_order_relaxed);

            constexpr uint32_t kVif1Ch = 0x10009000u;
            constexpr uint32_t kTag = 0x00027C00u;
            constexpr uint32_t kPayload = kTag + 16u;

            uint8_t *rdram = mem.getRDRAM();
            // CNT qwc=2: TTE NOPs (upper half zero) + 32 inline payload
            // bytes (CNT data always follows the tag).
            srcWriteDmaTag(rdram, kTag, srcMakeDmaTag(2u, 1u, 0u));
            srcWriteMpgPayload(rdram, kPayload, 0u);

            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x30u, kTag), "write VIF1 TADR should succeed");
            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x00u, 0x144u), "write VIF1 CHCR STR|CHAIN|TTE should succeed");
            mem.processPendingTransfers();

            ps2_mpg_src_trace::clearForTest();
            const std::string text = readWholeFile(tmp);
            // The same walk also emits the ctag dump (Part-4 shares the
            // master gate): CNT tag, END tag, then the delivery's mpgpay.
            char expected[512];
            std::snprintf(expected, sizeof(expected),
                          "ctag tag_at=0x00027c00 id=1 qwc=2 addr=0x00000000 tte=0000000000000000\n"
                          "ctag tag_at=0x00027c30 id=0 qwc=0 addr=0x00000000 tte=0000000000000000\n"
                          "mpgpay vsync=1310 imm=0 num=2 src=0x00027c14 srcmask=0x00027c14 mode=chain:1 tag_at=0x00027c00\n");
            t.Equals(text, std::string(expected), "chain dest-0 upload must log its EE source");
            std::remove(tmp.c_str());
        });

        tc.Run("normal-mode mpgpay uses the MADR source", [](TestCase &t)
        {
            const std::string tmp = srcTmpPath("ps2x-mpg-src-mpgpay-normal.txt");
            std::remove(tmp.c_str());
            t.IsTrue(ps2_mpg_src_trace::configureForTest(tmp.c_str()), "test config should install");

            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            mem.gs_regs.vsyncTick.store(1311u, std::memory_order_relaxed);

            constexpr uint32_t kVif1Ch = 0x10009000u;
            constexpr uint32_t kPayload = 0x00027E00u;

            uint8_t *rdram = mem.getRDRAM();
            srcWriteMpgPayload(rdram, kPayload, 0u);

            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x10u, kPayload), "write VIF1 MADR should succeed");
            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x20u, 2u), "write VIF1 QWC should succeed");
            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x00u, 0x100u), "write VIF1 CHCR STR mode0 should succeed");
            mem.processPendingTransfers();

            ps2_mpg_src_trace::clearForTest();
            const std::string text = readWholeFile(tmp);
            char expected[256];
            std::snprintf(expected, sizeof(expected),
                          "mpgpay vsync=1311 imm=0 num=2 src=0x00027e04 srcmask=0x00027e04 mode=normal tag_at=-\n");
            t.Equals(text, std::string(expected), "normal-mode dest-0 upload must log MADR source");
            std::remove(tmp.c_str());
        });

        tc.Run("mpgpay skips nonzero imm and out-of-window vsync", [](TestCase &t)
        {
            const std::string tmp = srcTmpPath("ps2x-mpg-src-mpgpay-window.txt");
            std::remove(tmp.c_str());
            t.IsTrue(ps2_mpg_src_trace::configureForTest(tmp.c_str(), 1300u, 1320u),
                     "test config should install");

            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kVif1Ch = 0x10009000u;
            constexpr uint32_t kPayload = 0x00027F00u;

            uint8_t *rdram = mem.getRDRAM();
            // The drain consumes QWC, so re-arm MADR+QWC before every kick
            // (mirrors real DMA programming).
            srcWriteMpgPayload(rdram, kPayload, 5u);
            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x10u, kPayload), "write VIF1 MADR should succeed");
            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x20u, 2u), "write VIF1 QWC should succeed");

            mem.gs_regs.vsyncTick.store(1310u, std::memory_order_relaxed);
            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x00u, 0x100u), "write VIF1 CHCR should succeed");
            mem.processPendingTransfers();

            srcWriteMpgPayload(rdram, kPayload, 0u);
            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x10u, kPayload), "write VIF1 MADR should succeed");
            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x20u, 2u), "write VIF1 QWC should succeed");
            mem.gs_regs.vsyncTick.store(1400u, std::memory_order_relaxed);
            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x00u, 0x100u), "write VIF1 CHCR should succeed");
            mem.processPendingTransfers();

            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x10u, kPayload), "write VIF1 MADR should succeed");
            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x20u, 2u), "write VIF1 QWC should succeed");
            mem.gs_regs.vsyncTick.store(1315u, std::memory_order_relaxed);
            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x00u, 0x100u), "write VIF1 CHCR should succeed");
            mem.processPendingTransfers();

            ps2_mpg_src_trace::clearForTest();
            const std::string text = readWholeFile(tmp);
            t.Equals(countLines(text), static_cast<size_t>(1u), "only the in-window dest-0 upload must log");
            t.IsTrue(text.find("mpgpay vsync=1315 imm=0 num=2 src=0x00027f04") != std::string::npos,
                     "in-window dest-0 upload must log its source");
            std::remove(tmp.c_str());
        });

        tc.Run("pay map lookup unit", [](TestCase &t)
        {
            const std::string tmp = srcTmpPath("ps2x-mpg-src-paymap.txt");
            std::remove(tmp.c_str());
            t.IsTrue(ps2_mpg_src_trace::configureForTest(tmp.c_str()), "test config should install");
            t.IsTrue(!ps2_mpg_src_trace::payArmed(), "no map installed yet");

            alignas(16) uint8_t buf[64];
            std::memset(buf, 0, sizeof(buf));
            Ps2VifSrcSpan spans[2];
            spans[0].bufOff = 0u;
            spans[0].len = 8u;
            spans[0].eeAddr = 0x00200008u;
            spans[0].tagId = 1;
            spans[0].tagAt = 0x00200000u;
            spans[1].bufOff = 8u;
            spans[1].len = 56u;
            spans[1].eeAddr = 0x00300000u;
            spans[1].tagId = 1;
            spans[1].tagAt = 0x00200000u;
            ps2_mpg_src_trace::setPayMap(buf, sizeof(buf), spans, 2u,
                                          ps2_mpg_src_trace::PayChain, 0u);
            t.IsTrue(ps2_mpg_src_trace::payArmed(), "installed map must arm the hook");

            uint32_t ee = 0u;
            int32_t tagId = -2;
            uint32_t tagAt = 0u;
            uint32_t mode = 0u;
            t.IsTrue(ps2_mpg_src_trace::lookupPay(buf + 4u, ee, tagId, tagAt, mode), "TTE byte must map");
            t.Equals(ee, 0x0020000Cu, "TTE byte maps to tagAt+8+off");
            t.Equals(tagId, 1, "span tag id must come through");
            t.Equals(tagAt, 0x00200000u, "span tagAt must come through");
            t.Equals(mode, ps2_mpg_src_trace::PayChain, "chain mode must come through");
            t.IsTrue(ps2_mpg_src_trace::lookupPay(buf + 8u, ee, tagId, tagAt, mode), "payload head must map");
            t.Equals(ee, 0x00300000u, "payload head maps to the data EE addr");
            t.Equals(tagAt, 0x00200000u, "span tag addr must come through");
            t.IsTrue(!ps2_mpg_src_trace::lookupPay(buf + 64u, ee, tagId, tagAt, mode), "past-end must miss");

            ps2_mpg_src_trace::clearPayMap();
            t.IsTrue(!ps2_mpg_src_trace::payArmed(), "cleared map must disarm");
            t.IsTrue(!ps2_mpg_src_trace::lookupPay(buf, ee, tagId, tagAt, mode), "cleared map must miss");
            ps2_mpg_src_trace::clearForTest();
            std::remove(tmp.c_str());
        });

        tc.Run("isDmareg names the four VIF1 regs", [](TestCase &t)
        {
            t.IsTrue(ps2_mpg_src_trace::isDmareg(0x10009000u), "CHCR must match");
            t.IsTrue(ps2_mpg_src_trace::isDmareg(0x10009010u), "MADR must match");
            t.IsTrue(ps2_mpg_src_trace::isDmareg(0x10009020u), "QWC must match");
            t.IsTrue(ps2_mpg_src_trace::isDmareg(0x10009030u), "TADR must match");
            t.IsTrue(!ps2_mpg_src_trace::isDmareg(0x10009004u), "CHCR+4 must not match");
            t.IsTrue(!ps2_mpg_src_trace::isDmareg(0x10008000u), "VIF0 CHCR must not match");
            t.IsTrue(!ps2_mpg_src_trace::isDmareg(0x10009040u), "ASR0 must not match");
            t.Equals(std::string(ps2_mpg_src_trace::dmaregName(0x10009000u)), std::string("CHCR"), "CHCR name");
            t.Equals(std::string(ps2_mpg_src_trace::dmaregName(0x10009010u)), std::string("MADR"), "MADR name");
            t.Equals(std::string(ps2_mpg_src_trace::dmaregName(0x10009020u)), std::string("QWC"), "QWC name");
            t.Equals(std::string(ps2_mpg_src_trace::dmaregName(0x10009030u)), std::string("TADR"), "TADR name");
        });

        tc.Run("dmareg line format window and 256 cap", [](TestCase &t)
        {
            const std::string tmp = srcTmpPath("ps2x-mpg-src-dmareg.txt");
            std::remove(tmp.c_str());
            t.IsTrue(ps2_mpg_src_trace::configureForTest(tmp.c_str(), 1300u, 1320u),
                     "test config should install");
            t.IsTrue(ps2_mpg_src_trace::dmaregArmed(), "watch must be armed after configure");

            R5900Context ctx{};
            ctx.pc = 0x00401100u;
            srcSetReg(ctx, 31, 0x00401200u);
            srcSetReg(ctx, 4, 0x00435000u);
            srcSetReg(ctx, 16, 0x00C0FFEEu);

            ps2_mpg_src_trace::noteDmareg(1200u, 0x10009010u, 0x00435BF8u,
                                          0x00401100u, 0x00401200u, "sub_dma", &ctx);
            ps2_mpg_src_trace::noteDmareg(1310u, 0x10009010u, 0x00435BF8u,
                                          0x00401100u, 0x00401200u, "sub_dma", &ctx);
            for (uint32_t i = 0u; i < 260u; ++i)
            {
                ps2_mpg_src_trace::noteDmareg(1310u, 0x10009030u, i,
                                              0x00401100u, 0x00401200u, "sub_dma", &ctx);
            }
            t.IsTrue(!ps2_mpg_src_trace::dmaregArmed(), "exhausted watch must disarm");

            ps2_mpg_src_trace::clearForTest();
            const std::string text = readWholeFile(tmp);
            t.Equals(countLines(text), static_cast<size_t>(256u), "watch must stop at 256 lines");
            t.IsTrue(text.find("vsync=1200") == std::string::npos, "out-of-window store must stay silent");
            t.IsTrue(text.find("dmareg vsync=1310 reg=MADR value=0x00435bf8 "
                               "pc=0x00401100 ra=0x00401200 fn=sub_dma") != std::string::npos,
                     "dmareg must carry reg value pc ra fn");
            t.IsTrue(text.find("a0=0x00435000") != std::string::npos, "dmareg must carry a0");
            t.IsTrue(text.find("s0=0x00c0ffee") != std::string::npos, "dmareg must carry s0");
            std::remove(tmp.c_str());
        });

        tc.Run("WRITE32 macro tap reports MADR with host fn", [](TestCase &t)
        {
            const std::string tmp = srcTmpPath("ps2x-mpg-src-dmamacro.txt");
            std::remove(tmp.c_str());
            t.IsTrue(ps2_mpg_src_trace::configureForTest(tmp.c_str()), "test config should install");

            PS2Runtime runtime;
            t.IsTrue(runtime.memory().initialize(), "PS2Memory initialize should succeed");
            uint8_t *rdram = runtime.memory().getRDRAM();

            R5900Context ctxStruct{};
            R5900Context *ctx = &ctxStruct;
            ctxStruct.pc = 0x00552288u;
            srcSetReg(ctxStruct, 31, 0x00552300u);
            srcSetReg(ctxStruct, 4, 0x00435BF8u);

            PS2Runtime *rt = &runtime;
            t.Equals(srcDmaProbe32(rdram, ctx, rt, 0x10009010u, 0x00435BF8u), 0x00435BF8u,
                     "MADR write must land in the register");
            t.Equals(srcDmaProbe32(rdram, ctx, rt, 0x10009030u, 0x00051000u), 0x00051000u,
                     "TADR write must land in the register");

            ps2_mpg_src_trace::clearForTest();
            const std::string text = readWholeFile(tmp);
            t.Equals(countLines(text), static_cast<size_t>(2u), "two dmareg lines expected");
            t.IsTrue(text.find("dmareg vsync=0 reg=MADR value=0x00435bf8 "
                               "pc=0x00552288 ra=0x00552300 fn=srcDmaProbe32") != std::string::npos,
                     "macro tap must log MADR with the host function name");
            t.IsTrue(text.find("reg=TADR value=0x00051000") != std::string::npos,
                     "macro tap must log TADR");
            std::remove(tmp.c_str());
        });

        tc.Run("arena match helpers", [](TestCase &t)
        {
            t.IsTrue(ps2_mpg_src_trace::isArenaWatched(0x0063B800u, 4u), "arena 1 head must match");
            t.IsTrue(ps2_mpg_src_trace::isArenaWatched(0x0063C3FCu, 4u), "arena 1 tail must match");
            t.IsTrue(ps2_mpg_src_trace::isArenaWatched(0x00708400u, 16u), "arena 2 head must match");
            t.IsTrue(ps2_mpg_src_trace::isArenaWatched(0x00708CFCu, 4u), "arena 2 tail must match");
            t.IsTrue(!ps2_mpg_src_trace::isArenaWatched(0x0063C400u, 4u), "arena 1 end is exclusive");
            t.IsTrue(!ps2_mpg_src_trace::isArenaWatched(0x00100000u, 4u), "plain RAM must not match");
            t.IsTrue(!ps2_mpg_src_trace::isArenaWatched(0x10009010u, 4u), "DMA regs must not match");
            t.IsTrue(ps2_mpg_src_trace::isArenaValue(0x00435BF8u), "cached library addr must match");
            t.IsTrue(ps2_mpg_src_trace::isArenaValue(0x20435BF8u), "0x20 mirror must match");
            t.IsTrue(ps2_mpg_src_trace::isArenaValue(0x30435BF8u), "0x30 mirror must match");
            t.IsTrue(ps2_mpg_src_trace::isArenaValue(0x80435BF8u), "0x80 mirror must match");
            t.IsTrue(!ps2_mpg_src_trace::isArenaValue(0x0042FFFFu), "below range must not match");
            t.IsTrue(!ps2_mpg_src_trace::isArenaValue(0x00440000u), "range end is exclusive");
            t.IsTrue(!ps2_mpg_src_trace::isArenaValue(0x0063B8A0u), "arena addr itself must not match");
        });

        tc.Run("arenastore line format lanes window and cap", [](TestCase &t)
        {
            const std::string tmp = srcTmpPath("ps2x-mpg-src-arena.txt");
            std::remove(tmp.c_str());
            t.IsTrue(ps2_mpg_src_trace::configureForTest(tmp.c_str(), 1300u, 1320u),
                     "test config should install");
            t.IsTrue(ps2_mpg_src_trace::arenastoreArmed(), "watch must be armed after configure");

            R5900Context ctx{};
            ctx.pc = 0x00667788u;
            srcSetReg(ctx, 31, 0x00667800u);
            srcSetReg(ctx, 4, 0x0063B900u);
            srcSetReg(ctx, 16, 0x00C0FFEEu);

            // Out-of-window: silent. Plain value: silent. Plain addr: silent.
            ps2_mpg_src_trace::noteArenastore(1200u, 0x0063B900u, 4u, 0x00435BF8u, 0u,
                                              0x00667788u, 0x00667800u, "sub_x", &ctx);
            ps2_mpg_src_trace::noteArenastore(1310u, 0x0063B900u, 4u, 0x00001000u, 0u,
                                              0x00667788u, 0x00667800u, "sub_x", &ctx);
            ps2_mpg_src_trace::noteArenastore(1310u, 0x00100000u, 4u, 0x00435BF8u, 0u,
                                              0x00667788u, 0x00667800u, "sub_x", &ctx);
            // 32-bit match, then a 64-bit store whose UPPER lane matches.
            ps2_mpg_src_trace::noteArenastore(1310u, 0x0063B900u, 4u, 0x00435BF8u, 0u,
                                              0x00667788u, 0x00667800u, "sub_arena", &ctx);
            const uint64_t wide = (static_cast<uint64_t>(0x80435BD0u) << 32u) | 0xDEADu;
            ps2_mpg_src_trace::noteArenastore(1310u, 0x00708500u, 8u, wide, 0u,
                                              0x00667788u, 0x00667800u, "sub_arena", &ctx);
            for (uint32_t i = 0u; i < 260u; ++i)
            {
                ps2_mpg_src_trace::noteArenastore(1310u, 0x0063B910u, 4u, 0x004349B8u, 0u,
                                                  0x00667788u, 0x00667800u, "sub_arena", &ctx);
            }
            t.IsTrue(!ps2_mpg_src_trace::arenastoreArmed(), "exhausted watch must disarm");

            ps2_mpg_src_trace::clearForTest();
            const std::string text = readWholeFile(tmp);
            t.Equals(countLines(text), static_cast<size_t>(256u), "watch must stop at 256 lines");
            t.IsTrue(text.find("vsync=1200") == std::string::npos, "out-of-window store must stay silent");
            t.IsTrue(text.find("arenastore vsync=1310 addr=0x0063b900 value=0x00435bf8 "
                               "pc=0x00667788 ra=0x00667800 fn=sub_arena") != std::string::npos,
                     "arenastore must carry addr value pc ra fn");
            t.IsTrue(text.find("addr=0x00708504 value=0x80435bd0") != std::string::npos,
                     "upper 64-bit lane must log its own addr and mirror value");
            t.IsTrue(text.find("s0=0x00c0ffee") != std::string::npos, "arenastore must carry s regs");
            std::remove(tmp.c_str());
        });

        tc.Run("WRITE32 macro tap logs arena store with host fn", [](TestCase &t)
        {
            const std::string tmp = srcTmpPath("ps2x-mpg-src-arenamacro.txt");
            std::remove(tmp.c_str());
            t.IsTrue(ps2_mpg_src_trace::configureForTest(tmp.c_str()), "test config should install");

            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            uint8_t *rdram = mem.getRDRAM();

            R5900Context ctxStruct{};
            R5900Context *ctx = &ctxStruct;
            ctxStruct.pc = 0x00ABCDEFu;
            srcSetReg(ctxStruct, 31, 0x00ABCE00u);
            PS2Runtime *runtime = nullptr;
            t.Equals(srcArenaProbe32(rdram, ctx, runtime, 0x0063B904u, 0x30435BD0u), 0x30435BD0u,
                     "macro probe must write through to arena RAM");
            t.Equals(srcArenaProbe32(rdram, ctx, runtime, 0x00100000u, 0x00435BF8u), 0x00435BF8u,
                     "non-arena store must still write through");

            ps2_mpg_src_trace::clearForTest();
            const std::string text = readWholeFile(tmp);
            t.Equals(countLines(text), static_cast<size_t>(2u), "arena store plus read-back uploadload must log");
            t.IsTrue(text.find("arenastore vsync=0 addr=0x0063b904 value=0x30435bd0 "
                               "pc=0x00abcdef ra=0x00abce00 fn=srcArenaProbe32") != std::string::npos,
                     "macro tap must log the arena store with host fn");
            t.IsTrue(text.find("uploadload vsync=0 pc=0x00abcdef ra=0x00abce00 fn=srcArenaProbe32 "
                               "addr=0x0063b904 value=0x30435bd0") != std::string::npos,
                     "read-back of the uploader word must log uploadload");
            std::remove(tmp.c_str());
        });

        tc.Run("parseArenas accepts pairs and rejects garbage", [](TestCase &t)
        {
            uint32_t base[8];
            uint32_t end[8];
            t.IsTrue(ps2_mpg_src_trace::parseArenas("0x600000-0x640000,0x700000-0x720000",
                                                    base, end, 8) == 2,
                     "two hex pairs must parse");
            t.IsTrue(base[0] == 0x00600000u && end[0] == 0x00640000u, "first pair must match");
            t.IsTrue(base[1] == 0x00700000u && end[1] == 0x00720000u, "second pair must match");
            t.IsTrue(ps2_mpg_src_trace::parseArenas("600000-640000", base, end, 8) == 1,
                     "0x prefix must be optional");
            t.IsTrue(base[0] == 0x00600000u && end[0] == 0x00640000u, "bare-hex pair must match");
            t.IsTrue(ps2_mpg_src_trace::parseArenas("0x10-0x8", base, end, 8) == -1,
                     "lo >= hi must fail");
            t.IsTrue(ps2_mpg_src_trace::parseArenas("xyz", base, end, 8) == -1,
                     "non-hex must fail");
            t.IsTrue(ps2_mpg_src_trace::parseArenas("", base, end, 8) == -1,
                     "empty text must fail");
            t.IsTrue(ps2_mpg_src_trace::parseArenas("0x1-0x2,", base, end, 8) == -1,
                     "trailing comma must fail");
            t.IsTrue(ps2_mpg_src_trace::parseArenas(nullptr, base, end, 8) == -1,
                     "null text must fail");
            t.IsTrue(ps2_mpg_src_trace::parseArenas("0x1-0x2,0x3-0x4", base, end, 1) == 1,
                     "over-long lists must truncate to maxN");
            t.IsTrue(base[0] == 0x1u && end[0] == 0x2u, "truncation must keep the head");
        });

        tc.Run("applyArenasForTest swaps ranges and restores defaults", [](TestCase &t)
        {
            // Disjoint range proves replacement (not union): defaults drop out.
            ps2_mpg_src_trace::applyArenasForTest("0x500000-0x510000");
            t.IsTrue(ps2_mpg_src_trace::isArenaWatched(0x00505000u, 4u), "custom range must match");
            t.IsTrue(!ps2_mpg_src_trace::isArenaWatched(0x0063B900u, 4u), "default range must be gone");
            // Part-5 wide ranges contain the defaults by design (superset hunt).
            ps2_mpg_src_trace::applyArenasForTest("0x600000-0x640000,0x700000-0x720000");
            t.IsTrue(ps2_mpg_src_trace::isArenaWatched(0x00600100u, 4u), "wide-only addr must match");
            t.IsTrue(ps2_mpg_src_trace::isArenaWatched(0x0063B900u, 4u), "overlap addr must still match");
            t.IsTrue(!ps2_mpg_src_trace::isArenaWatched(0x00650000u, 4u), "gap between ranges must not match");
            ps2_mpg_src_trace::applyArenasForTest("garbage");
            t.IsTrue(ps2_mpg_src_trace::isArenaWatched(0x0063B900u, 4u), "malformed env must restore defaults");
            t.IsTrue(!ps2_mpg_src_trace::isArenaWatched(0x00600100u, 4u), "wide-only addr must be gone");
            ps2_mpg_src_trace::clearForTest();
        });

        tc.Run("isUploaderValue matches exact alt and mirrors", [](TestCase &t)
        {
            t.IsTrue(ps2_mpg_src_trace::isUploaderValue(0x00435BD0u), "static uploader must match");
            t.IsTrue(ps2_mpg_src_trace::isUploaderValue(0x20435BD0u), "0x20 mirror must match");
            t.IsTrue(ps2_mpg_src_trace::isUploaderValue(0x80435BD0u), "0x80 mirror must match");
            t.IsTrue(ps2_mpg_src_trace::isUploaderValue(0x00434990u), "alt range lo must match");
            t.IsTrue(ps2_mpg_src_trace::isUploaderValue(0x004349B8u), "alt range hi must match");
            t.IsTrue(ps2_mpg_src_trace::isUploaderValue(0x004349A0u), "alt range mid must match");
            t.IsTrue(ps2_mpg_src_trace::isUploaderValue(0x304349B0u), "alt mirror must match");
            t.IsTrue(!ps2_mpg_src_trace::isUploaderValue(0x00435BD1u), "off-by-one exact must not match");
            t.IsTrue(!ps2_mpg_src_trace::isUploaderValue(0x0043498Fu), "below alt range must not match");
            t.IsTrue(!ps2_mpg_src_trace::isUploaderValue(0x004349B9u), "above alt range must not match");
            t.IsTrue(!ps2_mpg_src_trace::isUploaderValue(0x0044B140u), "REF code addr must not match");
            t.IsTrue(!ps2_mpg_src_trace::isUploaderValue(0x00435BF8u), "payload head must not match");
            t.IsTrue(!ps2_mpg_src_trace::isUploaderValue(0u), "zero must not match");
        });

        tc.Run("uploadload logs uploader-valued loads with pc ra fn", [](TestCase &t)
        {
            const std::string tmp = srcTmpPath("ps2x-mpg-src-uploadload.txt");
            std::remove(tmp.c_str());
            t.IsTrue(ps2_mpg_src_trace::configureForTest(tmp.c_str()), "test config should install");
            t.IsTrue(ps2_mpg_src_trace::uploadloadArmed(), "watch must be armed after configure");

            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            uint8_t *rdram = mem.getRDRAM();
            const uint32_t exact = 0x00435BD0u;
            const uint32_t alt = 0x004349A0u;
            const uint32_t other = 0x0044B140u;
            std::memcpy(rdram + 0x00300000u, &exact, sizeof(exact));
            std::memcpy(rdram + 0x00300004u, &alt, sizeof(alt));
            std::memcpy(rdram + 0x00300008u, &other, sizeof(other));

            R5900Context ctxStruct{};
            R5900Context *ctx = &ctxStruct;
            ctxStruct.pc = 0x00367DE4u;
            srcSetReg(ctxStruct, 31, 0x00367E00u);
            srcSetReg(ctxStruct, 4, 0x00300000u);
            PS2Runtime *runtime = nullptr;
            t.Equals(srcReadProbe32(rdram, ctx, runtime, 0x00300000u), exact, "exact load must read back");
            t.Equals(srcReadProbe32(rdram, ctx, runtime, 0x00300004u), alt, "alt load must read back");
            t.Equals(srcReadProbe32(rdram, ctx, runtime, 0x00300008u), other, "other load must read back");

            ps2_mpg_src_trace::clearForTest();
            const std::string text = readWholeFile(tmp);
            t.Equals(countLines(text), static_cast<size_t>(2u), "only uploader-valued loads must log");
            t.IsTrue(text.find("uploadload vsync=0 pc=0x00367de4 ra=0x00367e00 fn=srcReadProbe32 "
                               "addr=0x00300000 value=0x00435bd0") != std::string::npos,
                     "exact load must carry pc ra fn addr value");
            t.IsTrue(text.find("addr=0x00300004 value=0x004349a0") != std::string::npos,
                     "alt-range load must log");
            t.IsTrue(text.find("a0=0x00300000") != std::string::npos, "load must carry a regs");
            t.IsTrue(text.find("0x0044b140") == std::string::npos, "non-uploader load must stay silent");
            std::remove(tmp.c_str());
        });

        tc.Run("uploadload stops at 64 lines", [](TestCase &t)
        {
            const std::string tmp = srcTmpPath("ps2x-mpg-src-uploadload-cap.txt");
            std::remove(tmp.c_str());
            t.IsTrue(ps2_mpg_src_trace::configureForTest(tmp.c_str()), "test config should install");

            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            uint8_t *rdram = mem.getRDRAM();
            const uint32_t exact = 0x00435BD0u;
            std::memcpy(rdram + 0x00300000u, &exact, sizeof(exact));

            R5900Context ctxStruct{};
            R5900Context *ctx = &ctxStruct;
            ctxStruct.pc = 0x00367DE4u;
            PS2Runtime *runtime = nullptr;
            for (uint32_t i = 0u; i < 70u; ++i)
            {
                srcReadProbe32(rdram, ctx, runtime, 0x00300000u);
            }
            t.IsTrue(!ps2_mpg_src_trace::uploadloadArmed(), "exhausted watch must disarm");

            ps2_mpg_src_trace::clearForTest();
            t.Equals(countLines(readWholeFile(tmp)), static_cast<size_t>(64u), "watch must stop at 64 lines");
            std::remove(tmp.c_str());
        });

        tc.Run("ctag dumps first two in-window kicks", [](TestCase &t)
        {
            const std::string tmp = srcTmpPath("ps2x-mpg-src-ctag.txt");
            std::remove(tmp.c_str());
            t.IsTrue(ps2_mpg_src_trace::configureForTest(tmp.c_str(), 1300u, 1320u),
                     "test config should install");

            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            constexpr uint32_t kVif1Ch = 0x10009000u;
            constexpr uint32_t kTagA = 0x00027600u;
            constexpr uint32_t kTagB = 0x00027700u;
            constexpr uint32_t kTagC = 0x00027800u;

            uint8_t *rdram = mem.getRDRAM();
            const uint32_t nopCmd = srcMakeVifCmd(0x00u, 0u, 0u);
            for (uint32_t kTag : {kTagA, kTagB, kTagC})
            {
                srcWriteDmaTag(rdram, kTag, srcMakeDmaTag(1u, 1u, 0u));
                std::memcpy(rdram + kTag + 16u, &nopCmd, sizeof(nopCmd));
                std::memset(rdram + kTag + 20u, 0, 12u);
            }

            // Out-of-window kick first: must log nothing and consume nothing.
            mem.gs_regs.vsyncTick.store(1200u, std::memory_order_relaxed);
            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x30u, kTagA), "write VIF1 TADR should succeed");
            t.IsTrue(mem.writeIORegister(kVif1Ch + 0x00u, 0x104u), "write VIF1 CHCR should succeed");
            mem.processPendingTransfers();

            // Two in-window kicks log all their tags; the third stays silent.
            for (uint32_t k = 0u; k < 3u; ++k)
            {
                const uint32_t kTag = (k == 0u) ? kTagA : ((k == 1u) ? kTagB : kTagC);
                mem.gs_regs.vsyncTick.store(1310u + k, std::memory_order_relaxed);
                t.IsTrue(mem.writeIORegister(kVif1Ch + 0x30u, kTag), "write VIF1 TADR should succeed");
                t.IsTrue(mem.writeIORegister(kVif1Ch + 0x00u, 0x104u), "write VIF1 CHCR should succeed");
                mem.processPendingTransfers();
            }

            ps2_mpg_src_trace::clearForTest();
            const std::string text = readWholeFile(tmp);
            char expected[512];
            std::snprintf(expected, sizeof(expected),
                          "ctag tag_at=0x00027600 id=1 qwc=1 addr=0x00000000 tte=0000000000000000\n"
                          "ctag tag_at=0x00027620 id=0 qwc=0 addr=0x00000000 tte=0000000000000000\n"
                          "ctag tag_at=0x00027700 id=1 qwc=1 addr=0x00000000 tte=0000000000000000\n"
                          "ctag tag_at=0x00027720 id=0 qwc=0 addr=0x00000000 tte=0000000000000000\n");
            t.Equals(text, std::string(expected), "only the first two in-window kicks must dump");
            std::remove(tmp.c_str());
        });
    });
}
