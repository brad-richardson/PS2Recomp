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
// No speech callout: the Icons path is combo-bound and muted in races
// (TK45c REPORT §callout, brief's "or explain why not"). Pickup, mult,
// hide and bank-clear only; no nested guest calls anywhere.
constexpr uint32_t kRiderPosOff = 0x110u;
constexpr uint32_t kTrickOff = 0x790u;
constexpr uint32_t kMultOff = 0x1c4u;
constexpr uint32_t kTrickSize = 0x1ccu;
constexpr uint32_t kRaceClockOff = 0xcu;
constexpr uint32_t kAppliedOff = 0x10u; // bank-applied mult (pre-reset read proves consume)
constexpr uint32_t kInstPosOff = 0x40u;
constexpr uint32_t kDeadLow = 0x05u; // hide: instance+8 low byte (lab: 23->05 dies)
constexpr uint32_t kShowLow = 0x03u;  // world-loaded alive low byte (reshow target)
constexpr uint32_t kAliveLow2 = 0x23u; // lab-bound alive low byte (also hideable)
constexpr uint32_t kMaxRes = 4u;       // instances per gem (stacked triples + 1)
constexpr float kTeleportJump = 3000.0f;
constexpr size_t kMaxGems = 512u;
constexpr uint32_t kScanChunk = 1u << 20;

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
    uint32_t resolved[kMaxGems][kMaxRes] = {{0}};
    uint8_t nres[kMaxGems] = {0};
    bool ambig[kMaxGems] = {false};
};

inline State &state() noexcept
{
    static State st;
    return st;
}

// fullReset is defined after isCollected/setCollected (below).

inline bool isCollected(const State &st, size_t i) noexcept
{
    return ((st.collected[i >> 6] >> (i & 63u)) & 1u) != 0u;
}

inline void setCollected(State &st, size_t i) noexcept
{
    st.collected[i >> 6] |= uint64_t{1} << (i & 63u);
}

