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

constexpr size_t kPayloadLen = 51u; // ver u8 | epoch u8 | tick u32 | pos 3f | quat 4f | vel 3f | fin u8 | ftime f32
constexpr uint8_t kVersion = 2u;
constexpr uint64_t kTimeoutUs = 3000000u;

struct Knobs
{
    int own = -1, ghost = -1; // -1 = from the input queues
    uint64_t every = 6u, idelay = 12u;
    uint64_t dropStAt = 0u; // diag: stop sending STATE from this race tick (0 = never)
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
        o.dropStAt = (uint64_t)num("PS2X_NET_STOCK120_DROP_ST_AT", 0, 10000000, 0);
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

// Pure (unit-tested): one state payload and its decode. `epoch` is the
// sender's race counter (bumped at each of its race starts), so a tick
// decrease alone is never read as a new race (R4).
inline void encode(uint8_t *o, uint8_t epoch, uint32_t tick, const Sample &s)
{
    o[0] = kVersion;
    o[1] = epoch;
    std::memcpy(o + 2, &tick, 4);
    putF(o + 6, s.pos.x);
    putF(o + 10, s.pos.y);
    putF(o + 14, s.pos.z);
    putF(o + 18, s.qx);
    putF(o + 22, s.qy);
    putF(o + 26, s.qz);
    putF(o + 30, s.qw);
    putF(o + 34, s.vel.x);
    putF(o + 38, s.vel.y);
    putF(o + 42, s.vel.z);
    o[46] = s.fin;
    putF(o + 47, s.ftime);
}

// Pure (unit-tested): a sample is written into guest physics only if every
// field is finite, the quaternion is near unit length, and position and
// velocity are within course scale (worlds span ~±7e5 units; riders move
// ~2e3 units/s, a few times that at most). Checked on decode and again
// after interpolation/extrapolation (astra-online-review-1 R2).
constexpr float kPosLimit = 4.0e6f, kVelLimit = 6.0e4f, kTimeLimit = 3.6e4f;
inline bool saneSample(const Sample &s)
{
    const float f[] = {s.pos.x, s.pos.y, s.pos.z, s.qx, s.qy, s.qz, s.qw, s.vel.x, s.vel.y, s.vel.z, s.ftime};
    for (float v : f)
        if (!std::isfinite(v))
            return false;
    if (std::fabs(s.pos.x) > kPosLimit || std::fabs(s.pos.y) > kPosLimit || std::fabs(s.pos.z) > kPosLimit)
        return false;
    if (std::fabs(s.vel.x) > kVelLimit || std::fabs(s.vel.y) > kVelLimit || std::fabs(s.vel.z) > kVelLimit)
        return false;
    const float n2 = s.qx * s.qx + s.qy * s.qy + s.qz * s.qz + s.qw * s.qw;
    if (n2 < 0.81f || n2 > 1.21f)
        return false;
    return s.ftime >= -2.0f && s.ftime <= kTimeLimit;
}

inline bool decode(const uint8_t *p, size_t n, Sample &s, uint8_t &epoch)
{
    if (n != kPayloadLen || p[0] != kVersion)
        return false;
    uint32_t t = 0u;
    std::memcpy(&t, p + 2, 4);
    s = Sample{};
    epoch = p[1];
    s.tick = t;
    s.pos = {getF(p + 6), getF(p + 10), getF(p + 14)};
    s.qx = getF(p + 18);
    s.qy = getF(p + 22);
    s.qz = getF(p + 26);
    s.qw = getF(p + 30);
    s.vel = {getF(p + 34), getF(p + 38), getF(p + 42)};
    s.fin = p[46];
    s.ftime = getF(p + 47);
    return saneSample(s);
}

// Pure (unit-tested): one received sample into the tick-ordered ring (R4).
// A newer epoch (int8 distance > 0) is the peer's next race: the ring
// restarts. An older epoch, or one the local race has already moved past
// (`minEpoch`), is stale and dropped. Within an epoch, samples are
// inserted in tick order (a reordered older tick no longer wipes newer
// history); duplicates are dropped; the ring keeps the newest 64.
// Returns false when the sample was dropped.
struct RingEpoch
{
    bool have = false;
    uint8_t epoch = 0u;
    bool needNewer = false; // set at a local race start: the old race's samples are stale
};
inline bool ringInsert(std::vector<Sample> &ring, RingEpoch &e, uint8_t epoch, const Sample &smp)
{
    const int8_t d = static_cast<int8_t>(static_cast<uint8_t>(epoch - e.epoch));
    if (!e.have || d > 0)
    {
        ring.clear();
        e.have = true;
        e.epoch = epoch;
        e.needNewer = false;
    }
    else if (d < 0 || e.needNewer)
        return false;
    auto it = ring.begin();
    while (it != ring.end() && it->tick < smp.tick)
        ++it;
    if (it != ring.end() && it->tick == smp.tick)
        return false;
    ring.insert(it, smp);
    if (ring.size() > 64u)
        ring.erase(ring.begin());
    return true;
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

// Owner-authoritative finish (R1). The race ends (0x113db0 -> state 6)
// only when 0x12a250 sees every rider's R+0x470 >= 0. +0x470 is the
// rider's finished timer (float, -1 while racing, counting up from 0 at
// its crossing; v5r: 0.033 at the crossing edge, ~1.8 two seconds later).
// Each side reports its own rider's value; when the peer has finished but
// the local replica has not crossed the line within kFinishGraceTicks of
// that report, the owner's value is written into the replica's +0x470,
// so a missed local crossing can never leave the all-riders-finished wait
// hanging. A natural local crossing comes first in the normal case and is
// never overwritten, so each side's displayed order stays as it was (a
// canonical order is post-v0).
constexpr uint32_t kFinishOff = 0x470u;
constexpr uint64_t kFinishGraceTicks = 120u;
inline bool readFinish(const uint8_t *r, uint32_t rider, float &t)
{
    return ps2_netsync::readF32(r, rider + kFinishOff, t) && std::isfinite(t);
}
// Pure (unit-tested): true when the owner's finished state was written.
inline bool applyOwnerFinish(uint8_t *r, uint32_t replica, float ownerTime)
{
    float cur = 0.0f;
    if (!readFinish(r, replica, cur) || cur >= 0.0f || !(ownerTime >= 0.0f) || ownerTime > kTimeLimit)
        return false;
    return ps2_netsync::writeF32(r, replica + kFinishOff, ownerTime);
}

// Pure (unit-tested): STATE liveness while racing (R3). Stale when no
// valid sample arrived for longer than `timeout` since the later of the
// local race start and the last valid sample, so a link that never
// delivered one counts too.
inline bool stateStale(uint64_t now, uint64_t raceStartUs, uint64_t lastValidUs, uint64_t timeoutUs)
{
    const uint64_t base = lastValidUs > raceStartUs ? lastValidUs : raceStartUs;
    return now > base && now - base > timeoutUs;
}

struct State
{
    bool racing = false;
    uint64_t lastTick = UINT64_MAX, t0 = 0u;
    int64_t lastSend = -1;
    std::vector<Sample> ring;
    uint64_t lastRxUs = 0u;
    bool alive = false, mapped = false;
    uint64_t sent = 0u, recvd = 0u, bad = 0u, stale = 0u, shows = 0u, lastLog = 0u;
    double errMax = 0.0, errSum = 0.0;
    uint64_t errN = 0u, lagSum = 0u;
    uint8_t ownFin = 0u;
    bool peerFinLogged = false;
    uint8_t localEpoch = 0u;
    RingEpoch peerEpoch;
    uint64_t raceStartUs = 0u, lastValidUs = 0u;
    bool raceOver = false, forcedDown = false;
    bool peerFin = false, peerFinApplied = false;
    float peerFtime = -1.0f;
    uint64_t peerFinSeenRace = 0u;
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
            ps2_fh1::g_s120LocalQueue = -1; // the next link's gate sets it again
        }
        return;
    }
    for (size_t i = 0; i < pays.size(); ++i)
    {
        Sample smp;
        uint8_t epoch = 0u;
        if (!decode(pays[i].data(), pays[i].size(), smp, epoch))
        {
            ++s.bad;
            continue;
        }
        smp.rxUs = rx[i];
        if (!ringInsert(s.ring, s.peerEpoch, epoch, smp))
        {
            ++s.stale;
            continue;
        }
        ++s.recvd;
        s.lastRxUs = smp.rxUs;
        s.lastValidUs = smp.rxUs;
        s.alive = true;
        if (smp.fin && !s.peerFin)
        {
            s.peerFin = true;
            s.peerFtime = smp.ftime;
            s.peerFinSeenRace = s.racing ? tick - s.t0 : 0u;
            s.peerFinLogged = true;
            std::fprintf(stderr, "[stock120] peer finished peer_tick=%llu +0x470=%.3f t=%llu\n",
                         (unsigned long long)smp.tick, (double)smp.ftime, (unsigned long long)tick);
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
            ++s.localEpoch;
            // The peer's samples from an earlier race are stale from here
            // until its next race epoch arrives (a replay or restart).
            if (s.peerEpoch.have)
                s.peerEpoch.needNewer = s.localEpoch > 1u;
            s.ring.clear();
            s.alive = false;
            s.raceStartUs = nowUs();
            s.peerFin = s.peerFinApplied = false;
            s.peerFtime = -1.0f;
        }
        logLine(racing ? "race start" : "race end", tick);
    }
    if (!racing)
        return;
    if (st == 6)
        s.raceOver = true; // results/replay: liveness no longer decides anything
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
            float own470 = -1.0f;
            smp.fin = (readFinish(r, own, own470) && own470 >= 0.0f) ? 1u : 0u; // R1: own rider, not race over
            smp.ftime = smp.fin ? own470 : 0.0f;
            if (smp.fin != s.ownFin)
            {
                s.ownFin = smp.fin;
                std::fprintf(stderr, "[stock120] own finish edge=%u +0x470=%.3f race=%llu t=%llu\n", smp.fin,
                             (double)smp.ftime, (unsigned long long)raceTick, (unsigned long long)tick);
            }
            uint8_t pay[kPayloadLen];
            encode(pay, s.localEpoch, (uint32_t)raceTick, smp);
            if (k.dropStAt != 0u && raceTick >= k.dropStAt)
            {
                static bool logged = false;
                if (!logged)
                {
                    logged = true;
                    std::fprintf(stderr, "[stock120] diag: STATE sends dropped from race=%llu (stock traffic kept)\n",
                                 (unsigned long long)raceTick);
                }
            }
            else if (ps2_net::stateSend(pay, sizeof(pay)))
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
    // R3: STATE health decides the link while the race runs. No valid
    // STATE for kTimeoutUs (or none at all since the race start) -> report
    // the P2P link closed so the guest takes its stock peer-loss path,
    // instead of racing on held input with the checksum neutralized.
    if (!s.raceOver && !s.forcedDown && stateStale(now, s.raceStartUs, s.lastValidUs, kTimeoutUs))
    {
        s.forcedDown = true;
        ps2_net::forceLinkDown();
        logLine("STATE lost: stock disconnect", tick);
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
    // R1: the peer's own finish, if the replica missed its crossing.
    if (s.peerFin && !s.peerFinApplied && haveGhost && raceTick >= s.peerFinSeenRace + kFinishGraceTicks)
    {
        s.peerFinApplied = true;
        float cur = -1.0f;
        readFinish(r, ghost, cur);
        if (applyOwnerFinish(r, ghost, s.peerFtime))
            std::fprintf(stderr, "[stock120] replica finished from the owner's report: +0x470 %.3f (local was %.3f) t=%llu\n",
                         (double)s.peerFtime, (double)cur, (unsigned long long)tick);
        else
            std::fprintf(stderr, "[stock120] replica already finished locally: +0x470 %.3f (owner %.3f) t=%llu\n",
                         (double)cur, (double)s.peerFtime, (unsigned long long)tick);
    }
    uint64_t show = 0u;
    Sample smp;
    if (s.alive && haveGhost && showTick(s.ring, now, k.idelay, show) && ps2_netsync::showSample(s.ring, show, smp) &&
        saneSample(smp))
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
