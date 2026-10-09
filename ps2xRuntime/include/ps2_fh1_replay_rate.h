// RPL1: replays follow the recording's 60/120 rate per input sample
// (PS2X_SSX3_FULL120=events; PS2X_SSX3_REPLAY_RATE, default on, "0" = off).
//
// SSX 3 replays re-simulate the run from the recorded per-update controls,
// one input sample per app update. Under events mode the live run decides
// its 60/120 flips from race facts and applies them at the first update
// dispatch after the next VBlank commit; the replay re-derives them from its
// own update stream, so the two disagree whenever the update/VBlank pattern
// differs:
//   - the replay start runs a catch-up burst (I26: 7 updates in one VBlank)
//     and the start-edge flip waits for the next VBlank: the live run had 1
//     stock update before 120, the replay 7 (RPL1 a2);
//   - a live pause exits to stock and re-enters only when every rider is
//     grounded; the recording skips the paused updates, so the replay never
//     exits and plays the post-resume stock updates at 120.
// Each stock-vs-120 update changes dt and the update parities (session, rng,
// query, ...), and the replay drifts into a different run (HNG2 §3: 2,502
// units; the Ruthless Ridge freeze).
//
// Rule: the recorder notes, per input object, the guest rate (events words
// active or not) of every recorded sample as run edges. During playback the
// update about to run consumes sample k = the playback cursor, so at each
// app-update dispatch the guest words are set to the rate recorded for k,
// mid-burst included; the VBlank request follows the same rate. Live runs
// only observe (no live behaviour changes); stock recordings have no 120
// samples, so their replays never flip. Without a track (a state from before
// the recording started, an inconsistent cursor) the stock events rule runs.
//
// Guest addresses (LCY1 source map, RPL1 reads of the canonical codegen):
//   0x26d1fc jal 0x26d2b0  record one sample (mode 0, a0 = input object)
//   0x26d20c jal 0x26d420  play one sample (modes 1/2/4, a0 = input object)
//   0x26d168 / 0x26d130    input reset / destroy (a0 = input object; LCY2)
//   input object: +0 records, +4 cursor record, +8 cursor remaining (0 = the
//   cursor record is untouched), +0xc data; 8-byte records, run length in the
//   low 12 bits of word 0 (4095 cap, 4096 records).
//   replay mode word [[[gp-0x848]+0x84]+0x28]: 0 record, 1/2/4 play.
#pragma once

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace ps2_fh1_rpl
{
inline constexpr uint32_t kRecordSite = 0x26d1fcu, kRecord = 0x26d2b0u;
inline constexpr uint32_t kPlaySite = 0x26d20cu, kPlay = 0x26d420u;
inline constexpr uint32_t kInputReset = 0x26d168u, kInputFree = 0x26d130u;
inline constexpr uint32_t kMaxRecords = 4096u;
inline constexpr uint32_t kReplayGpOff = 0x848u; // [gp-0x848]
inline constexpr uint32_t kRamSize = 0x2000000u;

// Unset/empty/anything but "0" = on.
inline bool parseEnabled(const char *v) noexcept { return !(v && std::strcmp(v, "0") == 0); }

inline bool rd(const uint8_t *ram, uint32_t a, uint32_t &v) noexcept
{
    a &= 0x1fffffffu;
    if (a > kRamSize - 4u || (a & 3u))
        return false;
    std::memcpy(&v, ram + a, 4u);
    return true;
}

// Samples recorded so far (sum of the run lengths).
inline bool recorded(const uint8_t *ram, uint32_t input, uint32_t &samples) noexcept
{
    uint32_t n = 0u, data = 0u;
    samples = 0u;
    if (!input || !rd(ram, input, n) || n > kMaxRecords || !rd(ram, input + 0xcu, data))
        return false;
    for (uint32_t i = 0u; i < n; ++i)
    {
        uint32_t w = 0u;
        if (!rd(ram, data + 8u * i, w))
            return false;
        samples += w & 0xfffu;
    }
    return true;
}

// Samples the playback cursor has consumed = the ordinal the next play call reads.
inline bool consumed(const uint8_t *ram, uint32_t input, uint32_t &samples) noexcept
{
    uint32_t n = 0u, idx = 0u, rem = 0u, data = 0u;
    samples = 0u;
    if (!input || !rd(ram, input, n) || n > kMaxRecords || !rd(ram, input + 4u, idx) || idx > n ||
        !rd(ram, input + 8u, rem) || !rd(ram, input + 0xcu, data))
        return false;
    for (uint32_t i = 0u; i < idx; ++i)
    {
        uint32_t w = 0u;
        if (!rd(ram, data + 8u * i, w))
            return false;
        samples += w & 0xfffu;
    }
    if (idx < n && static_cast<int32_t>(rem) > 0)
    {
        uint32_t w = 0u;
        if (!rd(ram, data + 8u * idx, w) || rem > (w & 0xfffu))
            return false;
        samples += (w & 0xfffu) - rem;
    }
    return true;
}

// The guest replay mode is a playback mode (the decoder runs for 1, 2 and 4).
inline bool playbackMode(const uint8_t *ram, uint32_t gp) noexcept
{
    uint32_t a = 0u, b = 0u, c = 0u, m = 0u;
    if (!rd(ram, gp - kReplayGpOff, a) || !a || !rd(ram, a + 0x84u, b) || !b || !rd(ram, b + 0x28u, c) || !c ||
        !rd(ram, c, m))
        return false;
    return m == 1u || m == 2u || m == 4u;
}

struct Edge
{
    uint32_t ordinal = 0u; // first sample of this run
    uint8_t active = 0u;   // 1 = recorded with the events words active (120)
};

struct Track
{
    uint32_t input = 0u;
    uint32_t total = 0u; // samples noted
    bool valid = false;
    std::vector<Edge> edges;
};

// One recorded sample at `ordinal` (the count before the record call). A new
// recording starts at 0; any other gap (a capped record array, a missed
// call) invalidates the track until the next recording.
inline void note(Track &t, uint32_t ordinal, bool active)
{
    if (ordinal == 0u)
    {
        t.edges.clear();
        t.total = 0u;
        t.valid = true;
    }
    if (!t.valid || ordinal != t.total)
    {
        t.valid = false;
        t.edges.clear();
        t.total = 0u;
        return;
    }
    if (t.edges.empty() || (t.edges.back().active != 0u) != active)
        t.edges.push_back(Edge{ordinal, static_cast<uint8_t>(active ? 1u : 0u)});
    t.total = ordinal + 1u;
}

// Recorded rate of sample `ordinal`: 1 = 120, 0 = stock, -1 = unknown.
inline int rateAt(const Track &t, uint32_t ordinal) noexcept
{
    if (!t.valid || ordinal >= t.total || t.edges.empty())
        return -1;
    int r = -1;
    for (const Edge &e : t.edges)
    {
        if (e.ordinal > ordinal)
            break;
        r = e.active ? 1 : 0;
    }
    return r;
}

inline constexpr size_t kTracks = 4u;
struct State
{
    std::array<Track, kTracks> tracks{};
    uint32_t playInput = 0u; // input object seen by the play call (0 = none)
    bool forced = false;     // the last dispatch applied a recorded rate
};

inline Track *track(State &s, uint32_t input, bool create)
{
    if (!input)
        return nullptr;
    for (Track &t : s.tracks)
        if (t.input == input)
            return &t;
    if (!create)
        return nullptr;
    for (Track &t : s.tracks)
        if (!t.input)
        {
            t = Track{};
            t.input = input;
            return &t;
        }
    // Full: reuse the first slot (its recording is the oldest one kept).
    s.tracks[0] = Track{};
    s.tracks[0].input = input;
    return &s.tracks[0];
}

inline void forget(State &s, uint32_t input)
{
    if (Track *t = track(s, input, false))
        *t = Track{};
    if (s.playInput == input)
        s.playInput = 0u;
}

// The rate the next update must run at: the recorded rate of the sample the
// playback cursor reads next, while the guest is in a playback mode; -1 = no
// recorded rate applies (the stock events rule decides).
inline int forcedRate(State &s, const uint8_t *ram, uint32_t gp)
{
    if (!s.playInput)
        return -1;
    if (!playbackMode(ram, gp))
    {
        s.playInput = 0u;
        return -1;
    }
    const Track *t = track(s, s.playInput, false);
    uint32_t k = 0u;
    if (!t || !consumed(ram, s.playInput, k))
        return -1;
    return rateAt(*t, k);
}
} // namespace ps2_fh1_rpl
