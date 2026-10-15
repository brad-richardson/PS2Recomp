// np-link-1 (SRVP1 P1a): delay-based lockstep client, SSXNP/2-P1a subset.
// See ps2_net_link.h and REPORT §4a/§4b. All knobs default off; knob-off
// behavior is byte-identical to today.
#include "ps2_net_link.h"

#include "runtime/ps2_savestate.h" // ps2_savestate::sha256Hex (chain verify)

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ps2x
{
namespace nplink
{
namespace
{

// SSXNP v2 framing (loopback P1a profile; see the relay crate for the twin
// implementation and wire-vectors.json fixtures for Package 1).
constexpr uint16_t kVersion = 2u;
constexpr uint8_t kMagic[4] = {'S', 'N', 'P', '2'};
constexpr size_t kHeader = 48u;
constexpr uint32_t kMaxFrame = 16384u;
constexpr uint8_t kJoin = 0x01u, kConfig = 0x02u, kReady = 0x03u, kStart = 0x04u;
constexpr uint8_t kInput = 0x10u, kCommit = 0x11u;
constexpr uint16_t kRelaySender = 0xfffeu;
constexpr uint8_t kProfile = 1u; // P1a diagnostic profile
constexpr uint32_t kSchema = 1u;
constexpr uint8_t kStateAlg = 1u;
constexpr uint16_t kHashEvery = 120u;
constexpr uint16_t kLagLimit = 30u;
constexpr uint32_t kRosterRev = 1u;
constexpr uint8_t kPad6 = 0u, kRider12 = 1u;
constexpr uint32_t kSentinelDe4 = 0xFFFFFFFFu;

struct PadTuple
{
    uint16_t buttons = 0xFFFFu;
    uint8_t rx = 0x80u, ry = 0x80u, lx = 0x80u, ly = 0x80u;
};

struct RiderTuple
{
    uint32_t de4 = kSentinelDe4;
    uint32_t w0 = 0u, w1 = 0u;
};

struct State
{
    bool configured = false;
    bool ok = false;
    bool started = false; // START received: INPUTs may flow
    bool roleA = true;
    uint16_t slot = 0u;
    std::string relayHost = "127.0.0.1";
    uint16_t relayPort = 0u;
    uint64_t firstTick = 0u;
    uint64_t delay = 8u;
    int riderSlot = -1;
    std::map<uint64_t, PadTuple> aScript;
    std::map<uint64_t, RiderTuple> bScript;
    FILE *commitLog = nullptr;
    FILE *sentLog = nullptr;

    std::mutex netMutex;
    std::deque<std::vector<uint8_t>> sendQueue; // framed INPUT payloads (header added on send)
    uint64_t nextMsgSeq = 1u;                   // client message_seq (JOIN=1, READY=2, INPUTs from 3)
    uint64_t relaySeq = 0u;                     // last accepted relay frame seq
    uint64_t nextCommitSeq = 1u;                 // next expected COMMIT commit_seq

    std::mutex commitMutex;
    std::condition_variable commitCv;
    struct StoredCommit
    {
        Commit commit;
        uint8_t chain[32] = {0};
        uint64_t seq = 0u;
    };
    std::map<uint64_t, StoredCommit> commits;
    uint8_t lastChain[32] = {0};
    uint64_t sendUpTo = 0u;
    uint64_t lastWaited = 0u;
    Commit lastCommit;
    bool haveLast = false;
    uint64_t appliedTicks = 0u;
    uint64_t stallTicks = 0u;
    uint64_t stallWaits = 0u;
    uint64_t lastHeartbeat = 0u;

    int sock = -1;
};

State &state()
{
    static State s;
    return s;
}

std::once_flag &configOnce()
{
    static std::once_flag f;
    return f;
}

std::atomic<bool> &enabledCache()
{
    static std::atomic<bool> v{false};
    static std::once_flag f;
    std::call_once(f, [] {
        const char *e = std::getenv("PS2X_NP_LINK_ENABLE");
        v.store(e && std::string(e) == "1", std::memory_order_relaxed);
    });
    return v;
}

[[noreturn]] void fatal(const char *what)
{
    std::fprintf(stderr, "[np-link] FATAL: %s\n", what);
    std::fflush(stderr);
    std::exit(3);
}

bool readAll(int fd, uint8_t *buf, size_t n)
{
    size_t got = 0u;
    while (got < n)
    {
        ssize_t r = ::read(fd, buf + got, n - got);
        if (r <= 0)
            return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

bool writeAll(int fd, const uint8_t *buf, size_t n)
{
    size_t done = 0u;
    while (done < n)
    {
        ssize_t w = ::write(fd, buf + done, n - done);
        if (w <= 0)
            return false;
        done += static_cast<size_t>(w);
    }
    return true;
}

std::string stripAt(const char *spec)
{
    std::string p = spec ? spec : "";
    if (!p.empty() && p[0] == '@')
        p = p.substr(1);
    return p;
}

void putU16(std::vector<uint8_t> &v, uint16_t x)
{
    v.push_back(static_cast<uint8_t>(x & 0xFFu));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFFu));
}

void putU64(std::vector<uint8_t> &v, uint64_t x)
{
    for (int i = 0; i < 8; ++i)
        v.push_back(static_cast<uint8_t>((x >> (8 * i)) & 0xFFu));
}

// Frame a payload with the v2 header (session/epoch are P1a zeros).
std::vector<uint8_t> frame(uint8_t kind, uint16_t sender, uint64_t seq,
                           const std::vector<uint8_t> &payload)
{
    std::vector<uint8_t> f;
    uint32_t frameBytes = static_cast<uint32_t>(44u + payload.size());
    f.push_back(static_cast<uint8_t>(frameBytes & 0xFFu));
    f.push_back(static_cast<uint8_t>((frameBytes >> 8) & 0xFFu));
    f.push_back(static_cast<uint8_t>((frameBytes >> 16) & 0xFFu));
    f.push_back(static_cast<uint8_t>((frameBytes >> 24) & 0xFFu));
    f.insert(f.end(), kMagic, kMagic + 4);
    putU16(f, kVersion);
    f.push_back(kind);
    f.push_back(0u); // flags
    f.insert(f.end(), 16u, 0u); // session: P1a loopback zeros
    putU64(f, 0u);              // epoch: P1a zero
    putU16(f, sender);
    putU16(f, 0u); // reserved
    putU64(f, seq);
    f.insert(f.end(), payload.begin(), payload.end());
    return f;
}

// Strict v2 header validation; returns payload or null (loud + FATAL upstream).
const uint8_t *parseHeader(const std::vector<uint8_t> &bytes, uint8_t wantKind, uint16_t wantSender,
                           size_t wantPayload, const char *what, uint64_t &seqOut)
{
    if (bytes.size() < kHeader)
        fatal("short frame");
    uint32_t frameBytes = static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
                          (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
    if (frameBytes < 44u || frameBytes > kMaxFrame || bytes.size() != kHeader + frameBytes - 44u)
        fatal("bad frame_bytes/trailing");
    if (std::memcmp(bytes.data() + 4, kMagic, 4) != 0)
        fatal("bad magic");
    if (bytes[8] != 2u || bytes[9] != 0u)
        fatal("bad version");
    if (bytes[10] != wantKind)
        fatal("unexpected kind");
    if (bytes[11] != 0u)
        fatal("nonzero flags");
    for (int i = 0; i < 16; ++i)
        if (bytes[12 + i] != 0u)
            fatal("session mismatch");
    for (int i = 0; i < 8; ++i)
        if (bytes[28 + i] != 0u)
            fatal("epoch mismatch");
    uint16_t sender = static_cast<uint16_t>(bytes[36] | (bytes[37] << 8));
    if (sender != wantSender)
        fatal("sender mismatch");
    if (bytes[38] != 0u || bytes[39] != 0u)
        fatal("nonzero reserved");
    uint64_t seq = 0u;
    for (int i = 0; i < 8; ++i)
        seq |= static_cast<uint64_t>(bytes[40 + i]) << (8 * i);
    if (seq == 0u)
        fatal("seq starts at 1");
    if (bytes.size() != kHeader + wantPayload)
        fatal("bad payload length");
    (void)what;
    seqOut = seq;
    return bytes.data() + kHeader;
}

bool readFrame(int fd, uint8_t wantKind, uint16_t wantSender, size_t wantPayload, const char *what,
               std::vector<uint8_t> &payloadOut, uint64_t &seqOut)
{
    std::vector<uint8_t> hdr(kHeader);
    if (!readAll(fd, hdr.data(), kHeader))
        return false;
    uint32_t frameBytes = static_cast<uint32_t>(hdr[0]) | (static_cast<uint32_t>(hdr[1]) << 8) |
                          (static_cast<uint32_t>(hdr[2]) << 16) | (static_cast<uint32_t>(hdr[3]) << 24);
    if (frameBytes < 44u || frameBytes > kMaxFrame)
        fatal("bad frame_bytes");
    std::vector<uint8_t> full = hdr;
    full.resize(kHeader + frameBytes - 44u);
    if (!readAll(fd, full.data() + kHeader, frameBytes - 44u))
        return false;
    const uint8_t *p = parseHeader(full, wantKind, wantSender, wantPayload, what, seqOut);
    payloadOut.assign(p, p + wantPayload);
    return true;
}

void loadScripts(State &s)
{
    if (s.roleA)
    {
        const char *spec = std::getenv("PS2X_NP_LINK_A_SCRIPT");
        std::string path = stripAt(spec);
        if (path.empty())
            fatal("role A needs PS2X_NP_LINK_A_SCRIPT");
        FILE *f = std::fopen(path.c_str(), "r");
        if (!f)
            fatal("cannot read PS2X_NP_LINK_A_SCRIPT file");
        char line[256];
        size_t n = 0u, lineno = 0u;
        while (std::fgets(line, sizeof(line), f))
        {
            ++lineno;
            if (line[0] == '#' || line[0] == '\n')
                continue;
            unsigned long long tick = 0u;
            unsigned b = 0u, rx = 0u, ry = 0u, lx = 0u, ly = 0u;
            // np-review-1 F7: malformed lines and duplicate ticks are FATAL
            // before READY (never silent fallback).
            if (std::sscanf(line, "%llu %x %x %x %x %x", &tick, &b, &rx, &ry, &lx, &ly) != 6)
            {
                char msg[128];
                std::snprintf(msg, sizeof(msg), "malformed PS2X_NP_LINK_A_SCRIPT line %llu",
                              (unsigned long long)lineno);
                fatal(msg);
            }
            uint64_t t = static_cast<uint64_t>(tick);
            if (s.aScript.find(t) != s.aScript.end())
                fatal("duplicate tick in PS2X_NP_LINK_A_SCRIPT");
            PadTuple t2;
            t2.buttons = static_cast<uint16_t>(b & 0xFFFFu);
            t2.rx = static_cast<uint8_t>(rx & 0xFFu);
            t2.ry = static_cast<uint8_t>(ry & 0xFFu);
            t2.lx = static_cast<uint8_t>(lx & 0xFFu);
            t2.ly = static_cast<uint8_t>(ly & 0xFFu);
            s.aScript[t] = t2;
            ++n;
        }
        std::fclose(f);
        std::fprintf(stderr, "[np-link] role=A aScript=%zu entries\n", n);
    }
    else
    {
        const char *spec = std::getenv("PS2X_NP_LINK_B_SCRIPT");
        std::string path = stripAt(spec);
        if (path.empty())
            fatal("role B needs PS2X_NP_LINK_B_SCRIPT");
        FILE *f = std::fopen(path.c_str(), "r");
        if (!f)
            fatal("cannot read PS2X_NP_LINK_B_SCRIPT file");
        char line[256];
        size_t n = 0u, lineno = 0u;
        while (std::fgets(line, sizeof(line), f))
        {
            ++lineno;
            if (line[0] == '#' || line[0] == '\n')
                continue;
            unsigned long long tick = 0u;
            unsigned de4 = 0u, w0 = 0u, w1 = 0u;
            if (std::sscanf(line, "%llu %x %x %x", &tick, &de4, &w0, &w1) != 4)
            {
                char msg[128];
                std::snprintf(msg, sizeof(msg), "malformed PS2X_NP_LINK_B_SCRIPT line %llu",
                              (unsigned long long)lineno);
                fatal(msg);
            }
            uint64_t t = static_cast<uint64_t>(tick);
            if (s.bScript.find(t) != s.bScript.end())
                fatal("duplicate tick in PS2X_NP_LINK_B_SCRIPT");
            RiderTuple t2;
            t2.de4 = de4;
            t2.w0 = w0;
            t2.w1 = w1;
            s.bScript[t] = t2;
            ++n;
        }
        std::fclose(f);
        std::fprintf(stderr, "[np-link] role=B bScript=%zu entries\n", n);
    }
}

int connectRelay(const std::string &host, uint16_t port)
{
    for (int attempt = 0; attempt < 300; ++attempt)
    {
        struct addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo *list = nullptr;
        char portBuf[16];
        std::snprintf(portBuf, sizeof(portBuf), "%u", port);
        if (::getaddrinfo(host.c_str(), portBuf, &hints, &list) == 0)
        {
            int fd = -1;
            for (struct addrinfo *ai = list; ai; ai = ai->ai_next)
            {
                fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
                if (fd < 0)
                    continue;
                if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
                    break;
                ::close(fd);
                fd = -1;
            }
            ::freeaddrinfo(list);
            if (fd >= 0)
                return fd;
        }
        if (attempt % 10 == 0)
            std::fprintf(stderr, "[np-link] relay %s:%u unreachable (attempt %d)\n", host.c_str(), port,
                         attempt);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    return -1;
}

uint16_t getU16(const uint8_t *p)
{
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint64_t getU64(const uint8_t *p)
{
    uint64_t v = 0u;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}

// Hex-decode the sha256Hex string into 32 bytes (FATAL on malformed).
void hexToBytes(const std::string &hex, uint8_t out[32])
{
    if (hex.size() != 64u)
        fatal("sha256Hex wrong length");
    for (int i = 0; i < 32; ++i)
    {
        unsigned v = 0u;
        if (std::sscanf(hex.c_str() + 2 * i, "%2x", &v) != 1)
            fatal("sha256Hex malformed");
        out[i] = static_cast<uint8_t>(v);
    }
}

void netThreadMain()
{
    State &s = state();
    int fd = connectRelay(s.relayHost, s.relayPort);
    if (fd < 0)
        fatal("relay unreachable after ~150 s");
    // ---- JOIN (seq 1) ----
    {
        std::vector<uint8_t> payload(32 + 32 + 32, 0u); // credential/compat/build: P1a zeros
        payload.push_back(kProfile);
        std::vector<uint8_t> f = frame(kJoin, s.slot, s.nextMsgSeq++, payload);
        if (!writeAll(fd, f.data(), f.size()))
            fatal("JOIN write failed");
    }
    // ---- CONFIG (validate the frozen manifest/timing before READY) ----
    {
        std::vector<uint8_t> payload;
        uint64_t seq = 0u;
        if (!readFrame(fd, kConfig, kRelaySender, 67u, "CONFIG", payload, seq))
            fatal("CONFIG read failed (setup refused)");
        const uint8_t *p = payload.data();
        if (p[32] != kProfile)
            fatal("CONFIG profile mismatch");
        uint64_t first = getU64(p + 33);
        // delay u16 @41, hash_every @43, lag_limit @45, schema u32 @47
        uint16_t delay = getU16(p + 41);
        uint16_t hashEvery = getU16(p + 43);
        uint32_t schema = static_cast<uint32_t>(p[47] | (p[48] << 8) | (p[49] << 16) | (p[50] << 24));
        if (first != s.firstTick || delay != s.delay)
            fatal("CONFIG first_tick/delay mismatch (setup mismatch: abort, no in-race blame)");
        if (hashEvery != kHashEvery || schema != kSchema || p[51] != kStateAlg)
            fatal("CONFIG schema/state mismatch");
        uint32_t rosterRev =
            static_cast<uint32_t>(p[52] | (p[53] << 8) | (p[54] << 16) | (p[55] << 24));
        if (rosterRev != kRosterRev || p[56] != 2u)
            fatal("CONFIG roster mismatch");
        // Slots: (0, actor 0, Pad6, fb 1), (1, actor 1, RiderCmd12, fb 1).
        if (p[57] != 0u || p[58] != 0u || p[59] != 0u || p[60] != kPad6 || p[61] != 1u)
            fatal("CONFIG slot 0 mismatch");
        if (p[62] != 1u || p[63] != 0u || p[64] != 1u || p[65] != kRider12 || p[66] != 1u)
            fatal("CONFIG slot 1 mismatch");
        s.relaySeq = seq;
        std::fprintf(stderr, "[np-link] CONFIG ok first=%llu delay=%u\n", (unsigned long long)first,
                     delay);
    }
    // ---- READY (seq 2) ----
    {
        std::vector<uint8_t> payload;
        putU64(payload, s.firstTick); // boundary = first_tick
        payload.push_back(static_cast<uint8_t>(kSchema & 0xFFu));
        payload.push_back(0u);
        payload.push_back(0u);
        payload.push_back(0u);
        payload.push_back(kStateAlg);
        payload.insert(payload.end(), 32u, 0u); // state digest: P1a zeros (no referee)
        payload.insert(payload.end(), 32u, 0u); // boot_transcript: P1a zeros
        payload.push_back(static_cast<uint8_t>(kRosterRev & 0xFFu));
        payload.push_back(0u);
        payload.push_back(0u);
        payload.push_back(0u);
        std::vector<uint8_t> f = frame(kReady, s.slot, s.nextMsgSeq++, payload);
        if (!writeAll(fd, f.data(), f.size()))
            fatal("READY write failed");
    }
    // ---- START ----
    {
        std::vector<uint8_t> payload;
        uint64_t seq = 0u;
        if (!readFrame(fd, kStart, kRelaySender, 54u, "START", payload, seq))
            fatal("START read failed");
        if (seq != s.relaySeq + 1u)
            fatal("relay seq gap at START");
        s.relaySeq = seq;
        const uint8_t *p = payload.data();
        if (getU64(p) != s.firstTick || getU64(p + 8) != 1u)
            fatal("START first_tick/first_commit_seq mismatch");
        uint32_t rr =
            static_cast<uint32_t>(p[16] | (p[17] << 8) | (p[18] << 16) | (p[19] << 24));
        if (rr != kRosterRev)
            fatal("START roster_rev mismatch");
    }
    s.sock = fd;
    s.started = true;
    std::fprintf(stderr, "[np-link] STARTED role=%c slot=%u firstTick=%llu\n", s.roleA ? 'A' : 'B',
                 s.slot, (unsigned long long)s.firstTick);
    std::fflush(stderr);
    // ---- steady state: flush INPUTs, receive COMMITs ----
    std::vector<uint8_t> payload;
    while (true)
    {
        while (true)
        {
            std::vector<uint8_t> out;
            {
                std::lock_guard<std::mutex> lock(s.netMutex);
                if (s.sendQueue.empty())
                    break;
                // Frame under lock is cheap (small); sequence assigned here.
                std::vector<uint8_t> body = std::move(s.sendQueue.front());
                s.sendQueue.pop_front();
                out = frame(kInput, s.slot, s.nextMsgSeq++, body);
            }
            if (!writeAll(fd, out.data(), out.size()))
            {
                std::fprintf(stderr, "[np-link] relay write failed; stalling forever (no advance)\n");
                std::fflush(stderr);
                std::this_thread::sleep_for(std::chrono::hours(24 * 365));
                return;
            }
        }
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 100 * 1000;
        int r = ::select(fd + 1, &rfds, nullptr, nullptr, &tv);
        if (r < 0)
        {
            std::fprintf(stderr, "[np-link] relay select failed; stalling forever (no advance)\n");
            std::fflush(stderr);
            std::this_thread::sleep_for(std::chrono::hours(24 * 365));
            return;
        }
        if (r == 0)
            continue;
        uint64_t seq = 0u;
        payload.clear();
        // COMMIT payload: 8 seq + 8 tick + 4 roster + 1 events + 1 count +
        // (2+1+6) + (2+1+12) + 32 chain = 78.
        if (!readFrame(fd, kCommit, kRelaySender, 78u, "COMMIT", payload, seq))
        {
            std::fprintf(stderr, "[np-link] relay closed/bad COMMIT; stalling forever\n");
            std::fflush(stderr);
            std::this_thread::sleep_for(std::chrono::hours(24 * 365));
            return;
        }
        if (seq != s.relaySeq + 1u)
            fatal("relay seq gap (lost commit: never predict)");
        s.relaySeq = seq;
        const uint8_t *p = payload.data();
        uint64_t commitSeq = getU64(p);
        uint64_t tick = getU64(p + 8);
        uint32_t rr =
            static_cast<uint32_t>(p[16] | (p[17] << 8) | (p[18] << 16) | (p[19] << 24));
        if (commitSeq != s.nextCommitSeq || rr != kRosterRev || p[20] != 0u || p[21] != 2u)
            fatal("COMMIT seq/roster/events/count violation");
        if (p[22] != 0u || p[23] != 0u || p[24] != kPad6)
            fatal("COMMIT slot 0 shape violation");
        if (p[31] != 1u || p[32] != 0u || p[33] != kRider12)
            fatal("COMMIT slot 1 shape violation");
        Commit c;
        c.buttons = static_cast<uint16_t>(p[25] | (p[26] << 8));
        c.rx = p[27];
        c.ry = p[28];
        c.lx = p[29];
        c.ly = p[30];
        std::memcpy(&c.de4, p + 34, 4u);
        std::memcpy(&c.w0, p + 38, 4u);
        std::memcpy(&c.w1, p + 42, 4u);
        c.bLive = (c.de4 != kSentinelDe4);
        // Chain verify: SHA-256(domain || session || epoch || prev || body).
        // Body is the 46 payload bytes before the 32-byte chain.
        {
            std::vector<uint8_t> body(p, p + 46);
            std::vector<uint8_t> input;
            const char domain[] = "SSXNP2-COMMIT";
            input.insert(input.end(), domain, domain + 13);
            input.insert(input.end(), 16u, 0u);
            for (int i = 0; i < 8; ++i)
                input.push_back(0u);
            {
                std::lock_guard<std::mutex> lock(s.commitMutex);
                input.insert(input.end(), s.lastChain, s.lastChain + 32);
            }
            input.insert(input.end(), body.begin(), body.end());
            std::string hex = ps2_savestate::sha256Hex(input.data(), input.size());
            uint8_t want[32];
            hexToBytes(hex, want);
            if (std::memcmp(want, p + 46, 32) != 0)
                fatal("COMMIT chain mismatch");
        }
        {
            std::lock_guard<std::mutex> lock(s.commitMutex);
            if (s.commits.find(tick) == s.commits.end())
            {
                State::StoredCommit sc;
                sc.commit = c;
                std::memcpy(sc.chain, p + 46, 32);
                sc.seq = commitSeq;
                s.commits[tick] = sc;
                std::memcpy(s.lastChain, p + 46, 32);
                ++s.nextCommitSeq;
            }
            // A re-delivered COMMIT is a protocol violation; TCP makes it
            // impossible. Keep first, never apply twice (loud below).
            else
                std::fprintf(stderr, "[np-link] WARNING: duplicate COMMIT tick=%llu ignored\n",
                             (unsigned long long)tick);
        }
        s.commitCv.notify_all();
    }
}

void configure()
{
    State &s = state();
    if (s.configured)
        return;
    s.configured = true;
    if (!enabledCache().load(std::memory_order_relaxed))
        return;
    const char *role = std::getenv("PS2X_NP_LINK_ROLE");
    if (!role || (role[0] != 'A' && role[0] != 'B') || role[1] != '\0')
        fatal("PS2X_NP_LINK_ROLE must be A or B");
    s.roleA = (role[0] == 'A');
    s.slot = s.roleA ? 0u : 1u;
    const char *relay = std::getenv("PS2X_NP_LINK_RELAY");
    std::string rs = relay ? relay : "";
    const size_t colon = rs.rfind(':');
    if (rs.empty() || colon == std::string::npos)
        fatal("PS2X_NP_LINK_RELAY must be host:port");
    s.relayHost = rs.substr(0, colon);
    s.relayPort = static_cast<uint16_t>(std::atoi(rs.substr(colon + 1).c_str()));
    if (s.relayPort == 0u)
        fatal("PS2X_NP_LINK_RELAY has a bad port");
    const char *first = std::getenv("PS2X_NP_LINK_FIRST_TICK");
    const char *delay = std::getenv("PS2X_NP_LINK_DELAY");
    const char *slot = std::getenv("PS2X_NP_LINK_RIDER_SLOT");
    if (!first || !delay || !slot)
        fatal("PS2X_NP_LINK_FIRST_TICK/DELAY/RIDER_SLOT are all required");
    s.firstTick = static_cast<uint64_t>(std::strtoull(first, nullptr, 0));
    s.delay = static_cast<uint64_t>(std::strtoull(delay, nullptr, 0));
    s.riderSlot = std::atoi(slot);
    if (s.firstTick == 0u || s.delay == 0u || s.riderSlot < 0)
        fatal("bad FIRST_TICK/DELAY/RIDER_SLOT (first>0, delay>0, slot>=0)");
    // np-review-1 F4: validate the effective netplay profile before tick 1.
    {
        const char *canon = std::getenv("PS2X_NETPLAY_CANON");
        const char *det = std::getenv("PS2X_DETERMINISTIC");
        const char *vu1 = std::getenv("PS2X_VU1_ENGINE");
        if (!canon || std::string(canon) != "1")
            fatal("PS2X_NETPLAY_CANON=1 required for lockstep");
        if (!det || std::string(det) != "1")
            fatal("PS2X_DETERMINISTIC=1 required for lockstep");
        if (!vu1 || std::string(vu1) != "microvu")
            fatal("PS2X_VU1_ENGINE=microvu required for lockstep");
    }
    s.sendUpTo = s.firstTick;
    loadScripts(s);
    const char *dir = std::getenv("PS2X_NP_LINK_DIR");
    if (dir && dir[0])
    {
        std::string c = std::string(dir) + "/commit.tsv";
        std::string t = std::string(dir) + "/sent.tsv";
        s.commitLog = std::fopen(c.c_str(), "w");
        s.sentLog = std::fopen(t.c_str(), "w");
        if (s.commitLog)
            std::fprintf(s.commitLog, "# tick seq abtn arx ary alx aly bde4 bw0 bw1 blive chain\n");
        if (s.sentLog)
            std::fprintf(s.sentLog, "# tick hex\n");
    }
    std::fprintf(stderr, "[np-link] role=%c relay=%s:%u first=%llu delay=%llu slot=%d (SSXNP/2-P1a)\n",
                 s.roleA ? 'A' : 'B', s.relayHost.c_str(), s.relayPort,
                 (unsigned long long)s.firstTick, (unsigned long long)s.delay, s.riderSlot);
    std::fflush(stderr);
    s.ok = true;
    std::thread(netThreadMain).detach();
}

void queueInput(State &s, uint64_t tick)
{
    // INPUT payload: first_tick u64 + kind u8 + count u8(=1) + one sample.
    std::vector<uint8_t> body;
    putU64(body, tick);
    if (s.roleA)
    {
        body.push_back(kPad6);
        body.push_back(1u);
        PadTuple pt;
        auto it = s.aScript.find(tick);
        if (it != s.aScript.end())
            pt = it->second;
        body.push_back(static_cast<uint8_t>(pt.buttons & 0xFFu));
        body.push_back(static_cast<uint8_t>((pt.buttons >> 8) & 0xFFu));
        body.push_back(pt.rx);
        body.push_back(pt.ry);
        body.push_back(pt.lx);
        body.push_back(pt.ly);
        if (s.sentLog)
            std::fprintf(s.sentLog, "%llu %02x%02x%02x%02x%02x%02x\n", (unsigned long long)tick,
                         body[10], body[11], body[12], body[13], body[14], body[15]);
    }
    else
    {
        body.push_back(kRider12);
        body.push_back(1u);
        RiderTuple rt; // default = sentinel (AI default)
        auto it = s.bScript.find(tick);
        if (it != s.bScript.end())
            rt = it->second;
        for (int i = 0; i < 4; ++i)
            body.push_back(static_cast<uint8_t>((rt.de4 >> (8 * i)) & 0xFFu));
        for (int i = 0; i < 4; ++i)
            body.push_back(static_cast<uint8_t>((rt.w0 >> (8 * i)) & 0xFFu));
        for (int i = 0; i < 4; ++i)
            body.push_back(static_cast<uint8_t>((rt.w1 >> (8 * i)) & 0xFFu));
        if (s.sentLog)
            std::fprintf(s.sentLog, "%llu %08x%08x%08x\n", (unsigned long long)tick, rt.de4, rt.w0,
                         rt.w1);
    }
    {
        std::lock_guard<std::mutex> lock(s.netMutex);
        if (s.sendQueue.size() > 4096u)
            fatal("send queue over 4096 (relay not draining)");
        s.sendQueue.push_back(std::move(body));
    }
}

} // namespace

bool enabled()
{
    return enabledCache().load(std::memory_order_relaxed);
}

uint64_t firstTick()
{
    std::call_once(configOnce(), configure);
    return state().firstTick;
}

int riderSlot()
{
    std::call_once(configOnce(), configure);
    return state().riderSlot;
}

void onTick(uint64_t tick)
{
    if (!enabled())
        return;
    std::call_once(configOnce(), configure);
    State &s = state();
    if (!s.ok)
        return;
    const uint64_t due = tick + s.delay;
    while (s.sendUpTo <= due && s.sendUpTo < s.firstTick + 1000000u)
    {
        queueInput(s, s.sendUpTo);
        ++s.sendUpTo;
    }
}

bool waitCommit(uint64_t tick, Commit &out)
{
    if (!enabled())
        return false;
    std::call_once(configOnce(), configure);
    State &s = state();
    if (!s.ok)
        return false;
    std::unique_lock<std::mutex> lock(s.commitMutex);
    if (s.haveLast && tick <= s.lastWaited)
    {
        out = s.lastCommit;
        return true;
    }
    bool stalled = false;
    auto lastLog = std::chrono::steady_clock::now();
    while (s.commits.find(tick) == s.commits.end())
    {
        if (!stalled)
        {
            stalled = true;
            ++s.stallTicks;
            std::fprintf(stderr, "[np-link] stall: waiting COMMIT tick=%llu role=%c\n",
                         (unsigned long long)tick, s.roleA ? 'A' : 'B');
            std::fflush(stderr);
        }
        ++s.stallWaits;
        if (s.commitCv.wait_for(lock, std::chrono::seconds(5)) == std::cv_status::timeout)
        {
            auto now = std::chrono::steady_clock::now();
            if (now - lastLog >= std::chrono::seconds(5))
            {
                lastLog = now;
                std::fprintf(stderr, "[np-link] still stalled tick=%llu (waits=%llu)\n",
                             (unsigned long long)tick, (unsigned long long)s.stallWaits);
                std::fflush(stderr);
            }
        }
    }
    out = s.commits[tick].commit;
    uint64_t outSeq = s.commits[tick].seq;
    uint8_t outChain[32];
    std::memcpy(outChain, s.commits[tick].chain, 32);
    s.lastWaited = tick;
    s.lastCommit = out;
    s.haveLast = true;
    ++s.appliedTicks;
    auto it = s.commits.begin();
    while (it != s.commits.end() && it->first + 32u < tick)
        it = s.commits.erase(it);
    if (s.commitLog)
    {
        std::fprintf(s.commitLog, "%llu %llu %04x %02x %02x %02x %02x %08x %08x %08x %d ",
                     (unsigned long long)tick, (unsigned long long)outSeq, out.buttons, out.rx,
                     out.ry, out.lx, out.ly, out.de4, out.w0, out.w1, out.bLive ? 1 : 0);
        for (int i = 0; i < 32; ++i)
            std::fprintf(s.commitLog, "%02x", outChain[i]);
        std::fprintf(s.commitLog, "\n");
        if ((s.appliedTicks % 600u) == 0u)
            std::fflush(s.commitLog);
    }
    if (tick >= s.lastHeartbeat + 3600u)
    {
        s.lastHeartbeat = tick;
        std::fprintf(stderr, "[np-link] heartbeat tick=%llu applied=%llu stalls=%llu waits=%llu\n",
                     (unsigned long long)tick, (unsigned long long)s.appliedTicks,
                     (unsigned long long)s.stallTicks, (unsigned long long)s.stallWaits);
        std::fflush(stderr);
    }
    return true;
}

} // namespace nplink
} // namespace ps2x
