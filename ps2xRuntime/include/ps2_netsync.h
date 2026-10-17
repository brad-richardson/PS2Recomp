#pragma once
// Quick Play netplay: 120<->120 state sync through the ssx3-120 lobby
// (online-v0-1; the STATE wire, ghost drive and finish latch come from
// netsync-spike-1 Part 3). Master switch PS2X_NETSYNC (default off).
//
// Flow (all on the game thread except the socket thread):
//   main menu "Quick Play" (the stock Online row, relabelled) -> Single
//   Event (the row's accept is rewritten, the TK11 way) -> the player picks
//   a rider -> at the Map screen the tk-menu-1 closed-loop navigator drives
//   Peak 1 / Race / Happiness (the Peak 1 backcountry 1v1, open on a fresh
//   card) -> My Rules -> loading -> the Rival Challenge card. The card is
//   the START barrier: the guest's X is held off while the socket thread
//   queues a QP_JOIN; the lobby pairs two waiters and sends both QP_START
//   with one shared GO instant; at GO (minus RTT/2) the navigator presses
//   X, so both races drop in within a few ticks.
//   In the race the remote rider occupies the rival's AI slot (roster slot
//   1, solid, its own identity kept): its inputs are replaced at 0x111728
//   and pos/quat/vel are snapped from the interpolated stream each tick.
//   STATE ticks count from each side's race start (the race clock's first
//   tick); the show point is the remote's current tick estimated from
//   arrival times minus IDELAY (showTickFor), so a slower or hitching
//   device still shows the remote where it is now. No STATE for
//   TIMEOUT_MS, a lobby DROP_TO_AI or a QUIT puts the slot back to AI.
//   The lobby orders finishes from the owners' reports; RESULT shows as a
//   host notice (output-only).
//   Search timeout, refusal, an unreachable server or a navigator abort
//   end the session with a toast and hand the pad back: the player races
//   the AI from wherever they are (no hang).
//
// 120-only (needs PS2X_SSX3_FULL120=events; at 60 the row only explains).
// Server: PS2X_NETSYNC_RELAY=host[:port], else PS2X_NET_SERVER host, port
// 3757. Knob off: one cached bool per hook, nothing wrapped, zero guest
// writes.
#if defined(__APPLE__) || defined(__linux__) || defined(__ANDROID__)
#define PS2_NETSYNC_HAS_UDP 1
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#else
#define PS2_NETSYNC_HAS_UDP 0
#endif

#include "ps2_ssx3_course_manifest.h"
#include "ps2_ssx3_tricky_list.h"
#include "ps2_ui_toast.h"

