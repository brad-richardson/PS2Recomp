#pragma once

// GE2: external-GS backend shell (RV14 track 2).
//
// A GSRasterBackend behind PS2X_GS_BACKEND=external (default off) that owns
// everything on OUR side of a future PCSX2-GS adapter boundary:
// ordered raw PATH1/2/3 forwarding with persistent per-path state,
// in-stream priv-register mirroring, a guest-VSync command at every VBlank,
// lazy local->host FIFO reads with cursor + partial-read semantics, a
// versioned save blob, and a present export hook.
//
// This stage backs the shell with a RECORDING STUB: every call is logged
// (PS2X_GS_EXTERNAL_LOG=path) and delegated to an inner CPU backend, so a
// stub run's guest state must be bit-identical to the CPU backend's. GE1's
// extracted library later replaces the inner backend behind this same shell.

#include "runtime/gs/gs_backend.h"

#include <cstdint>
#include <memory>
#include <vector>

struct GSRegisters;

namespace ps2x_gs_external
{
// Always built (no extra linkage); true.
bool available();

// PS2X_GS_BACKEND=external (exact match). Read fresh (cheap; the runtime
// reads it once at startup) so tests can toggle it.
bool requested();

// `priv` is the runtime's priv-register block (tick stamps for the call
// log); the backend never writes it. Null when !available().
std::unique_ptr<GSRasterBackend> create(const GSRegisters *priv);

struct Stats
{
    uint64_t gifPackets = 0;
    uint64_t gifBytes = 0;
    uint64_t gifQwords[4] = {};
    uint64_t gifPacketsByPath[4] = {};
    uint64_t nativePacked = 0;
    uint64_t regWrites = 0;
    uint64_t privMirrored = 0;
    uint64_t vsyncs = 0;
    uint64_t vsyncGaps = 0; // GuestVsync ticks that skipped (must stay 0)
    uint64_t transfers = 0;
    uint64_t uploads = 0;
    uint64_t uploadBytes = 0;
    uint64_t submitsIgnored = 0; // decoded Submit calls (raw path owns draws)
    uint64_t consumes = 0;
    uint64_t consumeBytes = 0;
    uint64_t fifoDownloads = 0; // lazy local->host downloads at first consume
    uint64_t fifoDownloadBytes = 0;
    uint64_t fifoReplacedUnread = 0; // bytes dropped by a new local->host setup
    uint64_t presents = 0;
    uint64_t saves = 0;
    uint64_t loads = 0;
    bool logOpen = false;
    bool logTruncated = false;
    uint64_t logBytes = 0;
};
Stats stats();

// Last exported present (the hook a foreign present sink will read).
struct ExportedFrame
{
    uint64_t tick = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t displayFbp = 0;
    uint32_t sourceFbp = 0;
    bool usedPreferred = false;
    uint32_t hash = 0; // FNV-1a over pixels
    std::vector<uint8_t> pixels;
};
// False when no backend is live or no present has exported yet.
bool lastPresent(ExportedFrame &out);
} // namespace ps2x_gs_external
