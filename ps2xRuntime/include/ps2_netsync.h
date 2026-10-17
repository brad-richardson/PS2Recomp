#pragma once
// netsync-spike-1 Part 3: state sync over SNP2 via the lobby state-sync
// endpoint (evidence only). Master switch PS2X_NETSYNC (default off).
// 120-only session (requires PS2X_SSX3_FULL120=events); remote rider is a
// solid AI slot driven by interpolated STATE (input replace + snap);
// timeout reverts the slot to AI. Send once per tick at 20 Hz.
// Knob off: one cached env check per dispatch, zero guest writes/logs.
#if defined(__APPLE__) || defined(__linux__) || defined(__ANDROID__)
#define PS2_NETSYNC_HAS_UDP 1
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#else
#define PS2_NETSYNC_HAS_UDP 0
#endif

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace ps2_netsync
{
// SNP2 framing (ssx3-proto netplay): len u32 | SNP2 | ver u16 | kind u8 |
// flags u8 | session[16] | epoch u64 | sender u16 | reserved u16 | seq u64,
// all LE; then payload. Loopback test session/epoch are zero.
constexpr uint32_t kRamSize = 32u * 1024u * 1024u;
constexpr uint32_t kRamMask = 0x01ffffffu;
constexpr size_t kHeader = 48u;
constexpr uint8_t kKindState = 0x14u;
constexpr uint8_t kKindEvent = 0x16u;
constexpr uint8_t kKindPing = 0x40u;
constexpr uint8_t kKindFinalize = 0x24u;
constexpr uint8_t kKindResult = 0x32u;
constexpr uint8_t kKindQuit = 0x30u;
constexpr uint8_t kEvDropToAi = 1u;
constexpr size_t kStateLen = 111u; // core 91B + input tail 20B, digest covers all
// Rider offsets (Part-1 map; see REPORT).
constexpr uint32_t kChainRoot = 0x4a28a8u;
constexpr uint32_t kChainAOff = 0x84u;
constexpr uint32_t kChainBOff = 0x0cu;
constexpr uint32_t kChainROff = 0x28u;
constexpr uint32_t kPosOff = 0x110u;
constexpr uint32_t kQuatOff = 0x120u;
constexpr uint32_t kPos2Off = 0x1d0u;
constexpr uint32_t kVelOff = 0x1e0u;
constexpr uint32_t kCtlOff = 0x77cu;
constexpr uint32_t kDe4Off = 0xde4u;
constexpr uint32_t kSlotOff = 0x86cu;
constexpr uint32_t kResBase = 0x536640u;

struct F32Trip {
    float x = 0, y = 0, z = 0;
};

struct Sample {
    uint64_t tick = 0u;
    // state fields
    F32Trip pos, vel;
    float qx = 0, qy = 0, qz = 0, qw = 1;
    float boost = 0;
    uint8_t uber = 0, wipe = 0, ctl = 0, lap = 0, fin = 0;
    uint32_t chk = 0u;
    float dist = 0, ftime = 0;
    uint64_t peerTick = 0u;
    uint32_t peerMs = 0u;
    // input for the slot at state tick
    uint32_t de4 = 99u, w0 = 0u, w1 = 0u;
    bool hasInput = false;
};

struct Config {
    bool loaded = false;
    bool enabled = false;
    bool full120 = false;
    int sock = -1;
    struct sockaddr_in relay = {};
    std::string token;
    int slot = 0; // own session slot
    int peer = 1; // ghost AI slot
    uint64_t every = 6u;  // 20 Hz @120Hz ticks
    uint64_t idelay = 12u; // ~2 updates @120Hz
    uint64_t timeoutMs = 3000u;
    uint64_t seq = 1u;
    uint64_t sent = 0u, recvd = 0u, stale = 0u;
    uint64_t applied = 0u, kept = 0u, showWrites = 0u;
    uint64_t chainFail = 0u, sampleFail = 0u; // send-gate diagnostics
    uint8_t lastFinSent = 0u; // fin edge detector for finish forensics
    int64_t lastSendTick = -1, lastShowTick = -1, lastLogTick = -1;
    int64_t joinOff = 0;
    bool joined = false;
    std::vector<Sample> ring;
    uint32_t ownDe4 = 99u, ownW0 = 0u, ownW1 = 0u;
    uint64_t ownInTick = 0u;
    bool ownInValid = false;
    Sample show;
    bool showValid = false;
    uint32_t showDe4 = 99u, showW0 = 0u, showW1 = 0u;
    uint64_t showInTick = 0u;
    bool showHasInput = false;
    // ghost finish latch (peer's view of our ghost)
    bool ghostFin = false;
    uint64_t ghostFinTick = 0u;
    uint32_t ghostFinMs = 0u;
    // server result
    bool hasResult = false;
    uint32_t canonMs[6] = {0u, 0u, 0u, 0u, 0u, 0u};
    uint8_t canonRank[6] = {0u, 0u, 0u, 0u, 0u, 0u};
    bool resultApplied = false;
    uint64_t lastPeerMs = 0u; // wall ms of last STATE (timeout)
    bool peerAlive = false;
    bool dropLogged = false;
};

inline Config &config()
{
    static Config cfg;
    return cfg;
}

inline uint64_t wallMs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

inline bool inRam(uint32_t addr, size_t bytes)
{
    if (bytes == 0u || bytes > kRamSize)
        return false;
    uint32_t off = addr & kRamMask;
    return (size_t)off <= (size_t)kRamSize - bytes;
}

inline bool readU32(const uint8_t *r, uint32_t a, uint32_t &o)
{
    if (!inRam(a, 4u))
        return false;
    memcpy(&o, r + (a & kRamMask), 4u);
    return true;
}

inline bool readF32(const uint8_t *r, uint32_t a, float &o)
{
    uint32_t b = 0u;
    if (!readU32(r, a, b))
        return false;
    memcpy(&o, &b, 4u);
    return true;
}

inline bool writeU32(uint8_t *r, uint32_t a, uint32_t v)
{
    if (!inRam(a, 4u))
        return false;
    memcpy(r + (a & kRamMask), &v, 4u);
    return true;
}

inline bool writeF32(uint8_t *r, uint32_t a, float v)
{
    if (!inRam(a, 4u))
        return false;
    memcpy(r + (a & kRamMask), &v, 4u);
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
inline void putF32(uint8_t *d, float v) { memcpy(d, &v, 4u); }
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
    memcpy(&v, d, 4u);
    return v;
}

// Build one SNP2 frame around payload. Returns length (0 = no UDP).
inline size_t buildFrame(uint8_t *out, size_t cap, uint8_t kind, uint16_t sender, uint64_t seq,
                         const uint8_t *payload, size_t plen)
{
    if (kHeader + plen > cap || seq == 0u)
        return 0u;
    uint32_t body = (uint32_t)(44u + plen);
    putU32(out, body);
    memcpy(out + 4, "SNP2", 4);
    putU16(out + 8, 2u);
    out[10] = kind;
    out[11] = 0u;
    memset(out + 12, 0, 16);
    putU64(out + 28, 0u);
    putU16(out + 36, sender);
    putU16(out + 38, 0u);
    putU64(out + 40, seq);
    memcpy(out + 48, payload, plen);
    return kHeader + plen;
}

inline void loadOnce()
{
    Config &cfg = config();
    if (cfg.loaded)
        return;
    cfg.loaded = true;
    const char *m = std::getenv("PS2X_NETSYNC");
    if (!m || !*m || strcmp(m, "0") == 0 || strcmp(m, "off") == 0)
        return;
    if (strcmp(m, "1") != 0 && strcmp(m, "on") != 0)
    {
        fprintf(stderr, "[netsync] unknown PS2X_NETSYNC='%s'; disabled\n", m);
        return;
    }
    const char *f120 = std::getenv("PS2X_SSX3_FULL120");
    cfg.full120 = f120 && strcmp(f120, "events") == 0;
    if (!cfg.full120)
    {
        fprintf(stderr, "[netsync] 120-only session: needs PS2X_SSX3_FULL120=events; disabled\n");
        return;
    }
    const char *r = std::getenv("PS2X_NETSYNC_RELAY");
    std::string host = "127.0.0.1";
    uint16_t port = 36595u;
    if (r && *r)
    {
        std::string s = r;
        size_t c = s.find(':');
        if (c != std::string::npos)
        {
            host = s.substr(0, c);
            port = (uint16_t)atoi(s.substr(c + 1).c_str());
        }
        else
            host = s;
    }
    const char *tk = std::getenv("PS2X_NETSYNC_TOKEN");
    cfg.token = (tk && *tk) ? tk : "";
    const char *sl = std::getenv("PS2X_NETSYNC_SLOT");
    if (sl && *sl)
        cfg.slot = atoi(sl);
    const char *pe = std::getenv("PS2X_NETSYNC_PEER");
    cfg.peer = (pe && *pe) ? atoi(pe) : 1 - cfg.slot;
    const char *ev = std::getenv("PS2X_NETSYNC_EVERY");
    if (ev && *ev)
        cfg.every = strtoull(ev, nullptr, 0);
    if (cfg.every < 1u)
        cfg.every = 1u;
    const char *id = std::getenv("PS2X_NETSYNC_IDELAY");
    if (id && *id)
        cfg.idelay = strtoull(id, nullptr, 0);
    const char *to = std::getenv("PS2X_NETSYNC_TIMEOUT_MS");
    if (to && *to)
        cfg.timeoutMs = strtoull(to, nullptr, 0);
#if PS2_NETSYNC_HAS_UDP
    cfg.sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (cfg.sock < 0)
    {
        fprintf(stderr, "[netsync] socket failed; disabled\n");
        return;
    }
    int fl = ::fcntl(cfg.sock, F_GETFL, 0);
    ::fcntl(cfg.sock, F_SETFL, fl | O_NONBLOCK);
    memset(&cfg.relay, 0, sizeof(cfg.relay));
    cfg.relay.sin_family = AF_INET;
    cfg.relay.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &cfg.relay.sin_addr) != 1)
    {
        fprintf(stderr, "[netsync] bad relay addr; disabled\n");
        ::close(cfg.sock);
        cfg.sock = -1;
        return;
    }
    if (cfg.token.empty())
    {
        fprintf(stderr, "[netsync] PS2X_NETSYNC_TOKEN required; disabled\n");
        ::close(cfg.sock);
        cfg.sock = -1;
        return;
    }
    // enroll: raw token hello (loopback test auth)
    ::sendto(cfg.sock, cfg.token.data(), cfg.token.size(), 0,
             (struct sockaddr *)&cfg.relay, sizeof(cfg.relay));