#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <climits>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ps2_netsync
{
// SNP2 framing (ssx3-proto netplay): len u32 | SNP2 | ver u16 | kind u8 |
// flags u8 | session[16] | epoch u64 | sender u16 | reserved u16 | seq u64,
// all LE; then payload.
constexpr uint32_t kRamSize = 32u * 1024u * 1024u;
constexpr uint32_t kRamMask = 0x01ffffffu;
constexpr size_t kHeader = 48u;
constexpr uint8_t kKindState = 0x14u;
constexpr uint8_t kKindEvent = 0x16u;
constexpr uint8_t kKindFinalize = 0x24u;
constexpr uint8_t kKindQuit = 0x30u;
constexpr uint8_t kKindResult = 0x32u;
constexpr uint8_t kKindPing = 0x40u;
constexpr uint8_t kKindPong = 0x41u;
constexpr uint8_t kKindQpJoin = 0x50u;
constexpr uint8_t kKindQpWait = 0x51u;
constexpr uint8_t kKindQpStart = 0x54u;
constexpr uint8_t kKindQpGo = 0x55u;
constexpr uint8_t kEvDropToAi = 1u;
constexpr uint16_t kQpProto = 1u;
constexpr uint8_t kQpRate120 = 120u;
constexpr uint8_t kWaitQueued = 0u, kWaitFull = 1u, kWaitIncompatible = 2u;
constexpr uint16_t kReasonQuit = 1u;
constexpr uint16_t kDefaultPort = 3757u;
constexpr size_t kStateLen = 111u; // core 91B + input tail 20B, digest covers all
// Rider offsets (netsync-spike-1 Part-1 map).
constexpr uint32_t kChainRoot = 0x4a28a8u;
constexpr uint32_t kChainAOff = 0x84u;
constexpr uint32_t kChainBOff = 0x0cu;
constexpr uint32_t kChainROff = 0x28u;
constexpr uint32_t kClockOff = 0x0cu; // race clock at B+0xc (FH2 raceclock)
constexpr uint32_t kPosOff = 0x110u;
constexpr uint32_t kQuatOff = 0x120u;
constexpr uint32_t kPos2Off = 0x1d0u;
constexpr uint32_t kVelOff = 0x1e0u;
constexpr uint32_t kCtlOff = 0x77cu;
constexpr uint32_t kDe4Off = 0xde4u;
constexpr uint32_t kSlotOff = 0x86cu;
constexpr uint32_t kResBase = 0x536640u;
// In-game roster: the local rider is slot 0, the Rival Challenge rival
// (the remote rider's body) slot 1, on both clients.
constexpr int kOwnRoster = 0;
constexpr int kGhostRoster = 1;

// Quick Play course table (v0: one course). Happiness is the Peak 1
// backcountry race against one rival, open on a fresh card (I26 route).
struct QpCourse
{
    const char *name;
    ps2_ssx3_tricky_list::CourseOption at; // peak, mode type, Select Event row
    int event;                             // SSX 3 event id at that row
};
inline constexpr QpCourse kQpCourses[] = {{"Happiness", {0, 0, 2}, 14}};

// Session timing (guest ticks are 60/s in menus and at the card).
constexpr uint64_t kCardSettleTicks = 120u; // card on screen before JOIN
constexpr uint64_t kRulesTimeoutTicks = 1800u;
constexpr uint64_t kRulesPressEvery = 60u;
constexpr uint64_t kCardTimeoutTicks = 3000u;
constexpr uint64_t kPressHoldTicks = 12u;
constexpr uint64_t kGoRetryTicks = 90u;
constexpr uint64_t kSearchTimeoutMs = 120000u; // the other rider may still be picking a rider
constexpr uint64_t kSilentServerMs = 10000u;
constexpr uint64_t kRaceLostTicks = 240u;
constexpr uint64_t kExtrapolateMax = 30u;
// Pad bits (active-low word; Pad.cpp values).
constexpr uint16_t kPadStart = 0x0008u;
constexpr uint16_t kPadDown = 0x0040u;
constexpr uint16_t kPadTriangle = 0x1000u;
constexpr uint16_t kPadCross = 0x4000u;

struct F32Trip
{
    float x = 0, y = 0, z = 0;
};

struct Sample
{
    uint64_t tick = 0u; // race tick (ticks since the sender's race start)
    F32Trip pos, vel;
    float qx = 0, qy = 0, qz = 0, qw = 1;
    float boost = 0;
    uint8_t uber = 0, wipe = 0, ctl = 0, lap = 0, fin = 0;
    uint32_t chk = 0u;
    float dist = 0, ftime = 0;
    uint64_t peerTick = 0u;
    uint32_t peerMs = 0u;
    uint32_t de4 = 99u, w0 = 0u, w1 = 0u; // the rider's input at that tick
    bool hasInput = false;
    uint64_t rxUs = 0u; // local arrival (monotonic us; show-point estimate)
};

enum class Phase : uint8_t
{
    Idle = 0,
    Nav,      // waiting for / driving the Map screens to the course
    Rules,    // My Rules -> loading
    Card,     // at the Rival card, settling
    Search,   // QP_JOIN queued; guest X held off
    Starting, // QP_START received; GO scheduled
    Going,    // GO pressed; waiting for the race clock
    Racing,
};

inline const char *phaseName(Phase p)
{
    static const char *names[] = {"idle", "nav", "rules", "card", "search", "starting", "going", "racing"};
    return names[static_cast<size_t>(p)];
}

// ---- Pure helpers (unit-tested) --------------------------------------------

inline bool inRam(uint32_t addr, size_t bytes)
{
    if (bytes == 0u || bytes > kRamSize)
        return false;
    const uint32_t off = addr & kRamMask;
    return (size_t)off <= (size_t)kRamSize - bytes;
}

inline bool readU32(const uint8_t *r, uint32_t a, uint32_t &o)
{
    if (!inRam(a, 4u))
        return false;
    std::memcpy(&o, r + (a & kRamMask), 4u);
    return true;
}

inline bool readF32(const uint8_t *r, uint32_t a, float &o)
{
    uint32_t b = 0u;
    if (!readU32(r, a, b))
        return false;
    std::memcpy(&o, &b, 4u);
    return true;
}

inline bool writeF32(uint8_t *r, uint32_t a, float v)
{
    if (!inRam(a, 4u))
        return false;
    std::memcpy(r + (a & kRamMask), &v, 4u);
    return true;
}

inline bool readTrip(const uint8_t *r, uint32_t b, F32Trip &t)
{
    return readF32(r, b, t.x) && readF32(r, b + 4u, t.y) && readF32(r, b + 8u, t.z);
}

inline bool writeTrip(uint8_t *r, uint32_t b, const F32Trip &t)
{
    return writeF32(r, b, t.x) && writeF32(r, b + 4u, t.y) && writeF32(r, b + 8u, t.z);
}

inline bool resolveChain(const uint8_t *r, uint32_t &bOut, uint32_t &r0Out)
{
    bOut = 0u;
    r0Out = 0u;
    uint32_t g = 0u, a = 0u, b = 0u, rr = 0u;
    if (!readU32(r, kChainRoot, g) || g == 0u)
        return false;
    if (!readU32(r, (g & kRamMask) + kChainAOff, a) || a == 0u)
        return false;
    a &= kRamMask;
    if (!readU32(r, a + kChainBOff, b) || b == 0u)
        return false;
    b &= kRamMask;
    if (!readU32(r, b + kChainROff, rr) || rr == 0u)
        return false;
    bOut = b & kRamMask;
    r0Out = rr & kRamMask;
    return true;
}

inline bool resolveSlot(const uint8_t *r, uint32_t b, int slot, uint32_t &o)
{
    o = 0u;
    if (b == 0u || slot < 0 || slot > 5)
        return false;
    uint32_t p = 0u;
    if (!readU32(r, b + kChainROff + (uint32_t)slot * 4u, p) || p == 0u)
        return false;
    o = p & kRamMask;
    return true;
}

inline uint64_t fnv64(const uint8_t *d, size_t n)
{
    uint64_t h = 0xcbf29ce484222325u;
    for (size_t i = 0u; i < n; ++i)
    {
        h ^= d[i];
        h *= 0x100000001b3u;
    }
    return h;
}

inline void putU16(uint8_t *d, uint16_t v)
{
    d[0] = v & 0xffu;
    d[1] = (v >> 8) & 0xffu;
}
inline void putU32(uint8_t *d, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        d[i] = (v >> (8 * i)) & 0xffu;
}
inline void putU64(uint8_t *d, uint64_t v)
{
    for (int i = 0; i < 8; ++i)
        d[i] = (v >> (8 * i)) & 0xffu;
}
inline void putF32(uint8_t *d, float v) { std::memcpy(d, &v, 4u); }
inline uint16_t getU16(const uint8_t *d) { return (uint16_t)d[0] | ((uint16_t)d[1] << 8); }
inline uint32_t getU32(const uint8_t *d)
{
    uint32_t v = 0u;
    for (int i = 0; i < 4; ++i)
        v |= (uint32_t)d[i] << (8 * i);
    return v;
}
inline uint64_t getU64(const uint8_t *d)
{
    uint64_t v = 0u;
    for (int i = 0; i < 8; ++i)
        v |= (uint64_t)d[i] << (8 * i);
    return v;
}
inline float getF32(const uint8_t *d)
{
    float v = 0;
    std::memcpy(&v, d, 4u);
    return v;
}

// One SNP2 frame around payload. Returns length (0 = refused).
inline size_t buildFrame(uint8_t *out, size_t cap, uint8_t kind, uint16_t sender, uint64_t seq,
                         const uint8_t *payload, size_t plen, const uint8_t *session = nullptr)
{
    if (kHeader + plen > cap || seq == 0u)
        return 0u;
    putU32(out, (uint32_t)(44u + plen));
    std::memcpy(out + 4, "SNP2", 4);
    putU16(out + 8, 2u);
    out[10] = kind;
    out[11] = 0u;
    if (session)
        std::memcpy(out + 12, session, 16);
    else
        std::memset(out + 12, 0, 16);
    putU64(out + 28, 0u);
    putU16(out + 36, sender);
    putU16(out + 38, 0u);
    putU64(out + 40, seq);
    if (plen)
        std::memcpy(out + 48, payload, plen);
    return kHeader + plen;
}

// Strict subset of the lobby's decode_frame. Returns payload length or -1.
inline long checkFrame(const uint8_t *d, size_t n)
{
    if (n < kHeader || getU32(d) + 4u != (uint32_t)n || std::memcmp(d + 4, "SNP2", 4) != 0 ||
        getU16(d + 8) != 2u || d[11] != 0u || getU16(d + 38) != 0u || getU64(d + 40) == 0u)
        return -1;
    return (long)(n - kHeader);
}

// QP_JOIN = proto u16 | client[16] | rate u8 | course u8.
inline size_t encodeJoin(uint8_t *o, const uint8_t client[16], uint8_t course)
{
    putU16(o, kQpProto);
    std::memcpy(o + 2, client, 16);
    o[18] = kQpRate120;
    o[19] = course;
    return 20u;
}

struct QpStart
{
    uint32_t startId = 0u;
    uint8_t slot = 0u, players = 0u, course = 0u, seq = 0u;
    uint16_t goInMs = 0u;
};

// QP_START = start_id u32 | slot | players | course | seq | go_in_ms u16.
inline bool decodeStart(const uint8_t *p, size_t n, QpStart &s)
{
    if (n != 10u)
        return false;
    s.startId = getU32(p);
    s.slot = p[4];
    s.players = p[5];
    s.course = p[6];
    s.seq = p[7];
    s.goInMs = getU16(p + 8);
    return s.slot < 2u && s.players == 2u;
}

// QP_GO = start_id u32 | seq u8 | rtt_us u32 | recv_to_go_us u32 | go_tick u64.
inline size_t encodeGo(uint8_t *o, uint32_t startId, uint8_t seq, uint32_t rttUs, uint32_t recvToGoUs,
                       uint64_t goTick)
{
    putU32(o, startId);
    o[4] = seq;
    putU32(o + 5, rttUs);
    putU32(o + 9, recvToGoUs);
    putU64(o + 13, goTick);
    return 21u;
}

// Encode one KIND_STATE payload (111B, byte-identical to the lobby codec).
inline void encodeState(uint8_t *o, uint16_t slot, uint64_t tick, const Sample &s, uint32_t de4, uint32_t w0,
                        uint32_t w1, uint64_t inTick, uint64_t peerTick, uint32_t peerMs)
{
    putU16(o, slot);
    putU64(o + 2, tick);
    putF32(o + 10, s.pos.x);
    putF32(o + 14, s.pos.y);
    putF32(o + 18, s.pos.z);
    putF32(o + 22, s.qx);
    putF32(o + 26, s.qy);
    putF32(o + 30, s.qz);
    putF32(o + 34, s.qw);
    putF32(o + 38, s.vel.x);
    putF32(o + 42, s.vel.y);
    putF32(o + 46, s.vel.z);
    putF32(o + 50, s.boost);
    o[54] = s.uber;
    o[55] = s.wipe;
    o[56] = (uint8_t)(de4 & 0xffu);
    o[57] = s.lap;
    putU32(o + 58, s.chk);
    putF32(o + 62, s.dist);
    o[66] = s.fin;
    putF32(o + 67, s.ftime);
    putU64(o + 71, peerTick);
    putU32(o + 79, peerMs);
    putU64(o + 83, inTick);
    putU32(o + 91, de4);
    putU32(o + 95, w0);
    putU32(o + 99, w1);
    putU64(o + 103, fnv64(o, 103u));
}

// Decode one STATE payload. False on any shape/digest violation.
inline bool decodeState(const uint8_t *p, size_t n, Sample &s, uint32_t &de4, uint32_t &w0, uint32_t &w1,
                        uint64_t &inTick, uint16_t &slot)
{
    if (n != kStateLen || fnv64(p, 103u) != getU64(p + 103))
        return false;
    slot = getU16(p);
    s.tick = getU64(p + 2);
    s.pos.x = getF32(p + 10);
    s.pos.y = getF32(p + 14);
    s.pos.z = getF32(p + 18);
    s.qx = getF32(p + 22);
    s.qy = getF32(p + 26);
    s.qz = getF32(p + 30);
    s.qw = getF32(p + 34);
    s.vel.x = getF32(p + 38);
    s.vel.y = getF32(p + 42);
    s.vel.z = getF32(p + 46);
    s.boost = getF32(p + 50);
    s.uber = p[54];
    s.wipe = p[55];
    s.ctl = p[56];
    s.lap = p[57];
    s.chk = getU32(p + 58);
    s.dist = getF32(p + 62);
    s.fin = p[66];
    s.ftime = getF32(p + 67);
    s.peerTick = getU64(p + 71);
    s.peerMs = getU32(p + 79);
    inTick = getU64(p + 83);
    de4 = getU32(p + 91);
    w0 = getU32(p + 95);
    w1 = getU32(p + 99);
    s.de4 = de4;
    s.w0 = w0;
    s.w1 = w1;
    s.hasInput = true;
    return true;
}

// The show sample for `showTick` from a tick-ordered ring: lerp/nlerp
// between the bracketing samples; past the newest, dead-reckon pos along
// vel (units/s, 120 ticks/s) for at most kExtrapolateMax ticks, then hold.
inline bool showSample(const std::vector<Sample> &ring, uint64_t showTick, Sample &out)
{
    const Sample *lo = nullptr, *hi = nullptr;
    for (const Sample &s : ring)
    {
        if (s.tick <= showTick)
            lo = &s;
        else
        {
            hi = &s;
            break;
        }
    }
    if (!lo)
        return false;
    out = *lo;
    if (hi && hi->tick > lo->tick)
    {
        float a = float(showTick - lo->tick) / float(hi->tick - lo->tick);
        a = a < 0 ? 0 : (a > 1 ? 1 : a);
        out.pos.x += (hi->pos.x - lo->pos.x) * a;
        out.pos.y += (hi->pos.y - lo->pos.y) * a;
        out.pos.z += (hi->pos.z - lo->pos.z) * a;
        out.vel.x += (hi->vel.x - lo->vel.x) * a;
        out.vel.y += (hi->vel.y - lo->vel.y) * a;
        out.vel.z += (hi->vel.z - lo->vel.z) * a;
        const float qx = lo->qx + (hi->qx - lo->qx) * a, qy = lo->qy + (hi->qy - lo->qy) * a;
        const float qz = lo->qz + (hi->qz - lo->qz) * a, qw = lo->qw + (hi->qw - lo->qw) * a;
        const float n = std::sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
        if (n > 1e-6f)
        {
            out.qx = qx / n;
            out.qy = qy / n;
            out.qz = qz / n;
            out.qw = qw / n;
        }
        return true;
    }
    uint64_t ahead = showTick - lo->tick;
    if (ahead > kExtrapolateMax)
        ahead = kExtrapolateMax;
    const float dt = float(ahead) / 120.0f;
    out.pos.x += lo->vel.x * dt;
    out.pos.y += lo->vel.y * dt;
    out.pos.z += lo->vel.z * dt;
    return true;
}

// The remote race tick to show now: the remote's current tick estimated
// from arrival times (each sample's rx - tick/120 s; the least-delayed one
// is the reference), minus `idelay`. Wall-clock based, so a device running
// slower or faster than 120 (or one that hitched) still shows the remote
// where it is now, not where the local race tick says. Equal to local race
// tick - idelay when both run at 120. False when the ring is empty.
inline bool showTickFor(const std::vector<Sample> &ring, uint64_t nowUs, uint64_t idelay, uint64_t &out)
{
    if (ring.empty())
        return false;
    int64_t minOff = INT64_MAX;
    for (const Sample &s : ring)
    {
        const int64_t off = (int64_t)s.rxUs - (int64_t)(s.tick * 1000000u / 120u);
        if (off < minOff)
            minOff = off;
    }
    const int64_t est = ((int64_t)nowUs - minOff) * 120 / 1000000;
    out = est > (int64_t)idelay ? (uint64_t)(est - (int64_t)idelay) : 0u;
    return true;
}

// RESULT rows (NetResult layout: result_id[32], boundary u64, commit u64,
// chain[32], state digest[33], transcript[32], count u8 at [145], then
// 25-byte rows: slot u16, actor, disposition, rank, tick u64, ms u32,
// score i64). Returns the notice text, or "" when malformed.
inline std::string resultText(const uint8_t *p, size_t n, int mySlot)
{
    if (n < 146u)
        return {};
    const uint8_t rows = p[145];
    if (rows == 0u || rows > 6u || n != 146u + 25u * rows)
        return {};
    std::string text = "Quick Play result";
    for (uint8_t i = 0u; i < rows; ++i)
    {
        const uint8_t *r = p + 146u + 25u * i;
        const uint16_t slot = getU16(r);
        const uint8_t disp = r[3], rank = r[4];
        const uint32_t ms = getU32(r + 13);
        char line[64];
        const char *who = (int)slot == mySlot ? "You" : "Rival";
        if (disp == 0u)
            std::snprintf(line, sizeof(line), "\n%u%s  %-5s  %u:%02u.%02u", rank,
                          rank == 1 ? "st" : (rank == 2 ? "nd" : "th"), who, ms / 60000u, (ms / 1000u) % 60u,
                          (ms / 10u) % 100u);
        else
            std::snprintf(line, sizeof(line), "\n--   %-5s  DNF", who);
        text += line;
    }
    return text;
}

// "host[:port]" -> host, port (default kDefaultPort).
inline bool parseServer(const char *spec, std::string &host, uint16_t &port)
{
    host.clear();
    port = kDefaultPort;
    if (!spec || !*spec)
        return false;
    std::string s = spec;
    const size_t c = s.rfind(':');
    if (c != std::string::npos && s.find(':') == c)
    {
        const int p = std::atoi(s.substr(c + 1).c_str());
        if (p <= 0 || p > 65535)
            return false;
        port = (uint16_t)p;
        s = s.substr(0, c);
    }
    host = s;
    return !host.empty();
}

// ---- Knobs -------------------------------------------------------------------

struct Knobs
{
    bool set = false;     // PS2X_NETSYNC=1
    bool full120 = false; // PS2X_SSX3_FULL120=events
    bool on = false;      // set && full120 && server
    std::string host;
    uint16_t port = kDefaultPort;
    uint64_t every = 6u;   // 20 Hz at 120 ticks/s
    uint64_t idelay = 12u; // ~2 updates
    uint64_t timeoutMs = 3000u;
};

inline const Knobs &knobs()
{
    static const Knobs k = [] {
        Knobs k;
        const char *m = std::getenv("PS2X_NETSYNC");
        k.set = m && (std::strcmp(m, "1") == 0 || std::strcmp(m, "on") == 0);
        if (!k.set)
            return k;
        const char *f120 = std::getenv("PS2X_SSX3_FULL120");
        k.full120 = f120 && std::strcmp(f120, "events") == 0;
        const char *relay = std::getenv("PS2X_NETSYNC_RELAY");
        const char *server = std::getenv("PS2X_NET_SERVER");
        const bool haveServer = parseServer(relay && *relay ? relay : server, k.host, k.port);
        if (const char *ev = std::getenv("PS2X_NETSYNC_EVERY"); ev && *ev)
            k.every = std::max<uint64_t>(1u, std::strtoull(ev, nullptr, 0));
        if (const char *id = std::getenv("PS2X_NETSYNC_IDELAY"); id && *id)
            k.idelay = std::strtoull(id, nullptr, 0);
        if (const char *to = std::getenv("PS2X_NETSYNC_TIMEOUT_MS"); to && *to)
            k.timeoutMs = std::strtoull(to, nullptr, 0);
        k.on = PS2_NETSYNC_HAS_UDP && k.full120 && haveServer;
        if (!k.full120)
            std::fprintf(stderr, "[netsync] Quick Play is 120-only (PS2X_SSX3_FULL120=events); off at 60\n");
        else if (!haveServer)
            std::fprintf(stderr, "[netsync] no server (PS2X_NETSYNC_RELAY or PS2X_NET_SERVER); off\n");
        else
            std::fprintf(stderr, "[netsync] Quick Play on: server %s:%u every=%llu idelay=%llu timeout=%llums\n",
                         k.host.c_str(), k.port, (unsigned long long)k.every, (unsigned long long)k.idelay,
                         (unsigned long long)k.timeoutMs);
        return k;
    }();
    return k;
}

// The menu path is live (Quick Play label, Online -> Single Event).
inline bool menuOn() { return knobs().on; }

inline uint64_t wallUs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

// ---- Socket thread -------------------------------------------------------------
// Owns DNS + the connected UDP socket: resends QP_JOIN while asked, PINGs
// once a second (RTT), queues every other frame for the game thread, and
// sends QUIT on stop. The game thread sends STATE/QP_GO on the same socket.

struct Msg
{
    uint8_t kind = 0u;
    uint8_t session[16] = {};
    std::vector<uint8_t> payload;
    uint64_t rxUs = 0u;
};

struct Io
{
    std::thread th;
    std::atomic<bool> stop{false};
    std::atomic<bool> wantJoin{false};
    std::atomic<bool> ready{false};
    std::atomic<bool> failed{false};
    std::atomic<bool> sendQuit{false};
    std::atomic<int> sock{-1};
    std::atomic<uint32_t> rttUs{0u};
    std::atomic<uint64_t> lastRxUs{0u};
    std::atomic<uint64_t> seq{1u};
    std::mutex mu; // inbox, room, slot
    std::vector<Msg> inbox;
    uint8_t room[16] = {};
    uint16_t slot = 0u;
    uint8_t client[16] = {};
    uint8_t course = 0u;
};

inline Io &io()
{
    static Io i;
    return i;
}

inline bool ioSend(uint8_t kind, uint16_t sender, const uint8_t *payload, size_t plen, const uint8_t *session)
{
#if PS2_NETSYNC_HAS_UDP
    Io &x = io();
    const int fd = x.sock.load();
    if (fd < 0 || !x.ready.load())
        return false;
    uint8_t frame[256];
    const size_t fl = buildFrame(frame, sizeof(frame), kind, sender, x.seq.fetch_add(1u), payload, plen, session);
    return fl > 0u && ::send(fd, frame, fl, 0) == (ssize_t)fl;
#else
    (void)kind, (void)sender, (void)payload, (void)plen, (void)session;
    return false;
#endif
}

inline void ioLoop()
{
#if PS2_NETSYNC_HAS_UDP
    Io &x = io();
    const Knobs &k = knobs();
    struct addrinfo hints = {}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    char port[8];
    std::snprintf(port, sizeof(port), "%u", k.port);
    if (::getaddrinfo(k.host.c_str(), port, &hints, &res) != 0 || !res)
    {
        std::fprintf(stderr, "[netsync] server lookup failed (%s)\n", std::strerror(errno));
        x.failed = true;
        return;
    }
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0 || ::connect(fd, res->ai_addr, res->ai_addrlen) != 0)
    {
        const int err = errno;
        ::freeaddrinfo(res);
        if (fd >= 0)
            ::close(fd);
        std::fprintf(stderr, "[netsync] %s failed: %s\n", fd < 0 ? "socket" : "connect", std::strerror(err));
        x.failed = true;
        return;
    }
    ::freeaddrinfo(res);
    struct timeval tv = {0, 50000};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    x.sock = fd;
    x.ready = true;
    uint64_t lastJoin = 0u, lastPing = 0u, nonce = 0u;
    uint32_t rtts[5] = {};
    size_t nRtt = 0u;
    uint8_t buf[2048];
    while (!x.stop.load())
    {
        const uint64_t now = wallUs();
        if (x.wantJoin.load() && now - lastJoin >= 500000u)
        {
            uint8_t p[20];
            encodeJoin(p, x.client, x.course);
            ioSend(kKindQpJoin, 0u, p, sizeof(p), nullptr);
            lastJoin = now;
        }
        if (now - lastPing >= 1000000u)
        {
            uint8_t p[16];
            putU64(p, ++nonce);
            putU64(p + 8, now);
            ioSend(kKindPing, 0u, p, sizeof(p), nullptr);
            lastPing = now;
        }
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0)
            continue;
        const long plen = checkFrame(buf, (size_t)n);
        if (plen < 0)
            continue;
        const uint64_t rx = wallUs();
        x.lastRxUs = rx;
        if (buf[10] == kKindPong && plen == 16)
        {
            const uint64_t origin = getU64(buf + kHeader + 8);
            if (origin <= rx && rx - origin < 5000000u)
            {
                rtts[nRtt++ % 5u] = (uint32_t)(rx - origin);
                // RTT = the smallest of the last five (queueing only adds).
                uint32_t best = rtts[0];
                for (size_t i = 1; i < std::min<size_t>(nRtt, 5u); ++i)
                    best = std::min(best, rtts[i]);
                x.rttUs = best;
            }
            continue;
        }
        Msg m;
        m.kind = buf[10];
        std::memcpy(m.session, buf + 12, 16);
        m.payload.assign(buf + kHeader, buf + n);
        m.rxUs = rx;
        std::lock_guard<std::mutex> lock(x.mu);
        if (x.inbox.size() >= 512u)
            x.inbox.erase(x.inbox.begin());
        x.inbox.push_back(std::move(m));
    }
    if (x.sendQuit.load())
    {
        uint8_t room[16];
        uint16_t slot;
        {
            std::lock_guard<std::mutex> lock(x.mu);
            std::memcpy(room, x.room, 16);
            slot = x.slot;
        }
        uint8_t p[2];
        putU16(p, kReasonQuit);
        for (int i = 0; i < 3; ++i)
            ioSend(kKindQuit, slot, p, sizeof(p), room);
    }
    x.ready = false;
    x.sock = -1;
    ::close(fd);
