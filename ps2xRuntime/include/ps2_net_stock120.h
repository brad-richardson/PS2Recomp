#pragma once
// online-stock-2: SSX 3's own online race at 120 with state sync
// (PS2X_NET_STOCK120=1, needs PS2X_SSX3_FULL120=events and the stock
// Online path, PS2X_NET=1). The stock flow pairs, loads and starts both
// races (its own handshake is the start barrier); from the first racing
// tick the gate never waits on the peer (ps2_fh1::stock120GateFeed), the
// checksum verdict is neutralized (ps2_fh1::stock120CkHook), and this file
// keeps the peer's rider where the peer says it is: each side sends its
// own rider's pos/quat/vel on the HLE side channel (ps2_net::stateSend,
// never seen by the guest) and writes the peer's interpolated stream into
// the peer rider every tick (teleports within reason are accepted).
// Roster: own rider = the slot of the input queue that carries the local
// pad (ps2_fh1::g_s120LocalQueue), peer = the other of two (fallback 0/1;
// PS2X_NET_STOCK120_OWN / _GHOST override); the first received sample logs
// its distance to every slot.
#include "ps2_fh1_full120.h"
#include "ps2_net_dirtysock.h"
#include "ps2_netsync.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace ps2_net_stock120
{
using ps2_netsync::F32Trip;
using ps2_netsync::Sample;

constexpr size_t kPayloadLen = 50u; // ver u8 | tick u32 | pos 3f | quat 4f | vel 3f | fin u8 | ftime f32
constexpr uint8_t kVersion = 1u;
constexpr uint64_t kTimeoutUs = 3000000u;

struct Knobs
{
    int own = -1, ghost = -1; // -1 = from the input queues
    uint64_t every = 6u, idelay = 12u;
};

inline const Knobs &knobs()
{
    static const Knobs k = [] {
        Knobs o;
        auto num = [](const char *name, long lo, long hi, long dflt) {
            const char *e = std::getenv(name);
            if (!e || !*e)
                return dflt;
            const long v = std::strtol(e, nullptr, 10);
            return v >= lo && v <= hi ? v : dflt;
        };
        o.own = (int)num("PS2X_NET_STOCK120_OWN", 0, 5, -1);
        o.ghost = (int)num("PS2X_NET_STOCK120_GHOST", 0, 5, -1);
        o.every = (uint64_t)num("PS2X_NET_STOCK120_EVERY", 1, 60, 6);
        o.idelay = (uint64_t)num("PS2X_NET_STOCK120_IDELAY", 0, 120, 12);
        return o;
    }();
    return k;
}

inline uint64_t nowUs()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline void putF(uint8_t *o, float v) { std::memcpy(o, &v, 4); }
inline float getF(const uint8_t *p)
{
    float v;
    std::memcpy(&v, p, 4);
    return v;
}

// Pure (unit-tested): one state payload and its decode.
inline void encode(uint8_t *o, uint32_t tick, const Sample &s)
{
    o[0] = kVersion;
    std::memcpy(o + 1, &tick, 4);
    putF(o + 5, s.pos.x);
    putF(o + 9, s.pos.y);
    putF(o + 13, s.pos.z);
    putF(o + 17, s.qx);
    putF(o + 21, s.qy);
    putF(o + 25, s.qz);
    putF(o + 29, s.qw);
    putF(o + 33, s.vel.x);
    putF(o + 37, s.vel.y);
    putF(o + 41, s.vel.z);
    o[45] = s.fin;
    putF(o + 46, s.ftime);
}

inline bool decode(const uint8_t *p, size_t n, Sample &s)
{
    if (n != kPayloadLen || p[0] != kVersion)
        return false;
    uint32_t t = 0u;
    std::memcpy(&t, p + 1, 4);
    s = Sample{};
    s.tick = t;
    s.pos = {getF(p + 5), getF(p + 9), getF(p + 13)};
    s.qx = getF(p + 17);
    s.qy = getF(p + 21);
    s.qz = getF(p + 25);
    s.qw = getF(p + 29);
    s.vel = {getF(p + 33), getF(p + 37), getF(p + 41)};
    s.fin = p[45];
    s.ftime = getF(p + 46);
    return std::isfinite(s.pos.x) && std::isfinite(s.pos.y) && std::isfinite(s.pos.z) && std::isfinite(s.qw);
}

// Pure (unit-tested): the peer tick to show at `now`: the newest sample's
// tick plus the time since it arrived at the peer's observed rate (ticks/s
// from the ring's span, 120 until 0.25 s of samples exist, clamped
// 20..240), minus idelay. Unlike netsync's fixed 120, a peer that runs
// slower (a Mac det runner, a hitching device) is not over-extrapolated.
inline bool showTick(const std::vector<Sample> &ring, uint64_t now, uint64_t idelay, uint64_t &out)
{
    if (ring.empty())
        return false;
    const Sample &a = ring.front(), &b = ring.back();
    double rate = 120.0;
    if (b.rxUs > a.rxUs + 250000u && b.tick > a.tick)
        rate = double(b.tick - a.tick) * 1e6 / double(b.rxUs - a.rxUs);
    rate = rate < 20.0 ? 20.0 : (rate > 240.0 ? 240.0 : rate);
    const double ahead = now > b.rxUs ? double(now - b.rxUs) * rate / 1e6 : 0.0;
    const double t = double(b.tick) + ahead - double(idelay);
    out = t > 0.0 ? (uint64_t)t : 0u;
    return true;
}

struct State
{
    bool racing = false;
    uint64_t lastTick = UINT64_MAX, t0 = 0u;
    int64_t lastSend = -1;
    std::vector<Sample> ring;
    uint64_t lastRxUs = 0u;
    bool alive = false, mapped = false;
    uint64_t sent = 0u, recvd = 0u, bad = 0u, shows = 0u, lastLog = 0u;
    double errMax = 0.0, errSum = 0.0;
    uint64_t errN = 0u, lagSum = 0u;
    uint8_t ownFin = 0u;
    bool peerFinLogged = false;
};

inline State &state()
{
    static State s;
    return s;
}

inline bool rider(const uint8_t *r, int slot, uint32_t &out)
{
    uint32_t b = 0u, r0 = 0u;
    return ps2_netsync::resolveChain(r, b, r0) && ps2_netsync::resolveSlot(r, b, slot, out) &&
           ps2_netsync::inRam(out, 0x240u);
}

// This side's own rider while a stock120 race is linked (0 otherwise).
// Used by the APH1 autopilot (PS2X_TK12_AP_PTR=own) on test legs.
inline uint32_t ownRider(const uint8_t *r)
{
    if (!ps2_fh1::netStock120() || !ps2_net::lockstepLinkActive())
        return 0u;
    const int lq = ps2_fh1::g_s120LocalQueue;
    const int slot = knobs().own >= 0 ? knobs().own : (lq == 1 ? 1 : 0);
    uint32_t o = 0u;
    return rider(r, slot, o) ? o : 0u;
}

inline void logLine(const char *what, uint64_t tick)
{
    State &s = state();
    std::fprintf(stderr,
                 "[stock120] %s t=%llu race=%llu sent=%llu recvd=%llu bad=%llu shows=%llu ring=%zu alive=%d "
                 "err_mean=%.1f err_max=%.1f lag_mean=%.1f fed=%llu trim=%llu ck=%llu ckfix=%llu\n",
                 what, (unsigned long long)tick, (unsigned long long)(s.racing ? tick - s.t0 : 0u),
                 (unsigned long long)s.sent, (unsigned long long)s.recvd, (unsigned long long)s.bad,
                 (unsigned long long)s.shows, s.ring.size(), s.alive ? 1 : 0, s.errN ? s.errSum / double(s.errN) : 0.0,
                 s.errMax, s.shows ? double(s.lagSum) / double(s.shows) : 0.0,
                 (unsigned long long)ps2_fh1::g_s120Fed, (unsigned long long)ps2_fh1::g_s120Trim,
                 (unsigned long long)ps2_fh1::g_s120Ck, (unsigned long long)ps2_fh1::g_s120CkFixed);
}

// Once per guest vsync tick (game thread).
inline void onTick(const uint8_t *rdramConst, uint64_t tick)
{
    if (!ps2_fh1::netStock120())
        return;
    static const bool enabled = (ps2_net::stateEnable(), true);
    (void)enabled;
    State &s = state();
    if (tick == s.lastTick)
        return;
    s.lastTick = tick;
    uint8_t *r = const_cast<uint8_t *>(rdramConst);
    std::vector<std::vector<uint8_t>> pays;
    std::vector<uint64_t> rx;
    ps2_net::stateDrain(pays, rx);
    if (!ps2_net::lockstepLinkActive())
    {
        if (s.racing || !s.ring.empty())
        {
            logLine("link down", tick);
            s = State{};
            s.lastTick = tick;
        }
        return;
    }
    for (size_t i = 0; i < pays.size(); ++i)
    {
        Sample smp;
        if (!decode(pays[i].data(), pays[i].size(), smp))
        {
            ++s.bad;
            continue;
        }
        smp.rxUs = rx[i];
        if (!s.ring.empty() && smp.tick < s.ring.back().tick)
            s.ring.clear(); // the peer started a new race
        if (!s.ring.empty() && smp.tick == s.ring.back().tick)
            continue;
        s.ring.push_back(smp);
        if (s.ring.size() > 64u)
            s.ring.erase(s.ring.begin());
        ++s.recvd;
        s.lastRxUs = smp.rxUs;
        s.alive = true;
        if (smp.fin && !s.peerFinLogged)
        {
            s.peerFinLogged = true;
            std::fprintf(stderr, "[stock120] peer finished peer_tick=%llu t=%llu\n", (unsigned long long)smp.tick,
                         (unsigned long long)tick);
        }
    }
    const int32_t st = ps2_fh1::raceState(r);
    const bool racing = st >= 5 && st < 32;
    if (racing != s.racing)
    {
        s.racing = racing;
        if (racing)
        {
            s.t0 = tick;
            s.lastSend = -1;
            s.lastLog = tick;
        }
        logLine(racing ? "race start" : "race end", tick);
    }
    if (!racing)
        return;
    const Knobs &k = knobs();
    const uint64_t raceTick = tick - s.t0;
    const int lq = ps2_fh1::g_s120LocalQueue;
    const int ownSlot = k.own >= 0 ? k.own : (lq == 1 ? 1 : 0);
    const int ghostSlot = k.ghost >= 0 ? k.ghost : (ownSlot == 0 ? 1 : 0);
    uint32_t own = 0u, ghost = 0u;
    const bool haveOwn = rider(r, ownSlot, own), haveGhost = rider(r, ghostSlot, ghost) && ghost != own;
    if (haveOwn && (raceTick % k.every) == 0u && (int64_t)raceTick != s.lastSend)
    {
        Sample smp;
        if (ps2_netsync::readSample(r, own, smp, raceTick))
        {
            smp.fin = st == 6 ? 1u : 0u;
            if (smp.fin != s.ownFin)
            {
                s.ownFin = smp.fin;
                std::fprintf(stderr, "[stock120] own finish edge=%u race=%llu t=%llu\n", smp.fin,
                             (unsigned long long)raceTick, (unsigned long long)tick);
            }
            uint8_t pay[kPayloadLen];
            encode(pay, (uint32_t)raceTick, smp);
            if (ps2_net::stateSend(pay, sizeof(pay)))
                ++s.sent;
        }
        s.lastSend = (int64_t)raceTick;
    }
    const uint64_t now = nowUs();
    if (s.alive && now - s.lastRxUs > kTimeoutUs)
    {
        s.alive = false;
        logLine("peer state timeout", tick);
    }
    if (!s.mapped && !s.ring.empty())
    {
        s.mapped = true;
        const Sample &f = s.ring.back();
        char line[256];
        int n = std::snprintf(line, sizeof(line), "[stock120] map own=%d ghost=%d localq=%d peer_tick=%llu race=%llu",
                              ownSlot, ghostSlot, lq, (unsigned long long)f.tick, (unsigned long long)raceTick);
        for (int slot = 0; slot < 4 && n < (int)sizeof(line) - 24; ++slot)
        {
            uint32_t rr = 0u;
            F32Trip p;
            if (rider(r, slot, rr) && ps2_netsync::readTrip(r, rr + ps2_netsync::kPosOff, p))
            {
                const double d = std::sqrt(double(p.x - f.pos.x) * (p.x - f.pos.x) +
                                           double(p.y - f.pos.y) * (p.y - f.pos.y) +
                                           double(p.z - f.pos.z) * (p.z - f.pos.z));
                n += std::snprintf(line + n, sizeof(line) - n, " d%d=%.1f", slot, d);
            }
        }
        std::fprintf(stderr, "%s\n", line);
    }
    uint64_t show = 0u;
    Sample smp;
    if (s.alive && haveGhost && showTick(s.ring, now, k.idelay, show) && ps2_netsync::showSample(s.ring, show, smp))
    {
        F32Trip was;
        if (ps2_netsync::readTrip(r, ghost + ps2_netsync::kPosOff, was))
        {
            const double e = std::sqrt(double(was.x - smp.pos.x) * (was.x - smp.pos.x) +
                                       double(was.y - smp.pos.y) * (was.y - smp.pos.y) +
                                       double(was.z - smp.pos.z) * (was.z - smp.pos.z));
            s.errSum += e;
            s.errMax = e > s.errMax ? e : s.errMax;
            ++s.errN;
        }
        using namespace ps2_netsync;
        if (writeTrip(r, ghost + kPosOff, smp.pos) && writeTrip(r, ghost + kPos2Off, smp.pos) &&
            writeF32(r, ghost + kQuatOff, smp.qx) && writeF32(r, ghost + kQuatOff + 4u, smp.qy) &&
            writeF32(r, ghost + kQuatOff + 8u, smp.qz) && writeF32(r, ghost + kQuatOff + 12u, smp.qw) &&
            writeTrip(r, ghost + kVelOff, smp.vel))
        {
            ++s.shows;
            s.lagSum += raceTick > show ? raceTick - show : 0u;
        }
    }
    if (tick - s.lastLog >= 600u)
    {
        s.lastLog = tick;
        logLine("race", tick);
        // Diagnostic: both roster riders and the peer's newest sample.
        F32Trip p0, p1;
        uint32_t r0 = 0u, r1 = 0u;
        const bool g0 = rider(r, 0, r0) && ps2_netsync::readTrip(r, r0 + ps2_netsync::kPosOff, p0);
        const bool g1 = rider(r, 1, r1) && ps2_netsync::readTrip(r, r1 + ps2_netsync::kPosOff, p1);
        const Sample *nw = s.ring.empty() ? nullptr : &s.ring.back();
        std::fprintf(stderr,
                     "[stock120] pos race=%llu own=%d s0=%.0f,%.0f,%.0f s1=%.0f,%.0f,%.0f peer@%llu=%.0f,%.0f,%.0f\n",
                     (unsigned long long)raceTick, ownSlot, g0 ? p0.x : 0.f, g0 ? p0.y : 0.f, g0 ? p0.z : 0.f,
                     g1 ? p1.x : 0.f, g1 ? p1.y : 0.f, g1 ? p1.z : 0.f, nw ? (unsigned long long)nw->tick : 0ull,
                     nw ? nw->pos.x : 0.f, nw ? nw->pos.y : 0.f, nw ? nw->pos.z : 0.f);
    }
}
} // namespace ps2_net_stock120
