#pragma once
// TK45c: Tricky floating-gem multipliers (host-side pickup, guest-affecting).
//
// The engine contact path cannot work (TK45b: slot programs never run for
// bound gems; kind-2 defs don't draw), so the pickup is host-side: at the
// per-rider half-step boundary (0x128ddc -> 0x1216e0, a0 == player-1 R) the
// poll swept-tests the rider segment against every course's gem spheres
// (baked disjoint by 89k+ units, so no course key is needed) and, on a hit,
// max-tracks the trick-state multiplier +0x1c4, hides the gem instance and
// queues the Icons callout through the game's own speech call. +0x1c4 clears
// at bank-clear entry (all 8 paths incl. the wipeout-region caller).
// Knob PS2X_SSX3_TRICKY_GEMS=1 (default off); table
// PS2X_SSX3_TRICKY_GEMS_TABLE (staged sidecar from bake_gems.py).
//
// Threading (TKA1): everything runs on the EE thread at dispatch hooks (a
// simulation-update point, each physics step); nested guest calls happen
// only in hook context (synchronous, like any game call). Never on the GS
// worker, the present path or the audio callback.
//
// Fail-closed everywhere: any invalid chain read, unmapped word, non-finite
// float or ambiguous instance match skips that step's work. All guest
// writes are conditional (no-op when the value already holds), so an SSX 3
// course with GEMS=1 performs zero writes.