#endif
}

inline void ioStart(uint8_t course)
{
    Io &x = io();
    x.stop = false;
    x.wantJoin = false;
    x.ready = false;
    x.failed = false;
    x.sendQuit = true;
    x.rttUs = 0u;
    x.lastRxUs = 0u;
    {
        std::lock_guard<std::mutex> lock(x.mu);
        x.inbox.clear();
        std::memset(x.room, 0, 16);
        x.slot = 0u;
        x.course = course;
        // Per-session client id (pairing never matches a client with itself).
        uint64_t a = wallUs() ^ (uint64_t)(uintptr_t)&x, b = a * 0x9e3779b97f4a7c15ull;
        for (int i = 0; i < 8; ++i)
        {
            x.client[i] = (uint8_t)(a >> (8 * i));
            x.client[8 + i] = (uint8_t)(b >> (8 * i));
        }
    }
    if (x.th.joinable())
        x.th.join();
    x.th = std::thread(ioLoop);
}

inline void ioStop()
{
    Io &x = io();
    x.stop = true;
    if (x.th.joinable())
        x.th.join();
}

// ---- Session (game thread) -------------------------------------------------------

struct Session
{
    Phase phase = Phase::Idle;
    uint64_t sessions = 0u;
    // Nav / rules / card
    uint64_t mapFills = 0u, mapFillsAtAccept = 0u;
    uint32_t mapController = 0u;
    bool navArmed = false;
    ps2_ssx3_tricky_list::NavState nav;
    uint64_t phaseTick = 0u;   // tick the phase began
    uint64_t holdUntil = 0u;   // injected press held until this tick
    uint64_t nextPress = 0u;
    int presses = 0;
    uint64_t cardSince = 0u;   // first tick the card read live with this clock
    uint32_t cardClock = 0u;   // the standing race clock on the card
    uint16_t prevPhys = 0xffffu;
    // Search / start
    uint64_t searchUs = 0u;
    QpStart start;
    uint64_t startRxUs = 0u, goAtUs = 0u;
    uint64_t goPressTick = 0u, goPressUs = 0u;
    int goPresses = 0;
    // Race
    uint64_t t0 = 0u;
    uint32_t lastClock = 0u;
    uint64_t lostTicks = 0u;
    int64_t lastTick = -1, lastSendTick = -1, lastShowTick = -1, lastLogTick = -1;
    uint64_t sent = 0u, recvd = 0u, stale = 0u, applied = 0u, kept = 0u, showWrites = 0u;
    std::vector<Sample> ring;
    bool peerAlive = false;
    uint64_t lastPeerUs = 0u;
    uint32_t ownDe4 = 99u, ownW0 = 0u, ownW1 = 0u;
    uint64_t ownInTick = 0u;
    bool showHasInput = false;
    uint32_t showDe4 = 99u, showW0 = 0u, showW1 = 0u;
    uint8_t lastFinSent = 0u;
    bool ghostFin = false;
    uint64_t ghostFinTick = 0u;
    uint32_t ghostFinMs = 0u;
    bool resultShown = false;
};

