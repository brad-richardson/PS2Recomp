// TS2 diagnostic-only dynamic boundary census. Opt in with
// PS2X_TS2_OBSERVER=1; PS2X_TS2_OBSERVER_DUMP_TICK chooses one bounded dump.
#pragma once

#include "ps2_runtime.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <unordered_map>

namespace ps2_ts2_observer
{
struct Frame
{
    uint32_t depth = 0;
    uint32_t p = 0, r = 0, c = 0, a = 0, entrySp = 0;
};

struct Key
{
    uint32_t pc, region, offset, base, target, descriptor, width;
    bool operator==(const Key &x) const noexcept
    {
        return pc == x.pc && region == x.region && offset == x.offset &&
               (region <= 6u || base == x.base) && target == x.target && descriptor == x.descriptor &&
               width == x.width;
    }
};
struct Hash
{
    size_t operator()(const Key &k) const noexcept
    {
        uint64_t h = 1469598103934665603ull;
        for (uint32_t x : {k.pc, k.region, k.offset, k.region <= 6u ? 0u : k.base,
                           k.target, k.descriptor, k.width})
            h = (h ^ x) * 1099511628211ull;
        return static_cast<size_t>(h);
    }
};

struct StoreStats
{
    uint64_t equal = 0, increment = 0, decrement = 0, pointer = 0, stamp = 0, laterRead = 0;
    uint64_t firstOld = 0, firstNew = 0, lastStep = UINT64_MAX;
    uint32_t writesThisStep = 0, maxWritesPerStep = 0;
    bool hasSample = false;
};
struct LastWrite
{
    Key key;
    uint64_t tick;
};

// Guest execution and the EE scheduler run on the same host thread. The guest
// thread ID, not the host call stack, keeps the frame live across checkpoints.
inline thread_local uint32_t guestThread = 0;
inline thread_local bool guestInterrupt = false;
inline thread_local std::unordered_map<uint32_t, Frame> frames;
inline thread_local std::unordered_map<Key, uint64_t, Hash> counts;
inline thread_local std::unordered_map<Key, StoreStats, Hash> storeStats;
inline thread_local std::unordered_map<uint32_t, LastWrite> lastNamedWrite;
inline thread_local uint64_t riderStep = 0;
inline thread_local uint64_t dropped = 0;
inline thread_local uint64_t stackStores = 0;
inline thread_local uint64_t boundaryEntries = 0;
inline thread_local uint64_t boundaryExits = 0;
inline thread_local uint64_t riderEntries = 0;
inline thread_local uint64_t riderExits = 0;
inline thread_local uint64_t solverEntries = 0;
inline thread_local uint64_t solverExits = 0;
inline thread_local uint64_t scopeErrors = 0;
inline thread_local uint64_t tickMismatches = 0;
inline thread_local uint64_t firstBadTick = 0;
inline thread_local uint64_t currentTick = 0;
inline thread_local uint64_t tickEntries = 0;
inline thread_local uint64_t tickExits = 0;
inline thread_local bool dumped = false;
inline constexpr size_t kMaxKeys = 20000;

inline bool enabled() noexcept
{
    static const bool on = [] {
        const char *s = std::getenv("PS2X_TS2_OBSERVER");
        return s && s[0] == '1' && s[1] == '\0';
    }();
    return on;
}

inline uint32_t read32(const uint8_t *ram, uint32_t address) noexcept
{
    uint32_t value = 0;
    address &= 0x1fffffffu;
    if (ram && address < 0x02000000u - 3u)
        std::memcpy(&value, ram + address, 4);
    return value;
}

inline bool add(Key k) noexcept
{
    auto it = counts.find(k);
    if (it != counts.end()) { ++it->second; return true; }
    if (counts.size() >= kMaxKeys) { ++dropped; return false; }
    counts.emplace(k, 1);
    return true;
}

inline void setThread(uint32_t id, bool interrupt) noexcept
{
    if (enabled()) { guestThread = id; guestInterrupt = interrupt; }
}

inline void begin(const uint8_t *ram, R5900Context *ctx, uint32_t rider, bool solver) noexcept
{
    if (!enabled() || !ctx || guestInterrupt || dumped) return;
    Frame &f = frames[guestThread];
    if (f.depth == 0)
    {
        f.r = rider & 0x1fffffffu;
        f.p = read32(ram, f.r + 0x77cu) & 0x1fffffffu;
        f.c = read32(ram, f.r + 0x788u) & 0x1fffffffu;
        f.a = read32(ram, 0x4a5b64u) & 0x1fffffffu;
        f.entrySp = getRegU32(ctx, 29) & 0x1fffffffu;
    }
    if (f.depth >= 8u) { ++scopeErrors; return; }
    ++f.depth;
    ++boundaryEntries;
    ++tickEntries;
    if (solver) ++solverEntries; else ++riderEntries;
}

inline void beginRider(const uint8_t *ram, R5900Context *ctx) noexcept
{
    if (enabled() && !guestInterrupt && !dumped) ++riderStep;
    begin(ram, ctx, getRegU32(ctx, 4), false);
}

inline void beginSolver(const uint8_t *ram, R5900Context *ctx) noexcept
{
    // At 0x11144c, a1 is P. The outer rider scope normally already owns R.
    const uint32_t p = getRegU32(ctx, 5) & 0x1fffffffu;
    begin(ram, ctx, read32(ram, p + 0x24u), true);
}

inline void end(bool solver) noexcept
{
    if (!enabled() || guestInterrupt || dumped) return;
    auto it = frames.find(guestThread);
    if (it == frames.end() || it->second.depth == 0) { ++scopeErrors; return; }
    --it->second.depth;
    ++boundaryExits;
    ++tickExits;
    if (solver) ++solverExits; else ++riderExits;
}

inline void endRider() noexcept { end(false); }
inline void endSolver() noexcept { end(true); }

inline void dump(uint64_t tick) noexcept
{
    if (dumped) return;
    dumped = true;
    const bool clean = boundaryEntries > 0 && boundaryEntries == boundaryExits &&
                       riderEntries == riderExits && solverEntries == solverExits &&
                       dropped == 0 && scopeErrors == 0 && tickMismatches == 0 &&
                       stackStores > 0;
    std::fprintf(stderr, "ts2obs begin tick=%llu self_check=%s keys=%zu entries=%llu exits=%llu rider_entries=%llu rider_exits=%llu solver_entries=%llu solver_exits=%llu stack_excluded=%llu dropped=%llu scope_errors=%llu tick_mismatches=%llu first_bad_tick=%llu\n",
                 static_cast<unsigned long long>(tick), clean ? "PASS" : "FAIL", counts.size(),
                 static_cast<unsigned long long>(boundaryEntries),
                 static_cast<unsigned long long>(boundaryExits),
                 static_cast<unsigned long long>(riderEntries),
                 static_cast<unsigned long long>(riderExits),
                 static_cast<unsigned long long>(solverEntries),
                 static_cast<unsigned long long>(solverExits),
                 static_cast<unsigned long long>(stackStores),
                 static_cast<unsigned long long>(dropped),
                 static_cast<unsigned long long>(scopeErrors),
                 static_cast<unsigned long long>(tickMismatches),
                 static_cast<unsigned long long>(firstBadTick));
    if (!clean)
    {
        std::fprintf(stderr, "ts2obs end tick=%llu invalid=1\n", static_cast<unsigned long long>(tick));
        std::fflush(stderr);
        return;
    }
    for (const auto &[key, n] : counts)
    {
        const auto si = storeStats.find(key);
        const StoreStats empty{};
        const StoreStats &s = si == storeStats.end() ? empty : si->second;
        std::fprintf(stderr,
                     "ts2obs row pc=%08x region=%u off=%08x base=%08x target=%08x descriptor=%08x width=%u count=%llu eq=%llu inc=%llu dec=%llu ptr=%llu stamp=%llu later_read=%llu max_step=%u old=%016llx new=%016llx\n",
                     key.pc, key.region, key.offset, key.base, key.target,
                     key.descriptor, key.width, static_cast<unsigned long long>(n),
                     static_cast<unsigned long long>(s.equal),
                     static_cast<unsigned long long>(s.increment),
                     static_cast<unsigned long long>(s.decrement),
                     static_cast<unsigned long long>(s.pointer),
                     static_cast<unsigned long long>(s.stamp),
                     static_cast<unsigned long long>(s.laterRead), s.maxWritesPerStep,
                     static_cast<unsigned long long>(s.firstOld),
                     static_cast<unsigned long long>(s.firstNew));
    }
    std::fprintf(stderr, "ts2obs end tick=%llu\n", static_cast<unsigned long long>(tick));
    std::fflush(stderr);
}

inline void setTick(uint64_t tick) noexcept
{
    if (!enabled() || dumped) return;
    if (tick != currentTick)
    {
        if (currentTick != 0)
        {
            bool active = false;
            for (const auto &[id, frame] : frames)
                if (frame.depth != 0) active = true;
            if (tickEntries != tickExits || active)
            {
                ++tickMismatches;
                if (!firstBadTick) firstBadTick = currentTick;
            }
        }
        currentTick = tick;
        tickEntries = tickExits = 0;
    }
    static const uint64_t dumpTick = [] {
        const char *s = std::getenv("PS2X_TS2_OBSERVER_DUMP_TICK");
        return s ? std::strtoull(s, nullptr, 0) : 2300ull;
    }();
    if (tick >= dumpTick) dump(tick);
}

// Region IDs: 1 R, 2 P, 3 C, 4 manager A, 5 global/data, 6 other
// heap (256-byte address bucket, not a proved object base); 7 call,
// 8 indirect call/jump, 9 post-pass trigger, 10 selector case.
inline bool guestPointer(uint64_t value) noexcept
{
    return value >= 0x00100000u && value < 0x02000000u && (value & 3u) == 0u;
}

inline void noteLoad(R5900Context *ctx, uint32_t address, uint32_t width) noexcept
{
    if (!enabled() || !ctx || dumped || guestInterrupt) return;
    const auto fi = frames.find(guestThread);
    if (fi == frames.end() || fi->second.depth == 0) return;
    address &= 0x1fffffffu;
    for (uint32_t lane = 0; lane < width; lane += 4)
    {
        auto wi = lastNamedWrite.find((address + lane) & ~3u);
        if (wi == lastNamedWrite.end() || currentTick <= wi->second.tick) continue;
        auto si = storeStats.find(wi->second.key);
        if (si != storeStats.end()) ++si->second.laterRead;
    }
}

inline void noteStore(const uint8_t *ram, R5900Context *ctx, uint32_t address,
                      uint32_t width, uint64_t newLo, uint64_t newHi) noexcept
{
    if (!enabled() || !ctx || dumped || guestInterrupt) return;
    auto it = frames.find(guestThread);
    if (it == frames.end() || it->second.depth == 0) return;
    const Frame &f = it->second;
    const uint32_t sp = getRegU32(ctx, 29) & 0x1fffffffu;
    address &= 0x1fffffffu;
    if (address >= sp && address <= f.entrySp + 0x1000u)
    {
        ++stackStores;
        return;
    }
    uint32_t region = 6, base = address & ~0xffu, offset = address & 0xffu;
    if (f.c && address >= f.c && address < f.c + 0x200u)
        region = 3, base = f.c, offset = address - f.c;
    else if (f.r && address >= f.r && address < f.r + 0x2000u)
        region = 1, base = f.r, offset = address - f.r;
    else if (f.p && address >= f.p && address < f.p + 0xe10u)
        region = 2, base = f.p, offset = address - f.p;
    else if (f.a && address >= f.a && address < f.a + 0x400u)
        region = 4, base = f.a, offset = address - f.a;
    else if (address >= 0x00400000u && address < 0x00600000u)
        region = 5, base = 0, offset = address;
    // For named regions, aggregate across object instances; the first base is
    // still carried in the row as a concrete example address.
    const Key key{ctx->pc, region, offset, base, 0, 0, width};
    if (!add(key)) return;
    StoreStats &s = storeStats[key];
    if (s.lastStep != riderStep)
    {
        s.lastStep = riderStep;
        s.writesThisStep = 0;
    }
    if (++s.writesThisStep > s.maxWritesPerStep) s.maxWritesPerStep = s.writesThisStep;
    if (width <= 8 && ram && address <= 0x02000000u - width)
    {
        uint64_t oldValue = 0;
        std::memcpy(&oldValue, ram + address, width);
        const uint64_t mask = width == 8 ? UINT64_MAX : ((1ull << (width * 8u)) - 1ull);
        const uint64_t newValue = newLo & mask;
        if (!s.hasSample)
        {
            s.firstOld = oldValue;
            s.firstNew = newValue;
            s.hasSample = true;
        }
        const uint64_t delta = (newValue - oldValue) & mask;
        if (delta == 0) ++s.equal;
        if (delta == 1) ++s.increment;
        if (delta == mask) ++s.decrement;
        if (width == 4 && guestPointer(newValue) && (oldValue == 0 || guestPointer(oldValue))) ++s.pointer;
        if (newValue == currentTick || newValue == currentTick + 1u) ++s.stamp;
    }
    if (region >= 1u && region <= 3u)
    {
        for (uint32_t lane = 0; lane < width; lane += 4)
            lastNamedWrite[(address + lane) & ~3u] = LastWrite{key, currentTick};
    }
    (void)newHi;
}

inline uint32_t descriptorFor(R5900Context *ctx, uint32_t source) noexcept
{
    if (!ctx) return 0;
    switch (source)
    {
    case 0x13858cu: case 0x13a014u: case 0x13b534u: return getRegU32(ctx, 7);
    case 0x13ebecu: case 0x13b094u: return getRegU32(ctx, 2);
    case 0x13b038u: case 0x13aaacu: case 0x105e84u: return getRegU32(ctx, 3);
    default: return 0;
    }
}

inline void noteBranch(const uint8_t *ram, R5900Context *ctx, uint32_t source,
                       uint32_t target, bool call, bool indirect) noexcept
{
    if (!enabled() || !ctx || dumped || guestInterrupt) return;
    Frame &f = frames[guestThread];
    if (f.depth != 0 && (call || indirect))
    {
        uint32_t region = indirect ? 8u : 7u;
        if (source >= 0x11143cu && source <= 0x11148cu && ((source - 0x11143cu) & 0xfu) == 0)
            region = 10u;
        add({source, region, 0, f.p, target, descriptorFor(ctx, source), 0});
    }
    if (target == 0x139c88u || target == 0x105d98u || source == 0x13ac64u)
        add({source, 9u, 0, f.depth ? f.p : 0u, target, descriptorFor(ctx, source), 0});
}
} // namespace ps2_ts2_observer