#include "ps2_ssx3_course_manifest.h"
#include "ps2_ssx3_tricky_hud.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace ps2_tk45c
{

namespace
{

constexpr uint32_t kRamMask = 0x01ffffffu;
constexpr uint32_t kOneBits = 0x3f800000u;
constexpr uint32_t kBoundarySrc = 0x128ddcu;
constexpr uint32_t kBoundaryTgt = 0x1216e0u;
constexpr uint32_t kBankClear = 0x117838u;
constexpr uint32_t kSpeechFn = 0x2a3eb8u;
constexpr uint32_t kRiderPosOff = 0x110u;
constexpr uint32_t kTrickOff = 0x790u;
constexpr uint32_t kMultOff = 0x1c4u;
constexpr uint32_t kTrickSize = 0x1ccu;
constexpr uint32_t kRaceClockOff = 0xcu;
constexpr uint32_t kLiveScore = 0x117950u;
constexpr uint32_t kInstPosOff = 0x40u;
constexpr uint32_t kDeadLow = 0x05u;
constexpr float kTeleportJump = 3000.0f;
constexpr size_t kMaxGems = 512u;
constexpr uint32_t kScanChunk = 1u << 20;
constexpr uint32_t kCallSentinel = 0x0badc0deu;

inline bool readU32(const uint8_t *ram, size_t ramSize, uint32_t addr, uint32_t &out) noexcept
{
    const uint32_t p = addr & kRamMask;
    if (!ram || ramSize == 0u || p + 4u > ramSize)
        return false;
    std::memcpy(&out, ram + p, 4);
    return true;
}

inline bool readF32(const uint8_t *ram, size_t ramSize, uint32_t addr, float &out) noexcept
{
    uint32_t b = 0u;
    if (!readU32(ram, ramSize, addr, b))
        return false;
    std::memcpy(&out, &b, 4);
    return true;
}

inline bool writeU32(uint8_t *ram, size_t ramSize, uint32_t addr, uint32_t v) noexcept
{
    const uint32_t p = addr & kRamMask;
    if (!ram || ramSize == 0u || p + 4u > ramSize)
        return false;
    std::memcpy(ram + p, &v, 4);
    return true;
}

inline bool writeF32(uint8_t *ram, size_t ramSize, uint32_t addr, float v) noexcept
{
    uint32_t b = 0u;
    std::memcpy(&b, &v, 4);
    return writeU32(ram, ramSize, addr, b);
}

} // namespace

struct Gem
{
    char course[8] = {0};
    uint32_t rid = 0u;
    float xyz[3] = {0, 0, 0};
    float rad = 0.0f;
    uint8_t value = 0u;
    char donor[64] = {0};
};

// Swept segment-vs-sphere (tunneling-proof pickup). Degenerate (zero-length)
// segments test the point.
inline bool segSphereHit(const float p0[3], const float p1[3], const float c[3], float r) noexcept
{
    if (!(r > 0.0f) || !std::isfinite(p0[0]) || !std::isfinite(p0[1]) || !std::isfinite(p0[2]) ||
        !std::isfinite(p1[0]) || !std::isfinite(p1[1]) || !std::isfinite(p1[2]) || !std::isfinite(c[0]) ||
        !std::isfinite(c[1]) || !std::isfinite(c[2]))
        return false;
    const float dx = p1[0] - p0[0], dy = p1[1] - p0[1], dz = p1[2] - p0[2];
    const float len2 = dx * dx + dy * dy + dz * dz;
    float t = 0.0f;
    if (len2 > 0.0f)
    {
        t = ((c[0] - p0[0]) * dx + (c[1] - p0[1]) * dy + (c[2] - p0[2]) * dz) / len2;
        if (t < 0.0f)
            t = 0.0f;
        else if (t > 1.0f)
            t = 1.0f;
    }
    const float qx = p0[0] + t * dx - c[0], qy = p0[1] + t * dy - c[1], qz = p0[2] + t * dz - c[2];
    return qx * qx + qy * qy + qz * qz <= r * r;
}

inline bool teleportJump(const float a[3], const float b[3]) noexcept
{
    if (!std::isfinite(b[0]) || !std::isfinite(b[1]) || !std::isfinite(b[2]))
        return true;
    const float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
    return dx * dx + dy * dy + dz * dz > kTeleportJump * kTeleportJump;
}

// Parse the bake_gems.py sidecar. Refuses the whole table on any malformed
// row (fail closed); comments (#) and blank lines skipped.
inline bool parseTableText(const char *text, std::vector<Gem> &out, std::string &err)
{
    out.clear();
    if (!text)
    {
        err = "null text";
        return false;
    }
    size_t lineNo = 0u;
    const char *p = text;
    while (*p)
    {
        const char *e = std::strchr(p, '\n');
        const size_t len = e ? static_cast<size_t>(e - p) : std::strlen(p);
        ++lineNo;
        std::string line(p, len);
        p = e ? e + 1 : p + len;
        size_t s = line.find_first_not_of(" \t\r");
        if (s == std::string::npos || line[s] == '#')
            continue;
        char course[8] = {0}, donor[64] = {0};
        unsigned rid = 0u, value = 0u;
        float x = 0, y = 0, z = 0, rad = 0;
        int end = 0;
        if (std::sscanf(line.c_str() + s, "%7s %u %f %f %f %f %u %63s%n", course, &rid, &x, &y, &z, &rad,
                        &value, donor, &end) != 8 ||
            line.c_str()[s + end] != '\0')
        {
            err = "line " + std::to_string(lineNo) + ": malformed";
            out.clear();
            return false;
        }
        if (value != 2u && value != 3u && value != 5u)
        {
            err = "line " + std::to_string(lineNo) + ": bad value";
            out.clear();
            return false;
        }
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(rad) || rad <= 0.0f)
        {
            err = "line " + std::to_string(lineNo) + ": bad sphere";
            out.clear();
            return false;
        }
        if (out.size() >= kMaxGems)
        {
            err = "table exceeds max gems";
            out.clear();
            return false;
        }
        Gem g;
        std::memcpy(g.course, course, sizeof(g.course));
        g.rid = rid;
        g.xyz[0] = x;
        g.xyz[1] = y;
        g.xyz[2] = z;
        g.rad = rad;
        g.value = static_cast<uint8_t>(value);
        std::memcpy(g.donor, donor, sizeof(g.donor));
        out.push_back(g);
    }
    if (out.empty())
    {
        err = "no gem rows";
        return false;
    }
    err.clear();
    return true;
}

inline bool enabled() noexcept
{
    static const bool on = [] {
        const char *e = std::getenv("PS2X_SSX3_TRICKY_GEMS");
        return e && e[0] == '1';
    }();
    return on;
}

inline const std::vector<Gem> &table()
{
    static std::vector<Gem> t;
    static bool tried = false;
    if (!tried)
    {
        tried = true;
        const char *path = std::getenv("PS2X_SSX3_TRICKY_GEMS_TABLE");
        std::string err = "unset";
        if (path && *path)
        {
            FILE *f = std::fopen(path, "rb");
            if (!f)
            {
                err = "open failed";
            }
            else
            {
                std::string text;
                char buf[8192];
                size_t n = 0u;
                while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0u)
                    text.append(buf, n);
                std::fclose(f);
                if (!parseTableText(text.c_str(), t, err))
                    t.clear();
            }
        }
        std::fprintf(stderr, "[tk45c] table '%s': %s\n", path ? path : "(unset)",
                     t.empty() ? err.c_str() : (std::to_string(t.size()) + " gems").c_str());
    }
    return t;
}