inline Session &session()
{
    static Session s;
    return s;
}

inline void setPhase(Phase p, uint64_t tick, const char *why)
{
    Session &s = session();
    std::fprintf(stderr, "[netsync] phase %s -> %s at tick=%llu (%s)\n", phaseName(s.phase), phaseName(p),
                 (unsigned long long)tick, why);
    s.phase = p;
    s.phaseTick = tick;
    s.holdUntil = 0u;
    s.nextPress = tick;
    s.presses = 0;
}

// Ends the session (QUIT to the lobby when one was joined) and hands the pad
// back. `toast` tells the player what happens next.
inline void endSession(uint64_t tick, const char *why, const char *toast)
{
    Session &s = session();
    if (s.phase == Phase::Idle)
        return;
    std::fprintf(stderr,
                 "[netsync] end at tick=%llu phase=%s (%s) sent=%llu recvd=%llu stale=%llu applied=%llu kept=%llu "
                 "show=%llu\n",
                 (unsigned long long)tick, phaseName(s.phase), why, (unsigned long long)s.sent,
                 (unsigned long long)s.recvd, (unsigned long long)s.stale, (unsigned long long)s.applied,
                 (unsigned long long)s.kept, (unsigned long long)s.showWrites);
    ioStop();
    if (toast && *toast)
        ps2x::ui::toast(toast, 6.0f);
    s.phase = Phase::Idle;
    s.navArmed = false;
    ps2_ssx3_tricky_list::navReset(s.nav);
}

