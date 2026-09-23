#pragma once

// GB3 Part 2: paraLLEl-GS as the live GS backend (Mac, MoltenVK).
//
// A GSRasterBackend that takes the raw GIF stream (WantsRawGif) and renders
// it with paraLLEl-GS on the GS worker thread; the queue is required, so
// every call lands on that one thread. Present = paraLLEl flush + vsync
// scanout + one GPU->CPU copy (interim, GB1 step (b); step (d) removes the
// copy). SnapshotVram reads paraLLEl's VRAM back (map_vram_read).
//
// Selected by PS2X_GS_BACKEND=parallel (runtime), built only with
// PS2X_GS_SHADOW_PARALLEL=ON (G44's CMake wiring, local parallel-gs dir).
// Unset = CPU backend, zero behavior change.
//
// Not supported (counted, reported in the [gs:parallel] stats line):
// HLE ClearFramebuffer (returns false), debug ReadVram/WriteVram.
// Local->host transfers read paraLLEl's transfer FIFO.

#include "runtime/gs/gs_backend.h"

#include <cstdint>
#include <memory>

struct GSRegisters;

namespace ps2x_gs_parallel
{
// True only in PS2X_GS_SHADOW_PARALLEL builds.
bool available();

// PS2X_GS_BACKEND=parallel (exact match), read once.
bool requested();

// Null when !available(). `priv` is the runtime's priv-register block; the
// backend copies it into paraLLEl's priv state at every Present (on the GS
// worker, after in-stream priv stores, so the copy is stream-coherent).
std::unique_ptr<GSRasterBackend> create(const GSRegisters *priv);

struct Stats
{
    uint64_t gifPackets = 0;
    uint64_t gifBytes = 0;
    uint64_t regWrites = 0;
    uint64_t presents = 0;
    uint64_t nullScanouts = 0;
    uint64_t presentNanos = 0;   // flush + vsync + readback, summed
    uint64_t readbackNanos = 0;  // GPU->CPU copy + wait, summed
    uint64_t snapshots = 0;
    uint64_t unsupportedClears = 0;
    uint64_t unsupportedVramIo = 0;
    uint64_t localToHostBytes = 0;
    bool initOk = false;
    bool initFailed = false;
};
Stats stats();
} // namespace ps2x_gs_parallel