// New race/session (or Tricky->Stock): collected gems show again (the world
// is NOT reloaded — same instance addresses — so race 2 would otherwise run
// with invisible-but-live gems). Only 05->03 with a reverified translation;
// stale pointers (course change) mismatch and are skipped.
inline void fullReset(State &st, const std::vector<Gem> &gems, uint8_t *ram, size_t ramSize,
                      uint64_t tick) noexcept
{
    if (ram && ramSize != 0u && !gems.empty())
    {
        for (size_t i = 0u; i < gems.size() && i < kMaxGems; ++i)
        {
            if (!isCollected(st, i))
                continue;
            for (uint8_t j = 0u; j < st.nres[i]; ++j)
            {
                const uint32_t pb = st.resolved[i][j];
                if (pb == 0u)
                    continue;
                uint32_t v[3] = {0};
                uint32_t gg[3] = {0};
                std::memcpy(gg, gems[i].xyz, 12);
                if (!readU32(ram, ramSize, pb + kInstPosOff, v[0]) ||
                    !readU32(ram, ramSize, pb + kInstPosOff + 4u, v[1]) ||
                    !readU32(ram, ramSize, pb + kInstPosOff + 8u, v[2]) || v[0] != gg[0] ||
                    v[1] != gg[1] || v[2] != gg[2])
                    continue;
                uint32_t w8 = 0u;
                if (readU32(ram, ramSize, pb + 8u, w8) && (w8 & 0xffu) == kDeadLow &&
                    writeU32(ram, ramSize, pb + 8u, (w8 & ~0xffu) | kShowLow))
                    std::fprintf(stderr, "[tk45c] tick=%llu RESHOW %s rid=%u at=%08x\n",
                                 (unsigned long long)tick, gems[i].course, gems[i].rid, pb);
            }
        }
    }
    st.prevValid = false;
    st.scanDone = false;
    st.scanCursor = 0u;
    st.rehideArmed = false;
    st.raceClock = ps2_ssx3_tricky_hud::RaceClock{};
    std::memset(st.collected, 0, sizeof(st.collected));
    std::memset(st.resolved, 0, sizeof(st.resolved));
    std::memset(st.nres, 0, sizeof(st.nres));
    std::memset(st.ambig, 0, sizeof(st.ambig));
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
                      uint64_t tick, uint32_t clock)
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
            // Sanity: the node's link words must be null or mapped RAM.
            // Null links are legit (ram1: all 25 first-misses had a null
            // link); the hide write re-verifies the translation anyway.
            uint32_t w0 = 0u, w1 = 0u;
            std::memcpy(&w0, ram + q, 4);
            std::memcpy(&w1, ram + q + 4u, 4);
            const uint32_t p0 = w0 & kRamMask, p1 = w1 & kRamMask;
            if ((w0 & ~kRamMask) != 0u || (w1 & ~kRamMask) != 0u ||
                (w0 != 0u && p0 + 64u > ramSize) || (w1 != 0u && p1 + 64u > ramSize))
                continue;
            if (st.ambig[i])
                continue;
            bool seen = false;
            for (uint8_t j = 0u; j < st.nres[i]; ++j)
                seen = seen || st.resolved[i][j] == q;
            if (seen)
                continue;
            if (st.nres[i] >= kMaxRes)
            {
                std::fprintf(stderr, "[tk45c] tick=%llu %s rid=%u AMBIGUOUS (%u+ instances), hide off\n",
                             (unsigned long long)tick, gems[i].course, gems[i].rid,
                             (unsigned)kMaxRes);
                st.ambig[i] = true; // fail closed: never hide an ambiguous gem
                st.nres[i] = 0u;
                continue;
            }
            st.resolved[i][st.nres[i]++] = q;
        }
    }
    st.scanCursor = end;
    if (end >= ramSize)
    {
        st.scanDone = true;
        size_t n = 0u;
        for (size_t i = 0u; i < gems.size(); ++i)
            n += st.nres[i] != 0u ? 1u : 0u;
        std::fprintf(stderr, "[tk45c] tick=%llu scan complete: %zu/%zu gems resolved clk=%u\n",
                     (unsigned long long)tick, n, gems.size(), clock);
        // Re-hide pass after a reset/teleport re-scan: collected gems whose
        // instance came back alive (streaming revive) hide again.
        if (st.rehideArmed)
        {
            st.rehideArmed = false;
            for (size_t i = 0u; i < gems.size(); ++i)
            {
                if (!isCollected(st, i))
                    continue;
                for (uint8_t j = 0u; j < st.nres[i]; ++j)
                {
                    uint8_t *wram = const_cast<uint8_t *>(ram);
                    uint32_t v[3] = {0};
                    const uint32_t pb = st.resolved[i][j] & kRamMask;
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
                    const uint32_t lo = b & 0xffu;
                    if ((lo == kShowLow || lo == kAliveLow2) &&
                        writeU32(wram, ramSize, pb + 8u, (b & ~0xffu) | kDeadLow))
                        std::fprintf(stderr, "[tk45c] tick=%llu REHIDE %s rid=%u at=%08x\n",
                                     (unsigned long long)tick, gems[i].course, gems[i].rid, pb);
                }
            }
        }
    }
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
    // Full-span, wrap-free: t is already masked-small, so t + size cannot
    // overflow u32; the (x & mask) + 4 shape can never fire for aligned t.
    if ((t & ~kRamMask) != 0u || t + kTrickSize + 4u > ramSize)
    {
        t = 0u;
        return false;
    }
    return true;
}