struct Capture
{
    bool ok = false;
    uint64_t tick = 0u;
    uint32_t a0 = 0u, a1 = 0u;
    uint32_t s[8] = {0};
    float f20 = 0.0f;
};

struct State
{
    bool init = false;
    bool wasTricky = false;
    bool prevValid = false;
    bool scanDone = false;
    bool pendingClear = false;
    bool rehideArmed = false;
    float prev[3] = {0, 0, 0};
    uint32_t lastR = 0u;
    uint32_t lastClock = 0u;
    uint32_t scanCursor = 0u;
    ps2_ssx3_tricky_hud::RaceClock raceClock;
    uint64_t collected[(kMaxGems + 63u) / 64u] = {0};
    uint32_t resolved[kMaxGems] = {0};
    Capture cap;
};

inline State &state() noexcept
{
    static State st;
    return st;
}

inline void fullReset(State &st) noexcept
{
    st.prevValid = false;
    st.scanDone = false;
    st.scanCursor = 0u;
    st.rehideArmed = false;
    st.raceClock = ps2_ssx3_tricky_hud::RaceClock{};
    std::memset(st.collected, 0, sizeof(st.collected));
    std::memset(st.resolved, 0, sizeof(st.resolved));
}

inline bool isCollected(const State &st, size_t i) noexcept
{
    return ((st.collected[i >> 6] >> (i & 63u)) & 1u) != 0u;
}

inline void setCollected(State &st, size_t i) noexcept
{
    st.collected[i >> 6] |= uint64_t{1} << (i & 63u);
}

// x-index over the table (built once): per RAM word one binary search.
struct XIndex
{
    std::vector<std::pair<uint32_t, size_t>> byX;
};

inline const XIndex &xIndex(const std::vector<Gem> &gems)
{
    static XIndex xi;
    static size_t builtFor = 0u;
    static const void *builtPtr = nullptr;
    if (builtPtr != gems.data() || builtFor != gems.size())
    {
        xi.byX.clear();
        for (size_t i = 0u; i < gems.size(); ++i)
        {
            uint32_t xb = 0u;
            std::memcpy(&xb, &gems[i].xyz[0], 4);
            xi.byX.emplace_back(xb, i);
        }
        std::sort(xi.byX.begin(), xi.byX.end(),
                  [](const auto &a, const auto &b) { return a.first < b.first; });
        builtPtr = gems.data();
        builtFor = gems.size();
    }
    return xi;
}

