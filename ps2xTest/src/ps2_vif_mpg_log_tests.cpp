#include "MiniTest.h"
#include "ps2_vif_mpg_log.h"
#include "runtime/ps2_memory.h"

#include <atomic>
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
    std::string mpgTmpPath(const char *name)
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

    uint32_t makeVifMpg(uint8_t num, uint16_t imm)
    {
        return (static_cast<uint32_t>(0x4Au) << 24) |
               (static_cast<uint32_t>(num) << 16) |
               static_cast<uint32_t>(imm);
    }

    void appendU32(std::vector<uint8_t> &dst, uint32_t value)
    {
        const size_t pos = dst.size();
        dst.resize(pos + sizeof(uint32_t));
        std::memcpy(dst.data() + pos, &value, sizeof(uint32_t));
    }

    uint32_t localFnv1a32(const uint8_t *data, uint32_t size)
    {
        uint32_t hash = 0x811C9DC5u;
        for (uint32_t i = 0u; i < size; ++i)
        {
            hash ^= data[i];
            hash *= 0x01000193u;
        }
        return hash;
    }

    bool codeIsFill(const uint8_t *code, uint8_t fill)
    {
        for (uint32_t i = 0u; i < PS2_VU1_CODE_SIZE; ++i)
        {
            if (code[i] != fill)
                return false;
        }
        return true;
    }
}

