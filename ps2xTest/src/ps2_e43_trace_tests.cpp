#include "MiniTest.h"
#include "ps2_e43_trace.h"
#include "ps2_e41_trace.h"
#include "ps2_runtime_macros.h"
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
    std::string e43TmpPath(const char *name)
    {
        return (std::filesystem::temp_directory_path() / name).string();
    }

    std::string e43ReadWholeFile(const std::string &path)
    {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream out;
        out << in.rdbuf();
        return out.str();
    }

    size_t e43CountLines(const std::string &text)
    {
        size_t n = 0u;
        for (char c : text)
        {
            if (c == '\n')
            {
                ++n;
            }
        }
        return n;
    }

    size_t e43CountPrefix(const std::string &text, const std::string &prefix)
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

    void e43SetWord(std::vector<uint8_t> &ram, uint32_t addr, uint32_t value)
    {
        std::memcpy(ram.data() + (addr & PS2_RAM_MASK), &value, sizeof(value));
    }

    void e43SetReg(R5900Context &ctx, int reg, uint32_t value)
    {
        ctx.r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(value));
    }

    // WRITE-macro probe with the same identifier shape as generated code.
    void e43WriteProbe32(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t addr, uint32_t value)
    {
        (void)runtime;
        WRITE32(addr, value);
    }

    // Walker-call probe: a1 = mode, s3 = record base.
    void e43WalkerProbe(const uint8_t *rdram, uint32_t mode, uint32_t s3, uint32_t source)
    {
        R5900Context ctx{};
        e43SetReg(ctx, 5, mode);
        e43SetReg(ctx, 19, s3);
        ps2_e43_trace::noteWalkerCall(rdram, &ctx, source);
    }

    void register_ps2_e43_trace_tests_body(TestCase &tc)
    {
        tc.Run("trace is off by default", [](TestCase &t)
        {
            ps2_e43_trace::clearForTest();
            t.IsTrue(!ps2_e43_trace::enabled(), "trace must be disabled with no file configured");
            std::vector<uint8_t> ram(PS2_RAM_SIZE, 0u);
            R5900Context ctx{};
            ps2_e43_trace::noteWalkerCall(ram.data(), &ctx, 0x00363CF4u);
            ps2_e43_trace::noteProdSite(ram.data(), 0x0085AC24u, 4u);
            t.IsTrue(!ps2_e43_trace::enabled(), "disabled taps must stay off");
        });

        tc.Run("drec aggregates per tick and flushes on advance", [](TestCase &t)
        {
            const std::string tmp = e43TmpPath("ps2x-e43-drec.txt");
            std::remove(tmp.c_str());
            ps2_e43_trace::configureForTest(tmp.c_str(), 0u, ~0ull);
            std::vector<uint8_t> ram(PS2_RAM_SIZE, 0u);
            // Mode-3 record (w0=0xCC -> bits 6-9 = 3) and mode-6 (w0=0x1B0 -> 6).
            e43SetWord(ram, 0x0085AC24u, 0x000000CCu);
            e43SetWord(ram, 0x0085AA00u, 0x000001B0u);

            ps2_e41_trace::noteVsync(1270u);
            e43WalkerProbe(ram.data(), 3u, 0x0085AC24u, 0x00363CF4u);
            e43WalkerProbe(ram.data(), 3u, 0x0085AC28u, 0x00363CF4u);
            e43WalkerProbe(ram.data(), 6u, 0x0085AA00u, 0x00363CF4u);
            e43WalkerProbe(ram.data(), 20u, 0x0085AA04u, 0x00363CF4u);
            ps2_e41_trace::noteVsync(1271u);
            e43WalkerProbe(ram.data(), 3u, 0x0085AC24u, 0x00363CF4u);

            ps2_e43_trace::clearForTest(); // flushes the trailing tick
            const std::string text = e43ReadWholeFile(tmp);
            t.IsTrue(text.find("drec vsync=1270 src=0x00363cf4 mode=3 count=2") != std::string::npos,
                     "tick 1270 must count 2 mode-3 walks");
            t.IsTrue(text.find("drec vsync=1270 src=0x00363cf4 mode=6 count=1") != std::string::npos,
                     "tick 1270 must count 1 mode-6 walk");
            t.IsTrue(text.find("drec vsync=1270 src=0x00363cf4 mode=X count=1") != std::string::npos,
                     "out-of-range mode must land in the X bucket");
            t.IsTrue(text.find("drec vsync=1271 src=0x00363cf4 mode=3 count=1") != std::string::npos,
                     "advanced tick must flush the prior tick and start a new row");
            t.IsTrue(text.find("drecs vsync=1270 src=0x00363cf4 mode=3 addr=0x0085ac24 wmode=3 "
                               "w0=0x000000cc") != std::string::npos,
                     "drecs must carry the record words with a matching wmode");
            t.IsTrue(text.find("drecs vsync=1270 src=0x00363cf4 mode=6 addr=0x0085aa00 wmode=6 "
                               "w0=0x000001b0") != std::string::npos,
                     "mode-6 record must be captured with its w0");
            std::remove(tmp.c_str());
        });

        tc.Run("drecs keeps the first 8 records per mode", [](TestCase &t)
        {
            const std::string tmp = e43TmpPath("ps2x-e43-drecs.txt");
            std::remove(tmp.c_str());
            ps2_e43_trace::configureForTest(tmp.c_str(), 0u, ~0ull);
            std::vector<uint8_t> ram(PS2_RAM_SIZE, 0u);
            e43SetWord(ram, 0x0085AC24u, 0x000000CCu);

            ps2_e41_trace::noteVsync(1300u);
            for (int i = 0; i < 10; ++i)
            {
                e43WalkerProbe(ram.data(), 3u, 0x0085AC24u, 0x00363CF4u);
            }

            ps2_e43_trace::clearForTest();
            const std::string text = e43ReadWholeFile(tmp);
            t.Equals(e43CountPrefix(text, "drecs "), static_cast<size_t>(8u),
                     "only the first 8 records per mode log drecs");
            t.IsTrue(text.find("drec vsync=1300 src=0x00363cf4 mode=3 count=10") != std::string::npos,
                     "the count row still sees all 10 walks");
            std::remove(tmp.c_str());
        });

        tc.Run("producer watch learns pages and attributes macro stores", [](TestCase &t)
        {
            const std::string tmp = e43TmpPath("ps2x-e43-dprod.txt");
            std::remove(tmp.c_str());
            ps2_e43_trace::configureForTest(tmp.c_str(), 0u, ~0ull);
            std::vector<uint8_t> ram(PS2_RAM_SIZE, 0u);
            e43SetWord(ram, 0x0085AC24u, 0x000000CCu);

            ps2_e41_trace::noteVsync(1280u);
            // Census learns page 0x85 of the walker-source record...
            e43WalkerProbe(ram.data(), 3u, 0x0085AC24u, 0x00363CF4u);
            // ...but not of the other caller's records.
            e43WalkerProbe(ram.data(), 3u, 0x0095AC24u, 0x00364460u);
            t.IsTrue(ps2_e43_trace::isProdPage(0x0085AD00u), "learned page must match");
            t.IsTrue(ps2_e43_trace::isProdPage(0x3085AD00u), "0x30 mirror must fold onto the page");
            t.IsTrue(!ps2_e43_trace::isProdPage(0x0095AD00u), "unlearned page must not match");

            R5900Context ctxStruct{};
            R5900Context *ctx = &ctxStruct;
            ctxStruct.pc = 0x00380000u;
            e43SetReg(ctxStruct, 31, 0x00380010u);
            // Mode-6 word into the learned page via the 0x30 mirror.
            e43WriteProbe32(ram.data(), ctx, nullptr, 0x3085AC28u, 0x000001B0u);
            // Mode-3 word: silent under the default mode-6 filter.
            e43WriteProbe32(ram.data(), ctx, nullptr, 0x0085AC2Cu, 0x000000CCu);
            // Mode-6 word outside the region: silent.
            e43WriteProbe32(ram.data(), ctx, nullptr, 0x01000000u, 0x000001B0u);

            uint32_t back = 0u;
            std::memcpy(&back, ram.data() + (0x0085AC28u & PS2_RAM_MASK), sizeof(back));
            t.Equals(back, 0x000001B0u, "probed store must land in ram");

            ps2_e43_trace::clearForTest();
            const std::string text = e43ReadWholeFile(tmp);
            t.Equals(e43CountPrefix(text, "dprod "), static_cast<size_t>(1u),
                     "exactly one producer store logs (guard: no fast-path double)");
            t.IsTrue(text.find("dprod vsync=1280 addr=0x0085ac28 value=0x000001b0 mode=6 "
                               "pc=0x00380000 ra=0x00380010 fn=e43WriteProbe32 src=macro "
                               "raw=0x3085ac28") != std::string::npos,
                     "macro tap must log addr/value/mode/pc/ra/host-fn/raw");
            std::remove(tmp.c_str());
        });

        tc.Run("fast-path producer store resolves the host fn", [](TestCase &t)
        {
            const std::string tmp = e43TmpPath("ps2x-e43-fast.txt");
            std::remove(tmp.c_str());
            ps2_e43_trace::configureForTest(tmp.c_str(), 0u, ~0ull);
            std::vector<uint8_t> ram(PS2_RAM_SIZE, 0u);

            ps2_e41_trace::noteVsync(1281u);
            ps2_e43_trace::learnProdPage(0x0085AA00u);
            Ps2FastWrite32(ram.data(), 0x0085AA04u, 0x000001B0u);

            ps2_e43_trace::clearForTest();
            const std::string text = e43ReadWholeFile(tmp);
            t.Equals(e43CountLines(text), static_cast<size_t>(1u), "one fast-path line expected");
            t.IsTrue(text.find("src=fast") != std::string::npos, "line must be tagged fast");
            t.IsTrue(text.find("pc=0x00000000 ra=0x00000000") != std::string::npos,
                     "fast path carries no guest pc/ra");
            t.IsTrue(text.find(" fn=- ") == std::string::npos, "host fn must resolve (not stripped)");
            std::remove(tmp.c_str());
        });

        tc.Run("h394 logs hash, return value and the store flag", [](TestCase &t)
        {
            const std::string tmp = e43TmpPath("ps2x-e43-h394.txt");
            std::remove(tmp.c_str());
            ps2_e43_trace::configureForTest(tmp.c_str(), 1270u, 1400u);
            std::vector<uint8_t> ram(PS2_RAM_SIZE, 0u);
            e43SetWord(ram, 0x0085AA00u, 0x00000011u);
            e43SetWord(ram, 0x0085AA04u, 0x00000022u);
            e43SetWord(ram, 0x0085AA08u, 0x00000033u);
            e43SetWord(ram, 0x0085AA0Cu, 0x00000044u);

            ps2_e41_trace::noteVsync(1365u);
            t.IsTrue(ps2_e43_trace::h394CallArmed(0x00362F68u, 0x00394ED0u),
                     "the briefed call site must arm inside the h394 window");
            t.IsTrue(!ps2_e43_trace::h394CallArmed(0x00363CF4u, 0x00394ED0u),
                     "other sources must not arm h394");
            t.IsTrue(!ps2_e43_trace::h394CallArmed(0x00362F68u, 0x00364CD0u),
                     "other targets must not arm h394");

            R5900Context ctx{};
            e43SetReg(ctx, 17, 0x0085AA00u); // s1 = record ptr
            e43SetReg(ctx, 6, 0x000000ABu); // a2 = hash
            ps2_e43_trace::h394Pre(ram.data(), &ctx);
            // The synchronous call runs; a store at a watched pc fires...
            ctx.pc = 0x00394FE8u;
            ps2_e43_trace::noteWriteCtx(&ctx, ram.data(), 0x0085AA10u, 4u, "probe_fn", false);
            // ...a store elsewhere does not clear it.
            ctx.pc = 0x00380000u;
            ps2_e43_trace::noteWriteCtx(&ctx, ram.data(), 0x0085AA14u, 4u, "probe_fn", false);
            e43SetReg(ctx, 2, 0x0085BB00u); // v0 = return value
            ps2_e43_trace::h394Post(&ctx);

            // Out-of-window call: armed false, nothing logged.
            ps2_e41_trace::noteVsync(1500u);
            t.IsTrue(!ps2_e43_trace::h394CallArmed(0x00362F68u, 0x00394ED0u),
                     "h394 window must end at the TO bound");
            ps2_e43_trace::h394Pre(ram.data(), &ctx);
            ps2_e43_trace::h394Post(&ctx);

            ps2_e43_trace::clearForTest();
            const std::string text = e43ReadWholeFile(tmp);
            t.Equals(e43CountLines(text), static_cast<size_t>(1u), "one h394 line expected");
            t.IsTrue(text.find("h394 vsync=1365 s1=0x0085aa00 w0=0x00000011 w1=0x00000022 "
                               "w2=0x00000033 w3=0x00000044 hash=0xab ret=0x0085bb00 stores=1") !=
                         std::string::npos,
                     "h394 must log s1/words/hash/ret/stores flag");
            std::remove(tmp.c_str());
        });

        tc.Run("window filters census lines but keeps counting", [](TestCase &t)
        {
            const std::string tmp = e43TmpPath("ps2x-e43-window.txt");
            std::remove(tmp.c_str());
            ps2_e43_trace::configureForTest(tmp.c_str(), 100u, 200u);
            std::vector<uint8_t> ram(PS2_RAM_SIZE, 0u);
            e43SetWord(ram, 0x0085AC24u, 0x000000CCu);

            ps2_e41_trace::noteVsync(50u);
            e43WalkerProbe(ram.data(), 3u, 0x0085AC24u, 0x00363CF4u);
            ps2_e41_trace::noteVsync(150u);
            e43WalkerProbe(ram.data(), 3u, 0x0085AC24u, 0x00363CF4u);

            ps2_e43_trace::clearForTest();
            const std::string text = e43ReadWholeFile(tmp);
            t.IsTrue(text.find("vsync=50") == std::string::npos, "out-of-window tick emits nothing");
            t.IsTrue(text.find("drec vsync=150 src=0x00363cf4 mode=3 count=1") != std::string::npos,
                     "in-window tick emits its count");
            std::remove(tmp.c_str());
        });
    }
}

void register_ps2_e43_trace_tests()
{
    MiniTest::Case("Ps2E43Trace", register_ps2_e43_trace_tests_body);
}
