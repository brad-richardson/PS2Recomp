#include "MiniTest.h"
#include "ps2_netsync.h"
#include "ps2_net_stock120.h"

#include <cstdint>
#include <cstring>
#include <limits>
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
            // netsec-fix-1 F8: hostile numerics with a valid digest refuse.
            auto retampered = [&](const uint8_t *base, size_t n) {
                std::memcpy(bad, base, n);
                ps2_netsync::putU64(bad + 103, ps2_netsync::fnv64(bad, 103u));
            };
            std::memcpy(bad, pay, sizeof(pay));
            bad[10] = 0xFFu;
            bad[11] = 0xFFu;
            bad[12] = 0xFFu;
            bad[13] = 0x7Fu; // pos.x = NaN
            retampered(bad, sizeof(bad));
            t.IsFalse(ps2_netsync::decodeState(bad, sizeof(bad), back, de4, w0, w1, inTick, slot),
                      "NaN position refuses");
            std::memcpy(bad, pay, sizeof(pay));
            bad[42] = 0x00u;
            bad[43] = 0x00u;
            bad[44] = 0x80u;
            bad[45] = 0x7Fu; // vel.y = +inf
            retampered(bad, sizeof(bad));
            t.IsFalse(ps2_netsync::decodeState(bad, sizeof(bad), back, de4, w0, w1, inTick, slot),
                      "inf velocity refuses");
            std::memcpy(bad, pay, sizeof(pay));
            std::memset(bad + 22, 0, 16u); // zero quaternion
            retampered(bad, sizeof(bad));
            t.IsFalse(ps2_netsync::decodeState(bad, sizeof(bad), back, de4, w0, w1, inTick, slot),
                      "degenerate quaternion refuses");
            std::memcpy(bad, pay, sizeof(pay));
            bad[67] = 0x00u;
            bad[68] = 0x00u;
            bad[69] = 0x80u;
            bad[70] = 0xBFu; // ftime = -1.0
            retampered(bad, sizeof(bad));
            t.IsFalse(ps2_netsync::decodeState(bad, sizeof(bad), back, de4, w0, w1, inTick, slot),
                      "negative ftime refuses");
            std::memcpy(bad, pay, sizeof(pay));
            ps2_netsync::putF32(bad + 67, 1.0e10f); // ftime overflows the ms conversion
            retampered(bad, sizeof(bad));
            t.IsFalse(ps2_netsync::decodeState(bad, sizeof(bad), back, de4, w0, w1, inTick, slot),
                      "huge ftime refuses");
            std::memcpy(bad, pay, sizeof(pay));
            bad[66] = 2u; // fin outside {0, 1}
            retampered(bad, sizeof(bad));
            t.IsFalse(ps2_netsync::decodeState(bad, sizeof(bad), back, de4, w0, w1, inTick, slot),
                      "wild fin refuses");
            std::memcpy(bad, pay, sizeof(pay));
            ps2_netsync::putF32(bad + 10, 5.0e6f); // pos.x past the bound
            retampered(bad, sizeof(bad));
            t.IsFalse(ps2_netsync::decodeState(bad, sizeof(bad), back, de4, w0, w1, inTick, slot),
                      "wild position refuses");
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
        tc.Run("show point follows the remote clock, not the local tick", [](TestCase &t)
        {
            std::vector<ps2_netsync::Sample> ring(3);
            // Remote at 120/s: ticks 600/606/612 arrive 30 ms after they ran
            // (tick T ran at T/120 s; arrival jitter only adds delay).
            const uint64_t base = 1000000000u;
            const uint64_t ticks[3] = {600u, 606u, 612u};
            const uint64_t extra[3] = {10000u, 0u, 25000u};
            for (int i = 0; i < 3; ++i)
            {
                ring[i].tick = ticks[i];
                ring[i].rxUs = base + ticks[i] * 1000000u / 120u + 30000u + extra[i];
            }
            uint64_t show = 0u;
            const uint64_t now = base + 612u * 1000000u / 120u + 30000u + 50000u; // 50 ms after 612 would arrive
            t.IsTrue(ps2_netsync::showTickFor(ring, now, 12u, show), "estimates");
            t.Equals(show, (uint64_t)606u, "remote tick at now-30ms (618) minus idelay 12");
            t.IsFalse(ps2_netsync::showTickFor({}, now, 12u, show), "empty ring");
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

        tc.Run("online-stock-2: stock120 state payload round-trips", [](TestCase &t)
        {
            ps2_netsync::Sample a;
            a.pos = {1.5f, -2.25f, 300.0f};
            a.qx = 0.1f; a.qy = 0.2f; a.qz = 0.3f; a.qw = 0.9f;
            a.vel = {-4.0f, 5.0f, 6.5f};
            a.fin = 1u;
            a.ftime = 123.25f;
            uint8_t p[ps2_net_stock120::kPayloadLen];
            ps2_net_stock120::encode(p, 9u, 4321u, a);
            ps2_netsync::Sample b;
            uint8_t epoch = 0u;
            t.IsTrue(ps2_net_stock120::decode(p, sizeof(p), b, epoch), "decodes");
            t.Equals(epoch, (uint8_t)9u, "epoch");
            t.Equals(b.tick, (uint64_t)4321u, "tick");
            t.IsTrue(b.pos.z == 300.0f && b.vel.x == -4.0f && b.qw == 0.9f && b.fin == 1u && b.ftime == 123.25f,
                     "fields");
            t.IsFalse(ps2_net_stock120::decode(p, sizeof(p) - 1u, b, epoch), "short payload refused");
            p[0] = 9u;
            t.IsFalse(ps2_net_stock120::decode(p, sizeof(p), b, epoch), "wrong version refused");
        });

        tc.Run("online-stock-2: R2 every STATE field is validated on decode and after interpolation",
               [](TestCase &t)
        {
            ps2_netsync::Sample good;
            good.pos = {1000.0f, -2000.0f, 3000.0f};
            good.qw = 1.0f;
            good.vel = {100.0f, 0.0f, -50.0f};
            auto roundTrip = [&](const ps2_netsync::Sample &in) {
                uint8_t p[ps2_net_stock120::kPayloadLen];
                ps2_net_stock120::encode(p, 1u, 7u, in);
                ps2_netsync::Sample out;
                uint8_t epoch = 0u;
                return ps2_net_stock120::decode(p, sizeof(p), out, epoch);
            };
            t.IsTrue(roundTrip(good), "valid sample accepted");
            ps2_netsync::Sample bad = good;
            bad.vel.x = std::numeric_limits<float>::quiet_NaN();
            t.IsFalse(roundTrip(bad), "NaN velocity refused");
            bad = good;
            bad.qx = std::numeric_limits<float>::infinity();
            t.IsFalse(roundTrip(bad), "infinite qx refused");
            bad = good;
            bad.pos.x = std::numeric_limits<float>::max();
            t.IsFalse(roundTrip(bad), "FLT_MAX position refused");
            bad = good;
            bad.vel.y = 1.0e6f;
            t.IsFalse(roundTrip(bad), "absurd velocity refused");
            bad = good;
            bad.qw = 3.0f;
            t.IsFalse(roundTrip(bad), "non-unit quaternion refused");
            bad = good;
            bad.ftime = std::numeric_limits<float>::quiet_NaN();
            t.IsFalse(roundTrip(bad), "NaN finish time refused");
            // After interpolation: a show sample that drifted out of bounds
            // (e.g. dead reckoning from a huge but finite velocity) is refused.
            ps2_netsync::Sample show = good;
            show.pos.z = 5.0e6f;
            t.IsFalse(ps2_net_stock120::saneSample(show), "out-of-course show sample refused");
            t.IsTrue(ps2_net_stock120::saneSample(good), "in-course show sample written");
        });

        tc.Run("online-stock-2: R1 the owner's finish ends the replica's race", [](TestCase &t)
        {
            std::vector<uint8_t> ram(ps2_netsync::kRamSize);
            constexpr uint32_t replica = 0x100000u;
            ps2_netsync::writeF32(ram.data(), replica + ps2_net_stock120::kFinishOff, -1.0f);
            t.IsTrue(ps2_net_stock120::applyOwnerFinish(ram.data(), replica, 191.25f), "unfinished replica gets the owner's finished state");
            float v = 0.0f;
            ps2_netsync::readF32(ram.data(), replica + ps2_net_stock120::kFinishOff, v);
            t.IsTrue(v == 191.25f, "+0x470 holds the owner's value");
            t.IsFalse(ps2_net_stock120::applyOwnerFinish(ram.data(), replica, 200.0f), "a local crossing is never overwritten");
            ps2_netsync::writeF32(ram.data(), replica + ps2_net_stock120::kFinishOff, -1.0f);
            t.IsFalse(ps2_net_stock120::applyOwnerFinish(ram.data(), replica, -1.0f), "no finish without an owner time");
            t.IsFalse(ps2_net_stock120::applyOwnerFinish(ram.data(), replica, std::numeric_limits<float>::quiet_NaN()),
                      "a NaN time is refused");
        });

        tc.Run("online-stock-2: R3 STATE liveness counts from the race start", [](TestCase &t)
        {
            using ps2_net_stock120::stateStale;
            const uint64_t timeout = 3000000u, start = 10000000u;
            t.IsFalse(stateStale(start + 2000000u, start, 0u, timeout), "no STATE yet, inside the grace");
            t.IsTrue(stateStale(start + 3500000u, start, 0u, timeout), "no STATE at all since the race start");
            t.IsFalse(stateStale(start + 9000000u, start, start + 8000000u, timeout), "fresh STATE");
            t.IsTrue(stateStale(start + 12000000u, start, start + 8000000u, timeout), "STATE stopped for 4 s");
            t.IsFalse(stateStale(start, start, start + 100u, timeout), "clock never runs backwards");
        });

        tc.Run("online-stock-2: fresh STATE holds the stock peer-silence count", [](TestCase &t)
        {
            using ps2_net_stock120::holdPeerSilence;
            std::vector<uint8_t> ram(PS2_RAM_SIZE);
            constexpr uint32_t ng = 0x2000u;
            const uint64_t timeout = 3000000u, start = 10000000u;
            ps2_fh1::wr32(ram.data(), ng + 0x5cu, 900u);
            t.IsFalse(holdPeerSilence(ram.data(), start + 1000000u, start, start + 500000u, timeout),
                      "no NetGame: untouched");
            ps2_fh1::wr32(ram.data(), ps2_fh1::kNetGamePtr, ng);
            t.IsFalse(holdPeerSilence(ram.data(), start + 1000000u, start, 0u, timeout),
                      "no STATE since the race start: stock count runs");
            t.IsFalse(holdPeerSilence(ram.data(), start + 9000000u, start, start + 5000000u, timeout),
                      "stale STATE: stock count runs");
            uint32_t n = 0u;
            ps2_fh1::rd32(ram.data(), ng + 0x5cu, n);
            t.Equals(n, 900u, "count untouched while STATE is missing");
            t.IsTrue(holdPeerSilence(ram.data(), start + 6000000u, start, start + 5000000u, timeout),
                     "fresh STATE clears the count");
            ps2_fh1::rd32(ram.data(), ng + 0x5cu, n);
            t.Equals(n, 0u, "count cleared");
            t.IsFalse(holdPeerSilence(ram.data(), start + 6000000u, start, start + 5000000u, timeout),
                      "already zero: no write");
        });

        tc.Run("online-stock-2: R4 STATE ring is tick-ordered within a race epoch", [](TestCase &t)
        {
            using ps2_net_stock120::ringInsert;
            std::vector<ps2_netsync::Sample> ring;
            ps2_net_stock120::RingEpoch e;
            auto at = [](uint64_t tick) {
                ps2_netsync::Sample s;
                s.tick = tick;
                return s;
            };
            t.IsTrue(ringInsert(ring, e, 1u, at(120)), "first sample");
            t.IsTrue(ringInsert(ring, e, 1u, at(126)), "next sample");
            t.IsTrue(ringInsert(ring, e, 1u, at(114)), "reordered older tick inserted");
            t.Equals(ring.size(), (size_t)3u, "history kept");
            t.Equals(ring.front().tick, (uint64_t)114u, "ordered front");
            t.Equals(ring.back().tick, (uint64_t)126u, "newest stays the show anchor");
            t.IsFalse(ringInsert(ring, e, 1u, at(120)), "duplicate dropped");
            t.IsFalse(ringInsert(ring, e, 0u, at(500)), "older epoch dropped");
            t.IsTrue(ringInsert(ring, e, 2u, at(6)), "newer epoch restarts the ring");
            t.Equals(ring.size(), (size_t)1u, "old race history cleared");
            e.needNewer = true; // local race restarted
            t.IsFalse(ringInsert(ring, e, 2u, at(12)), "the old race's samples are stale after a local restart");
            t.IsTrue(ringInsert(ring, e, 3u, at(6)), "the peer's next race is accepted");
            ps2_net_stock120::RingEpoch w;
            w.have = true;
            w.epoch = 255u;
            t.IsTrue(ringInsert(ring, w, 0u, at(6)), "epoch wraps forward (255 -> 0)");
        });

        tc.Run("online-stock-2: stock120 show tick uses the peer's observed rate", [](TestCase &t)
        {
            std::vector<ps2_netsync::Sample> ring;
            for (int i = 0; i <= 10; ++i)
            {
                ps2_netsync::Sample s;
                s.tick = (uint64_t)i * 6u;
                s.rxUs = 1000000u + (uint64_t)i * 200000u; // 6 ticks per 0.2 s: 30 ticks/s
                ring.push_back(s);
            }
            uint64_t out = 0u;
            t.IsTrue(ps2_net_stock120::showTick(ring, 3000000u + 1000000u, 12u, out), "has a show tick");
            t.Equals(out, (uint64_t)(60u + 30u - 12u), "newest 60 + 1 s at 30/s - idelay 12");
            std::vector<ps2_netsync::Sample> one(ring.begin(), ring.begin() + 1);
            t.IsTrue(ps2_net_stock120::showTick(one, 1000000u + 100000u, 0u, out), "single sample");
            t.Equals(out, (uint64_t)12u, "falls back to 120/s");
            t.IsFalse(ps2_net_stock120::showTick({}, 0u, 0u, out), "empty ring");
        });
    }
} // namespace

void register_ps2_netsync_tests()
{
    MiniTest::Case("ZzNetSync", register_ps2_netsync_tests_body);
}