// One chunk of the instance-position scan (instance +40 == kind-3
// R-translation, bitwise; lab-proven). Called from the boundary hook;
// yields after kScanChunk bytes so no frame hitches.
inline void scanChunk(State &st, const std::vector<Gem> &gems, const uint8_t *ram, size_t ramSize,
                      uint64_t tick)
{
    if (st.scanDone || !ram || ramSize == 0u || gems.empty())
        return;
    const XIndex &xi = xIndex(gems);
    const uint32_t end =
        st.scanCursor + kScanChunk < ramSize ? st.scanCursor + kScanChunk : static_cast<uint32_t>(ramSize);
    for (uint32_t q = st.scanCursor; q + kInstPosOff + 12u <= end; q += 4u)
    {
        uint32_t x = 0u;
        std::memcpy(&x, ram + q + kInstPosOff, 4);
        auto lo = std::lower_bound(xi.byX.begin(), xi.byX.end(), x,
                                   [](const auto &e, uint32_t v) { return e.first < v; });
        for (auto it = lo; it != xi.byX.end() && it->first == x; ++it)
        {
            const size_t i = it->second;
            uint32_t v[3];
            std::memcpy(v, ram + q + kInstPosOff, 12);
            uint32_t g[3];
            std::memcpy(g, gems[i].xyz, 12);
            if (v[0] != g[0] || v[1] != g[1] || v[2] != g[2])
                continue;
            // Sanity: the node's link words must be mapped RAM (strict;
            // a hide-write to a mis-resolved struct would corrupt).
            uint32_t w0 = 0u, w1 = 0u;
            std::memcpy(&w0, ram + q, 4);
            std::memcpy(&w1, ram + q + 4u, 4);
            const uint32_t p0 = w0 & kRamMask, p1 = w1 & kRamMask;
            if (w0 == 0u || w1 == 0u || (w0 & ~kRamMask) != 0u || (w1 & ~kRamMask) != 0u ||
                p0 + 64u > ramSize || p1 + 64u > ramSize)
                continue;
            if (st.resolved[i] != 0u && st.resolved[i] != q)
            {
                std::fprintf(stderr, "[tk45c] tick=%llu %s rid=%u AMBIGUOUS (%08x vs %08x), hide off\n",
                             (unsigned long long)tick, gems[i].course, gems[i].rid, st.resolved[i], q);
                st.resolved[i] = 0u; // fail closed: never hide an ambiguous gem
                continue;
            }
            st.resolved[i] = q;
        }
    }
    st.scanCursor = end;
    if (end >= ramSize)
    {
        st.scanDone = true;
        size_t n = 0u;
        for (size_t i = 0u; i < gems.size(); ++i)
            n += st.resolved[i] != 0u ? 1u : 0u;
        std::fprintf(stderr, "[tk45c] tick=%llu scan complete: %zu/%zu gems resolved\n",
                     (unsigned long long)tick, n, gems.size());
        // Re-hide pass after a reset/teleport re-scan: collected gems whose
        // instance came back alive (streaming revive) hide again.
        if (st.rehideArmed)
        {
            st.rehideArmed = false;
            for (size_t i = 0u; i < gems.size(); ++i)
            {
                if (!isCollected(st, i) || st.resolved[i] == 0u)
                    continue;
                uint8_t *wram = const_cast<uint8_t *>(ram);
                uint32_t v[3] = {0};
                const uint32_t pb = st.resolved[i] & kRamMask;
                if (pb + kInstPosOff + 12u > ramSize)
                    continue;
                std::memcpy(v, wram + pb + kInstPosOff, 12);
                uint32_t g[3] = {0};
                std::memcpy(g, gems[i].xyz, 12);
                if (v[0] != g[0] || v[1] != g[1] || v[2] != g[2])
                    continue; // stale pointer: leave it alone
                uint32_t b = 0u;
                if (!readU32(wram, ramSize, pb + 8u, b))
                    continue;
                if ((b & 0xffu) != kDeadLow && writeU32(wram, ramSize, pb + 8u, (b & ~0xffu) | kDeadLow))
                    std::fprintf(stderr, "[tk45c] tick=%llu REHIDE %s rid=%u at=%08x\n",
                                 (unsigned long long)tick, gems[i].course, gems[i].rid, pb);
            }
        }
    }
}

struct Callout
{
    bool fire = false;
    uint32_t mask = 0u;
    float ladder = 0.0f;
};

inline uint32_t maskForValue(unsigned v) noexcept
{
    return v == 2u ? 1u : v == 3u ? 2u : 4u; // Icons jal sites 0x29d1e4/20c/238
}

// Validate the trick-state pointer (TKA1: full span, alignment, finite use).
inline bool trickState(const uint8_t *ram, size_t ramSize, uint32_t r, uint32_t &t) noexcept
{
    t = 0u;
    if (!readU32(ram, ramSize, r + kTrickOff, t) || t == 0u || (t & 3u) != 0u)
    {
        t = 0u;
        return false;
    }
    if (((t + kTrickSize) & kRamMask) + 4u > ramSize || (t & ~kRamMask) != 0u)
    {
        t = 0u;
        return false;
    }
    return true;
}

