#include "MiniTest.h"
#include "ps2_netsync.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace
{
    void register_ps2_netsync_tests_body(TestCase &tc)
    {
        tc.Run("snp2 frame header encodes strict-decodable bytes", [](TestCase &t)
        {
            uint8_t payload[4] = {1, 2, 3, 4};
            uint8_t frame[256];
            const size_t fl = ps2_netsync::buildFrame(frame, sizeof(frame), 0x14u, 1u, 9u,
                                                      payload, sizeof(payload));
            t.Equals(fl, ps2_netsync::kHeader + sizeof(payload), "frame length");
            t.Equals(ps2_netsync::getU32(frame), 44u + (uint32_t)sizeof(payload), "frame_bytes");
            t.IsTrue(std::memcmp(frame + 4, "SNP2", 4) == 0, "magic");
            t.Equals(ps2_netsync::getU16(frame + 8), (uint16_t)2u, "version");
            t.Equals(frame[10], (uint8_t)0x14u, "kind");
            t.Equals(frame[11], (uint8_t)0u, "flags");
            t.Equals(ps2_netsync::getU16(frame + 36), (uint16_t)1u, "sender");
            t.Equals(ps2_netsync::getU64(frame + 40), (uint64_t)9u, "seq nonzero");
            t.Equals(ps2_netsync::buildFrame(frame, sizeof(frame), 0x14u, 1u, 0u, payload,
                                             sizeof(payload)),
                     (size_t)0u, "zero seq refuses");
        });
        tc.Run("state payload round-trips and tampering is rejected", [](TestCase &t)
        {
            ps2_netsync::Sample s;
            s.tick = 30703u;
            s.pos.x = 8195.5f;
            s.pos.y = -42985.25f;
            s.pos.z = 91071.5f;
            s.qx = -0.2662f;
            s.qy = 0.1706f;
            s.qz = -0.5357f;
            s.qw = 0.7830f;
            s.vel.x = 894.64f;
            s.boost = 0.5f;
            s.uber = 3u;
            s.ctl = 5u;
            s.chk = 0x000038a1u;
            s.dist = 6496.0f;
            s.fin = 1u;
            s.ftime = 241.5f;
            uint8_t pay[ps2_netsync::kStateLen];
            ps2_netsync::encodeState(pay, 1u, 30703u, s, 5u, 0x11223344u, 0x55667788u, 30703u,
                                     30496u, 239500u);
            ps2_netsync::Sample back;
            uint32_t de4 = 99u, w0 = 0u, w1 = 0u;
            uint64_t inTick = 0u;
            uint16_t slot = 99u;
            t.IsTrue(ps2_netsync::decodeState(pay, sizeof(pay), back, de4, w0, w1, inTick, slot),
                     "valid decodes");
            t.Equals(slot, (uint16_t)1u, "slot");
            t.Equals(back.tick, (uint64_t)30703u, "tick");
            t.Equals(de4, (uint32_t)5u, "de4");
            t.Equals(w0, (uint32_t)0x11223344u, "w0");
            t.Equals(back.fin, (uint8_t)1u, "fin");
            uint8_t bad[sizeof(pay)];
            std::memcpy(bad, pay, sizeof(pay));
            bad[0] ^= 0xffu;
            t.IsFalse(ps2_netsync::decodeState(bad, sizeof(bad), back, de4, w0, w1, inTick, slot),
                      "payload tamper refuses");
            std::memcpy(bad, pay, sizeof(pay));
            bad[sizeof(bad) - 1] ^= 0x01u;
            t.IsFalse(ps2_netsync::decodeState(bad, sizeof(bad), back, de4, w0, w1, inTick, slot),
                      "tail tamper refuses");
            t.IsFalse(ps2_netsync::decodeState(pay, sizeof(pay) - 1, back, de4, w0, w1, inTick,
                                               slot),
                      "short refuses");
        });
    }
} // namespace

void register_ps2_netsync_tests()
{
    MiniTest::Case("ZzNetSync", register_ps2_netsync_tests_body);
}