// The boundary-hook poll (state, table and course flag injected, so tests
// drive it with fake RAM; the glue passes the singletons). All writes
// conditional.
inline void poll(State &st, const std::vector<Gem> &gems, uint8_t *ram, size_t ramSize, uint64_t tick,
                 uint32_t a0, bool tricky)
{
    if (!tricky)
    {
        if (st.wasTricky)
        {
            fullReset(st, gems, ram, ramSize, tick);
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
        return;
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
        return;
    }
    uint32_t clock = 0u;
    if (!readU32(ram, ramSize, b + kRaceClockOff, clock))
    {
        st.prevValid = false;
        return;
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
        fullReset(st, gems, ram, ramSize, tick);
        st.pendingClear = true;
    }
    st.lastR = r;
    st.lastClock = clock;
    if (!racing)
    {
        st.prevValid = false;
        return;
    }
    float cur[3] = {0, 0, 0};
    if (!readF32(ram, ramSize, r + kRiderPosOff, cur[0]) ||
        !readF32(ram, ramSize, r + kRiderPosOff + 4u, cur[1]) ||
        !readF32(ram, ramSize, r + kRiderPosOff + 8u, cur[2]))
    {
        st.prevValid = false;
        return;
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
        scanChunk(st, gems, ram, ramSize, tick, clock);
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
            std::memset(st.nres, 0, sizeof(st.nres));
            std::memset(st.ambig, 0, sizeof(st.ambig));
            std::fprintf(stderr, "[tk45c] tick=%llu TELEPORT, re-scan armed\n", (unsigned long long)tick);
        }
        return;
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
        // Hide EVERY live instance (stacked gems share the spot); the line
        // reports the worst outcome (refused/failed > hidden > already > stale).
        int ho = 0; // 0 stale, 1 hidden, 2 already, 3 refused, 4 write-failed
        if (st.nres[i] != 0u && !st.ambig[i])
        {
            uint32_t gg[3] = {0};
            std::memcpy(gg, g.xyz, 12);
            for (uint8_t j = 0u; j < st.nres[i]; ++j)
            {
                const uint32_t pb = st.resolved[i][j];
                if (pb == 0u)
                    continue;
                uint32_t v[3] = {0};
                if (!readU32(ram, ramSize, pb + kInstPosOff, v[0]) ||
                    !readU32(ram, ramSize, pb + kInstPosOff + 4u, v[1]) ||
                    !readU32(ram, ramSize, pb + kInstPosOff + 8u, v[2]) || v[0] != gg[0] ||
                    v[1] != gg[1] || v[2] != gg[2])
                    continue;
                uint32_t w8 = 0u;
                if (!readU32(ram, ramSize, pb + 8u, w8))
                {
                    ho = 4;
                    continue;
                }
                const uint32_t lo = w8 & 0xffu;
                if (lo == kDeadLow)
                {
                    if (ho == 0)
                        ho = 2;
                }
                else if (lo != kShowLow && lo != kAliveLow2)
                    ho = 3;
                else if (writeU32(ram, ramSize, pb + 8u, (w8 & ~0xffu) | kDeadLow))
                {
                    if (ho != 3 && ho != 4)
                        ho = 1;
                }
                else
                    ho = 4;
            }
        }
        const char *hide = st.ambig[i] ? "NOHIDE-ambiguous"
            : st.nres[i] == 0u           ? "NOHIDE-unresolved"
            : ho == 1                    ? "HIDDEN"
            : ho == 2                    ? "already-hidden"
            : ho == 3                    ? "NOHIDE-refused"
            : ho == 4                    ? "NOHIDE-write-failed"
                                         : "NOHIDE-stale";
        float ap = 0.0f;
        const bool apOk = tOk && readF32(ram, ramSize, t + kAppliedOff, ap);
        std::fprintf(stderr,
                     "[tk45c] tick=%llu PICKUP %s rid=%u x%u mult %.1f->%.1f %s ap=%s T=%08x clk=%u %s\n",
                     (unsigned long long)tick, g.course, g.rid, (unsigned)g.value, (double)had,
                     (double)(wrote ? want : had), hide, apOk ? std::to_string((double)ap).c_str() : "n/a",
                     t, clock, g.donor);
    }
    std::memcpy(st.prev, cur, sizeof(cur));
}

// Bank-clear entry (a0 = trick struct): clear +0x1c4 where the game clears
// +0x18. Unconditional on mode (fixes stale across courses); the write
// itself is conditional, so clean states see zero writes. sub_00117838 is a
// straight-line reset (no +0x1c4 reads, no calls), so entry == exit; the bank
// consumes +0x1c4 in the caller before the reset (TK45b: bank 0x11962c).
// 0x117860 is the sole +0x18 clear (the only other +0x18 writer is the
// ladder max-track at 0x1194a0), so wipeout clears here too, via one of the
// 8 reset callers (src= identifies which, empirically).
inline void onBankClear(uint8_t *ram, size_t ramSize, uint64_t tick, uint32_t t, uint32_t clock,
                        uint32_t src)
{
    // (Knob-gated by the glue, like poll.)
    if (!ram || ramSize == 0u || t == 0u || (t & 3u) != 0u || (t & ~kRamMask) != 0u)
        return;
    if (t + kTrickSize + 4u > ramSize)
        return;
    uint32_t cur = 0u;
    if (!readU32(ram, ramSize, t + kMultOff, cur) || cur == kOneBits)
        return;
    float f = 0.0f;
    std::memcpy(&f, &cur, 4);
    if (!std::isfinite(f))
        return;
    float ap = 0.0f;
    const bool apOk = readF32(ram, ramSize, t + kAppliedOff, ap);
    if (writeF32(ram, ramSize, t + kMultOff, 1.0f))
        std::fprintf(stderr, "[tk45c] tick=%llu BANK-CLEAR T=%08x was=%.1f ap=%s clk=%u src=%08x\n",
                     (unsigned long long)tick, t, (double)f,
                     apOk ? std::to_string((double)ap).c_str() : "n/a", clock, src);
}

} // namespace ps2_tk45c
