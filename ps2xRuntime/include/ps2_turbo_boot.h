// PS2X_TURBO_BOOT=1 (BTS1): run the boot unpaced with host audio muted,
// then return to normal pacing at the first input-needing screen.
//
// Output-only by construction: while armed the runtime skips vsync sleeps
// and feeds the host audio callback silence; the guest trajectory is never
// touched (no guest writes, no HLE result changes). Knob off (default)
// wraps nothing and adds one relaxed bool check per vsync.
//
// Yield signal: the first FE.LUI title-screen (06title) lookup or FE.LUI
// file open at/after tick 200, plus a fixed offset; any pad button press
// or a hard cap yields immediately. Exactly one stderr line is emitted,
// at yield.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace ps2_turbo_boot
{

inline constexpr uint32_t kScreenLookupPc = 0x397CC0u;
inline constexpr uint32_t kTitleScreenHash = 0x03db0b45u; // PJW("06title"), FE.LUI screen table
inline constexpr uint64_t kLatchFloorTick = 200u;
inline constexpr uint64_t kYieldOffsetTicks = 400u;
inline constexpr uint64_t kCapTick = 2000u;
inline constexpr uint64_t kAudioReserveFrames = 3840u; // 80 ms @ 48 kHz stereo target latency
inline constexpr size_t kLookupLogCap = 16u;

enum class Yield : uint8_t
{
    None = 0,
    Signal = 1,
    Input = 2,
    Cap = 3,
};

// Pure yield rule (unit-tested): the signal fires at the earliest latched
// anchor plus the fixed offset; the cap backstops a missing signal.
inline uint64_t signalTickFor(uint64_t latch06, uint64_t latchFE)
{
    uint64_t first = 0u;
    if (latch06 != 0u)
        first = latch06;
    if (latchFE != 0u && (first == 0u || latchFE < first))
        first = latchFE;
    return first == 0u ? 0u : first + kYieldOffsetTicks;
}

inline Yield evalTick(uint64_t tick, uint64_t signalTick)
{
    if (signalTick != 0u && tick >= signalTick)
        return Yield::Signal;
    if (tick >= kCapTick)
        return Yield::Cap;
    return Yield::None;
}

inline bool hashMatchesTitle(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    return a0 == kTitleScreenHash || a1 == kTitleScreenHash || a2 == kTitleScreenHash ||
           a3 == kTitleScreenHash;
}

inline bool latchable(uint64_t tick)
{
    return tick >= kLatchFloorTick;
}

inline bool pathIsFrontendLui(const char *path)
{
    // Case-insensitive substring match for FE.LUI (no platform-local API).
    static constexpr char kNeedle[] = "FE.LUI";
    if (path == nullptr)
        return false;
    for (const char *p = path; *p != '\0'; ++p)
    {
        size_t i = 0u;
        for (; kNeedle[i] != '\0'; ++i)
        {
            const char c = p[i];
            if (c == '\0')
                break;
            char a = c;
            if (a >= 'a' && a <= 'z')
                a = static_cast<char>(a - 'a' + 'A');
            if (a != kNeedle[i])
                break;
        }
        if (kNeedle[i] == '\0')
            return true;
    }
    return false;
}

inline bool enabled()
{
    static const bool on = [] {
        const char *v = std::getenv("PS2X_TURBO_BOOT");
        return v != nullptr && v[0] == '1' && v[1] == '\0';
    }();
    return on;
}

struct LookupEntry
{
    uint64_t tick = 0u;
    uint32_t hash = 0u; // a0 of the lookup call
    bool title = false;
};

struct State
{
    std::atomic<bool> armed{false};
    std::atomic<bool> done{false};
    uint64_t latch06 = 0u;
    uint64_t latchFE = 0u;
    LookupEntry lookups[kLookupLogCap]{};
    size_t nLookups = 0u;
    uint64_t totalLookups = 0u;
};

inline State &state()
{
    static State s;
    return s;
}

inline bool pacingSuspended()
{
    const State &s = state();
    return s.armed.load(std::memory_order_relaxed) && !s.done.load(std::memory_order_relaxed);
}

inline bool muted()
{
    return pacingSuspended();
}

inline void arm()
{
    if (!enabled())
        return;
    State &s = state();
    s.armed.store(false, std::memory_order_relaxed);
    s.done.store(false, std::memory_order_relaxed);
    s.latch06 = 0u;
    s.latchFE = 0u;
    s.nLookups = 0u;
    s.totalLookups = 0u;
    for (size_t i = 0u; i < kLookupLogCap; ++i)
        s.lookups[i] = LookupEntry{};
    s.armed.store(true, std::memory_order_relaxed);
}

inline void emitYieldLine(uint64_t tick, Yield reason)
{
    const State &s = state();
    const char *name = reason == Yield::Signal ? "signal" : reason == Yield::Input ? "input" : "cap";
    std::fprintf(stderr, "[turbo-boot] yield tick=%llu reason=%s latch06=%llu latchFE=%llu lookups=%llu [",
                 static_cast<unsigned long long>(tick), name,
                 static_cast<unsigned long long>(s.latch06),
                 static_cast<unsigned long long>(s.latchFE),
                 static_cast<unsigned long long>(s.totalLookups));
    for (size_t i = 0u; i < s.nLookups; ++i)
        std::fprintf(stderr, "%s%llu:%08x%s", i == 0u ? "" : ",",
                     static_cast<unsigned long long>(s.lookups[i].tick), s.lookups[i].hash,
                     s.lookups[i].title ? "*" : "");
    std::fprintf(stderr, "]\n");
}

inline bool tryYield(uint64_t tick, Yield reason)
{
    State &s = state();
    bool expected = false;
    if (!s.done.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return false;
    emitYieldLine(tick, reason);
    return true;
}

// Per-vsync poll (EE thread). Keeps the host audio ring near the rejoin
// reserve while armed, then evaluates the signal/cap rule.
template <typename Ring> inline void onVsyncTick(uint64_t tick, Ring &ring)
{
    State &s = state();
    if (!s.armed.load(std::memory_order_relaxed) || s.done.load(std::memory_order_relaxed))
        return;
    uint32_t frame = 0u;
    size_t drained = 0u;
    while (ring.size() > kAudioReserveFrames && drained < 65536u)
    {
        if (!ring.pop(frame))
            break;
        ++drained;
    }
    const uint64_t signalTick = signalTickFor(s.latch06, s.latchFE);
    const Yield r = evalTick(tick, signalTick);
    if (r != Yield::None)
        tryYield(tick, r);
}

inline void noteScreenLookup(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint64_t tick)
{
    State &s = state();
    if (!s.armed.load(std::memory_order_relaxed) || s.done.load(std::memory_order_relaxed))
        return;
    const bool title = hashMatchesTitle(a0, a1, a2, a3);
    ++s.totalLookups;
    if (s.nLookups < kLookupLogCap)
        s.lookups[s.nLookups++] = LookupEntry{tick, a0, title};
    if (title && s.latch06 == 0u && latchable(tick))
        s.latch06 = tick;
}

inline void noteFileOpen(const char *path, uint64_t tick)
{
    State &s = state();
    if (!s.armed.load(std::memory_order_relaxed) || s.done.load(std::memory_order_relaxed))
        return;
    if (s.latchFE == 0u && latchable(tick) && pathIsFrontendLui(path))
        s.latchFE = tick;
}

inline void notePadInput(uint64_t tick)
{
    State &s = state();
    if (!s.armed.load(std::memory_order_relaxed) || s.done.load(std::memory_order_relaxed))
        return;
    tryYield(tick, Yield::Input);
}

inline void noteSavestateLoad()
{
    // A loaded state resumes paced; turbo is a boot-path feature. Silent:
    // no yield happened, so no yield line.
    state().armed.store(false, std::memory_order_relaxed);
}

} // namespace ps2_turbo_boot