// The boundary-hook poll (state, table and course flag injected, so tests
// drive it with fake RAM; the glue passes the singletons). Returns a
// callout action for the glue (which owns ctx/dispatch); every other
// effect lands here. All writes conditional.
inline Callout poll(State &st, const std::vector<Gem> &gems, uint8_t *ram, size_t ramSize, uint64_t tick,
                    uint32_t a0, bool tricky)
{
    Callout out;
    if (!tricky)
    {
        if (st.wasTricky)
        {
            fullReset(st);
            st.pendingClear = true; // a stale multiplier must not leak into Stock
        }
        st.wasTricky = false;
        // Stock path: only the pending stale-clear (no sweep/scan/hide).
        if (st.pendingClear && ram && ramSize != 0u)
        {
            const uint32_t r = ps2_ssx3_tricky_hud::resolveChainR(ram, ramSize);
            uint32_t t = 0u;
            if (r != 0u && trickState(ram, ramSize, r, t))
            {
                uint32_t cur = 0u;
                if (readU32(ram, ramSize, t + kMultOff, cur) && cur != kOneBits)
                {
                    float f = 0.0f;
                    std::memcpy(&f, &cur, 4);
                    if (std::isfinite(f) && writeF32(ram, ramSize, t + kMultOff, 1.0f))
                        std::fprintf(stderr, "[tk45c] tick=%llu STALE-CLEAR T=%08x was=%f\n",
                                     (unsigned long long)tick, t, (double)f);
                }
                st.pendingClear = false;
            }
        }
        return out;
    }
    st.wasTricky = true;
    if (gems.empty())
        return out;
    const uint32_t r = ps2_ssx3_tricky_hud::resolveChainR(ram, ramSize);
    const uint32_t b = ps2_ssx3_tricky_hud::resolveChainB(ram, ramSize);
    if (r == 0u || b == 0u || a0 != r)
    {
        // Not player-1's pass (AI rider) or an unreadable chain: hold.
        if (r == 0u || b == 0u)
            st.prevValid = false;
        return out;
    }
    uint32_t clock = 0u;
    if (!readU32(ram, ramSize, b + kRaceClockOff, clock))
    {
        st.prevValid = false;
        return out;
    }
    if (!st.init)
    {
        st.init = true;
        st.lastR = r;
        st.lastClock = clock;
    }
    const bool racing = ps2_ssx3_tricky_hud::updateRaceClock(st.raceClock, clock, tick);
    if (r != st.lastR || clock < st.lastClock)
    {
        // New race/session or retry: per-race state resets (TKA1), the world
        // reloaded, and any stale multiplier clears once T is valid.
        fullReset(st);
        st.pendingClear = true;
    }
    st.lastR = r;
    st.lastClock = clock;
    if (!racing)
    {
        st.prevValid = false;
        return out;
    }
    float cur[3] = {0, 0, 0};
    if (!readF32(ram, ramSize, r + kRiderPosOff, cur[0]) ||
        !readF32(ram, ramSize, r + kRiderPosOff + 4u, cur[1]) ||
        !readF32(ram, ramSize, r + kRiderPosOff + 8u, cur[2]))
    {
        st.prevValid = false;
        return out;
    }
    uint32_t t = 0u;
    const bool tOk = trickState(ram, ramSize, r, t);
    if (st.pendingClear && tOk)
    {
        // Ahead of the scan gate: stale multipliers die on the first
        // valid trick state, not after the 16-vsync scan.
        uint32_t curBits = 0u;
        if (readU32(ram, ramSize, t + kMultOff, curBits) && curBits != kOneBits)
        {
            float f = 0.0f;
            std::memcpy(&f, &curBits, 4);
            if (std::isfinite(f) && writeF32(ram, ramSize, t + kMultOff, 1.0f))
                std::fprintf(stderr, "[tk45c] tick=%llu RACE-CLEAR T=%08x was=%f\n", (unsigned long long)tick,
                             t, (double)f);
        }
        st.pendingClear = false;
    }
    if (!st.scanDone)
    {
        scanChunk(st, gems, ram, ramSize, tick);
        if (!st.scanDone)
        {
            std::memcpy(st.prev, cur, sizeof(cur));
            st.prevValid = true;
            return out;
        }
    }
    if (!st.prevValid || teleportJump(st.prev, cur))
    {
        const bool jump = st.prevValid && teleportJump(st.prev, cur);
        std::memcpy(st.prev, cur, sizeof(cur));
        st.prevValid = true;
        if (jump)
        {
            // Reset-teleport: the swept path is meaningless, and streaming
            // may have revived collected gems -> re-scan + re-hide.
            st.scanDone = false;
            st.scanCursor = 0u;
            st.rehideArmed = true;
            std::memset(st.resolved, 0, sizeof(st.resolved)); // re-scan re-resolves
            std::fprintf(stderr, "[tk45c] tick=%llu TELEPORT, re-scan armed\n", (unsigned long long)tick);
        }
        return out;
    }
    for (size_t i = 0u; i < gems.size(); ++i)
    {
        if (isCollected(st, i))
            continue;
        const Gem &g = gems[i];
        if (!segSphereHit(st.prev, cur, g.xyz, g.rad))
            continue;
        setCollected(st, i);
        const float want = g.value == 2u ? 2.0f : g.value == 3u ? 3.0f : 5.0f;
        float had = 1.0f;
        bool wrote = false;
        if (tOk)
        {
            uint32_t curBits = 0u;
            if (readU32(ram, ramSize, t + kMultOff, curBits))
            {
                std::memcpy(&had, &curBits, 4);
                if (std::isfinite(had) && want > had)
                    wrote = writeF32(ram, ramSize, t + kMultOff, want);
            }
        }
        const char *hide = "NOHIDE-unresolved";
        const uint32_t pb = st.resolved[i];
        if (pb != 0u)
        {
            hide = "NOHIDE-stale";
            uint32_t v[3] = {0};
            if (readU32(ram, ramSize, pb + kInstPosOff, v[0]) &&
                readU32(ram, ramSize, pb + kInstPosOff + 4u, v[1]) &&
                readU32(ram, ramSize, pb + kInstPosOff + 8u, v[2]))
            {
                uint32_t gg[3] = {0};
                std::memcpy(gg, g.xyz, 12);
                if (v[0] == gg[0] && v[1] == gg[1] && v[2] == gg[2])
                {
                    uint32_t w8 = 0u;
                    if (readU32(ram, ramSize, pb + 8u, w8))
                    {
                        if ((w8 & 0xffu) == kDeadLow)
                        {
                            hide = "already-hidden";
                        }
                        else if (writeU32(ram, ramSize, pb + 8u, (w8 & ~0xffu) | kDeadLow))
                        {
                            hide = "HIDDEN";
                        }
                        else
                        {
                            hide = "NOHIDE-write-failed";
                        }
                    }
                }
            }
        }
        float live = 0.0f;
        const bool liveOk = readF32(ram, ramSize, kLiveScore, live);
        std::fprintf(stderr,
                     "[tk45c] tick=%llu PICKUP %s rid=%u x%u mult %.1f->%.1f %s live=%s T=%08x %s\n",
                     (unsigned long long)tick, g.course, g.rid, (unsigned)g.value, (double)had,
                     (double)(wrote ? want : had), hide, liveOk ? std::to_string((double)live).c_str() : "n/a",
                     t, g.donor);
        if (st.cap.ok)
        {
            out.fire = true;
            out.mask = maskForValue(g.value);
            out.ladder = want;
        }
        else
        {
            std::fprintf(stderr, "[tk45c] tick=%llu callout SKIP (no Icons capture yet)\n",
                         (unsigned long long)tick);
        }
    }
    std::memcpy(st.prev, cur, sizeof(cur));
    return out;
}