void register_ps2_vif_mpg_log_tests()
{
    MiniTest::Case("Ps2VifMpgLog", [](TestCase &tc)
    {
        tc.Run("log is off by default", [](TestCase &t)
        {
            ps2_vif_mpg_log::clearForTest();
            t.IsTrue(!ps2_vif_mpg_log::enabled(), "log must be disabled with no file configured");
            ps2_vif_mpg_log::note(0u, 0u, 1u, 0u, 8u, "copied", 8u, 0u,
                                  false, 0u, 0u, false, false, 0u, 0u, false);
            t.IsTrue(!ps2_vif_mpg_log::enabled(), "disabled taps must not arm the module");
        });

        tc.Run("fnv helper matches the FNV-1a vectors", [](TestCase &t)
        {
            t.Equals(ps2_vif_mpg_log::fnv1a32(nullptr, 0u), 0x811C9DC5u, "empty input is the offset basis");
            const uint8_t a[] = {0x61u};
            t.Equals(ps2_vif_mpg_log::fnv1a32(a, 1u), 0xE40C292Cu, "single 'a' must match the standard vector");
        });

        tc.Run("slot offset wraps per instruction", [](TestCase &t)
        {
            t.Equals(ps2_vif_mpg_log::slotPayloadOffset(0u, 128u, 2u), 16u, "slot 2 of an imm=0 upload sits at payload offset 16");
            t.Equals(ps2_vif_mpg_log::slotPayloadOffset(0u, 128u, 8u), 64u, "slot 8 of an imm=0 upload sits at payload offset 64");
            t.Equals(ps2_vif_mpg_log::slotPayloadOffset(0u, 16u, 2u), ps2_vif_mpg_log::kNoOffset, "slot 2 is outside a 2-instruction upload");
            t.Equals(ps2_vif_mpg_log::slotPayloadOffset(16376u, 16u, 2047u), 0u, "wrapped upload starts at slot 2047");
            t.Equals(ps2_vif_mpg_log::slotPayloadOffset(16376u, 16u, 0u), 8u, "wrapped upload continues at slot 0");
            t.Equals(ps2_vif_mpg_log::slotPayloadOffset(16376u, 16u, 1u), ps2_vif_mpg_log::kNoOffset, "wrapped upload does not reach slot 1");
        });

        tc.Run("copied MPG logs words and lands in code", [](TestCase &t)
        {
            const std::string tmp = mpgTmpPath("ps2x-vif-mpg-copied.txt");
            std::remove(tmp.c_str());
            t.IsTrue(ps2_vif_mpg_log::configureForTest(tmp.c_str()), "test config should install");

            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            std::memset(mem.getVU1Code(), 0xA5, PS2_VU1_CODE_SIZE);

            // num=16 at imm=0: 128 payload bytes covering slots 2 (off 16)
            // and 8 (off 64). Slot 2 carries the PCSX2 B words.
            std::vector<uint8_t> packet;
            appendU32(packet, makeVifMpg(16u, 0u));
            for (uint32_t i = 0u; i < 128u; ++i)
                packet.push_back(static_cast<uint8_t>(i & 0xFFu));
            const uint32_t s2lo = 0x40000048u, s2hi = 0x000002FFu;
            const uint32_t s8lo = 0x400000F2u, s8hi = 0x000002FFu;
            std::memcpy(packet.data() + 4u + 16u, &s2lo, 4u);
            std::memcpy(packet.data() + 4u + 20u, &s2hi, 4u);
            std::memcpy(packet.data() + 4u + 64u, &s8lo, 4u);
            std::memcpy(packet.data() + 4u + 68u, &s8hi, 4u);
            mem.processVIF1Data(packet.data(), static_cast<uint32_t>(packet.size()));

            uint32_t gotLo = 0u, gotHi = 0u;
            std::memcpy(&gotLo, mem.getVU1Code() + 16u, 4u);
            std::memcpy(&gotHi, mem.getVU1Code() + 20u, 4u);
            t.Equals(gotLo, s2lo, "copied MPG must land slot 2 lower word in code");
            t.Equals(gotHi, s2hi, "copied MPG must land slot 2 upper word in code");

            ps2_vif_mpg_log::clearForTest();
            const std::string text = readWholeFile(tmp);
            const uint32_t fnv = localFnv1a32(packet.data() + 4u, 128u);
            char expected[256];
            std::snprintf(expected, sizeof(expected),
                          "mpg vsync=0 imm=0 num=16 dest=0-128 outcome=copied avail=128 fnv=%08x slot2=40000048 000002ff slot8=400000f2 000002ff\n",
                          fnv);
            t.Equals(text, std::string(expected), "copied line must carry outcome, fnv and slot words");
            std::remove(tmp.c_str());
        });

        tc.Run("out-of-range imm drops and logs would-write words", [](TestCase &t)
        {
            const std::string tmp = mpgTmpPath("ps2x-vif-mpg-dropaddr.txt");
            std::remove(tmp.c_str());
            t.IsTrue(ps2_vif_mpg_log::configureForTest(tmp.c_str()), "test config should install");

            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            std::memset(mem.getVU1Code(), 0xA5, PS2_VU1_CODE_SIZE);

            // imm=2048: linear dest 16384 is out of range (PCSX2 wraps to 0).
            std::vector<uint8_t> packet;
            appendU32(packet, makeVifMpg(16u, 2048u));
            for (uint32_t i = 0u; i < 128u; ++i)
                packet.push_back(static_cast<uint8_t>((i * 5u) & 0xFFu));
            const uint32_t s2lo = 0x40000048u, s2hi = 0x000002FFu;
            std::memcpy(packet.data() + 4u + 16u, &s2lo, 4u);
            std::memcpy(packet.data() + 4u + 20u, &s2hi, 4u);
            mem.processVIF1Data(packet.data(), static_cast<uint32_t>(packet.size()));

            t.IsTrue(codeIsFill(mem.getVU1Code(), 0xA5), "drop_addr MPG must not touch code memory");

            ps2_vif_mpg_log::clearForTest();
            const std::string text = readWholeFile(tmp);
            t.IsTrue(text.find("imm=2048") != std::string::npos, "log must print the full imm, not imm&0x1FF");
            t.IsTrue(text.find("outcome=drop_addr") != std::string::npos, "out-of-range imm must log drop_addr");
            t.IsTrue(text.find("slot2=40000048 000002ff") != std::string::npos, "drop_addr line must show the would-write slot 2 words under the masked mapping");
            std::remove(tmp.c_str());
        });

        tc.Run("truncated payload drops and marks the missing slot short", [](TestCase &t)
        {
            const std::string tmp = mpgTmpPath("ps2x-vif-mpg-droppartial.txt");
            std::remove(tmp.c_str());
            t.IsTrue(ps2_vif_mpg_log::configureForTest(tmp.c_str()), "test config should install");

            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            std::memset(mem.getVU1Code(), 0xA5, PS2_VU1_CODE_SIZE);

            // num=16 needs 128 payload bytes; offer only 64.
            std::vector<uint8_t> packet;
            appendU32(packet, makeVifMpg(16u, 0u));
            for (uint32_t i = 0u; i < 64u; ++i)
                packet.push_back(static_cast<uint8_t>(i & 0xFFu));
            mem.processVIF1Data(packet.data(), static_cast<uint32_t>(packet.size()));

            t.IsTrue(codeIsFill(mem.getVU1Code(), 0xA5), "drop_partial MPG must not touch code memory");

            ps2_vif_mpg_log::clearForTest();
            const std::string text = readWholeFile(tmp);
            t.IsTrue(text.find("outcome=drop_partial") != std::string::npos, "short buffer must log drop_partial");
            t.IsTrue(text.find("avail=64") != std::string::npos, "line must report the bytes available");
            t.IsTrue(text.find("slot8=short") != std::string::npos, "slot 8 (offset 64) must read short when only 64 bytes arrived");
            std::remove(tmp.c_str());
        });

        tc.Run("overflowing upload clips and logs clipped", [](TestCase &t)
        {
            const std::string tmp = mpgTmpPath("ps2x-vif-mpg-clipped.txt");
            std::remove(tmp.c_str());
            t.IsTrue(ps2_vif_mpg_log::configureForTest(tmp.c_str()), "test config should install");

            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");
            std::memset(mem.getVU1Code(), 0xA5, PS2_VU1_CODE_SIZE);

            // imm=2047 num=2: dest 16376 + 16 bytes overflows 16K; the
            // interpreter clips to the final 8 bytes instead of wrapping.
            std::vector<uint8_t> packet;
            appendU32(packet, makeVifMpg(2u, 2047u));
            for (uint32_t i = 0u; i < 16u; ++i)
                packet.push_back(static_cast<uint8_t>(0xC0u + i));
            mem.processVIF1Data(packet.data(), static_cast<uint32_t>(packet.size()));

            const uint8_t *code = mem.getVU1Code();
            bool headOk = true;
            for (uint32_t i = 0u; i < 8u; ++i)
            {
                if (code[16376u + i] != static_cast<uint8_t>(0xC0u + i))
                    headOk = false;
            }
            t.IsTrue(headOk, "clipped MPG must land its in-range head");
            t.Equals(static_cast<uint32_t>(code[0]), 0xA5u, "clipped MPG must not wrap to slot 0");

            ps2_vif_mpg_log::clearForTest();
            const std::string text = readWholeFile(tmp);
            t.IsTrue(text.find("outcome=clipped") != std::string::npos, "overflowing upload must log clipped");
            t.IsTrue(text.find("dest=16376-16392") != std::string::npos, "line must show the linear dest range");
            std::remove(tmp.c_str());
        });

        tc.Run("from/to window filters vsyncs", [](TestCase &t)
        {
            const std::string tmp = mpgTmpPath("ps2x-vif-mpg-window.txt");
            std::remove(tmp.c_str());
            t.IsTrue(ps2_vif_mpg_log::configureForTest(tmp.c_str(), 10u, 20u), "test config should install");

            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            std::vector<uint8_t> packet;
            appendU32(packet, makeVifMpg(1u, 0u));
            for (uint32_t i = 0u; i < 8u; ++i)
                packet.push_back(static_cast<uint8_t>(i));
            mem.processVIF1Data(packet.data(), static_cast<uint32_t>(packet.size()));

            mem.gs_regs.vsyncTick.store(15u, std::memory_order_relaxed);
            mem.processVIF1Data(packet.data(), static_cast<uint32_t>(packet.size()));

            ps2_vif_mpg_log::clearForTest();
            const std::string text = readWholeFile(tmp);
            t.IsTrue(text.find("vsync=0") == std::string::npos, "vsync 0 must fall outside the 10..20 window");
            t.IsTrue(text.find("vsync=15") != std::string::npos, "vsync 15 must fall inside the 10..20 window");
            std::remove(tmp.c_str());
        });
    });
}
