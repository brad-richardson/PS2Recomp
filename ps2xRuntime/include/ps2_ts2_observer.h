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
    bool active = false;
    uint32_t p = 0, r = 0, c = 0, a = 0, entrySp = 0;
};

struct Key
{
    uint32_t pc, region, offset, base, target, descriptor, width;
    bool operator==(const Key &x) const noexcept
    {
        return pc == x.pc && region == x.region && offset == x.offset &&
               (region <= 5u || base == x.base) && target == x.target && descriptor == x.descriptor &&
               width == x.width;
    }
};
struct Hash
{
    size_t operator()(const Key &k) const noexcept
    {
        uint64_t h = 1469598103934665603ull;
        for (uint32_t x : {k.pc, k.region, k.offset, k.region <= 5u ? 0u : k.base,
                           k.target, k.descriptor, k.width})
            h = (h ^ x) * 1099511628211ull;
        return static_cast<size_t>(h);
    }
};

// Guest execution and the EE scheduler run on the same host thread. The guest
// thread ID, not the host call stack, keeps the frame live across checkpoints.
inline thread_local uint32_t guestThread = 0;
inline thread_local bool guestInterrupt = false;
inline thread_local std::unordered_map<uint32_t, Frame> frames;
inline thread_local std::unordered_map<Key, uint64_t, Hash> counts;
inline thread_local uint64_t dropped = 0;
inline thread_local uint64_t stackStores = 0;
inline thread_local uint64_t boundaryEntries = 0;
inline thread_local uint64_t boundaryExits = 0;
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

inline void add(Key k) noexcept
{
    auto it = counts.find(k);
    if (it != counts.end()) { ++it->second; return; }
    if (counts.size() >= kMaxKeys) { ++dropped; return; }
    counts.emplace(k, 1);
}

inline void setThread(uint32_t id, bool interrupt) noexcept
{
    if (enabled()) { guestThread = id; guestInterrupt = interrupt; }
}

inline void dump(uint64_t tick) noexcept
{
    if (dumped) return;
    dumped = true;
    std::fprintf(stderr, "ts2obs begin tick=%llu keys=%zu entries=%llu exits=%llu stack=%llu dropped=%llu\n",
                 static_cast<unsigned long long>(tick), counts.size(),
                 static_cast<unsigned long long>(boundaryEntries),
                 static_cast<unsigned long long>(boundaryExits),
                 static_cast<unsigned long long>(stackStores),
                 static_cast<unsigned long long>(dropped));
    for (const auto &[key, n] : counts)
        std::fprintf(stderr,
                     "ts2obs row pc=%08x region=%u off=%08x base=%08x target=%08x descriptor=%08x width=%u count=%llu\n",
                     key.pc, key.region, key.offset, key.base, key.target,
                     key.descriptor, key.width, static_cast<unsigned long long>(n));
    std::fprintf(stderr, "ts2obs end tick=%llu\n", static_cast<unsigned long long>(tick));
    std::fflush(stderr);
}

inline void setTick(uint64_t tick) noexcept
{
    if (!enabled() || dumped) return;
    static const uint64_t dumpTick = [] {
        const char *s = std::getenv("PS2X_TS2_OBSERVER_DUMP_TICK");
        return s ? std::strtoull(s, nullptr, 0) : 2300ull;
    }();
    if (tick >= dumpTick) dump(tick);
}

// Region IDs: 1 R, 2 P, 3 C, 4 manager A, 5 global/data, 6 other
// heap (256-byte address bucket, not a proved object base); 7 call,
// 8 indirect call/jump, 9 post-pass trigger, 10 selector case.
inline void noteStore(R5900Context *ctx, uint32_t address, uint32_t width) noexcept
{
    if (!enabled() || !ctx || dumped || guestInterrupt) return;
    auto it = frames.find(guestThread);
    if (it == frames.end() || !it->second.active) return;
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
    add({ctx->pc, region, offset, base, 0, 0, width});
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
    if (source == 0x128ddcu && target == 0x1216e0u)
    {
        f.active = true;
        f.p = getRegU32(ctx, 4) & 0x1fffffffu;
        f.r = read32(ram, f.p + 0x24u) & 0x1fffffffu;
        f.c = read32(ram, f.r + 0x788u) & 0x1fffffffu;
        f.a = read32(ram, 0x4a5b64u) & 0x1fffffffu;
        f.entrySp = getRegU32(ctx, 29) & 0x1fffffffu;
        ++boundaryEntries;
    }
    if (f.active && (call || indirect))
    {
        uint32_t region = indirect ? 8u : 7u;
        if (source >= 0x11143cu && source <= 0x11148cu && ((source - 0x11143cu) & 0xfu) == 0)
            region = 10u;
        add({source, region, 0, f.p, target, descriptorFor(ctx, source), 0});
    }
    if (target == 0x139c88u || target == 0x105d98u || source == 0x13ac64u)
        add({source, 9u, 0, f.p, target, descriptorFor(ctx, source), 0});
    if (source == 0x1216f4u && target == 0x128de4u && f.active)
    {
        f.active = false;
        ++boundaryExits;
    }
}
} // namespace ps2_ts2_observer
