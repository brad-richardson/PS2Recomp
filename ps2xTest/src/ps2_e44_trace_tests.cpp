#include "MiniTest.h"
#include "ps2_e44_trace.h"
#include "ps2_e41_trace.h"
#include "runtime/ps2_memory.h"

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
    std::string e44TmpPath(const char *name)
    {
        return (std::filesystem::temp_directory_path() / name).string();
    }

    std::string e44ReadWholeFile(const std::string &path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open())
        {
            return "";
        }
        std::ostringstream out;
        out << in.rdbuf();
        return out.str();
    }

    size_t e44CountPrefix(const std::string &text, const std::string &prefix)
    {
        size_t n = 0u;
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line))
        {
            if (line.compare(0, prefix.size(), prefix) == 0)
            {
                ++n;
            }
        }
        return n;
    }

    void e44SetSpWord(std::vector<uint8_t> &sp, uint32_t off, uint32_t value)
    {
        std::memcpy(sp.data() + off, &value, sizeof(value));
    }

    void e44SetRamWord(std::vector<uint8_t> &ram, uint32_t addr, uint32_t value)
    {
        std::memcpy(ram.data() + (addr & PS2_RAM_MASK), &value, sizeof(value));
    }

    void e44SetReg(R5900Context &ctx, int reg, uint32_t value)
    {
        ctx.r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(value));
    }

    void register_ps2_e44_trace_tests_body(TestCase &tc)
    {
        tc.Run("trace is off by default", [](TestCase &t)
        {
            ps2_e44_trace::clearForTest();
            t.IsTrue(!ps2_e44_trace::enabled(), "trace must be disabled with no file configured");
            std::vector<uint8_t> ram(PS2_RAM_SIZE, 0u);
            std::vector<uint8_t> sp(PS2_SCRATCHPAD_SIZE, 0u);
            ps2SetScratchpadHostPtr(sp.data());
            R5900Context ctx{};
            ctx.pc = 0x362c34u;
            ps2_e41_trace::noteVsync(1275u);
            ps2_e44_trace::noteStore(ram.data(), &ctx, 0x70000000u, 4u, "Store32");
            ps2_e44_trace::noteFast(ram.data(), nullptr, 0x70000000u, 4u, 0x30u, "Ps2FastWrite32");
            ps2_e44_trace::noteMemWrite(ram.data(), 0x70000000u, 4u, "write32");
            ps2_e44_trace::noteSprDma(ram.data(), 0x00100000u, 0u, 16u);
            t.IsTrue(!ps2_e44_trace::enabled(), "disabled taps must stay off");
            ps2SetScratchpadHostPtr(nullptr);
            ps2_e44_trace::clearForTest();
        });

        tc.Run("store32 on watched word emits spw with regs", [](TestCase &t)
        {
            const std::string tmp = e44TmpPath("ps2x-e44-store.txt");
            std::remove(tmp.c_str());
            ps2_e44_trace::configureForTest(tmp.c_str(), 1270u, 1280u);
            std::vector<uint8_t> ram(PS2_RAM_SIZE, 0u);
            std::vector<uint8_t> sp(PS2_SCRATCHPAD_SIZE, 0u);
            e44SetSpWord(sp, 0u, 0x00000030u);
            ps2SetScratchpadHostPtr(sp.data());
            R5900Context ctx{};
            ctx.pc = 0x362c34u;
            e44SetReg(ctx, 31, 0x00362DE8u);
            e44SetReg(ctx, 4, 0x11111111u);
            e44SetReg(ctx, 9, 0x22222222u);

            ps2_e41_trace::noteVsync(1275u);
            ps2_e44_trace::noteStore(ram.data(), &ctx, 0x70000000u, 4u, "Store32");

            ps2_e44_trace::clearForTest();
            ps2SetScratchpadHostPtr(nullptr);
            const std::string text = e44ReadWholeFile(tmp);
            t.IsTrue(text.find("spw vsync=1275 addr=0x70000000 value=0x00000030 via=store32") != std::string::npos,
                     "watched store must log addr+value+via");
            t.IsTrue(text.find("pc=0x00362c34 ra=0x00362de8 fn=Store32") != std::string::npos,
                     "line must carry pc/ra/fn");
            t.IsTrue(text.find("a0=0x11111111") != std::string::npos, "line must carry a0");
            t.IsTrue(text.find("t1=0x22222222") != std::string::npos, "line must carry t1");
            std::remove(tmp.c_str());
        });

        tc.Run("sub-word store reports containing word", [](TestCase &t)
        {
            const std::string tmp = e44TmpPath("ps2x-e44-sub.txt");
            std::remove(tmp.c_str());
            ps2_e44_trace::configureForTest(tmp.c_str(), 1270u, 1280u);
            std::vector<uint8_t> ram(PS2_RAM_SIZE, 0u);
            std::vector<uint8_t> sp(PS2_SCRATCHPAD_SIZE, 0u);
            e44SetSpWord(sp, 0u, 0x12345678u);
            ps2SetScratchpadHostPtr(sp.data());
            R5900Context ctx{};
            ctx.pc = 0x362bacu;

            ps2_e41_trace::noteVsync(1275u);
            ps2_e44_trace::noteStore(ram.data(), &ctx, 0x70000001u, 1u, "Store8");

            ps2_e44_trace::clearForTest();
            ps2SetScratchpadHostPtr(nullptr);
            const std::string text = e44ReadWholeFile(tmp);
            t.IsTrue(text.find("spw vsync=1275 addr=0x70000000 value=0x12345678 via=store8") != std::string::npos,
                     "byte store must log the containing word read-back");
            std::remove(tmp.c_str());
        });

        tc.Run("unwatched scratchpad word stays silent", [](TestCase &t)
        {
            const std::string tmp = e44TmpPath("ps2x-e44-quiet.txt");
            std::remove(tmp.c_str());
            ps2_e44_trace::configureForTest(tmp.c_str(), 1270u, 1280u);
            std::vector<uint8_t> ram(PS2_RAM_SIZE, 0u);
            std::vector<uint8_t> sp(PS2_SCRATCHPAD_SIZE, 0u);
            e44SetSpWord(sp, 0x10u, 0x000001B0u);
            ps2SetScratchpadHostPtr(sp.data());
            R5900Context ctx{};
            ctx.pc = 0x362c34u;

            ps2_e41_trace::noteVsync(1275u);
            ps2_e44_trace::noteStore(ram.data(), &ctx, 0x70000010u, 4u, "Store32");

            ps2_e44_trace::clearForTest();
            ps2SetScratchpadHostPtr(nullptr);
            const std::string text = e44ReadWholeFile(tmp);
            t.IsTrue(text.empty(), "unwatched word must emit nothing");
            std::remove(tmp.c_str());
        });

        tc.Run("per-word cap holds at 64", [](TestCase &t)
        {
            const std::string tmp = e44TmpPath("ps2x-e44-cap.txt");
            std::remove(tmp.c_str());
            ps2_e44_trace::configureForTest(tmp.c_str(), 1270u, 1280u);
            std::vector<uint8_t> ram(PS2_RAM_SIZE, 0u);
            std::vector<uint8_t> sp(PS2_SCRATCHPAD_SIZE, 0u);
            e44SetSpWord(sp, 0u, 0x00000030u);
            ps2SetScratchpadHostPtr(sp.data());
            R5900Context ctx{};
            ctx.pc = 0x362c34u;

            ps2_e41_trace::noteVsync(1275u);
            for (int i = 0; i < 70; ++i)
            {
                ps2_e44_trace::noteStore(ram.data(), &ctx, 0x70000000u, 4u, "Store32");
            }

            ps2_e44_trace::clearForTest();
            ps2SetScratchpadHostPtr(nullptr);
            const std::string text = e44ReadWholeFile(tmp);
            t.IsTrue(e44CountPrefix(text, "spw ") == 64u, "first 64 per word, then quiet");
            std::remove(tmp.c_str());
        });

        tc.Run("window gates lines", [](TestCase &t)
        {
            const std::string tmp = e44TmpPath("ps2x-e44-window.txt");
            std::remove(tmp.c_str());
            ps2_e44_trace::configureForTest(tmp.c_str(), 1270u, 1280u);
            std::vector<uint8_t> ram(PS2_RAM_SIZE, 0u);
            std::vector<uint8_t> sp(PS2_SCRATCHPAD_SIZE, 0u);
            e44SetSpWord(sp, 0u, 0x00000030u);
            ps2SetScratchpadHostPtr(sp.data());
            R5900Context ctx{};
            ctx.pc = 0x362c34u;

            ps2_e41_trace::noteVsync(1269u);
            ps2_e44_trace::noteStore(ram.data(), &ctx, 0x70000000u, 4u, "Store32");
            ps2_e41_trace::noteVsync(1275u);
            ps2_e44_trace::noteStore(ram.data(), &ctx, 0x70000000u, 4u, "Store32");

            ps2_e44_trace::clearForTest();
            ps2SetScratchpadHostPtr(nullptr);
            const std::string text = e44ReadWholeFile(tmp);
            t.IsTrue(text.find("vsync=1269") == std::string::npos, "out-of-window tick emits nothing");
            t.IsTrue(e44CountPrefix(text, "spw ") == 1u, "in-window tick emits once");
            std::remove(tmp.c_str());
        });

        tc.Run("spr-dma reports EE source per word", [](TestCase &t)
        {
            const std::string tmp = e44TmpPath("ps2x-e44-dma.txt");
            std::remove(tmp.c_str());
            ps2_e44_trace::configureForTest(tmp.c_str(), 1270u, 1280u);
            std::vector<uint8_t> ram(PS2_RAM_SIZE, 0u);
            std::vector<uint8_t> sp(PS2_SCRATCHPAD_SIZE, 0u);
            e44SetSpWord(sp, 0u, 0x000001B0u);
            e44SetSpWord(sp, 0x500u, 0x00000030u);
            ps2SetScratchpadHostPtr(sp.data());

            ps2_e41_trace::noteVsync(1275u);
            // Transfer covers spr 0..15: all four low watched words log,
            // word 0x500 stays silent.
            ps2_e44_trace::noteSprDma(ram.data(), 0x00100000u, 0u, 16u);
            // Transfer covers spr 0x500..0x50f: src advances by 0x500.
            ps2_e44_trace::noteSprDma(ram.data(), 0x00200000u, 0x500u, 16u);

            ps2_e44_trace::clearForTest();
            ps2SetScratchpadHostPtr(nullptr);
            const std::string text = e44ReadWholeFile(tmp);
            t.IsTrue(text.find("addr=0x70000000 value=0x000001b0 via=spr-dma src=0x00100000") != std::string::npos,
                     "dma word 0 must carry its EE source");
            t.IsTrue(text.find("addr=0x70000500 value=0x00000030 via=spr-dma src=0x00200000") != std::string::npos,
                     "dma word 0x500 must carry its EE source");
            t.IsTrue(text.find("addr=0x7000000c value=0x00000000 via=spr-dma src=0x0010000c") != std::string::npos,
                     "first transfer covers all four low watched words");
            t.IsTrue(text.find("addr=0x7000050c value=0x00000000 via=spr-dma src=0x0020000c") != std::string::npos,
                     "second transfer covers all four 0x500 watched words");
            t.IsTrue(e44CountPrefix(text, "spw ") == 8u, "only overlapped watched words log (4+4)");
            std::remove(tmp.c_str());
        });

        tc.Run("extra EE word watch for Boot B", [](TestCase &t)
        {
            const std::string tmp = e44TmpPath("ps2x-e44-extra.txt");
            std::remove(tmp.c_str());
            const uint32_t extras[1] = {0x0085AAC0u};
            ps2_e44_trace::configureForTest(tmp.c_str(), 1270u, 1280u, extras, 1);
            std::vector<uint8_t> ram(PS2_RAM_SIZE, 0u);
            std::vector<uint8_t> sp(PS2_SCRATCHPAD_SIZE, 0u);
            e44SetRamWord(ram, 0x0085AAC0u, 0xDEADBEEFu);
            ps2SetScratchpadHostPtr(sp.data());
            R5900Context ctx{};
            ctx.pc = 0x394ED0u;

            ps2_e41_trace::noteVsync(1275u);
            ps2_e44_trace::noteStore(ram.data(), &ctx, 0x0085AAC0u, 4u, "Store32");
            ps2_e44_trace::noteStore(ram.data(), &ctx, 0x0085AAC4u, 4u, "Store32");

            ps2_e44_trace::clearForTest();
            ps2SetScratchpadHostPtr(nullptr);
            const std::string text = e44ReadWholeFile(tmp);
            t.IsTrue(text.find("spw vsync=1275 addr=0x0085aac0 value=0xdeadbeef via=store32") != std::string::npos,
                     "extra EE word must log with RAM read-back");
            t.IsTrue(e44CountPrefix(text, "spw ") == 1u, "non-extra RAM stays silent");
            std::remove(tmp.c_str());
        });

        tc.Run("mem-write suppressed under Store guard only", [](TestCase &t)
        {
            const std::string tmp = e44TmpPath("ps2x-e44-mem.txt");
            std::remove(tmp.c_str());
            ps2_e44_trace::configureForTest(tmp.c_str(), 1270u, 1280u);
            std::vector<uint8_t> ram(PS2_RAM_SIZE, 0u);
            std::vector<uint8_t> sp(PS2_SCRATCHPAD_SIZE, 0u);
            e44SetSpWord(sp, 0u, 0x00000030u);
            ps2SetScratchpadHostPtr(sp.data());

            ps2_e41_trace::noteVsync(1275u);
            {
                ps2_e44_trace::detail::ScopedMemSuppress suppress(true);
                ps2_e44_trace::noteMemWrite(ram.data(), 0x70000000u, 4u, "write32");
            }
            ps2_e44_trace::noteMemWrite(ram.data(), 0x70000000u, 4u, "write32");

            ps2_e44_trace::clearForTest();
            ps2SetScratchpadHostPtr(nullptr);
            const std::string text = e44ReadWholeFile(tmp);
            t.IsTrue(e44CountPrefix(text, "spw ") == 1u, "guarded write silent, host write logs");
            t.IsTrue(text.find("via=mem-write") != std::string::npos, "fallback carries mem-write via");
            std::remove(tmp.c_str());
        });

        tc.Run("vif0unk formats and gates", [](TestCase &t)
        {
            const std::string tmp = e44TmpPath("ps2x-e44-vif0unk.txt");
            std::remove(tmp.c_str());
            ps2_e44_trace::configureForTest(tmp.c_str(), 1270u, 1280u);

            ps2_e41_trace::noteVsync(1269u);
            ps2_e44_trace::noteVif0Unk(0x14u, 0x0080u, 8u, 128u);
            ps2_e41_trace::noteVsync(1275u);
            ps2_e44_trace::noteVif0Unk(0x14u, 0x0080u, 8u, 128u);

            ps2_e44_trace::clearForTest();
            const std::string text = e44ReadWholeFile(tmp);
            t.IsTrue(e44CountPrefix(text, "vif0unk ") == 1u, "only the in-window unk logs");
            t.IsTrue(text.find("vif0unk vsync=1275 op=0x14 imm=0x0080 num=8 rem=128") != std::string::npos,
                     "unk line carries op/imm/num/rem");
            std::remove(tmp.c_str());
        });

        tc.Run("vif0unk caps at 1024", [](TestCase &t)
        {
            const std::string tmp = e44TmpPath("ps2x-e44-vif0cap.txt");
            std::remove(tmp.c_str());
            ps2_e44_trace::configureForTest(tmp.c_str(), 1270u, 1280u);

            ps2_e41_trace::noteVsync(1275u);
            for (int i = 0; i < 1030; ++i)
            {
                ps2_e44_trace::noteVif0Unk(0x15u, 0u, 0u, 0u);
            }

            ps2_e44_trace::clearForTest();
            const std::string text = e44ReadWholeFile(tmp);
            t.IsTrue(e44CountPrefix(text, "vif0unk ") == 1024u, "unk caps at 1024");
            std::remove(tmp.c_str());
        });

        tc.Run("vif0kick aggregates per tick", [](TestCase &t)
        {
            const std::string tmp = e44TmpPath("ps2x-e44-vif0kick.txt");
            std::remove(tmp.c_str());
            ps2_e44_trace::configureForTest(tmp.c_str(), 1270u, 1280u);

            ps2_e41_trace::noteVsync(1275u);
            ps2_e44_trace::noteVif0Kick();
            ps2_e44_trace::noteVif0Kick();
            ps2_e44_trace::noteVif0Kick();
            ps2_e41_trace::noteVsync(1276u);
            ps2_e44_trace::noteVif0Kick();

            ps2_e44_trace::clearForTest();
            const std::string text = e44ReadWholeFile(tmp);
            t.IsTrue(text.find("vif0kick vsync=1275 n=3") != std::string::npos,
                     "completed tick flushes its kick count");
            std::remove(tmp.c_str());
        });

        tc.Run("vu0call formats, gates and caps", [](TestCase &t)
        {
            const std::string tmp = e44TmpPath("ps2x-e44-vu0.txt");
            std::remove(tmp.c_str());
            ps2_e44_trace::configureForTest(tmp.c_str(), 1270u, 1280u);
            R5900Context ctx{};
            ctx.pc = 0x37deb8u;

            ps2_e41_trace::noteVsync(1269u);
            ps2_e44_trace::noteVu0Call(&ctx, 0x100u, 100u, false, 1, 2);
            ps2_e41_trace::noteVsync(1275u);
            ps2_e44_trace::noteVu0Call(&ctx, 0x100u, 100u, false, 1, 2);
            ps2_e44_trace::noteVu0Call(&ctx, 0x200u, 4096u, true, 3, 4);
            for (int i = 0; i < 2005; ++i)
            {
                ps2_e44_trace::noteVu0Call(&ctx, 0x300u, 10u, false, 0, 0);
            }

            ps2_e44_trace::clearForTest();
            const std::string text = e44ReadWholeFile(tmp);
            t.IsTrue(text.find("vu0call vsync=1275 caller=0x0037deb8 start=0x00000100 cycles=100 budgethit=0 vi1=1 vi2=2") != std::string::npos,
                     "vu0call carries caller/start/cycles/vi");
            t.IsTrue(text.find("budgethit=1 vi1=3 vi2=4") != std::string::npos,
                     "budget exhaustion is flagged");
            t.IsTrue(e44CountPrefix(text, "vu0call ") == 2000u, "vu0call caps at 2000");
            std::remove(tmp.c_str());
        });

        tc.Run("spr-from taps extra EE words only", [](TestCase &t)
        {
            const std::string tmp = e44TmpPath("ps2x-e44-sprfrom.txt");
            std::remove(tmp.c_str());
            const uint32_t extras[1] = {0x00809670u};
            ps2_e44_trace::configureForTest(tmp.c_str(), 1270u, 1280u, extras, 1);
            std::vector<uint8_t> ram(PS2_RAM_SIZE, 0u);
            e44SetRamWord(ram, 0x00809670u, 0x000001B0u);
            std::vector<uint8_t> sp(PS2_SCRATCHPAD_SIZE, 0u);
            ps2SetScratchpadHostPtr(sp.data());

            ps2_e41_trace::noteVsync(1275u);
            // FROM dest covers the extra word: logs with scratchpad src.
            ps2_e44_trace::noteSprFromDma(ram.data(), 0x00809670u, 0x200u, 16u);
            // FROM dest elsewhere: silent.
            ps2_e44_trace::noteSprFromDma(ram.data(), 0x00809000u, 0x200u, 16u);

            ps2_e44_trace::clearForTest();
            ps2SetScratchpadHostPtr(nullptr);
            const std::string text = e44ReadWholeFile(tmp);
            t.IsTrue(e44CountPrefix(text, "spw ") == 1u, "only the covered extra logs");
            t.IsTrue(text.find("addr=0x00809670 value=0x000001b0 via=spr-from src=0x70000200") != std::string::npos,
                     "spr-from carries the scratchpad source");
            std::remove(tmp.c_str());
        });

        tc.Run("sprmod logs mode kicks", [](TestCase &t)
        {
            const std::string tmp = e44TmpPath("ps2x-e44-sprmod.txt");
            std::remove(tmp.c_str());
            ps2_e44_trace::configureForTest(tmp.c_str(), 1270u, 1280u);

            ps2_e41_trace::noteVsync(1275u);
            ps2_e44_trace::noteSprMod(true, 1u, 0x00809000u, 0x200u, 4u);

            ps2_e44_trace::clearForTest();
            const std::string text = e44ReadWholeFile(tmp);
            t.IsTrue(text.find("sprmod vsync=1275 dir=from mode=1 madr=0x00809000 sadr=0x00000200 qwc=4") != std::string::npos,
                     "sprmod carries dir/mode/addrs");
            std::remove(tmp.c_str());
        });

        tc.Run("last-writer tracks out-of-window, readout gates", [](TestCase &t)
        {
            const std::string tmp = e44TmpPath("ps2x-e44-last.txt");
            std::remove(tmp.c_str());
            ps2_e44_trace::configureForTest(tmp.c_str(), 1355u, 1400u);
            std::vector<uint8_t> ram(PS2_RAM_SIZE, 0u);
            std::vector<uint8_t> sp(PS2_SCRATCHPAD_SIZE, 0u);
            e44SetSpWord(sp, 0u, 0x00000030u);
            e44SetSpWord(sp, 4u, 0x00814884u);
            ps2SetScratchpadHostPtr(sp.data());
            R5900Context ctx{};
            ctx.pc = 0x362c34u;
            e44SetReg(ctx, 31, 0x00362DE8u);

            // Out-of-window store: no spw line, but the record updates.
            ps2_e41_trace::noteVsync(1300u);
            ps2_e44_trace::noteStore(ram.data(), &ctx, 0x70000000u, 4u, "Store32");
            ps2_e44_trace::noteItem0Walk(&ctx, 0x70000000u);
            // Wrong item: silent even in-window.
            ps2_e41_trace::noteVsync(1360u);
            ps2_e44_trace::noteItem0Walk(&ctx, 0x70000080u);
            // Item-0 walk in-window: one spwlast line, no spw lines.
            ps2_e44_trace::noteItem0Walk(&ctx, 0x70000000u);

            ps2_e44_trace::clearForTest();
            ps2SetScratchpadHostPtr(nullptr);
            const std::string text = e44ReadWholeFile(tmp);
            t.IsTrue(e44CountPrefix(text, "spw ") == 0u, "out-of-window store emits no spw");
            t.IsTrue(e44CountPrefix(text, "spwlast ") == 1u, "one readout line");
            t.IsTrue(text.find("spwlast vsync=1360 w0=store32,0x00362c34,0x00362de8,Store32,0x00000030,-,1300") != std::string::npos,
                     "readout carries via/pc/ra/fn/value/src/wtick");
            t.IsTrue(text.find("w1=- w2=- w3=-") != std::string::npos,
                     "untouched words read as empty");
            std::remove(tmp.c_str());
        });

        tc.Run("last-writer survives the spw cap and dma overwrites", [](TestCase &t)
        {
            const std::string tmp = e44TmpPath("ps2x-e44-lastcap.txt");
            std::remove(tmp.c_str());
            ps2_e44_trace::configureForTest(tmp.c_str(), 1355u, 1400u);
            std::vector<uint8_t> ram(PS2_RAM_SIZE, 0u);
            std::vector<uint8_t> sp(PS2_SCRATCHPAD_SIZE, 0u);
            ps2SetScratchpadHostPtr(sp.data());
            R5900Context ctx{};
            ctx.pc = 0x3796b4u;

            ps2_e41_trace::noteVsync(1360u);
            e44SetSpWord(sp, 0u, 0x10000010u);
            for (int i = 0; i < 64; ++i)
            {
                ps2_e44_trace::noteStore(ram.data(), &ctx, 0x70000000u, 4u, "Store32");
            }
            // Post-cap DMA overwrite of word 0 only (production calls both,
            // as the SPR_TO site does).
            e44SetSpWord(sp, 0u, 0x000000CCu);
            ps2_e44_trace::noteSprDma(ram.data(), 0x00100000u, 0u, 4u);
            ps2_e44_trace::trackLastWriterDma(ram.data(), 0x00100000u, 0u, 4u);
            ps2_e44_trace::noteItem0Walk(&ctx, 0x70000000u);

            ps2_e44_trace::clearForTest();
            ps2SetScratchpadHostPtr(nullptr);
            const std::string text = e44ReadWholeFile(tmp);
            t.IsTrue(e44CountPrefix(text, "spw ") == 64u, "spw still caps at 64");
            t.IsTrue(text.find("w0=spr-dma,0x00000000,0x00000000,spr-to,0x000000cc,0x00100000,1360") != std::string::npos,
                     "record follows the post-cap DMA overwrite with its EE src");
            std::remove(tmp.c_str());
        });

        tc.Run("range overlap covers bulk host copies", [](TestCase &t)
        {
            const std::string tmp = e44TmpPath("ps2x-e44-range.txt");
            std::remove(tmp.c_str());
            const uint32_t extras[2] = {0x00809670u, 0x00809B70u};
            ps2_e44_trace::configureForTest(tmp.c_str(), 0u, 2000u, extras, 2);
            std::vector<uint8_t> ram(PS2_RAM_SIZE, 0u);
            e44SetRamWord(ram, 0x00809670u, 0x00000030u);
            e44SetRamWord(ram, 0x00809674u, 0x00814884u);
            std::vector<uint8_t> sp(PS2_SCRATCHPAD_SIZE, 0u);
            ps2SetScratchpadHostPtr(sp.data());
            R5900Context ctx{};
            ctx.pc = 0x00123456u;

            ps2_e41_trace::noteVsync(100u);
            // Bulk libc-memcpy-shaped range covering the first extra quad.
            ps2_e44_trace::emitRangeOverlap(ram.data(), &ctx, 0x00809670u, 16u,
                                            "libc-memcpy", 0x00809000u, true, "memcpy");
            // Disjoint range: silent.
            ps2_e44_trace::emitRangeOverlap(ram.data(), &ctx, 0x00800000u, 16u,
                                            "libc-memcpy", 0x00809000u, true, "memcpy");

            ps2_e44_trace::clearForTest();
            ps2SetScratchpadHostPtr(nullptr);
            const std::string text = e44ReadWholeFile(tmp);
            t.IsTrue(e44CountPrefix(text, "spw ") == 1u, "only the covered extra word logs");
            t.IsTrue(text.find("spw vsync=100 addr=0x00809670 value=0x00000030 via=libc-memcpy src=0x00809000") != std::string::npos,
                     "range line carries linear src mapping");
            t.IsTrue(text.find("pc=0x00123456") != std::string::npos &&
                         text.find("fn=memcpy") != std::string::npos,
                     "range line carries caller regs");
            std::remove(tmp.c_str());
        });

        tc.Run("ebwlast flushes one line per changed word per tick", [](TestCase &t)
        {
            const std::string tmp = e44TmpPath("ps2x-e44-eb.txt");
            std::remove(tmp.c_str());
            const uint32_t extras[1] = {0x00809670u};
            ps2_e44_trace::configureForTest(tmp.c_str(), 0u, 2000u, extras, 1);
            std::vector<uint8_t> ram(PS2_RAM_SIZE, 0u);
            std::vector<uint8_t> sp(PS2_SCRATCHPAD_SIZE, 0u);
            ps2SetScratchpadHostPtr(sp.data());
            R5900Context ctx{};
            ctx.pc = 0x00AAAAAAu;

            // Two writes in tick 100 (last wins), one in tick 101; the
            // tick-101 store drives the flush of tick 100's record via
            // the scratchpad storm word.
            e44SetRamWord(ram, 0x00809670u, 0x00000001u);
            ps2_e41_trace::noteVsync(100u);
            ps2_e44_trace::noteStore(ram.data(), &ctx, 0x00809670u, 4u, "Store32");
            e44SetRamWord(ram, 0x00809670u, 0x00000030u);
            ps2_e44_trace::noteStore(ram.data(), &ctx, 0x00809670u, 4u, "Store32");
            e44SetSpWord(sp, 0u, 0x12345678u);
            ps2_e41_trace::noteVsync(101u);
            ps2_e44_trace::noteStore(ram.data(), &ctx, 0x70000000u, 4u, "Store32");

            ps2_e44_trace::clearForTest();
            ps2SetScratchpadHostPtr(nullptr);
            const std::string text = e44ReadWholeFile(tmp);
            t.IsTrue(e44CountPrefix(text, "ebwlast ") == 1u, "one line for the changed tick");
            t.IsTrue(text.find("ebwlast vsync=100 addr=0x00809670 value=0x00000030 via=store32 pc=0x00aaaaaa") != std::string::npos,
                     "ebwlast carries last-writer state of that tick");
            std::remove(tmp.c_str());
        });

        tc.Run("eb record follows post-cap writes", [](TestCase &t)
        {
            const std::string tmp = e44TmpPath("ps2x-e44-ebcap.txt");
            std::remove(tmp.c_str());
            const uint32_t extras[1] = {0x00809670u};
            ps2_e44_trace::configureForTest(tmp.c_str(), 0u, 2000u, extras, 1);
            std::vector<uint8_t> ram(PS2_RAM_SIZE, 0u);
            std::vector<uint8_t> sp(PS2_SCRATCHPAD_SIZE, 0u);
            ps2SetScratchpadHostPtr(sp.data());
            R5900Context ctx{};

            ps2_e41_trace::noteVsync(500u);
            e44SetRamWord(ram, 0x00809670u, 0x11111111u);
            for (int i = 0; i < 64; ++i)
            {
                ps2_e44_trace::noteStore(ram.data(), &ctx, 0x00809670u, 4u, "Store32");
            }
            // 65th: spw-capped, but the change record must follow.
            e44SetRamWord(ram, 0x00809670u, 0x00000030u);
            ps2_e44_trace::noteStore(ram.data(), &ctx, 0x00809670u, 4u, "Store32");
            e44SetSpWord(sp, 0u, 0u);
            ps2_e41_trace::noteVsync(501u);
            ps2_e44_trace::noteStore(ram.data(), &ctx, 0x70000000u, 4u, "Store32");

            ps2_e44_trace::clearForTest();
            ps2SetScratchpadHostPtr(nullptr);
            const std::string text = e44ReadWholeFile(tmp);
            t.IsTrue(e44CountPrefix(text, "spw ") == 65u, "64 extra + 1 storm spw");
            t.IsTrue(text.find("ebwlast vsync=500 addr=0x00809670 value=0x00000030") != std::string::npos,
                     "eb record follows the post-cap write");
            std::remove(tmp.c_str());
        });

        tc.Run("spwlast caps at 40", [](TestCase &t)
        {
            const std::string tmp = e44TmpPath("ps2x-e44-lastcap40.txt");
            std::remove(tmp.c_str());
            ps2_e44_trace::configureForTest(tmp.c_str(), 1355u, 1400u);
            R5900Context ctx{};

            ps2_e41_trace::noteVsync(1360u);
            for (int i = 0; i < 45; ++i)
            {
                ps2_e44_trace::noteItem0Walk(&ctx, 0x70000000u);
            }

            ps2_e44_trace::clearForTest();
            const std::string text = e44ReadWholeFile(tmp);
            t.IsTrue(e44CountPrefix(text, "spwlast ") == 40u, "readout caps at 40");
            std::remove(tmp.c_str());
        });

        tc.Run("fast tap on watched word logs via fast", [](TestCase &t)
        {
            const std::string tmp = e44TmpPath("ps2x-e44-fast.txt");
            std::remove(tmp.c_str());
            ps2_e44_trace::configureForTest(tmp.c_str(), 1270u, 1280u);
            std::vector<uint8_t> ram(PS2_RAM_SIZE, 0u);
            std::vector<uint8_t> sp(PS2_SCRATCHPAD_SIZE, 0u);
            e44SetSpWord(sp, 0u, 0x00000030u);
            ps2SetScratchpadHostPtr(sp.data());

            ps2_e41_trace::noteVsync(1275u);
            ps2_e44_trace::noteFast(ram.data(), nullptr, 0x70000000u, 4u, 0x30u, "Ps2FastWrite32");
            ps2_e44_trace::noteFast(ram.data(), nullptr, 0x00001000u, 4u, 0x30u, "Ps2FastWrite32");

            ps2_e44_trace::clearForTest();
            ps2SetScratchpadHostPtr(nullptr);
            const std::string text = e44ReadWholeFile(tmp);
            t.IsTrue(e44CountPrefix(text, "spw ") == 1u, "only the watched word logs");
            t.IsTrue(text.find("via=fast") != std::string::npos, "fast path carries fast via");
            std::remove(tmp.c_str());
        });
    }
}

void register_ps2_e44_trace_tests()
{
    MiniTest::Case("Ps2E44Trace", register_ps2_e44_trace_tests_body);
}