// Main-menu accept on the Online row: start a session. Returns true when the
// row should run Single Event instead (the caller rewrites the item state).
inline bool onOnlineAccept(uint64_t tick)
{
    const Knobs &k = knobs();
    if (!k.set)
        return false;
    if (!k.on)
    {
        ps2x::ui::toast(k.full120 ? "Quick Play: no server configured"
                                  : "Quick Play needs 120 Hz (Settings > Rate)",
                        6.0f);
        return false;
    }
    Session &s = session();
    if (s.phase != Phase::Idle)
        endSession(tick, "new entry", nullptr);
    const uint64_t n = s.sessions + 1u;
    s = Session{};
    s.sessions = n;
    s.mapFillsAtAccept = s.mapFills = 0u;
    ioStart(0u);
    setPhase(Phase::Nav, tick, "Quick Play selected");
    ps2x::ui::toast("Quick Play: pick your rider - we'll take you to Happiness", 6.0f);
    return true;
}

// Main-menu build (the player is back at the menu): drop a pre-race session.
inline void onMainMenuBuild(uint64_t tick)
{
    if (!menuOn())
        return;
    Session &s = session();
    if (s.phase != Phase::Idle)
        endSession(tick, "main menu", s.phase >= Phase::Card && s.phase < Phase::Racing ? "Quick Play cancelled" : nullptr);
}

// Map-screen row fill (once per Map-screen entry): the controller the
// navigator reads the Event cursor through.
inline void onMapFill(uint32_t controller)
{
    if (!menuOn())
        return;
    Session &s = session();
    s.mapController = controller;
    ++s.mapFills;
}

// The race is loaded with both riders in place (the Rival card or the
// race). `clock` = the race clock, which stands still until the drop-in
// (FH5: it restarts there; a stale value from an earlier race may sit on
// the card, so the caller wants it stable, not zero).
inline bool raceLoaded(const uint8_t *ram, uint32_t &clock)
{
    clock = 0u;
    uint32_t b = 0u, r0 = 0u, r1 = 0u;
    return ps2_ssx3_course::appUpdateFn(ram) == ps2_ssx3_course::kGameUpdate && resolveChain(ram, b, r0) &&
           resolveSlot(ram, b, kGhostRoster, r1) && readU32(ram, b + kClockOff, clock);
}