// Bank-clear entry (a0 = trick struct): clear +0x1c4 where the game clears
// +0x18. Unconditional on mode (fixes stale across courses); the write
// itself is conditional, so clean states see zero writes.
inline void onBankClear(uint8_t *ram, size_t ramSize, uint64_t tick, uint32_t t)
{
    // (Knob-gated by the glue, like poll/captureIcons.)
    if (!ram || ramSize == 0u || t == 0u || (t & 3u) != 0u || (t & ~kRamMask) != 0u)
        return;
    if (((t + kMultOff) & kRamMask) + 4u > ramSize)
        return;
    uint32_t cur = 0u;
    if (!readU32(ram, ramSize, t + kMultOff, cur) || cur == kOneBits)
        return;
    float f = 0.0f;
    std::memcpy(&f, &cur, 4);
    if (!std::isfinite(f))
        return;
    if (writeF32(ram, ramSize, t + kMultOff, 1.0f))
        std::fprintf(stderr, "[tk45c] tick=%llu BANK-CLEAR T=%08x was=%.1f\n", (unsigned long long)tick, t,
                     (double)f);
}

inline void captureIcons(State &st, uint32_t a0, uint32_t a1, const uint32_t s[8], float f20,
                         uint64_t tick)
{
    const bool first = !st.cap.ok;
    st.cap.ok = true;
    st.cap.tick = tick;
    st.cap.a0 = a0;
    st.cap.a1 = a1;
    std::memcpy(st.cap.s, s, sizeof(st.cap.s));
    st.cap.f20 = f20;
    if (first)
        std::fprintf(stderr, "[tk45c] tick=%llu Icons capture a0=%08x a1=%08x s2=%08x s3=%08x\n",
                     (unsigned long long)tick, a0, a1, s[2], s[3]);
}

} // namespace ps2_tk45c
