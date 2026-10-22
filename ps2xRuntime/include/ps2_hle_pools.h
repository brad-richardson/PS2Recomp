#pragma once

// TK39: PS2X_HLE_POOLS_LOW=1 (default off) moves the runtime's HLE pools
// (RPC packets, RPC servers, TLS, boot mode: 0x31000 bytes at 0x01F00000)
// into the unused low band [0x80000, 0xB1000) and lifts the guest heap
// ceiling that SetupHeap(-1)/EndOfHeap report.
//
// Why: a real kernel ends a size -1 heap at the main thread's stack
// (SSX 3: 0x01FE0000), so the runtime's 0x01F00000 ceiling gives the game
// 896 KB less arena than hardware. SSX 3 sizes its whole arena from
// EndOfHeap, and heavy Tricky race loads peak within KBs of the ceiling:
// when fragmentation leaves no 64 KB hole, the per-frame memalign(0x80,
// 0x10080) at race load fails forever (ENOMEM) and the race never starts.
//
// tk-alaska-opus-1: with the knob on, every runtime-owned guest block lives
// in the low band too: the HNG1 arena at [0xB1000, 0xB3000) and the SND
// packet + done ring at [0xB3000, 0xB3300); the async callback stacks keep
// [0xB4000, 0x100000) (observed use reaches ~0xD0000). TK34's patch cache
// now grows inside the game's own heap, so nothing runtime-owned sits above
// the ceiling and it rises to 0x01FD8000 (32 KB under the main stack).
//
// Off = every address byte-identical to before.

#include <cstdint>
#include <cstdlib>

namespace ps2_hle_pools
{
inline bool low()
{
    static const bool on = [] {
        const char *e = std::getenv("PS2X_HLE_POOLS_LOW");
        return e != nullptr && e[0] == '1' && e[1] == '\0';
    }();
    return on;
}

inline constexpr uint32_t kHighBase = 0x01F00000u;
inline constexpr uint32_t kLowBase = 0x00080000u;

// A pool address laid out from kHighBase, relocated when the knob is on.
inline uint32_t place(uint32_t highAddr)
{
    return low() ? highAddr - kHighBase + kLowBase : highAddr;
}

inline constexpr uint32_t kHighHeapCeiling = 0x01F00000u;
inline constexpr uint32_t kLowHeapCeiling = 0x01FD8000u;
inline constexpr uint32_t kMainStackBase = 0x01FE0000u; // SSX 3 crt0 SetupThread stack

inline uint32_t heapCeiling()
{
    return low() ? kLowHeapCeiling : kHighHeapCeiling;
}

// HNG1: the runtime's private arena for guest blocks it hands to the game
// (MPEG callback data and work). SetupHeap/EndOfHeap give the game
// [_end, heapCeiling()) as its own arena, and SSX 3's recompiled allocator
// manages that range itself, so a block from the SetupHeap heap can land on
// memory the game already owns (10-06: a 16-byte pool link became 1). The
// arena sits outside the game's heap in both modes: above the ceiling after
// the SND packet's done ring (0x01F31100..0x01F31300) with the knob off, in
// the low band after the HLE pools with it on.
inline constexpr uint32_t kHleArenaBase = 0x01F31400u; // knob off
inline constexpr uint32_t kLowHleArenaBase = 0x000B1000u;
inline constexpr uint32_t kHleArenaBytes = 0x00002000u;

inline uint32_t hleArenaBase()
{
    return low() ? kLowHleArenaBase : kHleArenaBase;
}

// SND HLE packet (0x100) + done ring (16 x 0x20) the game module hands to the
// guest's SIF handler.
inline constexpr uint32_t kSndPacketBase = 0x01F31000u; // knob off
inline constexpr uint32_t kLowSndPacketBase = 0x000B3000u;
inline constexpr uint32_t kSndPacketBytes = 0x00000300u;

inline uint32_t sndPacketBase()
{
    return low() ? kLowSndPacketBase : kSndPacketBase;
}

inline constexpr uint32_t kLowCallbackStackFloor = 0x000B4000u;

inline uint32_t callbackStackFloor()
{
    return low() ? kLowCallbackStackFloor : 0x00080000u;
}

static_assert(kLowBase + 0x31000u <= kLowHleArenaBase, "HLE pools end before the HNG1 arena");
static_assert(kLowHleArenaBase + kHleArenaBytes <= kLowSndPacketBase, "HNG1 arena ends before the SND packet");
static_assert(kLowSndPacketBase + kSndPacketBytes <= kLowCallbackStackFloor, "SND packet ends before the callback stacks");
static_assert(kLowHeapCeiling + 0x8000u <= kMainStackBase, "32 KB guard under the main stack");
static_assert(kSndPacketBase + kSndPacketBytes <= kHleArenaBase, "knob-off SND ring ends before the HNG1 arena");
} // namespace ps2_hle_pools