inline bool loadingNow(const uint8_t *ram)
{
    using namespace ps2_ssx3_course;
    for (uint32_t i = 0; i < kSlots; ++i)
        if (rd32(ram, kSlotBase + i * kSlotStride + 8u) == kSlotRequested)
            return true;
    return false;
}

// One peer STATE (from drainInbox).
inline void onPeerState(const Msg &m)
{
    Session &s = session();
    Sample smp;
    uint32_t de4 = 99u, w0 = 0u, w1 = 0u;
    uint64_t inTick = 0u;
    uint16_t slot = 99u;
    if (!decodeState(m.payload.data(), m.payload.size(), smp, de4, w0, w1, inTick, slot) || slot == s.start.slot ||
        (!s.ring.empty() && smp.tick <= s.ring.back().tick))
    {
        ++s.stale;
        return;
    }
    smp.rxUs = m.rxUs;
    s.ring.push_back(smp);
    if (s.ring.size() > 64u)
        s.ring.erase(s.ring.begin());
    ++s.recvd;
    s.lastPeerUs = m.rxUs;
    s.peerAlive = true;
    if (smp.fin != 0 && !s.ghostFin)
    {
        s.ghostFin = true;
        s.ghostFinTick = smp.tick;
        s.ghostFinMs = (uint32_t)(smp.ftime * 1000.0f);
    }
}

// Applies queued lobby frames (game thread).
inline void drainInbox(uint64_t tick)
{
    Io &x = io();
    std::vector<Msg> msgs;
    {
        std::lock_guard<std::mutex> lock(x.mu);
        msgs.swap(x.inbox);
    }
    Session &s = session();
    for (const Msg &m : msgs)
    {
        if (m.kind == kKindState)
        {
            if (s.phase == Phase::Racing)
                onPeerState(m);
            continue;
        }
        const uint8_t *p = m.payload.data();
        const size_t n = m.payload.size();
        if (m.kind == kKindQpWait && n == 3u)
        {
            if (p[0] == kWaitFull && s.phase == Phase::Search)
                endSession(tick, "server full", "Quick Play is full right now - press X to race the AI");
            else if (p[0] == kWaitIncompatible && s.phase == Phase::Search)
                endSession(tick, "incompatible", "This build can't join Quick Play - press X to race the AI");
        }
        else if (m.kind == kKindQpStart)
        {
            QpStart st;
            if (!decodeStart(p, n, st) || st.course != 0u)
                continue;
            if (s.phase == Phase::Search)
            {
                {
                    std::lock_guard<std::mutex> lock(x.mu);
                    std::memcpy(x.room, m.session, 16);
                    x.slot = st.slot;
                }
                x.wantJoin = false;
                s.start = st;
                s.startRxUs = m.rxUs;
                const uint64_t half = x.rttUs.load() / 2u;
                const uint64_t lead = (uint64_t)st.goInMs * 1000u;
                s.goAtUs = m.rxUs + (lead > half ? lead - half : 0u);
                std::fprintf(stderr,
                             "[netsync] START room=%02x%02x%02x%02x slot=%u go_in=%ums rtt=%uus seq=%u at tick=%llu\n",
                             m.session[0], m.session[1], m.session[2], m.session[3], st.slot, st.goInMs,
                             x.rttUs.load(), st.seq, (unsigned long long)tick);
                setPhase(Phase::Starting, tick, "paired");
                ps2x::ui::toast("Rider found - get ready!", 3.0f);
            }
        }
        else if (m.kind == kKindEvent)
        {
            // standalone Event: event_seq u64 + type u8 + drop(slot u16, reason u16, boundary u64)
            if (n >= 21u && p[8] == kEvDropToAi && s.phase >= Phase::Starting)
            {
                const uint16_t dslot = getU16(p + 9), reason = getU16(p + 11);
                if (dslot != s.start.slot)
                {
                    const bool wasRacing = s.phase == Phase::Racing && s.peerAlive;
                    s.peerAlive = false;
                    std::fprintf(stderr, "[netsync] DROP_TO_AI slot=%u reason=%u tick=%llu\n", dslot, reason,
                                 (unsigned long long)tick);
                    ps2x::ui::toast(wasRacing ? "Rider disconnected - the AI takes over"
                                              : "Rider left - racing the AI",
                                    4.0f);
                }
            }
        }
        else if (m.kind == kKindResult && !s.resultShown && s.phase == Phase::Racing)
        {
            const std::string text = resultText(p, n, s.start.slot);
            if (!text.empty())
            {
                s.resultShown = true;
                std::string flat = text;
                for (char &c : flat)
                    if (c == '\n')
                        c = '|';
                std::fprintf(stderr, "[netsync] RESULT %s\n", flat.c_str());
                ps2x::ui::notice(text, 10.0f);
            }
        }
    }
}

