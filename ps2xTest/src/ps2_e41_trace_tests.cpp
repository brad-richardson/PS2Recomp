#include "MiniTest.h"
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
    std::string e41TmpPath(const char *name)
    {
        return (std::filesystem::temp_directory_path() / name).string();
    }

    std::string e41ReadWholeFile(const std::string &path)
    {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream out;
        out << in.rdbuf();
        return out.str();
    }

    void e41SetWord(std::vector<uint8_t> &ram, uint32_t addr, uint32_t value)
    {
        std::memcpy(ram.data() + (addr & PS2_RAM_MASK), &value, sizeof(value));
    }

    void register_ps2_e41_trace_tests_body(TestCase &tc)
    {
        tc.Run("trace is off by default", [](TestCase &t)
        {
            ps2_e41_trace::clearForTest();
            t.IsTrue(!ps2_e41_trace::armed(), "trace must be disabled with no file configured");
            t.IsTrue(!ps2_e41_trace::plantArmed(), "plant watch must be off with no file");
            std::vector<uint8_t> ram(8u * 1024u * 1024u, 0u);
            t.Equals(ps2_e41_trace::noteCdRead(10u, 0x100u, 4u, 0x1000u, "sceCdRead", "f"),
                     0ull, "disabled cdread must return seq 0");
            ps2_e41_trace::noteFastWrite(ram.data(), 0x63B994u, 4u);
            t.IsTrue(!ps2_e41_trace::armed(), "disabled fast-write tap must stay off");
        });

        tc.Run("cdread line format and seq increments", [](TestCase &t)
        {
            const std::string tmp = e41TmpPath("ps2x-e41-cdread.txt");
            std::remove(tmp.c_str());
            ps2_e41_trace::configureForTest(tmp.c_str(), 0u, ~0ull);
            t.IsTrue(ps2_e41_trace::armed(), "configured trace must be armed");

            t.Equals(ps2_e41_trace::noteCdRead(1238u, 0x12ABu, 16u, 0x01A3B400u,
                                               "sceCdRead", "DATA.BIN"),
                     1ull, "first seq is 1");
            t.Equals(ps2_e41_trace::noteCdRead(1238u, 0x12BBu, 1u, 0x01A3C400u,
                                               "sceCdStRead", nullptr),
                     2ull, "second seq is 2");

            ps2_e41_trace::clearForTest();
            const std::string text = e41ReadWholeFile(tmp);
            const std::string expected =
                "cdread seq=1 vsync=1238 lbn=0x12ab sectors=16 mode=sceCdRead dest=0x01a3b400 file=DATA.BIN\n"
                "cdread seq=2 vsync=1238 lbn=0x12bb sectors=1 mode=sceCdStRead dest=0x01a3c400 file=-\n";
            t.Equals(text, expected, "cdread lines must match the T52-compatible format exactly");
            std::remove(tmp.c_str());
        });

        tc.Run("cdsearch line format", [](TestCase &t)
        {
            const std::string tmp = e41TmpPath("ps2x-e41-cdsearch.txt");
            std::remove(tmp.c_str());
            ps2_e41_trace::configureForTest(tmp.c_str(), 0u, ~0ull);

            ps2_e41_trace::noteCdSearch(500u, "\\CDROM0:\\DATA.BIN;1", 0x200u, 0x123400u);

            ps2_e41_trace::clearForTest();
            const std::string text = e41ReadWholeFile(tmp);
            const std::string expected =
                "cdsearch vsync=500 name=\\CDROM0:\\DATA.BIN;1 lbn=0x200 size=1192960\n";
            t.Equals(text, expected, "cdsearch line must match the T52 format exactly");
            std::remove(tmp.c_str());
        });

        tc.Run("cdopen and fioread join on fd", [](TestCase &t)
        {
            const std::string tmp = e41TmpPath("ps2x-e41-fio.txt");
            std::remove(tmp.c_str());
            ps2_e41_trace::configureForTest(tmp.c_str(), 0u, ~0ull);

            ps2_e41_trace::noteFioOpen(600u, "host0:/music.wav", "/data/music.wav", 7);
            ps2_e41_trace::noteFioRead(601u, 7, 0x02000000u, 2048u);
            ps2_e41_trace::noteFioRead(602u, 99, 0x02001000u, 512u);

            ps2_e41_trace::clearForTest();
            const std::string text = e41ReadWholeFile(tmp);
            const std::string expected =
                "cdopen vsync=600 name=host0:/music.wav host=/data/music.wav fd=7\n"
                "fioread vsync=601 fd=7 buf=0x02000000 bytes=2048 name=host0:/music.wav\n"
                "fioread vsync=602 fd=99 buf=0x02001000 bytes=512 name=-\n";
            t.Equals(text, expected, "fioread must resolve the fd name from the earlier open");
            std::remove(tmp.c_str());
        });

        tc.Run("plant folds uncached mirror onto canonical word", [](TestCase &t)
        {
            const std::string tmp = e41TmpPath("ps2x-e41-plant.txt");
            std::remove(tmp.c_str());
            ps2_e41_trace::configureForTest(tmp.c_str(), 0u, ~0ull);
            std::vector<uint8_t> ram(8u * 1024u * 1024u, 0u);

            // Uncached-alias write of the uploader value into the first word.
            e41SetWord(ram, 0x0063B994u, 0x00435BD0u);
            ps2_e41_trace::notePlantRange(900u, 0x2063B994u, 4u, ram.data(),
                                          "sceCdRead", "lbn=0x12ab", 42u);
            // Adjacent word: silent. Far range: silent.
            e41SetWord(ram, 0x0063B998u, 0x00435BD0u);
            ps2_e41_trace::notePlantRange(901u, 0x0063B998u, 4u, ram.data(),
                                          "ee-store", "-", 0u);
            ps2_e41_trace::notePlantRange(902u, 0x01000000u, 64u, ram.data(),
                                          "sif-dma", "src=0x1000", 0u);

            ps2_e41_trace::clearForTest();
            const std::string text = e41ReadWholeFile(tmp);
            const std::string expected =
                "plant vsync=900 addr=0x0063b994 value=0x00435bd0 via=sceCdRead src=lbn=0x12ab seq=42\n";
            t.Equals(text, expected, "mirror write must log under the canonical word");
            std::remove(tmp.c_str());
        });

        tc.Run("fast-write tap fires with the vblank mirror vsync", [](TestCase &t)
        {
            const std::string tmp = e41TmpPath("ps2x-e41-fast.txt");
            std::remove(tmp.c_str());
            ps2_e41_trace::configureForTest(tmp.c_str(), 0u, ~0ull);
            // Full guest-RAM size: the negative probe below writes at 0x1000000.
            std::vector<uint8_t> ram(PS2_RAM_SIZE, 0u);

            ps2_e41_trace::noteVsync(77u);
            Ps2FastWrite32(ram.data(), 0x0063BBE4u, 0x00435BD0u);
            Ps2FastWrite32(ram.data(), 0x01000000u, 0x00435BD0u);

            ps2_e41_trace::clearForTest();
            const std::string text = e41ReadWholeFile(tmp);
            const std::string expected =
                "plant vsync=77 addr=0x0063bbe4 value=0x00435bd0 via=ee-store src=- seq=-\n";
            t.Equals(text, expected, "Ps2FastWrite32 must feed the plant watch");
            std::remove(tmp.c_str());
        });

        tc.Run("window filters lines but seq stays gapless", [](TestCase &t)
        {
            const std::string tmp = e41TmpPath("ps2x-e41-window.txt");
            std::remove(tmp.c_str());
            ps2_e41_trace::configureForTest(tmp.c_str(), 100u, 200u);
            std::vector<uint8_t> ram(8u * 1024u * 1024u, 0u);

            t.Equals(ps2_e41_trace::noteCdRead(50u, 0x100u, 4u, 0x1000u, "sceCdRead", "-"),
                     1ull, "out-of-window read still takes seq 1");
            t.Equals(ps2_e41_trace::noteCdRead(150u, 0x104u, 4u, 0x2000u, "sceCdRead", "-"),
                     2ull, "in-window read takes seq 2");
            e41SetWord(ram, 0x0063BEA4u, 0x00434990u);
            ps2_e41_trace::notePlantRange(50u, 0x0063BEA4u, 4u, ram.data(),
                                          "sif-dma", "src=0x2000", 0u);

            ps2_e41_trace::clearForTest();
            const std::string text = e41ReadWholeFile(tmp);
            const std::string expected =
                "cdread seq=2 vsync=150 lbn=0x104 sectors=4 mode=sceCdRead dest=0x00002000 file=-\n";
            t.Equals(text, expected, "only the in-window line is emitted");
            std::remove(tmp.c_str());
        });

        tc.Run("names are sanitized onto one line", [](TestCase &t)
        {
            const std::string tmp = e41TmpPath("ps2x-e41-sanitize.txt");
            std::remove(tmp.c_str());
            ps2_e41_trace::configureForTest(tmp.c_str(), 0u, ~0ull);

            ps2_e41_trace::noteCdSearch(10u, "a b=c\nd", 0x300u, 8u);

            ps2_e41_trace::clearForTest();
            const std::string text = e41ReadWholeFile(tmp);
            const std::string expected = "cdsearch vsync=10 name=a_b=c_d lbn=0x300 size=8\n";
            t.Equals(text, expected, "spaces and newlines map to underscores, = is kept");
            std::remove(tmp.c_str());
        });
    }
}

void register_ps2_e41_trace_tests()
{
    MiniTest::Case("Ps2E41Trace", register_ps2_e41_trace_tests_body);
}
