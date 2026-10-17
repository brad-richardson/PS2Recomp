#include "MiniTest.h"
#include "ps2_netsync.h"

#include <cstdint>
#include <cstring>
#include <string>
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
        tc.Run("quick play join/start/go bytes match the lobby codec", [](TestCase &t)
        {
            uint8_t client[16];
            for (int i = 0; i < 16; ++i)
                client[i] = (uint8_t)(0xa0 + i);
            uint8_t join[20];
            t.Equals(ps2_netsync::encodeJoin(join, client, 0u), (size_t)20u, "join length");
            t.Equals(ps2_netsync::getU16(join), (uint16_t)1u, "proto");
            t.IsTrue(std::memcmp(join + 2, client, 16) == 0, "client id");
            t.Equals(join[18], (uint8_t)120u, "rate");
            t.Equals(join[19], (uint8_t)0u, "course");
            // QpStart { start_id 0xdeadbeef, slot 1, players 2, course 0, seq 2, go_in_ms 1500 }
            const uint8_t start[10] = {0xef, 0xbe, 0xad, 0xde, 1, 2, 0, 2, 0xdc, 0x05};
            ps2_netsync::QpStart st;
            t.IsTrue(ps2_netsync::decodeStart(start, sizeof(start), st), "start decodes");
            t.Equals(st.startId, (uint32_t)0xdeadbeefu, "start id");
            t.Equals(st.slot, (uint8_t)1u, "slot");
            t.Equals(st.seq, (uint8_t)2u, "seq");
            t.Equals(st.goInMs, (uint16_t)1500u, "lead");
            t.IsFalse(ps2_netsync::decodeStart(start, 9u, st), "short start refuses");
            uint8_t bad[10];
            std::memcpy(bad, start, sizeof(bad));
            bad[4] = 2u;
            t.IsFalse(ps2_netsync::decodeStart(bad, sizeof(bad), st), "slot 2 refuses (two seats)");
            uint8_t go[21];
            t.Equals(ps2_netsync::encodeGo(go, 9u, 0u, 42000u, 1480000u, 123456u), (size_t)21u, "go length");
            t.Equals(ps2_netsync::getU32(go + 5), (uint32_t)42000u, "rtt");
            t.Equals(ps2_netsync::getU64(go + 13), (uint64_t)123456u, "go tick");
            uint8_t frame[96];
            uint8_t room[16];
            std::memset(room, 0x5a, sizeof(room));
            const size_t fl = ps2_netsync::buildFrame(frame, sizeof(frame), ps2_netsync::kKindQpGo, 1u, 7u, go,
                                                      sizeof(go), room);
            t.Equals(ps2_netsync::checkFrame(frame, fl), (long)sizeof(go), "frame checks");
            t.IsTrue(std::memcmp(frame + 12, room, 16) == 0, "session carries the room");
            frame[38] = 1u;
            t.Equals(ps2_netsync::checkFrame(frame, fl), -1L, "nonzero reserved refuses");
        });
        tc.Run("show sample interpolates, dead-reckons and caps", [](TestCase &t)
        {
            std::vector<ps2_netsync::Sample> ring(2);
            ring[0].tick = 100u;
            ring[0].pos = {0.0f, 0.0f, 0.0f};
            ring[0].vel = {120.0f, 0.0f, 0.0f};
            ring[1].tick = 106u;
            ring[1].pos = {6.0f, 0.0f, 12.0f};
            ring[1].vel = {120.0f, 0.0f, 0.0f};
            ps2_netsync::Sample out;
            t.IsFalse(ps2_netsync::showSample(ring, 99u, out), "nothing before the first sample");
            t.IsTrue(ps2_netsync::showSample(ring, 103u, out), "inside");
            t.Equals(out.pos.x, 3.0f, "lerp x");
            t.Equals(out.pos.z, 6.0f, "lerp z");
            t.IsTrue(ps2_netsync::showSample(ring, 112u, out), "past the newest");
            t.Equals(out.pos.x, 12.0f, "dead-reckon 6 ticks at 120 u/s");
            t.IsTrue(ps2_netsync::showSample(ring, 1000u, out), "far past");
            t.Equals(out.pos.x, 36.0f, "capped at 30 ticks");
        });
        tc.Run("result notice names you and the rival in canonical order", [](TestCase &t)
        {
            std::vector<uint8_t> p(146u + 50u, 0u);
            p[145] = 2u;
            auto row = [&](size_t i, uint16_t slot, uint8_t disp, uint8_t rank, uint32_t ms) {
                uint8_t *r = p.data() + 146u + 25u * i;
                ps2_netsync::putU16(r, slot);
                r[3] = disp;
                r[4] = rank;
                ps2_netsync::putU32(r + 13, ms);
            };
            row(0, 1u, 0u, 1u, 202099u);
            row(1, 0u, 0u, 2u, 253916u);
            const std::string text = ps2_netsync::resultText(p.data(), p.size(), 0);
            t.Equals(text, std::string("Quick Play result\n1st  Rival  3:22.09\n2nd  You    4:13.91"), "text");
            row(1, 0u, 1u, 0u, 0xffffffffu);
            t.Equals(ps2_netsync::resultText(p.data(), p.size(), 1),
                     std::string("Quick Play result\n1st  You    3:22.09\n--   Rival  DNF"), "dnf");
            t.Equals(ps2_netsync::resultText(p.data(), p.size() - 1u, 0), std::string(), "malformed refuses");
        });
        tc.Run("server spec parses host and port", [](TestCase &t)
        {
            std::string host;
            uint16_t port = 0u;
            t.IsTrue(ps2_netsync::parseServer("ssx.bradr.dev", host, port), "host only");
            t.Equals(host, std::string("ssx.bradr.dev"), "host");
            t.Equals(port, (uint16_t)3757u, "default port");
            t.IsTrue(ps2_netsync::parseServer("127.0.0.1:41000", host, port), "host:port");
            t.Equals(port, (uint16_t)41000u, "port");
            t.IsFalse(ps2_netsync::parseServer("", host, port), "empty refuses");
            t.IsFalse(ps2_netsync::parseServer("h:0", host, port), "port 0 refuses");
        });
    }
} // namespace

void register_ps2_netsync_tests()
{
    MiniTest::Case("ZzNetSync", register_ps2_netsync_tests_body);
}