#else
    fprintf(stderr, "[netsync] no UDP on this platform; disabled\n");
    return;
#endif
    cfg.enabled = true;
    fprintf(stderr, "[netsync] on relay=%s:%u slot=%d peer=%d every=%llu idelay=%llu timeout=%llums\n",
            host.c_str(), port, cfg.slot, cfg.peer, (unsigned long long)cfg.every,
            (unsigned long long)cfg.idelay, (unsigned long long)cfg.timeoutMs);
}

// Read one rider's ghost sample from guest memory.
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
        ctl &= kRamMask;
        uint32_t d = 99u;
        if (readU32(r, ctl + kDe4Off, d))
            de4 = d;
    }
    s.ctl = (uint8_t)(de4 & 0xffu);
    s.fin = 0u;
    s.ftime = 0;
    return true;
}

// Encode one KIND_STATE payload (111B, byte-identical to the spec).
// Input words ride inside the digest-covered region.
inline void encodeState(uint8_t *o, uint16_t slot, uint64_t tick, const Sample &s,
                        uint32_t de4, uint32_t w0, uint32_t w1, uint64_t inTick,
                        uint64_t peerTick, uint32_t peerMs)
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

// Decode one STATE frame payload+tail into a sample. Returns false on any
// shape/digest/slot violation (caller counts).
inline bool decodeState(const uint8_t *p, size_t n, Sample &s, uint32_t &de4, uint32_t &w0,
                        uint32_t &w1, uint64_t &inTick, uint16_t &slot)
{
    if (n != kStateLen)
        return false;
    if (fnv64(p, 103u) != getU64(p + 103))
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

// Apply one interpolated show sample to the ghost slot. Slot-field checked.
inline bool writeGhost(uint8_t *r, int peer, const Sample &s)
{
    uint32_t b = 0u, r1 = 0u, bb = 0u, r0 = 0u;
    (void)bb;
    (void)r0;
    if (!resolveChain(r, b, r0) || !resolveSlot(r, b, peer, r1))
        return false;
    uint32_t live = 99u;
    if (!readU32(r, r1 + kSlotOff, live) || live != (uint32_t)(peer < 0 ? 99 : peer))
        return false;
    return writeTrip(r, r1 + kPosOff, s.pos) && writeTrip(r, r1 + kPos2Off, s.pos) &&
           writeF32(r, r1 + kQuatOff, s.qx) && writeF32(r, r1 + kQuatOff + 4u, s.qy) &&
           writeF32(r, r1 + kQuatOff + 8u, s.qz) && writeF32(r, r1 + kQuatOff + 12u, s.qw) &&
           writeTrip(r, r1 + kVelOff, s.vel);
}

// One-line periodic status (applied/mismatch + send/recv rates).
inline void maybeLog(uint64_t tick)
{
    Config &cfg = config();
    if (cfg.lastLogTick >= 0 && tick - (uint64_t)cfg.lastLogTick < 600u)
        return;
    cfg.lastLogTick = (int64_t)tick;
    fprintf(stderr,
            "[netsync] tick=%llu sent=%llu recvd=%llu stale=%llu applied=%llu kept=%llu show=%llu ring=%zu alive=%d chainfail=%llu samplefail=%llu\n",
            (unsigned long long)tick, (unsigned long long)cfg.sent, (unsigned long long)cfg.recvd,
            (unsigned long long)cfg.stale, (unsigned long long)cfg.applied,
            (unsigned long long)cfg.kept, (unsigned long long)cfg.showWrites, cfg.ring.size(),
            cfg.peerAlive ? 1 : 0, (unsigned long long)cfg.chainFail,
            (unsigned long long)cfg.sampleFail);
}

// RESULT application: write canonical times into guest result rows when the
// local race is over (clock frozen), else defer. Returns applied?.
inline bool maybeApplyResult(uint8_t *r, uint64_t tick)
{
    Config &cfg = config();
    if (!cfg.hasResult || cfg.resultApplied)
        return cfg.resultApplied;
    uint32_t b = 0u, r0 = 0u;
    if (!resolveChain(r, b, r0))
        return false;
    static uint32_t prevClock = 0u;
    static uint64_t stillSince = 0u;
    uint32_t clock = 0u;
    if (!readU32(r, b + 0xcu, clock))
        return false;
    if (clock != prevClock)
    {
        prevClock = clock;
        stillSince = tick;
        return false;
    }
    if (tick - stillSince < 600u)
        return false; // race still running; defer
    for (int s = 0; s < 2; ++s)
    {
        uint32_t rowTicks = cfg.canonMs[s] * 60u / 1000u;
        writeU32(r, kResBase + (uint32_t)s * 4u, rowTicks);
    }
    cfg.resultApplied = true;
    fprintf(stderr, "[netsync] RESULT applied canonical (ms,rank)=(%u,%u)+(%u,%u)\n",
            cfg.canonMs[0], cfg.canonRank[0], cfg.canonMs[1], cfg.canonRank[1]);
    return true;
}

// Tick path: drain socket, send own STATE, write interpolated ghost.
inline void onTick(const uint8_t *rdramConst, uint64_t tick)
{
    loadOnce();
    Config &cfg = config();
    if (!cfg.enabled)
        return;
    uint8_t *r = const_cast<uint8_t *>(rdramConst);
#if PS2_NETSYNC_HAS_UDP
    uint8_t dgram[2048];
    for (;;)
    {
        ssize_t n = ::recv(cfg.sock, dgram, sizeof(dgram), 0);
        if (n <= 0)
            break;
        if ((size_t)n < kHeader + 1)
        {
            ++cfg.stale;
            continue;
        }
        // validate SNP2 header (strict subset of decode_frame)
        if (getU32(dgram) + 4u != (uint32_t)n || memcmp(dgram + 4, "SNP2", 4) != 0 ||
            getU16(dgram + 8) != 2u || dgram[11] != 0u || getU64(dgram + 40) == 0u)
        {
            ++cfg.stale;
            continue;
        }
        uint8_t kind = dgram[10];
        const uint8_t *pay = dgram + kHeader;
        size_t plen = (size_t)n - kHeader;
        if (kind == kKindState)
        {
            Sample s;
            uint32_t de4 = 99u, w0 = 0u, w1 = 0u;
            uint64_t inTick = 0u;
            uint16_t slot = 99u;
            if (!decodeState(pay, plen, s, de4, w0, w1, inTick, slot) || (int)slot == cfg.slot)
            {
                ++cfg.stale;
                continue;
            }
            if (!cfg.joined)
            {
                cfg.joined = true;
                cfg.joinOff = (int64_t)s.tick - (int64_t)tick;
                fprintf(stderr, "[netsync] join local=%llu remote=%llu off=%lld\n",
                        (unsigned long long)tick, (unsigned long long)s.tick,
                        (long long)cfg.joinOff);
            }
            if (!cfg.ring.empty() && s.tick <= cfg.ring.back().tick)
            {
                ++cfg.stale;
                continue;
            }
            cfg.ring.push_back(s);
            if (cfg.ring.size() > 64u)
                cfg.ring.erase(cfg.ring.begin());
            ++cfg.recvd;
            cfg.lastPeerMs = wallMs();
            cfg.peerAlive = true;
            cfg.dropLogged = false;
            // latch ghost finish (peer's view Latched once)
            if (s.fin != 0 && !cfg.ghostFin)
            {
                cfg.ghostFin = true;
                cfg.ghostFinTick = s.tick;
                cfg.ghostFinMs = (uint32_t)(s.ftime * 1000.0f);
            }
        }
        else if (kind == kKindEvent)
        {
            // standalone Event: event_seq u64 + type u8 + drop(12B)
            if (plen >= 21u && pay[8] == kEvDropToAi)
            {
                uint16_t dslot = getU16(pay + 9);
                uint16_t reason = getU16(pay + 11);
                if ((int)dslot == cfg.peer)
                {
                    cfg.peerAlive = false;
                    fprintf(stderr, "[netsync] DROP_TO_AI slot=%u reason=%u tick=%llu\n",
                            dslot, reason, (unsigned long long)tick);
                }
            }
        }
        else if (kind == kKindFinalize || kind == kKindResult)
        {
            // FINALIZE: boundary u64 (payload[0..8]); RESULT rows follow
            // NetResult layout: result_id[32], boundary u64, ... rows at
            // [145] count-prefixed 25B rows (slot,actor,disp,rank,tick u64,ms u32,score i64).
            if (kind == kKindResult && plen >= 146u)
            {
                uint8_t nrows = pay[145];
                size_t off = 146u;
                for (uint8_t i = 0u; i < nrows && off + 25u <= plen; ++i)
                {
                    uint16_t rs = getU16(pay + off);
                    uint8_t disp = pay[off + 3];
                    uint8_t rank = pay[off + 4];
                    uint32_t ms = getU32(pay + off + 13);
                    if (rs < 6u)
                    {
                        cfg.canonMs[rs] = (disp == 0u) ? ms : 0u;
                        cfg.canonRank[rs] = rank;
                    }
                    off += 25u;
                }
                cfg.hasResult = true;
                fprintf(stderr, "[netsync] RESULT stored rows=%u\n", nrows);
            }
        }
        // other kinds (PING/PONG/QUIT/CLOSE) need nothing on this endpoint
    }
    // send own STATE at cadence (once per tick)
    uint32_t b = 0u, r0 = 0u;
    if ((tick % cfg.every) == 0u && (int64_t)tick != cfg.lastSendTick)
    {
        if (!resolveChain(r, b, r0) || !inRam(r0, 0x240u))
            ++cfg.chainFail;
        else
        {
            Sample s;
            // Local rider is always game-slot 0 (single-player engine);
            // the net slot only labels the frame, never the actor.
            uint32_t actor = r0;
            if (!readSample(r, actor, s, tick))
                ++cfg.sampleFail;
            else
            {
                uint8_t pay[kStateLen];
            // finish latch: own res row filled + ramped => fin
            uint32_t res = 0u;
            uint32_t finSt = 0u;
            readU32(r, kResBase + (uint32_t)cfg.slot * 4u, res);
            readU32(r, actor + 0x470u, finSt);
            if (res != 0u && res != 0x09090909u)
            {
                s.fin = 1u;
                s.ftime = (float)res / 60.0f;
            }
            if (s.fin != cfg.lastFinSent)
            {
                fprintf(stderr, "[netsync] fin edge=%u tick=%llu ftime=%.3f res=0x%08x\n",
                        s.fin, (unsigned long long)tick, (double)s.ftime, res);
                cfg.lastFinSent = s.fin;
            }
            encodeState(pay, (uint16_t)cfg.slot, tick, s, cfg.ownDe4, cfg.ownW0, cfg.ownW1,
                        cfg.ownInTick, cfg.ghostFinTick, cfg.ghostFinMs);
            uint8_t frame[2048];
            size_t fl = buildFrame(frame, sizeof(frame), kKindState, (uint16_t)cfg.slot, cfg.seq,
                                   pay, sizeof(pay));
            if (fl > 0u &&
                ::sendto(cfg.sock, frame, fl, 0, (struct sockaddr *)&cfg.relay, sizeof(cfg.relay)) ==
                    (ssize_t)fl)
            {
                ++cfg.sent;
                ++cfg.seq;
            }
            cfg.lastSendTick = (int64_t)tick;
            }
        }
    }
#endif
    // local timeout: no STATE for TIMEOUT_MS => revert to AI
    if (cfg.peerAlive && cfg.lastPeerMs != 0u && wallMs() - cfg.lastPeerMs > cfg.timeoutMs)
    {
        cfg.peerAlive = false;
        if (!cfg.dropLogged)
        {
            cfg.dropLogged = true;
            fprintf(stderr, "[netsync] local timeout %.0fms: slot %d back to AI at tick=%llu\n",
                    (double)cfg.timeoutMs, cfg.peer, (unsigned long long)tick);
        }
    }
    // show
    if (cfg.joined && cfg.peerAlive && !cfg.ring.empty())
    {
        int64_t want = (int64_t)tick + cfg.joinOff - (int64_t)cfg.idelay;
        if (want >= 0)
        {
            uint64_t showTick = (uint64_t)want;
            const Sample *lo = nullptr, *hi = nullptr;
            for (size_t i = 0u; i < cfg.ring.size(); ++i)
            {
                if (cfg.ring[i].tick <= showTick)
                    lo = &cfg.ring[i];
                else
                {
                    hi = &cfg.ring[i];
                    break;
                }
            }
            if (lo && (int64_t)tick != cfg.lastShowTick)
            {
                Sample s = *lo;
                if (hi && hi->tick > lo->tick)
                {
                    float a = float(showTick - lo->tick) / float(hi->tick - lo->tick);
                    if (a < 0)
                        a = 0;
                    if (a > 1)
                        a = 1;
                    s.pos.x += (hi->pos.x - lo->pos.x) * a;
                    s.pos.y += (hi->pos.y - lo->pos.y) * a;
                    s.pos.z += (hi->pos.z - lo->pos.z) * a;
                    float qx = lo->qx + (hi->qx - lo->qx) * a;
                    float qy = lo->qy + (hi->qy - lo->qy) * a;
                    float qz = lo->qz + (hi->qz - lo->qz) * a;
                    float qw = lo->qw + (hi->qw - lo->qw) * a;
                    float n = sqrtf(qx * qx + qy * qy + qz * qz + qw * qw);
                    if (n > 1e-6f)
                    {
                        s.qx = qx / n;
                        s.qy = qy / n;
                        s.qz = qz / n;
                        s.qw = qw / n;
                    }
                    if (hi->hasInput && hi->tick <= showTick)
                    {
                        s.de4 = hi->de4;
                        s.w0 = hi->w0;
                        s.w1 = hi->w1;
                        s.hasInput = hi->hasInput;
                    }
                }
                // dead-reckon cap: hold + project at most 30 ticks past newest
                // Rival is always game-slot 1, never the peer's net slot.
                if (writeGhost(r, 1, s))
                {
                    ++cfg.showWrites;
                    cfg.show = s;
                    cfg.showDe4 = s.de4;
                    cfg.showW0 = s.w0;
                    cfg.showW1 = s.w1;
                    cfg.showInTick = s.tick;
                    cfg.showHasInput = s.hasInput;
                }
                cfg.lastShowTick = (int64_t)tick;
            }
        }
    }
    maybeApplyResult(r, tick);
    maybeLog(tick);
}

// 0x111728 path: cache own input; replace peer input from show sample.
inline bool onInput(uint64_t tick, uint32_t ctl, uint32_t rec, const uint8_t *rdramConst,
                    uint32_t &repW0, uint32_t &repW1, const char *&why)
{
    loadOnce();
    Config &cfg = config();
    why = "off";
    if (!cfg.enabled)
        return false;
    uint8_t *r = const_cast<uint8_t *>(rdramConst);
    if (!inRam(ctl, 0xdf0u) || !inRam(rec, 8u))
    {
        why = "bounds";
        return false;
    }
    uint32_t actor = 0u, de4 = 0u, w0 = 0u, w1 = 0u;
    memcpy(&actor, r + (ctl & kRamMask) + 0x18u, 4u);
    actor &= kRamMask;
    memcpy(&de4, r + (ctl & kRamMask) + kDe4Off, 4u);
    memcpy(&w0, r + (rec & kRamMask), 4u);
    memcpy(&w1, r + (rec & kRamMask) + 4u, 4u);
    int s = -1;
    if (inRam(actor, 0x870u))
    {
        int32_t sv = 0;
        memcpy(&sv, r + (actor & kRamMask) + kSlotOff, 4u);
        if (sv >= 0 && sv <= 5)
            s = sv;
    }
    // Game-slot view (2-rider net race): the single-player engine always
    // writes the local pad to game-slot 0, and the net peer's rider is the
    // rival at game-slot 1. Net slots identify devices on the wire only and
    // must never select the actor: on net slot != 0, s == cfg.slot never
    // fires for the local pad while s == cfg.peer collides with it, so both
    // pads ended up driving rider0. (Part 5: iPad drove the phone's rider.)
    if (s == 0)
    {
        cfg.ownDe4 = de4;
        cfg.ownW0 = w0;
        cfg.ownW1 = w1;
        cfg.ownInTick = tick;
        cfg.ownInValid = true;
        why = "own-cached";
        return false;
    }
    if (s != 1 || !cfg.peerAlive)
    {
        why = "not-peer";
        return false;
    }
    if (de4 == 9u || de4 == 10u || de4 == 13u)
    {
        why = "protected";
        ++cfg.kept;
        return false;
    }
    if (!cfg.showHasInput)
    {
        why = "no-show";
        ++cfg.kept;
        return false;
    }
    if (cfg.showDe4 != de4)
    {
        why = "state-mismatch";
        ++cfg.kept;
        return false;
    }
    repW0 = cfg.showW0;
    repW1 = cfg.showW1;
    why = "applied";
    ++cfg.applied;
    return true;
}

} // namespace ps2_netsync