// Port-0 pad merge point (game thread, Pad.cpp): drives the menus, holds the
// card, presses GO. `b` is the active-low word the guest will read.
inline void onPadRead(uint16_t &b, uint64_t tick, uint8_t *ram)
{
    if (!menuOn() || !ram)
        return;
    Session &s = session();
    if (s.phase == Phase::Idle)
        return;
    drainInbox(tick);
    if (s.phase == Phase::Idle)
        return;
    const uint16_t phys = b;
    const bool triEdge = !(phys & kPadTriangle) && (s.prevPhys & kPadTriangle);
    s.prevPhys = phys;
    Io &x = io();
    if (x.failed.load() && s.phase < Phase::Starting)
    {
        endSession(tick, "server unreachable", "Can't reach the Quick Play server - riding solo");
        return;
    }
    const QpCourse &course = kQpCourses[0];
    namespace tkl = ps2_ssx3_tricky_list;
    switch (s.phase)
    {
    case Phase::Nav:
    {
        if (!s.navArmed)
        {
            // The player picks a rider; the navigator starts at the first
            // Map-screen entry after the accept.
            if (s.mapFills == s.mapFillsAtAccept)
                return;
            tkl::navStart(s.nav, course.at, course.event, tick);
            s.navArmed = true;
            std::fprintf(stderr, "[netsync] nav armed at tick=%llu -> %s (peak %u type %u row %u event %d)\n",
                         (unsigned long long)tick, course.name, course.at.peak, course.at.type, course.at.row,
                         course.event);
        }
        if (triEdge)
        {
            endSession(tick, "cancelled at the Map", "Quick Play cancelled");
            return;
        }
        const tkl::FeView v = tkl::readView(ram, kRamSize, s.mapController);
        const tkl::NavOut o = tkl::navStep(s.nav, v, tick, false, false);
        b = static_cast<uint16_t>(0xffffu & ~o.mask);
        if (o.aborted)
        {
            std::fprintf(stderr, "[netsync] nav aborted at tick=%llu after %d presses\n", (unsigned long long)tick,
                         s.nav.presses);
            b = phys;
            endSession(tick, "nav aborted", "Quick Play couldn't open Happiness - pick any event");
        }
        else if (o.done)
        {
            std::fprintf(stderr, "[netsync] nav done at tick=%llu (%s, %d presses)\n", (unsigned long long)tick,
                         s.nav.handedOff ? "handoff" : "backout", s.nav.presses);
            if (s.nav.handedOff)
                setPhase(Phase::Rules, tick, "event accepted");
            else
                endSession(tick, "nav backed out", "Quick Play cancelled");
        }
        return;
    }
    case Phase::Rules:
    {
        // My Rules: Continue, then loading. No presses while loading or in
        // the game app (an X at the card would start the race unsynced).
        b = 0xffffu;
        if (ps2_ssx3_course::appUpdateFn(ram) == ps2_ssx3_course::kGameUpdate)
        {
            setPhase(Phase::Card, tick, "race loaded");
            return;
        }
        if (tick - s.phaseTick > kRulesTimeoutTicks)
        {
            endSession(tick, "rules timeout", "Quick Play couldn't start the event");
            return;
        }
        if (tick < s.holdUntil)
        {
            b = static_cast<uint16_t>(b & ~kPadCross);
            return;
        }
        if (loadingNow(ram) || tick < s.nextPress)
            return;
        if (++s.presses > 8)
        {
            endSession(tick, "rules presses spent", "Quick Play couldn't start the event");
            return;
        }
        s.holdUntil = tick + kPressHoldTicks;
        s.nextPress = tick + kRulesPressEvery;
        b = static_cast<uint16_t>(b & ~kPadCross);
        std::fprintf(stderr, "[netsync] rules press %d at tick=%llu\n", s.presses, (unsigned long long)tick);
        return;
    }
    case Phase::Card:
    case Phase::Search:
    case Phase::Starting:
    {
        // The card waits for GO: the guest never sees X, Triangle or Start.
        b = static_cast<uint16_t>(b | kPadCross | kPadTriangle | kPadStart);
        uint32_t clock = 0u;
        const bool card = raceLoaded(ram, clock);
        if (s.phase == Phase::Card)
        {
            if (!card || (s.cardSince && clock != s.cardClock))
            {
                s.cardSince = 0u; // not loaded yet, or the clock moved: start over
                if (tick - s.phaseTick > kCardTimeoutTicks)
                    endSession(tick, "no card", "Quick Play couldn't reach the start");
                return;
            }
            if (!s.cardSince)
            {
                s.cardSince = tick;
                s.cardClock = clock;
                std::fprintf(stderr, "[netsync] card live at tick=%llu clock=%u\n", (unsigned long long)tick, clock);
            }
            if (tick - s.cardSince >= kCardSettleTicks)
            {
                s.searchUs = wallUs();
                x.wantJoin = true;
                setPhase(Phase::Search, tick, "card settled");
                ps2x::ui::toast("Finding a rider... (Triangle: race the AI)", 6.0f);
            }
            return;
        }
        if (s.phase == Phase::Search)
        {
            const uint64_t waited = (wallUs() - s.searchUs) / 1000u;
            if (triEdge)
                endSession(tick, "player chose AI", "Racing the AI - press X");
            else if (waited > kSearchTimeoutMs)
                endSession(tick, "search timeout", "No riders found - press X to race the AI");
            else if (x.lastRxUs.load() == 0u && waited > kSilentServerMs)
                endSession(tick, "server silent", "Quick Play server not answering - press X to race the AI");
            return;
        }
        // Starting: GO on the shared instant.
        const uint64_t now = wallUs();
        if (now >= s.goAtUs)
        {
            s.goPressTick = tick;
            s.goPressUs = now;
            s.goPresses = 1;
            setPhase(Phase::Going, tick, "GO");
            s.holdUntil = tick + kPressHoldTicks;
            b = static_cast<uint16_t>((b | kPadDown) & ~kPadCross);
            std::fprintf(stderr, "[netsync] GO press at tick=%llu wall_us=%llu late_us=%lld\n",
                         (unsigned long long)tick, (unsigned long long)now, (long long)(now - s.goAtUs));
        }
        return;
    }
    case Phase::Going:
    {
        b = static_cast<uint16_t>(b | kPadTriangle | kPadStart);
        if (tick < s.holdUntil)
        {
            b = static_cast<uint16_t>((b | kPadDown) & ~kPadCross);
            return;
        }
        b = static_cast<uint16_t>(b | kPadCross);
        if (tick - s.goPressTick > kGoRetryTicks * (uint64_t)s.goPresses)
        {
            if (s.goPresses >= 3)
            {
                endSession(tick, "GO did not start the race", "Quick Play: press X to start");
                return;
            }
            ++s.goPresses;
            s.holdUntil = tick + kPressHoldTicks;
            b = static_cast<uint16_t>((b | kPadDown) & ~kPadCross);
            std::fprintf(stderr, "[netsync] GO re-press %d at tick=%llu\n", s.goPresses, (unsigned long long)tick);
        }
        return;
    }
    default:
        return;
    }
}

// Reads the local rider's sample.
inline bool readSample(const uint8_t *r, uint32_t actor, Sample &s, uint64_t tick)
{
    s.tick = tick;
    if (!readTrip(r, actor + kPosOff, s.pos))
        return false;
    if (!readF32(r, actor + kQuatOff, s.qx) || !readF32(r, actor + kQuatOff + 4u, s.qy) ||
        !readF32(r, actor + kQuatOff + 8u, s.qz) || !readF32(r, actor + kQuatOff + 12u, s.qw))
        return false;
    if (!readTrip(r, actor + kVelOff, s.vel))
        return false;
    readF32(r, actor + 0x2f8u, s.boost);
    uint32_t u = 0u;
    if (readU32(r, actor + 0x2f4u, u))
        s.uber = (uint8_t)(u & 0xffu);
    s.wipe = 0u;
    s.lap = 1u;
    readU32(r, actor + 0x5fcu, s.chk);
    readF32(r, actor + 0x4c0u, s.dist);
    uint32_t ctl = 0u, de4 = 99u;
    if (readU32(r, actor + kCtlOff, ctl) && ctl != 0u)
    {
        uint32_t d = 99u;
        if (readU32(r, (ctl & kRamMask) + kDe4Off, d))
            de4 = d;
    }
    s.ctl = (uint8_t)(de4 & 0xffu);
    s.fin = 0u;
    s.ftime = 0;
    return true;
}

// Writes one show sample into the ghost slot (slot field checked).
inline bool writeGhost(uint8_t *r, const Sample &s)
{
    uint32_t b = 0u, r0 = 0u, r1 = 0u, live = 99u;
    if (!resolveChain(r, b, r0) || !resolveSlot(r, b, kGhostRoster, r1) || !readU32(r, r1 + kSlotOff, live) ||
        live != (uint32_t)kGhostRoster)
        return false;
    return writeTrip(r, r1 + kPosOff, s.pos) && writeTrip(r, r1 + kPos2Off, s.pos) &&
           writeF32(r, r1 + kQuatOff, s.qx) && writeF32(r, r1 + kQuatOff + 4u, s.qy) &&
           writeF32(r, r1 + kQuatOff + 8u, s.qz) && writeF32(r, r1 + kQuatOff + 12u, s.qw) &&
           writeTrip(r, r1 + kVelOff, s.vel);
}

inline void maybeLog(uint64_t raceTick)
{
    Session &s = session();
    if (s.lastLogTick >= 0 && raceTick - (uint64_t)s.lastLogTick < 600u)
        return;
    s.lastLogTick = (int64_t)raceTick;
    std::fprintf(stderr,
                 "[netsync] race=%llu sent=%llu recvd=%llu stale=%llu applied=%llu kept=%llu show=%llu ring=%zu "
                 "alive=%d rtt=%uus\n",
                 (unsigned long long)raceTick, (unsigned long long)s.sent, (unsigned long long)s.recvd,
                 (unsigned long long)s.stale, (unsigned long long)s.applied, (unsigned long long)s.kept,
                 (unsigned long long)s.showWrites, s.ring.size(), s.peerAlive ? 1 : 0, io().rttUs.load());
}

