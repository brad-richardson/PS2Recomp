#pragma once
#include <stdint.h>

#if defined(__GNUC__)
#define GE1_API __attribute__((visibility("default")))
#else
#define GE1_API
#endif

#ifdef __cplusplus
extern "C" {
#endif
// One active GS per process. The implementation exclusively calls public GS.h
// entry points; the C boundary keeps PCSX2 C++ types and symbols private.
GE1_API int ge1_gs_open(int blending_level);
GE1_API void ge1_gs_close(void);
GE1_API int ge1_gs_reset(const uint64_t privileged_words[20], const uint8_t* vram, uint32_t vram_size);
GE1_API int ge1_gs_priv_write(uint32_t offset, uint64_t value);
GE1_API int ge1_gs_packet(uint8_t path, const uint8_t* bytes, uint32_t byte_count);
GE1_API int ge1_gs_vsync(uint32_t field, uint64_t csr, uint64_t smode1, uint64_t syncv);
GE1_API int ge1_gs_read_fifo(uint8_t* bytes, uint32_t qwords);
// LT1b: ticketed asynchronous local->host probes (PS2X_VIF1_REVERSE_DMA=lagF).
// request: at the probe's stream point, right after the packet carrying its
// TRXDIR=1 (GS register layouts). resolve_frame: at VSync, resolves every queued
// probe with ticket < ticket_hi whose record already executed (returns the count).
// take: copies a resolved probe's bytes once (returns bytes, 0 = not resolved).
// stats: 8 counters (GSState::m_probe_stats). Optional symbols (pre-LT1b libraries
// lack them; lagF then falls back to a sync read at the set).
GE1_API int ge1_gs_probe_request(uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg, uint64_t ticket);
GE1_API int ge1_gs_probe_resolve_frame(uint64_t ticket_lo, uint64_t ticket_hi);
GE1_API int ge1_gs_probe_take(uint64_t ticket, uint8_t* out, uint32_t bytes);
GE1_API int ge1_gs_probe_stats(uint64_t out[8]);
GE1_API int ge1_gs_snapshot(uint32_t* width, uint32_t* height, const uint32_t** rgba);
typedef void (*ge1_gs_export_done_fn)(void* ctx, int ok);
GE1_API int ge1_gs_export_iosurface(void* iosurface, uint32_t width, uint32_t height,
                                    ge1_gs_export_done_fn done, void* ctx);
GE1_API void ge1_gs_release_iosurface(void* iosurface);
GE1_API int ge1_gs_export_ahb(void* buffer, uint32_t width, uint32_t height, uint64_t* fence_counter);
GE1_API void ge1_gs_wait_export(uint64_t fence_counter);
GE1_API void ge1_gs_release_ahb(void* buffer);
GE1_API float ge1_gs_gpu_ms(void);
// PT2: accumulated GS back-thread drain busy ms since the last call (reset on
// read, like gpu_ms); <0 when no back thread is engaged. Optional symbol.
GE1_API float ge1_gs_back_ms(void);
// PW1: flush the Vulkan pipeline cache and persist newly recorded TFX selectors now.
// For a future app pause/stop hook; the periodic ge1_gs_vsync path covers force-stop.
GE1_API int ge1_gs_flush_caches(void);
// PCF1: GS-thread policy setter; explicit app-pause flush still overrides it.
GE1_API void ge1_gs_set_cache_flush_deferred(int deferred);
// DS1: quick-save support. Thin wrappers over GSfreeze (the GS runs
// single-threaded, GSVSyncMode::Disabled, so the caller owns the renderer).
// freeze_size returns the blob bytes (>0) or 0 when closed/failed;
// freeze_save writes exactly that many bytes (1 ok, 0 fail); freeze_load
// restores (1 ok, 0 fail). The save reads render targets back into VRAM so
// target-resident pixels survive the round trip.
GE1_API int ge1_gs_freeze_size(void);
GE1_API int ge1_gs_freeze_save(uint8_t* out, uint32_t size);
GE1_API int ge1_gs_freeze_load(const uint8_t* data, uint32_t size);
// NRT1 2b: a native record, one per natively served VU1 job: the job's
// PATH1 GIF packets in kick order, delivered in one call. GE1 runs
// GSgifTransfer on each packet in order, exactly as that many ge1_gs_packet
// (path 1) calls would. Layout (little-endian, 16-byte aligned):
//   qword 0: GE1_NATIVE_RECORD_MAGIC_LO, GE1_NATIVE_RECORD_MAGIC_HI (as a GIF
//            tag: NLOOP 0, EOP 0, the signature in the must-be-zero bits)
//   u32 packet count, u32 0, then count u32 byte sizes, padded to 16 bytes
//   the packets, each a whole GIF packet (multiple of 16 bytes).
// Optional symbol; without it the runtime delivers the packets one by one.
#define GE1_NATIVE_RECORD_MAGIC_LO 0x00002A4E52540000ull
#define GE1_NATIVE_RECORD_MAGIC_HI 0x5245434F52440001ull
GE1_API int ge1_gs_native_record(const uint8_t* bytes, uint32_t byte_count);
// NRS1: the compact native record (PS2X_SSX3_NATIVE_COMPACT=1). Layout and
// walker in ps2xRuntime/include/runtime/gs/ge1_gs_api.h; the struct is
// mirrored here and in the vendor (pcsx2/GS/GSCompactRecord.h) per the buddy
// rule there. The adapter only forwards whole records to the vendor.
#define GE1_COMPACT_RECORD_MAGIC_LO 0x00002A4E52430000ull
#define GE1_COMPACT_RECORD_MAGIC_HI 0x5245434F52440002ull
typedef struct Ge1CompactVertex
{
    uint32_t S, T;
    uint32_t RGBA;
    uint32_t Q;
    uint32_t X, Y;
    uint32_t Z;
    uint32_t W3;
} Ge1CompactVertex;
GE1_API int ge1_gs_native_record_compact(const uint8_t* bytes, uint32_t byte_count);
// RZV1 S4a: the keyed native record (PS2X_SSX3_NATIVE_KEYED=1). Layout and
// field meanings in ps2xRuntime/include/runtime/gs/ge1_gs_api.h; mirrored here
// per the buddy rule (sizes pinned below). The adapter ingests the embedded
// compact record exactly as ge1_gs_native_record_compact does;
// GE1_S4A_CHECK=1 first recomputes every pass's facts with GE1's own vertex
// parse (ParseCompactXYZF2 / FmmPos) and counts mismatches.
GE1_API int ge1_gs_native_record_keyed(const uint8_t* bytes, uint32_t byte_count);
// HUD4: composite one Tricky HUD scene onto an already-exported AHB, on
// GE1's own Vulkan queue (Android only). See the runtime-side contract in
// ps2xRuntime/include/runtime/gs/ge1_gs_api.h. This struct mirrors it
// exactly (all members 4 bytes, no padding); the adapter checks magic +
// exact size before passing the blob to the vendor, fail-closed.
#define GE1_HUD_SCENE_MAGIC 0x44554847u // 'HUDG'
#define GE1_HUD_SCENE_VERSION 1u
#define GE1_HUD_SCENE_MAX_QUADS 26
typedef struct Ge1HudScene
{
    uint32_t magic;
    uint32_t version;
    int32_t regionX, regionY, regionW, regionH;
    int32_t nsmears;
    int32_t smearX0[2], smearY0[2], smearX1[2], smearY1[2];
    int32_t smearLX[2], smearRX[2];
    int32_t nquads;
    int32_t quadSrcX[GE1_HUD_SCENE_MAX_QUADS];
    int32_t quadSrcY[GE1_HUD_SCENE_MAX_QUADS];
    int32_t quadSrcW[GE1_HUD_SCENE_MAX_QUADS];
    int32_t quadSrcH[GE1_HUD_SCENE_MAX_QUADS];
    int32_t quadDX[GE1_HUD_SCENE_MAX_QUADS];
    int32_t quadDY[GE1_HUD_SCENE_MAX_QUADS];
    int32_t quadDW[GE1_HUD_SCENE_MAX_QUADS];
    int32_t quadDH[GE1_HUD_SCENE_MAX_QUADS];
    float quadDim[GE1_HUD_SCENE_MAX_QUADS];
} Ge1HudScene;
GE1_API int ge1_gs_hud_scene(void* buffer, const Ge1HudScene* scene, const uint8_t* atlasPx,
                             uint32_t atlasW, uint32_t atlasH, uint64_t atlasId,
                             uint64_t* fence_counter);
#ifdef __cplusplus
}

