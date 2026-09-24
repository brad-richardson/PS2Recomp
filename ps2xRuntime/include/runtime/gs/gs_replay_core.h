#pragma once

// N8D7M12 Part 1: shared GS-stream replay core (N8D7M11 §3).
//
// The record decoding/dispatch loop factored out of the desktop replay test
// (ps2xTest/src/ps2_gs_replay_tests.cpp) so the desktop wrapper and a
// future Android app branch call the same parser. Pure stdlib + GS frontend +
// parallel backend; no desktop test-harness symbol here (the harness must not
// ship in the app). Behavior is unchanged: ordered path+priv+transfer+marker processing,
// Present descriptor, tick2050 selected capture, 4 MiB snapshot and 448-tile
// census flow through the same call sequence; the desktop-only N8D7M5
// word-watch block is preserved verbatim behind PS2X_GS_REPLAY_WORD_WATCH
// (defined for that TU only when PS2X_BUILD_TEST=ON; Android-target builds
// omit it entirely) and stays inert unless PS2X_GS_REPLAY_WORDS is set.
// Unset PS2X_GS_REPLAY_CAPTURE remains a skip.
// Normal runtime boot is untouched (no main.cpp change in Part 1).

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Length-prefixed record framing over an open PS2XGSC1 capture (after the
// 8-byte magic): Record = one full record read, End = clean EOF,
// Invalid = truncated header/body or out-of-range length.
enum class Ps2xGsReplayReadResult
{
    Record,
    End,
    Invalid
};

Ps2xGsReplayReadResult ps2x_gs_replay_read_event(FILE *f, uint32_t &length,
                                                 std::vector<uint8_t> &record);

// Narrow result/error API for one replay run. Console emission keeps the
// existing GB4_REPLAY_SUMMARY / GB4_FRAME / GB4_REPLAY / [n8d7m5] formats so
// Mac controls compare line-for-line; the result carries the outcome for the
// caller to assert (desktop test) or gate (future Android branch).
struct Ps2xGsReplayResult
{
    bool skipped = false; // PS2X_GS_REPLAY_CAPTURE unset/empty: nothing ran
    bool openOk = true; // capture file opened
    bool headerOk = true; // PS2XGSC1 magic present
    bool rtzOk = true; // PS2X_GS_REPLAY_RTZ empty/path1/all
    bool pathFileOk = true; // PS2X_GS_REPLAY_PATH_FILE (when set) parses
    bool wordsOk = true; // PS2X_GS_REPLAY_WORDS (when set) parses
    bool backendOk = true; // parallel backend available when requested
    bool parseOk = false; // every record decoded cleanly to EOF marker
    bool packetTraceOk = true; // PS2X_GS_REPLAY_PACKET_TRACE written (when set)
    bool outOk = true; // PS2X_GS_REPLAY_OUT written (when set)
    bool expectOk = true; // PS2X_GS_REPLAY_EXPECT rows match (when set)
    std::string expectMessage; // first-mismatch detail when !expectOk
    bool hasStream = false; // packets > 0 && markers > 0
    bool hasSamples = false; // at least one sampled GB4_REPLAY row
    uint64_t packets = 0u;
    uint64_t priv = 0u;
    uint64_t transfers = 0u;
    uint64_t markers = 0u;
    uint64_t readbacks = 0u;
    uint64_t clears = 0u;
    uint64_t roundedPackets = 0u;
    bool queued = false;
    bool parallelBackend = false;
    bool dropPriv = false;
    std::string rtz = "off";
    std::vector<std::string> rows; // sampled GB4_REPLAY rows
};

// Runs one replay from the standard PS2X_GS_REPLAY_* environment. Emits the
// existing stdout/stderr lines; returns the outcome for the caller to assert
// (desktop test) or gate (future Android branch).
Ps2xGsReplayResult ps2x_gs_replay_run();
