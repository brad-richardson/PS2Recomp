// TS2 stock-H extraction adapter. The guest call remains unchanged; host-only
// context makes the physical pass an explicit, resumable boundary for G1.
#pragma once

#include "ps2_runtime.h"
#include "ps2_ts2_splitsites.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

// EE1P2: this header is the split120 product path and stays compiled in
// release builds.

namespace ps2_ts2_split60
{
struct GuestContext
{
    uint64_t macroOrdinal = 0;
    uint32_t halfIndex = 0;
    uint32_t rider = 0;
    uint32_t riderIndex = 0;
    uint32_t predictorEpoch = 0;
    uint32_t continuation = 0;
    uint32_t stockHNumerator = 1;
    uint32_t stockHDenominator = 60;
    // TS3: the 111408 selector (P+0xde0) read at begin(). Only cases 0/1/2
    // were ever observed and converted to H/2; 3/4/5 run once at full H.
    // Unreadable addresses read as 0 (converted), preserving old behavior.
    uint32_t selectorCase = 0;
    bool active = false;
    bool restartCheckpointPending = false;
};
struct PredictorIdentity
{
    uint32_t context = 0;
    uint32_t epoch = 0;
};

// TT1: one plain static instead of nine `inline thread_local`s. The old
// thread_locals cost 1.64 ms/frame of tlsdesc_resolver_dynamic + __tls_init
// on the Odin GameThread (MV3 Part 2), plus an unordered_map hash lookup on
// every intercepted guest load. Audit (TT1 REPORT): every entry point below
// runs on the EE executor thread only — halfLoad via READ32 from EE
// recompiled code and EE-thread syscall stubs, the rest via EeScheduler /
// dispatchGuestBranch / eeCheckpointDue. The MTVU worker, GS worker, audio
// and IOP threads touch none of this state (the mtvu::sync TLS samples in
// the MV3 profile are ps2_mtvu.h's own t_* vars, untouched here).
struct ThreadData
{
    GuestContext ctx;
    uint32_t half = 0;
    bool helper = false;
};
struct State
{
    uint32_t guestThread = 0;
    bool guestInterrupt = false;
    uint64_t scopeErrors = 0;
    // TK34/SPL1: rdram seen at begin() and which word sets hold their half
    // value for the current converted half.
    uint8_t *ram = nullptr;
    bool crashBodyHalf = false;
    bool bounceHalf = false;
    // Guest-thread-keyed records. Occupancy in practice is 1 (the main guest
    // thread runs the rider pass); the linear scan hits slot 0. Overflow
    // keeps exact map semantics past the flat slots; it is cold-only.
    static constexpr uint32_t kThreadSlots = 8;
    struct ThreadSlot
    {
        bool used = false;
        uint32_t key = 0;
        ThreadData data;
    };
    ThreadSlot threads[kThreadSlots];
    std::unordered_map<uint32_t, ThreadData> threadOverflow;
    // Rider-address-keyed predictor identities (<= ~6 riders per event).
    static constexpr uint32_t kRiderSlots = 16;
    struct RiderSlot
    {
        bool used = false;
        uint32_t key = 0;
        PredictorIdentity id;
    };
    RiderSlot riders[kRiderSlots];
    std::unordered_map<uint32_t, PredictorIdentity> riderOverflow;
    // HL1: halfLoad runs on every guest 32-bit load, so its findThread scan
    // per load is the #1 GameThread cost. This cache holds the current guest
    // thread's record and refreshes only when the scheduler switches guest
    // threads (observed via the key check) or getThread() inserts. Slot
    // addresses are fixed, and unordered_map never invalidates pointers on
    // insert/rehash (nothing erases), so the cached pointer is exact.
    uint32_t cachedKey = 0;
    ThreadData *cachedData = nullptr;
    bool cachedValid = false;
    ThreadData *findThread(uint32_t key) noexcept
    {
        for (uint32_t i = 0; i < kThreadSlots; ++i)
            if (threads[i].used && threads[i].key == key) return &threads[i].data;
        const auto it = threadOverflow.find(key);
        return it == threadOverflow.end() ? nullptr : &it->second;
    }
    ThreadData &getThread(uint32_t key) noexcept
    {
        if (ThreadData *d = findThread(key)) return *d;
        cachedValid = false; // the insert below may create the cached key
        for (uint32_t i = 0; i < kThreadSlots; ++i)
        {
            if (threads[i].used) continue;
            threads[i].used = true;
            threads[i].key = key;
            return threads[i].data;
        }
        return threadOverflow[key];
    }
    PredictorIdentity &getRider(uint32_t key) noexcept
    {
        for (uint32_t i = 0; i < kRiderSlots; ++i)
            if (riders[i].used && riders[i].key == key) return riders[i].id;
        const auto it = riderOverflow.find(key);
        if (it != riderOverflow.end()) return it->second;
        for (uint32_t i = 0; i < kRiderSlots; ++i)
        {
            if (riders[i].used) continue;
            riders[i].used = true;
            riders[i].key = key;
            return riders[i].id;
        }
        return riderOverflow[key];
    }
};
inline State g_state;

inline bool enabled() noexcept
{
    static const bool on = [] {
        const char *mode = std::getenv("PS2X_SSX3_SIM_MODE");
        return mode && (std::strcmp(mode, "split60_v1") == 0 ||
                        std::strcmp(mode, "split120_render60_v1") == 0);
    }();
    return on;
}

// TT1/GT1(b): one plain load. The guarded function-local static cost an
// ldarb + ~6 insns on EVERY guest load even with SIM off (GT1 §4). The flag
// is filled by setThread()/begin() (EE thread; setThread precedes all guest
// execution, and conversions need an active context from begin), so the
// value is always correct where observed; see the fill points.
inline bool g_halfModeFlag = false;
inline bool g_halfModeDone = false;
inline void initHalfMode() noexcept
{
    if (g_halfModeDone) return;
    const char *mode = std::getenv("PS2X_SSX3_SIM_MODE");
    g_halfModeFlag = mode && std::strcmp(mode, "split120_render60_v1") == 0;
    g_halfModeDone = true;
}
inline bool halfMode() noexcept
{
    return g_halfModeFlag;
}

// TS3: run unconverted selector cases (3/4/5) once per stock update at full
// H. This is the product default in split mode (the PS2X_TS3_NOFIX=1 pre-fix
// A/B path is deleted).
inline bool fixUnconverted() noexcept
{
    return true;
}

// TK34: the wipeout (selector case 2, handler sub_00136E98) reads four
// private 1/60 dt words that are not HL2 split sites: [0x49be38] by the body
// integrator sub_00136F30 (FH23 "crashbody", single reader 0x136f48), and
// [0x49be78] / [0x49bef4] / [0x49befc] by the motion-solver callers
// sub_00137750 / sub_001391A8 (FH12 "crash", single readers 0x137754,
// 0x13940c, 0x1394f4). The solver quantum is a split site (113808, 1/120), so
// each converted half advanced the crashed rider a full 1/60 in two quanta:
// 2x per half, two halves per stock update, so the crash fall ran at 4x
// gravity (TK33 §3: d2z -2.00 vs stock -0.50 per stock tick).
// PS2X_SSX3_SPLIT120_CRASHBODY=1 (default off; guest-affecting) holds the
// four words at 1/120 for exactly the halves halfLoad converts (active rider
// context, selector case 0/1/2, or TS3 NOFIX) and puts 1/60 back at finish(),
// so every other reader time (case 5's once-per-update 1391a8) sees stock.
// SPL1: the bounce oscillator (FH22 "bounce") has the same shape. The case-0
// integrate handler sub_0013D818 advances the rider bounce phase by
// f20*[0x49c13c] (2.65, single reader 0x13f0d4) per call, and split120 calls
// it in both halves, so the 60 entry ran the bounce at ~2x (SPL1: phase
// advance 1.9-2.3x stock per tick). PS2X_SSX3_SPLIT120_BOUNCE=1 (default off;
// guest-affecting) holds [0x49c13c] at 1.325 for the same converted halves.
//
// One word set per knob. Every word must read either its stock or its half
// value (a savestate taken while the rider pass is suspended mid-half carries
// the half value; both are known-good), then all are written. Any other value
// refuses that set for the session (logged once) and leaves RAM untouched.
struct HalfWords
{
    const char *tag;
    const uint32_t *addrs;
    uint32_t count;
    uint32_t stock;
    uint32_t half;
    bool refused = false;
};

inline bool halfWordsWrite(uint8_t *ram, HalfWords &w, uint32_t value) noexcept
{
    if (!ram || w.refused) return false;
    for (uint32_t i = 0; i < w.count; ++i)
    {
        uint32_t got = 0;
        std::memcpy(&got, ram + w.addrs[i], 4);
        if (got != w.stock && got != w.half)
        {
            w.refused = true;
            std::fprintf(stderr, "[%s] refused: [0x%x]=%08x expected %08x or %08x; disabled\n", w.tag, w.addrs[i], got,
                         w.stock, w.half);
            return false;
        }
    }
    for (uint32_t i = 0; i < w.count; ++i)
        std::memcpy(ram + w.addrs[i], &value, 4);
    return true;
}

// Knob values: exactly "1" turns a set on.
inline bool knobOn(const char *v) noexcept
{
    return v && v[0] == '1' && v[1] == '\0';
}

inline constexpr uint32_t kCrashWords[] = {0x49be38u, 0x49be78u, 0x49bef4u, 0x49befcu};
inline constexpr uint32_t kCrashBodyStock = 0x3c888889u; // 1/60
inline constexpr uint32_t kCrashBodyHalf = 0x3c088889u;  // 1/120
inline constexpr uint32_t kBounceWords[] = {0x49c13cu};
inline constexpr uint32_t kBounceStock = 0x4029999au; // 2.65
inline constexpr uint32_t kBounceHalf = 0x3fa9999au;  // 1.325
inline HalfWords g_crashWords{"ts2-crashbody", kCrashWords, 4u, kCrashBodyStock, kCrashBodyHalf};
inline HalfWords g_bounceWords{"ts2-bounce", kBounceWords, 1u, kBounceStock, kBounceHalf};

inline bool crashBodyEnabled() noexcept
{
    static const bool on = [] {
        const bool b = knobOn(std::getenv("PS2X_SSX3_SPLIT120_CRASHBODY"));
        if (b)
            std::fprintf(stderr, "[ts2-crashbody] armed (0x49be38/0x49be78/0x49bef4/0x49befc 1/60 -> 1/120 in "
                                 "converted split halves)\n");
        return b;
    }();
    return on;
}

inline bool bounceEnabled() noexcept
{
    static const bool on = [] {
        const bool b = knobOn(std::getenv("PS2X_SSX3_SPLIT120_BOUNCE"));
        if (b)
            std::fprintf(stderr, "[ts2-bounce] armed (0x49c13c 2.65 -> 1.325 in converted split halves)\n");
        return b;
    }();
    return on;
}

// HL1: guest-thread record cache, unconditional (CU4 B3: the
// PS2X_TS2_HL1_CACHE=0 legacy per-load scan is deleted). Exact either way.

inline uint32_t read32(const uint8_t *ram, uint32_t address) noexcept
{
    uint32_t value = 0;
    address &= 0x1fffffffu;
    if (ram && address <= 0x02000000u - 4u)
        std::memcpy(&value, ram + address, 4);
    return value;
}



// Called only after the first rider pass has fully returned to 128dec.
inline bool beginSecondHalf() noexcept
{
    State &s = g_state;
    if (!halfMode() || s.guestInterrupt) return false;
    uint32_t &half = s.getThread(s.guestThread).half;
    if (half == 0) { half = 1; return true; }
    half = 0;
    return false;
}

inline void noteHelperCall(uint32_t source, uint32_t target) noexcept
{
    if (!halfMode() || target != 0x1139a0u) return;
    State &s = g_state;
    if (source == 0x1132e0u) s.getThread(s.guestThread).helper = true;
    if (source == 0x113844u || source == 0x113880u)
        s.getThread(s.guestThread).helper = false;
}

inline uint32_t halfLoad(uint32_t pc, uint32_t address, uint32_t bits) noexcept
{
    if (!halfMode()) return bits;
    State &s = g_state;
    if (s.guestInterrupt) return bits;

    const ThreadData *td;
    {
        if (!s.cachedValid || s.cachedKey != s.guestThread)
        {
            s.cachedData = s.findThread(s.guestThread);
            s.cachedKey = s.guestThread;
            s.cachedValid = true;

        }
        td = s.cachedData;
    }
    if (td == nullptr || !td->ctx.active) return bits;

    // TS3: cases 4/5 reach converted sites through the shared 121aa0/113648
    // helpers. They run once at full H, so their loads keep stock values.
    if (fixUnconverted() && td->ctx.selectorCase >= 3u) return bits;
    address &= 0x1fffffffu;
    // Only reviewed, twice-executed instruction sites are converted. The
    // predictor's call to the shared 1139a0 helper retains stock values.
    // (A missing helper record reads false, as the old map's operator[] did.)
    const bool helper = td->helper;
    uint32_t out = bits;
    switch (pc)
    {
    case ps2_ts2_splitsites::kSite113808: if (address == 0x49b494u) out = 0x3c088889u; break;
    case ps2_ts2_splitsites::kSite113860: if (address == 0x49b498u) out = 0x3ba3d70au; break;
    case ps2_ts2_splitsites::kSite113888: if (address == 0x49b49cu) out = 0x42efffffu; break;
    case ps2_ts2_splitsites::kSite1139a4: if (address == 0x49b4a0u && !helper) out = 0x3c088889u; break;
    case ps2_ts2_splitsites::kSite1139c4: if (address == 0x49b4a4u && !helper) out = 0xbadaa2bdu; break;
    case ps2_ts2_splitsites::kSite1139dc: if (address == 0x49b4a8u && !helper) out = 0xc17d5556u; break;
    case ps2_ts2_splitsites::kSite113a0c: if (address == 0x49b4acu && !helper) out = 0xc0e2aaabu; break;
    case ps2_ts2_splitsites::kSite121e64: if (address == 0x49b828u) out = 0x3c088889u; break;
    case ps2_ts2_splitsites::kSite137d68: if (address == 0x49be9cu) out = 0x3c088889u; break;
    case ps2_ts2_splitsites::kSite139a48: if (address == 0x49bf1cu) out = 0x3c088889u; break;
    case ps2_ts2_splitsites::kSite13d8e4: if (address == 0x49c08cu) out = 0x3c088889u; break;
    case ps2_ts2_splitsites::kSite13ee80: if (address == 0x49c12cu) out = 0x3c088889u; break;
    default: break;
    }

    return out;
}

inline void setThread(uint32_t id, bool interrupt) noexcept
{
    // Fill first, unconditionally: the scheduler calls this before any guest
    // code runs, so the halfMode flag is correct for every guarded load.
    initHalfMode();
    if (enabled()) { g_state.guestThread = id; g_state.guestInterrupt = interrupt; }
}

// The catch-up scanner belongs to the stock-rate predictor. The first half
// services it; the second reuses the result if the front already covers the
// caller's requested threshold. An uncovered front must still catch up.
inline bool skipSecondHalfPrediction(const uint8_t *ram, R5900Context *ctx,
                                    uint32_t source, uint32_t target) noexcept
{
    if (!halfMode()) return false;
    State &s = g_state;
    if (s.guestInterrupt || target != 0x113200u || !ctx ||
        (source != 0x113744u && source != 0x113770u &&
         source != 0x1137a8u && source != 0x1137d8u)) return false;
    const ThreadData *td = s.findThread(s.guestThread);
    if (td == nullptr || !td->ctx.active || td->ctx.halfIndex != 1)
        return false;
    const uint32_t c = getRegU32(ctx, 4) & 0x1fffffffu;
    if (!ram || c > 0x02000000u - 0xa4u) return false;
    const uint32_t frontBits = read32(ram, c + 0x98u);
    const uint32_t baseBits = read32(ram, c + 0xa0u);
    float front = 0.0f, base = 0.0f;
    std::memcpy(&front, &frontBits, 4);
    std::memcpy(&base, &baseBits, 4);
    if (front < base + ctx->f[22])
        return false;
    return true;
}

inline void begin(uint8_t *ram, R5900Context *ctx) noexcept
{
    // Belt-and-braces for non-scheduler flows: conversions need an active
    // context, so the flag is filled before any converting load can run.
    initHalfMode();
    if (!enabled() || !ctx) return;
    State &s = g_state;
    if (s.guestInterrupt) return;
    ThreadData &td = s.getThread(s.guestThread);
    GuestContext &current = td.ctx;
    if (current.active) { ++s.scopeErrors; return; }
    const uint32_t remaining = getRegU32(ctx, 16);
    const uint32_t total = getRegU32(ctx, 18);
    if (remaining + 1u == total)
    {
        if (td.half == 0) ++current.macroOrdinal;
        current.riderIndex = 0;
    }
    else
    {
        ++current.riderIndex;
    }
    current.halfIndex = td.half;
    current.rider = getRegU32(ctx, 4) & 0x1fffffffu;
    // TS3: record the 111408 selector fresh every half, so a rider entering
    // or leaving a converted case mid-macro is decided per half, not per macro.
    current.selectorCase =
        read32(ram, (read32(ram, current.rider + 0x77cu) & 0x1fffffffu) + 0xde0u);
    const uint32_t predictorContext = read32(ram, current.rider + 0x788u) & 0x1fffffffu;
    PredictorIdentity &identity = s.getRider(current.rider);
    if (identity.context != predictorContext)
    {
        identity.context = predictorContext;
        ++identity.epoch;
    }
    current.predictorEpoch = identity.epoch;
    current.continuation = 0x128de4u;
    current.active = true;
    // TK34/SPL1: same conversion rule as halfLoad (cases >= 3 keep stock values).
    if (halfMode() && !(fixUnconverted() && current.selectorCase >= 3u))
    {
        if (crashBodyEnabled() && halfWordsWrite(ram, g_crashWords, kCrashBodyHalf))
        {
            s.ram = ram;
            s.crashBodyHalf = true;
        }
        if (bounceEnabled() && halfWordsWrite(ram, g_bounceWords, kBounceHalf))
        {
            s.ram = ram;
            s.bounceHalf = true;
        }
    }
}

inline void finish() noexcept
{
    if (!enabled()) return;
    State &s = g_state;
    if (s.guestInterrupt) return;
    ThreadData *td = s.findThread(s.guestThread);
    // A helper-only record (no begin) reads inactive, as the old map's
    // missing key did; either way this is a scope error.
    if (td == nullptr || !td->ctx.active) { ++s.scopeErrors; return; }
    td->ctx.active = false;
    if (s.crashBodyHalf)
    {
        s.crashBodyHalf = false;
        halfWordsWrite(s.ram, g_crashWords, kCrashBodyStock);
    }
    if (s.bounceHalf)
    {
        s.bounceHalf = false;
        halfWordsWrite(s.ram, g_bounceWords, kBounceStock);
    }
}

// TS3: unconverted cases run once per stock update. In half 1 the dispatch
// skips the 128ddc->1216e0 call for that rider (1216e0 is a pure wrapper
// around the 111408 dispatch, and the 128de4 continuation ignores its return
// value), then still runs finishIfContinuation so the half 1->0 flip on the
// last rider is preserved. Half 0 runs the handler at full H (halfLoad above
// keeps stock values for these cases). Transitions are per half: the case
// recorded at this half's begin() decides, so a rider that changes case
// mid-macro runs or skips each half correctly.
inline bool skipSecondHalfUnconverted() noexcept
{
    if (!halfMode() || !fixUnconverted()) return false;
    State &s = g_state;
    if (s.guestInterrupt) return false;
    const ThreadData *td = s.findThread(s.guestThread);
    if (td == nullptr || !td->ctx.active || td->ctx.halfIndex != 1)
        return false;
    return td->ctx.selectorCase >= 3u;
}

// The canonical generated 128af0 loop resumes at 128de4 after each rider.
// On the last rider of half 0, re-arm its existing backward branch with the
// original rider list. This uses the generated continuation, including when
// the callee yielded and the scheduler later resumes it.
inline void finishIfContinuation(R5900Context *ctx) noexcept
{
    if (!enabled() || !ctx || ctx->pc != 0x128de4u) return;
    State &s = g_state;
    if (s.guestInterrupt) return;
    ThreadData *td = s.findThread(s.guestThread);
    if (td == nullptr || !td->ctx.active) return;
    const uint32_t remaining = getRegU32(ctx, 16);
    const uint32_t total = getRegU32(ctx, 18);
    finish();
    if (!halfMode() || remaining != 0u || total == 0u) return;
    // The record exists: active required begin(), which always creates it.
    // finish() above only clears active, so td stays valid.
    uint32_t &half = td->half;
    if (half == 0u)
    {
        half = 1u;
        td->ctx.restartCheckpointPending = true;
        uint64_t firstRider = 0;
        std::memcpy(&firstRider, &ctx->r[29], sizeof(firstRider));
        const uint64_t remainingRiders = total;
        // Match SET_GPR_U64's low-half write while keeping this header usable
        // before ps2_runtime_macros.h defines the generated-code macros.
        std::memcpy(&ctx->r[16], &remainingRiders, sizeof(remainingRiders));
        std::memcpy(&ctx->r[17], &firstRider, sizeof(firstRider));
    }
    else
    {
        half = 0u;
    }
}

// The generated loop charges a checkpoint on its backward edge. G2d's
// second-half restart jumped directly to 128dd8, so skip only that extra
// checkpoint; all ordinary rider-loop checkpoints remain intact.
inline bool consumeRestartCheckpoint() noexcept
{
    // EE1P2: the sole caller gates on halfMode(), so only the interrupt
    // check remains here; behaviour is identical in all modes.
    State &s = g_state;
    if (s.guestInterrupt) return false;
    ThreadData *td = s.findThread(s.guestThread);
    if (td == nullptr || !td->ctx.restartCheckpointPending) return false;
    td->ctx.restartCheckpointPending = false;
    return true;
}
} // namespace ps2_ts2_split60
