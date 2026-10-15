#pragma once
// np-link-1 (SRVP1 P1a): delay-based lockstep client for two Mac processes.
//
// One candidate module, diagnostic knobs, all default off. The sim thread
// calls onTick()/waitCommit() from the two guest-input funnels (slot-0 pad
// read, AI-slot 0x111728 hook); a network thread owns the TCP socket to the
// local ordered relay. Knob off: enabled() is false after one cached getenv
// and no other function has any effect (no thread, socket, file or guest
// change).
#include <cstdint>

namespace ps2x
{
namespace nplink
{

struct Commit
{
    uint16_t buttons = 0xFFFFu; // commit.A: active-low pad buttons
    uint8_t rx = 0x80u;
    uint8_t ry = 0x80u;
    uint8_t lx = 0x80u;
    uint8_t ly = 0x80u;
    uint32_t de4 = 0xFFFFFFFFu; // commit.B: command triple, or sentinel
    uint32_t w0 = 0u;
    uint32_t w1 = 0u;
    bool bLive = false; // false = sentinel: keep AI words
};

// True once iff PS2X_NP_LINK_ENABLE=1 (cached; one getenv per process).
bool enabled();

// First committed tick (PS2X_NP_LINK_FIRST_TICK). Valid when enabled().
uint64_t firstTick();

// AI target slot (PS2X_NP_LINK_RIDER_SLOT). Valid when enabled().
int riderSlot();

// Sample the role-local script and queue INPUT frames for every unsent tick
// <= tick + delay. No-op unless enabled(). Safe to call for any tick.
void onTick(uint64_t tick);

// Block until COMMIT(tick) is in the map, then copy it to out. Returns false
// only when disabled. Never returns without the commit (no timeout): the
// sim thread stalls while the network thread keeps receiving.
bool waitCommit(uint64_t tick, Commit &out);

} // namespace nplink
} // namespace ps2x
