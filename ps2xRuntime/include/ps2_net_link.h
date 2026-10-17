#pragma once
// np-link-1 (SRVP1 P1a): delay-based lockstep client for two Mac processes.
//
// One candidate module, diagnostic knobs, all default off. The sim thread
// calls onTick()/waitCommit() from the two guest-input funnels (slot-0 pad
// read, AI-slot 0x111728 hook); a network thread owns the TCP socket to the
// local ordered relay. Knob off: enabled() is false after one cached getenv
// and no other function has any effect (no thread, socket, file or guest
// change).
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace ps2x
{
namespace nplink
{

// netsec-fix-1 F10: a relay/protocol failure ends the link session (the sim
// thread keeps running on local inputs; waitCommit reports false) instead of
// exiting the process.
struct SessionError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

// netsec-fix-1 F9: COMMIT admission window (pure, unit-tested). A COMMIT
// tick must lie in [firstTick, sendUpTo + kCommitWindow] and past the last
// applied tick; anything else is ignored, never queued, so a malicious
// relay cannot accumulate unconsumed future commits. kCommitWindow covers
// the in-flight INPUT lead (delay) with margin; the relay only echoes ticks
// the client already queued.
constexpr uint64_t kCommitWindow = 256u;
// Backstop on queued (accepted, unconsumed) commits; the window above bounds
// the map far below this in legitimate use.
constexpr size_t kMaxPendingCommits = 4096u;

inline bool commitAcceptable(uint64_t firstTick, uint64_t sendUpTo, bool haveLast,
                             uint64_t lastWaited, uint64_t tick)
{
    if (tick < firstTick)
        return false;
    if (tick > sendUpTo + kCommitWindow)
        return false;
    if (haveLast && tick <= lastWaited)
        return false;
    return true;
}

// netsec-fix-1 F10: SSXNP/2 frame-size gate (pure, unit-tested): throws
// SessionError instead of exiting on a corrupt declared size.
constexpr uint32_t kLinkFrameMin = 44u;
constexpr uint32_t kLinkFrameMax = 16384u;

inline void checkLinkFrameSize(uint32_t frameBytes)
{
    if (frameBytes < kLinkFrameMin || frameBytes > kLinkFrameMax)
        throw SessionError("bad frame_bytes");
}

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
// when disabled or after the link session ended (netsec-fix-1 F10): the sim
// thread keeps running on local inputs. No timeout while the relay is
// healthy: the sim thread stalls while the network thread keeps receiving.
// A dead relay (write/select failure, closed or malformed COMMIT) or a tick
// past the input queue cap ends the session, so a lost relay reads as a
// link failure, not a hang and not a process exit.
bool waitCommit(uint64_t tick, Commit &out);

} // namespace nplink
} // namespace ps2x
