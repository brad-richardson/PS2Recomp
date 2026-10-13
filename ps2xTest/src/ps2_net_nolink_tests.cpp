#include "MiniTest.h"
#include "ps2_net_nolink.h"
#include "runtime/ps2_memory.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    constexpr uint32_t kState = 0x51F0C0u;
    constexpr uint32_t kRamSize = 32u * 1024u * 1024u;

    void nolinkSetWord(std::vector<uint8_t> &ram, uint32_t addr, uint32_t value)
    {
        std::memcpy(ram.data() + (addr & PS2_RAM_MASK), &value, sizeof(value));
    }

    uint32_t nolinkGetWord(const std::vector<uint8_t> &ram, uint32_t addr)
    {
        uint32_t value = 0u;
        std::memcpy(&value, ram.data() + (addr & PS2_RAM_MASK), sizeof(value));
        return value;
    }

    void nolinkSetSlot(std::vector<uint8_t> &ram, uint32_t slotBase, uint32_t i,
                       uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3,
                       uint32_t w4, uint32_t w5, uint32_t w6, uint32_t w7)
    {
        const uint32_t slot = slotBase + i * 32u;
        nolinkSetWord(ram, slot + 0u, w0);
        nolinkSetWord(ram, slot + 4u, w1);
        nolinkSetWord(ram, slot + 8u, w2);
        nolinkSetWord(ram, slot + 12u, w3);
        nolinkSetWord(ram, slot + 16u, w4);
        nolinkSetWord(ram, slot + 20u, w5);
        nolinkSetWord(ram, slot + 24u, w6);
        nolinkSetWord(ram, slot + 28u, w7);
    }

    // Doorbell send: 16 B cid=1 packet with flags at +12.
    bool nolinkDoorbell(std::vector<uint8_t> &ram, uint32_t flags, uint64_t tick)
    {
        constexpr uint32_t kPacket = 0x10000u;
        nolinkSetWord(ram, kPacket + 0u, 0x10u);
        nolinkSetWord(ram, kPacket + 4u, 0u);
        nolinkSetWord(ram, kPacket + 8u, 0u);
        nolinkSetWord(ram, kPacket + 12u, flags);
        return ps2_nolink::onSendCmd(ram.data(), nullptr, nullptr, 1u, kPacket, 16u,
                                     0u, 0u, 0u, tick);
    }

    void register_ps2_net_nolink_tests_body(TestCase &tc)
    {
        tc.Run("version poll writes the IOP version string", [](TestCase &t)
        {
            std::vector<uint8_t> ram(kRamSize, 0u);
            constexpr uint32_t kPacket = 0x10000u;
            constexpr uint32_t kEeAddr = 0x20000u;
            nolinkSetWord(ram, kPacket + 0u, 0x10u);
            nolinkSetWord(ram, kPacket + 4u, 0u);
            nolinkSetWord(ram, kPacket + 8u, 0u);
            nolinkSetWord(ram, kPacket + 12u, kEeAddr | 1u);
            const bool replied = ps2_nolink::onSendCmd(ram.data(), nullptr, nullptr, 1u,
                                                       kPacket, 16u, 0u, 0u, 0u, 1234u);
            t.IsTrue(replied, "poll must reply");
            t.IsTrue(std::memcmp(ram.data() + kEeAddr, "RPC IOP Ver 1.0 ", 16u) == 0,
                     "poll must DMA the IOP version bytes");
        });

        tc.Run("non-DirtySock sends are untouched", [](TestCase &t)
        {
            std::vector<uint8_t> ram(kRamSize, 0u);
            constexpr uint32_t kPacket = 0x10000u;
            nolinkSetWord(ram, kPacket + 12u, 0xFFFFFFFFu);
            t.IsFalse(ps2_nolink::onSendCmd(ram.data(), nullptr, nullptr, 2u, kPacket, 16u,
                                            0u, 0u, 0u, 1u),
                      "other cids must not reply");
            t.IsFalse(ps2_nolink::onSendCmd(ram.data(), nullptr, nullptr, 1u, kPacket, 8u,
                                            0u, 0u, 0u, 1u),
                      "short packets must not reply");
            t.Equals(nolinkGetWord(ram, kState + 0x4D94u), 0u, "no state may be touched");
        });

        tc.Run("doorbell answers the member table with echo", [](TestCase &t)
        {
            std::vector<uint8_t> ram(kRamSize, 0u);
            constexpr uint32_t kSlots = 0x30000u;
            constexpr uint32_t kReply = 0x31000u;
            constexpr uint32_t kFlags = 0x32000u;
            constexpr uint32_t kTable = 0x33000u;
            nolinkSetWord(ram, kState + 0x4D94u, kSlots);
            nolinkSetWord(ram, kState + 0x4DA0u, kReply);
            nolinkSetWord(ram, kState + 0x4DB8u, kFlags);
            nolinkSetWord(ram, kState + 0x4DD8u, 16u);
            nolinkSetSlot(ram, kSlots, 0u, 0u, 0u, kTable, 47u * 16u, 0u, 0u, 1u, 0xABu);
            t.IsTrue(nolinkDoorbell(ram, 0xFFFFFFFFu, 2u), "table call must reply");
            t.Equals(nolinkGetWord(ram, kTable + 0u), 0u, "entry 0 index");
            t.Equals(nolinkGetWord(ram, kTable + 4u), 0x6e657477u, "entry 0 member netw");
            t.Equals(nolinkGetWord(ram, kTable + 8u), 0x63726561u, "entry 0 sub crea");
            t.Equals(nolinkGetWord(ram, kTable + 12u), 0x100u, "entry 0 cookie");
            t.Equals(nolinkGetWord(ram, kReply + 12u), 47u * 16u, "table outlen");
            t.Equals(nolinkGetWord(ram, kReply + 20u), 47u, "table status = count");
            t.Equals(nolinkGetWord(ram, kReply + 28u), 0xABu, "reply must echo the seq");
            t.Equals(nolinkGetWord(ram, kFlags + 0u), 1u, "completion flag must be set");
        });

        tc.Run("netw/stat reports the link down", [](TestCase &t)
        {
            std::vector<uint8_t> ram(kRamSize, 0u);
            constexpr uint32_t kSlots = 0x30000u;
            constexpr uint32_t kReply = 0x31000u;
            nolinkSetWord(ram, kState + 0x4D94u, kSlots);
            nolinkSetWord(ram, kState + 0x4DA0u, kReply);
            nolinkSetWord(ram, kState + 0x4DD8u, 16u);
            // Slot 1 (slot 0 was consumed by the table test's dedup).
            nolinkSetSlot(ram, kSlots, 1u, 0u, 0u, 0u, 0u, 0u, 0x636f6e6eu, 0x104u, 0xCDu);
            t.IsTrue(nolinkDoorbell(ram, 0xFFFFFFFFu, 3u), "stat call must reply");
            t.Equals(nolinkGetWord(ram, kReply + 32u + 20u), 0u, "conn query must read down");
            t.Equals(nolinkGetWord(ram, kReply + 32u + 28u), 0xCDu, "reply must echo the seq");
            // mach is link-independent hardware identity.
            nolinkSetSlot(ram, kSlots, 3u, 0u, 0u, 0x34000u, 16u, 0u, 0x6d616378u, 0x104u, 0xCEu);
            t.IsTrue(nolinkDoorbell(ram, 0xFFFFFFFFu, 4u), "mach call must reply");
            t.Equals(nolinkGetWord(ram, kReply + 96u + 12u), 6u, "mach outlen");
            const uint8_t want[6] = {0x02u, 0x0Cu, 0x29u, 0x4Fu, 0x4Eu, 0x4Cu};
            t.IsTrue(std::memcmp(ram.data() + 0x34000u, want, sizeof(want)) == 0,
                     "mach must fill the fixed MAC");
        });

        tc.Run("netw/list serves one fake broadband config", [](TestCase &t)
        {
            std::vector<uint8_t> ram(kRamSize, 0u);
            constexpr uint32_t kSlots = 0x30000u;
            constexpr uint32_t kReply = 0x31000u;
            constexpr uint32_t kList = 0x35000u;
            nolinkSetWord(ram, kState + 0x4D94u, kSlots);
            nolinkSetWord(ram, kState + 0x4DA0u, kReply);
            nolinkSetWord(ram, kState + 0x4DD8u, 16u);
            const char src[] = "mc0:";
            std::memcpy(ram.data() + 0x40000u, src, sizeof(src));
            ps2_nolink::noteDma(0x40000u, 0xA4001D00u, static_cast<uint32_t>(sizeof(src)),
                                ram.data());
            // Slot 2 (slots 0/1/3 consumed above; dedup is shared).
            nolinkSetSlot(ram, kSlots, 2u, 0xA4001D00u, 0u, kList, 2560u, 0u, 0u, 0x105u, 0xEFu);
            t.IsTrue(nolinkDoorbell(ram, 0xFFFFFFFFu, 5u), "list call must reply");
            t.Equals(nolinkGetWord(ram, kReply + 64u + 20u), 1u, "list status = 1 config");
            t.Equals(nolinkGetWord(ram, kReply + 64u + 12u), 256u, "list outlen");
            t.IsTrue(std::memcmp(ram.data() + kList + 4u, "Broadband", 9u) == 0,
                     "config name");
            const char path[] = "mc0:Broadband#0";
            t.IsTrue(std::memcmp(ram.data() + kList + 128u, path, sizeof(path)) == 0,
                     "config path");
        });
    }
}

void register_ps2_net_nolink_tests()
{
    MiniTest::Case("Ps2NetNolink", register_ps2_net_nolink_tests_body);
}
