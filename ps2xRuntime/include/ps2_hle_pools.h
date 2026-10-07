#pragma once

// TK39: PS2X_HLE_POOLS_LOW=1 (default off) moves the runtime's HLE pools
// (RPC packets, RPC servers, TLS, boot mode: 0x31000 bytes at 0x01F00000)
// into the unused low band [0x80000, 0xB1000) and lifts the guest heap
// ceiling that SetupHeap(-1)/EndOfHeap report from 0x01F00000 to 0x01F31000
// (the sound packet at 0x01F31000 and the TK34 grow pool above it stay put).
//
// Why: a real kernel ends a size -1 heap at the main thread's stack
// (SSX 3: 0x01FE0000), so the runtime's 0x01F00000 ceiling gives the game
// 896 KB less arena than hardware. SSX 3 sizes its whole arena from
// EndOfHeap, and heavy Tricky race loads peak within 8 KB of the ceiling:
// when fragmentation leaves no 64 KB hole, the per-frame memalign(0x80,
// 0x10080) at race load fails forever (ENOMEM) and the race never starts.
//
// Off = every address byte-identical to before. The async callback stacks
// keep [0xB2000, 0x100000) when on (observed use reaches ~0xD0000).

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

inline uint32_t heapCeiling()
{
    return low() ? 0x01F31000u : 0x01F00000u;
}

// HNG1: the runtime's private arena for guest blocks it hands to the game
// (MPEG callback data and work). SetupHeap/EndOfHeap give the game
// [_end, heapCeiling()) as its own arena, and SSX 3's recompiled allocator
// manages that range itself, so a block from the SetupHeap heap can land on
// memory the game already owns (10-06: a 16-byte pool link became 1). This
// band sits above the ceiling in both modes, after the sound packet's done
// ring (0x01F31100..0x01F31300); the TK34 grow region starts after it.
inline constexpr uint32_t kHleArenaBase = 0x01F31400u;
inline constexpr uint32_t kHleArenaBytes = 0x00002000u;

inline uint32_t callbackStackFloor()
{
    return low() ? 0x000B2000u : 0x00080000u;
}
} // namespace ps2_hle_pools