// Race tick path (game thread, once per guest vsync tick from the full
// dispatch): race start, STATE send, ghost show, timeouts, race end.
inline void onTick(const uint8_t *rdramConst, uint64_t tick)
{
    if (!menuOn())
        return;
    Session &s = session();
    if (s.phase < Phase::Going || (int64_t)tick == s.lastTick)
        return;
    s.lastTick = (int64_t)tick;
    uint8_t *r = const_cast<uint8_t *>(rdramConst);
    drainInbox(tick);
    if (s.phase < Phase::Going)
        return;
    Io &x = io();
    uint32_t b = 0u, r0 = 0u, clock = 0u;
    const bool chain = resolveChain(r, b, r0) && readU32(r, b + kClockOff, clock) && inRam(r0, 0x240u);
    if (s.phase == Phase::Going)
    {
        if (chain && clock != s.cardClock)
        {
            s.t0 = tick;
            s.lastClock = clock;
            const uint64_t now = wallUs();
            const uint32_t rtt = x.rttUs.load();
            std::fprintf(stderr,
                         "[netsync] race-go tick=%llu wall_us=%llu start_rx_to_go_us=%llu press_to_go_ticks=%llu "
                         "rtt=%uus\n",
                         (unsigned long long)tick, (unsigned long long)now,
                         (unsigned long long)(now - s.startRxUs), (unsigned long long)(tick - s.goPressTick), rtt);
            uint8_t p[21];
            encodeGo(p, s.start.startId, s.start.seq, rtt, (uint32_t)std::min<uint64_t>(now - s.startRxUs, 0xffffffffu),
                     tick);
            uint8_t room[16];
            {
                std::lock_guard<std::mutex> lock(x.mu);
                std::memcpy(room, x.room, 16);
            }
            ioSend(kKindQpGo, s.start.slot, p, sizeof(p), room);
            setPhase(Phase::Racing, tick, "race clock running");
        }
        return;
    }
    // Racing.
    if (!chain || clock < s.lastClock)
    {
        if (clock < s.lastClock && chain)
        {
            // After our finish the clock resets for the next event: over, not a restart.
            if (s.lastFinSent)
                endSession(tick, "race over", nullptr);
            else
                endSession(tick, "race restarted", "Quick Play ended - restart races the AI");
            return;
        }
        if (++s.lostTicks > kRaceLostTicks)
            endSession(tick, "race over", nullptr);
        return;
    }
    s.lostTicks = 0u;
    s.lastClock = clock;
    const uint64_t raceTick = tick - s.t0;
    uint8_t room[16];
    {
        std::lock_guard<std::mutex> lock(x.mu);
        std::memcpy(room, x.room, 16);
    }
    // Own STATE at cadence.
    if ((raceTick % knobs().every) == 0u && (int64_t)raceTick != s.lastSendTick)
    {
        Sample smp;
        if (readSample(r, r0, smp, raceTick))
        {
            uint32_t res = 0u;
            readU32(r, kResBase + (uint32_t)kOwnRoster * 4u, res);
            if (res != 0u && res != 0x09090909u)
            {
                smp.fin = 1u;
                smp.ftime = (float)res / 60.0f;
            }
            if (smp.fin != s.lastFinSent)
            {
                std::fprintf(stderr, "[netsync] fin edge=%u race=%llu ftime=%.3f res=0x%08x\n", smp.fin,
                             (unsigned long long)raceTick, (double)smp.ftime, res);
                s.lastFinSent = smp.fin;
            }
            uint8_t pay[kStateLen];
            encodeState(pay, s.start.slot, raceTick, smp, s.ownDe4, s.ownW0, s.ownW1, s.ownInTick, s.ghostFinTick,
                        s.ghostFinMs);
            if (ioSend(kKindState, s.start.slot, pay, sizeof(pay), room))
                ++s.sent;
        }
        s.lastSendTick = (int64_t)raceTick;
    }
    // Local timeout: no STATE for TIMEOUT_MS -> the slot is AI again.
    if (s.peerAlive && s.lastPeerUs && wallUs() - s.lastPeerUs > knobs().timeoutMs * 1000u)
    {
        s.peerAlive = false;
        std::fprintf(stderr, "[netsync] local timeout: slot %d back to AI at race=%llu\n", kGhostRoster,
                     (unsigned long long)raceTick);
        ps2x::ui::toast("Rider disconnected - the AI takes over", 4.0f);
    }
    uint64_t showTick = 0u;
    if (s.peerAlive && (int64_t)raceTick != s.lastShowTick &&
        showTickFor(s.ring, wallUs(), knobs().idelay, showTick))
    {
        Sample show;
        if (showSample(s.ring, showTick, show) && writeGhost(r, show))
        {
            ++s.showWrites;
            s.showHasInput = show.hasInput;
            s.showDe4 = show.de4;
            s.showW0 = show.w0;
            s.showW1 = show.w1;
        }
        s.lastShowTick = (int64_t)raceTick;
    }
    maybeLog(raceTick);
}

// 0x111728 (rider input consumer): cache the local rider's input, replace
// the ghost slot's input from the show sample (game thread).
inline bool onInput(uint64_t tick, uint32_t ctl, uint32_t rec, const uint8_t *r, uint32_t &repW0, uint32_t &repW1,
                    const char *&why)
{
    why = "off";
    if (!menuOn())
        return false;
    Session &s = session();
    if (s.phase != Phase::Racing)
        return false;
    if (!inRam(ctl, 0xdf0u) || !inRam(rec, 8u))
    {
        why = "bounds";
        return false;
    }
    uint32_t actor = 0u, de4 = 0u, w0 = 0u, w1 = 0u;
    std::memcpy(&actor, r + (ctl & kRamMask) + 0x18u, 4u);
    actor &= kRamMask;
    std::memcpy(&de4, r + (ctl & kRamMask) + kDe4Off, 4u);
    std::memcpy(&w0, r + (rec & kRamMask), 4u);
    std::memcpy(&w1, r + (rec & kRamMask) + 4u, 4u);
    int slot = -1;
    if (inRam(actor, 0x870u))
    {
        int32_t sv = 0;
        std::memcpy(&sv, r + (actor & kRamMask) + kSlotOff, 4u);
        if (sv >= 0 && sv <= 5)
            slot = sv;
    }
    if (slot == kOwnRoster)
    {
        s.ownDe4 = de4;
        s.ownW0 = w0;
        s.ownW1 = w1;
        s.ownInTick = tick >= s.t0 ? tick - s.t0 : 0u;
        why = "own-cached";
        return false;
    }
    if (slot != kGhostRoster || !s.peerAlive)
    {
        why = "not-peer";
        return false;
    }
    if (de4 == 9u || de4 == 10u || de4 == 13u)
    {
        why = "protected";
        ++s.kept;
        return false;
    }
    if (!s.showHasInput || s.showDe4 != de4)
    {
        why = s.showHasInput ? "state-mismatch" : "no-show";
        ++s.kept;
        return false;
    }
    repW0 = s.showW0;
    repW1 = s.showW1;
    why = "applied";
    ++s.applied;
    return true;
}

} // namespace ps2_netsync
