#include "MiniTest.h"
#include "ps2_snd_spike.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace
{
    void putU32(std::vector<uint8_t> &data, size_t offset, uint32_t value)
    {
        std::memcpy(data.data() + offset, &value, sizeof(value));
    }
}

void register_ps2_snd_tests()
{
    MiniTest::Case("Ps2SoundHle", [](TestCase &tc)
    {
        tc.Run("SND tick cadence follows guest cycles at 93.75 Hz", [](TestCase &t)
        {
        using namespace ps2_snd_spike;
        t.Equals(kTickCycles, 3145728ull, "one tick is exactly 3,145,728 EE cycles");
        t.Equals(ticksForGuestCycles(10u * kTickCycles), 10ull, "ten periods yield ten ticks");
        t.Equals(ticksForGuestCycles(10u * kTickCycles + kTickCycles - 1u), 10ull,
                 "one cycle before the next deadline does not yield an extra tick");
        t.Equals(ticksForGuestCycles(10u * kTickCycles + kTickCycles), 11ull,
                 "the next period boundary yields tick eleven");
        });

        tc.Run("tag buffer parser extracts exactly 384 stereo s16 frames", [](TestCase &t)
        {
        using namespace ps2_snd_spike;
        std::vector<uint8_t> data(16u + 8u + kPcmBytesPerTick + 16u, 0u);
        putU32(data, 0u, 0u); // tag 0: 16-byte fixed record
        putU32(data, 16u, 1u);
        putU32(data, 20u, kPcmBytesPerTick);
        for (size_t i = 0; i < kPcmBytesPerTick; ++i)
            data[24u + i] = static_cast<uint8_t>(i & 0xffu);
        putU32(data, 24u + kPcmBytesPerTick, 6u);
        Tag1PcmView view{};
        t.IsTrue(findTag1Pcm(data.data(), data.size(), view), "tag 1 should parse");
        t.Equals(view.offset, static_cast<size_t>(24u), "PCM begins immediately after tag and byte length");
        t.Equals(view.size, static_cast<size_t>(kPcmBytesPerTick), "PCM is 1,536 bytes");
        t.Equals(data[view.offset + 511u], static_cast<uint8_t>(0xffu), "PCM payload remains byte exact");
        t.IsFalse(findTag1Pcm(data.data(), 23u, view), "truncated tag header must be rejected");
        });

        tc.Run("_sceSifSendCmd decodes the seven argument registers unconditionally", [](TestCase &t)
        {
        using namespace ps2_snd_spike;
        std::array<uint32_t, 11> gpr{};
        gpr[4] = 0x11u; // cid
        gpr[5] = 0x22u; // mode, ignored by this runtime
        gpr[6] = 0x33u; // packet
        gpr[7] = 0x44u; // packet size
        gpr[8] = 0x55u; // source extra
        gpr[9] = 0x66u; // destination extra
        gpr[10] = 0x77u; // extra size
        const SendCmdArgs args = decodeSendCmdArgs(gpr.data(), gpr.size());
        t.Equals(args.cid, 0x11u, "a0 supplies cid");
        t.Equals(args.packet, 0x33u, "a2 supplies packet");
        t.Equals(args.packetSize, 0x44u, "a3 supplies packet size");
        t.Equals(args.srcExtra, 0x55u, "a4 supplies extra source");
        t.Equals(args.dstExtra, 0x66u, "a5 supplies extra destination");
        t.Equals(args.extraSize, 0x77u, "a6 supplies extra size");
        const SendCmdArgs shortArgs = decodeSendCmdArgs(gpr.data(), 10u);
        t.Equals(shortArgs.packet, 0u, "incomplete register frame is rejected");
        });
    });
}