// Shared by the runtime (builder, unpack fallback) and the adapter. Calls
// fn(packet, size) per packet in order; false on a malformed record.
template <class Fn>
inline bool ge1_native_record_for_each(const uint8_t* bytes, uint32_t size, Fn&& fn)
{
    uint64_t lo = 0, hi = 0;
    if (!bytes || size < 32u || (size & 15u))
        return false;
    __builtin_memcpy(&lo, bytes, 8);
    __builtin_memcpy(&hi, bytes + 8, 8);
    if (lo != GE1_NATIVE_RECORD_MAGIC_LO || hi != GE1_NATIVE_RECORD_MAGIC_HI)
        return false;
    uint32_t count = 0;
    __builtin_memcpy(&count, bytes + 16, 4);
    const uint32_t table = (8u + 4u * count + 15u) & ~15u;
    if (count == 0 || count > 64u || 16u + table > size)
        return false;
    uint32_t off = 16u + table;
    for (uint32_t i = 0; i < count; ++i)
    {
        uint32_t n = 0;
        __builtin_memcpy(&n, bytes + 24 + 4 * i, 4);
        if (n < 16u || (n & 15u) || n > size - off)
            return false;
        fn(bytes + off, n);
        off += n;
    }
    return off == size;
}
inline bool ge1_is_native_record(const uint8_t* bytes, uint32_t size)
{
    uint64_t lo = 0, hi = 0;
    if (!bytes || size < 32u)
        return false;
    __builtin_memcpy(&lo, bytes, 8);
    __builtin_memcpy(&hi, bytes + 8, 8);
    return lo == GE1_NATIVE_RECORD_MAGIC_LO && hi == GE1_NATIVE_RECORD_MAGIC_HI;
}
#include "ge1_keyed.h"
#endif
