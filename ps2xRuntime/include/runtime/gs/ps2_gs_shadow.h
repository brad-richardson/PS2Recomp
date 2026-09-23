#pragma once

// G44 synchronous paraLLEl shadow (GPU GS backend pathfinder).
//
// Feeds paraLLEl-GS the same GIF stream the CPU backend sees, then compares
// paraLLEl's per-vsync scanout against the CPU present pixels. Synchronous,
// no threading: the CPU backend stays the presenter; paraLLEl output is
// read back for comparison only.
//
// Env-gated: PS2X_GS_SHADOW=parallel enables. Unset (or any other value) =
// zero behavior change: no Vulkan init, entry points return immediately.
// Builds without PS2X_GS_SHADOW_PARALLEL never initialize a backend even
// when the env var is set (hasBackend() is false).
//
// Env knobs (read once, latched):
//   PS2X_GS_SHADOW      "parallel" to enable.
//   PS2X_GS_SHADOW_DIR  per-vsync PNG pairs + pairs.csv + shadow-stats.txt.
//                       Default: ./g44-shadow (created on first capture).
//   PS2X_GS_SHADOW_FROM first guest vsync tick captured (default 0).
//   PS2X_GS_SHADOW_TO   last guest vsync tick captured, exclusive
//                       (default UINT64_MAX).
//   PS2X_GS_SHADOW_STRIDE G46: capture at most one pair per N ticks
//                       (default 1 = every eligible present).
//   PS2X_GS_SHADOW_REC  G46 DIAGNOSTIC: record the fed stream to this file
//                       (GIF packets, HLE reg writes, priv regs per present)
//                       for offline replay; stops at PS2X_GS_SHADOW_TO or at
//                       PS2X_GS_SHADOW_REC_CAP_MB (default 3000).
// Hard cap: 200 pairs per process.

#include <cstdint>

struct GSRegisters; // runtime/ps2_memory.h (kept out of this header)

namespace ps2x_gs_shadow
{

constexpr uint64_t kPairCap = 200u;

struct Config
{
    bool wantParallel = false;
    // G44 Part-3 DIAGNOSTIC-ONLY: force the shadow's SMODE1 copy to
    // NTSC-480i when the game leaves SMODE1 at 0. Shadow copy only; the
    // CPU backend and guest state are untouched. Default off.
    bool forceSmode1Ntsc = false;
    uint64_t from = 0u;
    uint64_t to = ~0ull;
    uint64_t cap = kPairCap;
    uint64_t stride = 1u;
};

const Config &config(); // parses the environment once, latched
bool hasBackend();      // true only in PS2X_GS_SHADOW_PARALLEL builds
bool enabled();         // wantParallel && hasBackend() && !initFailed()

// True while the shadow is feeding: P6/P7 native fast paths must route via
// the arbiter so both backends see identical traffic in identical order.
// False when the flag is unset or the shadow backend failed to init.
bool routeGifViaArbiter();

// Feed points (no-ops unless enabled(); thread-safe, serialized internally).
void onGifPacket(uint32_t path, const uint8_t *data, uint32_t sizeBytes);
void onWriteRegister(uint8_t regAddr, uint64_t value);
void onReset();

// Per-vsync compare against the CPU present pixels (no-op unless the tick
// is inside [from,to) and the pair cap is not reached). priv supplies the
// current EE-visible privileged registers for scanout setup.
void onPresentFrame(uint64_t tick,
                    const uint8_t *cpuRgba,
                    uint32_t cpuWidth,
                    uint32_t cpuHeight,
                    const GSRegisters *priv);

// Observability (for REPORT receipts; lock-free loads).
uint64_t gifPacketsFed();
uint64_t regWritesFed();
uint64_t presentsSeen();
uint64_t pairsWrote();
const char *shadowDir(); // effective output dir (after latch)
bool initFailed();

// Pure helpers (unit-tested, no Vulkan, no env latch).
bool parseModeParallel(const char *value);
bool parseForceSmode1Ntsc(const char *value);
uint64_t parseU64(const char *value, uint64_t dflt);
bool tickEligible(uint64_t tick, uint64_t from, uint64_t to, uint64_t pairsDone, uint64_t cap);

// Test-only: drop the latched config/counters (precedent: ps2xSet...ForTest).
void resetForTest();

} // namespace ps2x_gs_shadow
