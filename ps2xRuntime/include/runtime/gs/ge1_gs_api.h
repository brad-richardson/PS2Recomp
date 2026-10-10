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
GE1_API int ge1_gs_export_ahb(void* buffer, uint32_t width, uint32_t height, uint64_t* fence_counter);
GE1_API void ge1_gs_wait_export(uint64_t fence_counter);
GE1_API void ge1_gs_release_ahb(void* buffer);
GE1_API float ge1_gs_gpu_ms(void);
// PT2 Part 2: accumulated GS back-thread busy ms since the last call (reset
// on read, like gpu_ms); sampled once per GuestVsync into the gsback.busy
// stage. <0 when the back thread is off (GE1_BACKTHREAD=off) or unsupported.
// Optional symbol (pre-query libraries lack it; the stage then reads n=0).
GE1_API float ge1_gs_back_ms(void);
#if defined(PS2X_GE1_STATIC_IOSURFACE)
// GI1: async GPU blit of the merged snapshot into an IOSurface-backed sink.
// 1=queued (done(ctx, ok) then fires exactly once from the command buffer's
// completion handler, on an arbitrary thread; the surface must stay alive
// until then), 0/-1=no callback.
typedef void (*ge1_gs_export_done_fn)(void* ctx, int ok);
GE1_API int ge1_gs_export_iosurface(void* iosurface, uint32_t width, uint32_t height,
                                    ge1_gs_export_done_fn done, void* ctx);
#endif
// PW1: flush the Vulkan pipeline cache and persist newly recorded TFX selectors now.
// For the app pause/stop hook (BG1); the periodic ge1_gs_vsync path covers force-stop.
GE1_API int ge1_gs_flush_caches(void);
// PCF1: GS-thread policy setter; explicit app-pause flush still overrides it.
GE1_API void ge1_gs_set_cache_flush_deferred(int deferred);
// DS1: quick-save support over GSfreeze. freeze_size returns the blob bytes
// (>0) or 0 when closed/failed; freeze_save writes exactly that many bytes
// (1 ok, 0 fail); freeze_load restores (1 ok, 0 fail).
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
// NRS1: a compact native record, one per natively served VU1 job, emitted
// when PS2X_SSX3_NATIVE_COMPACT=1: the job's PATH1 vertex packets with the
// GIF register padding stripped, so GE1's GsWorker stops re-parsing them.
// Same hook, same VU1-memory writes as the GIF record, so the det hashes
// (vu1Data included) are unchanged. Layout (little-endian, 16-byte aligned):
//   qword 0: GE1_COMPACT_RECORD_MAGIC_LO, GE1_COMPACT_RECORD_MAGIC_HI
//   u32 packet count, u32 0, then count u32 byte sizes, padded to 16 bytes
//   the packets: each a 16-byte verbatim GIF tag (PACKED, PRE, NREG 3,
//   REGS {STQ, RGBAQ, XYZF2}) followed by NLOOP Ge1CompactVertex (32 B).
// Optional symbol (ge1_gs_native_record_compact); without it the runtime
// expands each packet back to GIF packets (every byte the kick consumes is
// carried verbatim; ST.w3 and the RGBAQ upper bytes, which no kick path
// reads, expand as zero).
// Buddy rule: this struct is mirrored in the adapter
// (ps2xRuntime/third_party/armsx2/ge1/ge1_gs.h) and in the vendor
// (pcsx2/GS/GSCompactRecord.h). All members are 4 bytes (no padding);
// magic + exact size are checked at both layers, fail-closed.
#define GE1_COMPACT_RECORD_MAGIC_LO 0x00002A4E52430000ull
#define GE1_COMPACT_RECORD_MAGIC_HI 0x5245434F52440002ull
typedef struct Ge1CompactVertex
{
    uint32_t S, T; // ST.w0, ST.w1 (raw bits)
    uint32_t RGBA; // R | G<<8 | B<<16 | A<<24 (low bytes of the RGBAQ words)
    uint32_t Q;    // ST.w2 (raw bits; Q==+0.0 -> FLT_MIN applies at kick)
    uint32_t X, Y; // XYZF2.w0, XYZF2.w1 verbatim (the kick reads the low halves)
    uint32_t Z;    // XYZF2.w2 verbatim
    uint32_t W3;   // XYZF2.w3 verbatim (F + ADC)
} Ge1CompactVertex;
GE1_API int ge1_gs_native_record_compact(const uint8_t* bytes, uint32_t byte_count);
// RZV1 S4a: a keyed native record (PS2X_SSX3_NATIVE_KEYED=1, needs
// PS2X_SSX3_NATIVE_COMPACT=1): the job's compact record verbatim, preceded by
// what a GPU-resident static world needs to know about it without the
// vertices: a content key over the job's VU1 inputs, the job's transform
// constants, and per packet the exact facts the CPU model already has (ADC
// mask, triangle list, the vertex-trace bounds over referenced vertices).
// GE1 ingests the embedded compact record exactly as
// ge1_gs_native_record_compact does (same vertices, same kick); the header
// is carried for S4b and checked by GE1_S4A_CHECK=1. Layout (16-byte aligned):
//   qword 0: GE1_KEYED_RECORD_MAGIC_LO, GE1_KEYED_RECORD_MAGIC_HI
//   u32 total size, u32 pass count, u32 triangle-list bytes (padded to 16), u32 0
//   Ge1KeyedJob (256 B), Ge1KeyedPass x pass count (128 B each),
//   the triangle lists (u8 vertex-index triples per pass, in pass order),
//   the compact record (GE1_COMPACT_RECORD_MAGIC_*, its own sizes) to the end.
// Pass i describes compact packet i. Buddy rule as for the compact record:
// mirrored in the adapter (ge1_gs.h); sizes are pinned by static_asserts.
#define GE1_KEYED_RECORD_MAGIC_LO 0x00002A4E524B0000ull
#define GE1_KEYED_RECORD_MAGIC_HI 0x5245434F52440003ull
typedef struct Ge1KeyedJob
{
    uint64_t key;        // XXH3-64 of the job's VU1 input bytes (geometry: positions, UVs, chunk)
    uint64_t check;      // XXH64 (seed 0x5334) of the same bytes: a second hash for collision checks
    uint32_t kind;       // 1 terrain, 2 scenery instance
    uint32_t pc;         // VU1 entry; | 0x10000 for a scenery second pass
    uint32_t n;          // terrain grid n; scenery chunk vertex count
    uint32_t variant;    // terrain: 0; scenery: 0 clip (s = offset*1 + t*scale), 1 no-clip (M folded by the model)
    uint32_t inputBytes; // bytes the key covers
    uint32_t flags;      // scenery: the flags word (mem[34].w); terrain: 0
    uint32_t passes;     // compact packets in this job
    uint32_t reserved0;
    float m[16];         // clip matrix rows (terrain mem[0..3]; scenery V x model, mem[6..9])
    float scale[4];      // viewport scale (mem[4])
    float offset[4];     // viewport offset (mem[5]; scenery: w forced to 1.0 as the model does)
    uint32_t rgba[4];    // terrain constant vertex colour (mem[6]); scenery 0
    float uvm[16];       // terrain texgen M2 (TOP+0x80..0x83) / scenery UV matrix (mem[10..13]); else 0
    uint32_t reserved[8];
} Ge1KeyedJob;
typedef struct Ge1KeyedPass
{
    uint32_t nverts, ntris;
    uint32_t prim;       // the packet tag's PRIM register value
    uint32_t flags;      // bit 0 facts valid (triangle prim), bit 1 flat (every triangle one RGBA), bit 2 zeq
    uint32_t adc[2];     // bit v: vertex v has ADC (no drawing kick)
    uint32_t tnan;       // bit l: a NaN in t lane l (S/Q, T/Q, Q) among referenced vertices
    uint32_t reserved0;
    uint32_t pmin[4], pmax[4]; // u32 {x, y, z, fog} as GE1 parses them (XY 12.4, Z24, F) over referenced vertices
    float tmin[4], tmax[4];    // {S/Q, T/Q, Q, Q} (Q == +0 -> FLT_MIN), NaNs excluded
    uint8_t cmin[4], cmax[4];  // RGBA bytes; IIP=0: provoking (last) vertices only
    uint32_t reserved[6];
} Ge1KeyedPass;
GE1_API int ge1_gs_native_record_keyed(const uint8_t* bytes, uint32_t byte_count);
// HUD4: composite one Tricky HUD scene onto an already-exported AHB, on
// GE1's own Vulkan queue (Android only). The AHB holds the final frame (the
// export copy just submitted); GE1 submits the composite right behind it on
// the same queue and returns the composite fence in fence_counter (which
// covers the export too, in order). Optional symbol; without it (or on any
// failure, return != 1) the runtime keeps its CPU stamp.
//   scene: a Ge1HudScene blob (magic + version + fixed-size geometry).
//   atlasPx: atlasW*atlasH*4 RGBA bytes, top-left origin; atlasId re-uploads
//     the GPU atlas when (id, w, h) change (the runtime passes its Atlas*).
// Buddy rule: this struct is mirrored in the adapter
// (ps2xRuntime/third_party/armsx2/ge1/ge1_gs.h) and in the vendor
// (GSDeviceVK). All members are 4 bytes (no padding); magic + exact size
// are checked at both layers, fail-closed, so any drift refuses loudly
// instead of misdrawing.
#define GE1_HUD_SCENE_MAGIC 0x44554847u // 'HUDG'
#define GE1_HUD_SCENE_VERSION 1u
#define GE1_HUD_SCENE_MAX_QUADS 26
typedef struct Ge1HudScene
{
    uint32_t magic;   // GE1_HUD_SCENE_MAGIC
    uint32_t version; // GE1_HUD_SCENE_VERSION
    int32_t regionX, regionY, regionW, regionH; // HUD region, frame coords
    int32_t nsmears;                            // 0..2
    int32_t smearX0[2], smearY0[2], smearX1[2], smearY1[2]; // frame coords
    int32_t smearLX[2], smearRX[2]; // edge columns smearCoverS would read, frame coords
    int32_t nquads;                 // 0..GE1_HUD_SCENE_MAX_QUADS
    int32_t quadSrcX[GE1_HUD_SCENE_MAX_QUADS]; // atlas cell, atlas px
    int32_t quadSrcY[GE1_HUD_SCENE_MAX_QUADS];
    int32_t quadSrcW[GE1_HUD_SCENE_MAX_QUADS];
    int32_t quadSrcH[GE1_HUD_SCENE_MAX_QUADS];
    int32_t quadDX[GE1_HUD_SCENE_MAX_QUADS]; // dest rect, frame coords
    int32_t quadDY[GE1_HUD_SCENE_MAX_QUADS];
    int32_t quadDW[GE1_HUD_SCENE_MAX_QUADS];
    int32_t quadDH[GE1_HUD_SCENE_MAX_QUADS];
    float quadDim[GE1_HUD_SCENE_MAX_QUADS]; // RGB multiplier (alpha unchanged)
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
// NRS1: the compact record walker. Calls fn(packet, size) per compact
// packet (16-byte tag + NLOOP 32-byte vertices) in order; false on a
// malformed record.
template <class Fn>
inline bool ge1_compact_record_for_each(const uint8_t* bytes, uint32_t size, Fn&& fn)
{
    uint64_t lo = 0, hi = 0;
    if (!bytes || size < 32u || (size & 15u))
        return false;
    __builtin_memcpy(&lo, bytes, 8);
    __builtin_memcpy(&hi, bytes + 8, 8);
    if (lo != GE1_COMPACT_RECORD_MAGIC_LO || hi != GE1_COMPACT_RECORD_MAGIC_HI)
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
        if (n < 16u || (n & 15u) || n > size - off || (n - 16u) % 32u)
            return false;
        fn(bytes + off, n);
        off += n;
    }
    return off == size;
}
inline bool ge1_is_compact_record(const uint8_t* bytes, uint32_t size)
{
    uint64_t lo = 0, hi = 0;
    if (!bytes || size < 32u)
        return false;
    __builtin_memcpy(&lo, bytes, 8);
    __builtin_memcpy(&hi, bytes + 8, 8);
    return lo == GE1_COMPACT_RECORD_MAGIC_LO && hi == GE1_COMPACT_RECORD_MAGIC_HI;
}
inline bool ge1_is_any_native_record(const uint8_t* bytes, uint32_t size)
{
    if (ge1_is_native_record(bytes, size) || ge1_is_compact_record(bytes, size))
        return true;
    // RZV1 S4a: keyed records (a compact record with its S4a header).
    uint64_t lo = 0, hi = 0;
    if (!bytes || size < 32u)
        return false;
    __builtin_memcpy(&lo, bytes, 8);
    __builtin_memcpy(&hi, bytes + 8, 8);
    return lo == GE1_KEYED_RECORD_MAGIC_LO && hi == GE1_KEYED_RECORD_MAGIC_HI;
}
// Expands one compact packet to the GIF packet bytes the kick is equivalent
// to: the tag verbatim, then per vertex ST {S,T,Q,0}, RGBAQ {R,G,B,A} and the
// XYZF2 words verbatim. Returns false (out untouched) on a malformed packet.
inline bool ge1_compact_expand_packet(const uint8_t* pkt, uint32_t size, uint8_t* out, uint32_t outSize)
{
    if (!pkt || !out || size < 16u || ((size - 16u) % 32u))
        return false;
    const uint32_t n = (size - 16u) / 32u;
    if (outSize < 16u + 48u * n)
        return false;
    __builtin_memcpy(out, pkt, 16);
    for (uint32_t i = 0; i < n; ++i)
    {
        Ge1CompactVertex v;
        __builtin_memcpy(&v, pkt + 16 + 32u * i, 32);
        uint32_t* reg = reinterpret_cast<uint32_t*>(out + 16 + 48u * i);
        reg[0] = v.S;
        reg[1] = v.T;
        reg[2] = v.Q;
        reg[3] = 0;
        reg[4] = v.RGBA & 0xffu;
        reg[5] = (v.RGBA >> 8) & 0xffu;
        reg[6] = (v.RGBA >> 16) & 0xffu;
        reg[7] = (v.RGBA >> 24) & 0xffu;
        reg[8] = v.X;
        reg[9] = v.Y;
        reg[10] = v.Z;
        reg[11] = v.W3;
    }
    return true;
}
// RZV1 S4a: the keyed record's parts (all pointers into bytes); false on a malformed record.
struct Ge1KeyedParts
{
    const Ge1KeyedJob* job;
    const Ge1KeyedPass* pass;
    uint32_t passes;
    const uint8_t* tris;
    uint32_t trisBytes;
    const uint8_t* compact;
    uint32_t compactSize;
    const uint8_t* s4b; // RZV1 S4b block (KEYED=2), else null
    uint32_t s4bBytes;
};
inline bool ge1_is_keyed_record(const uint8_t* bytes, uint32_t size)
{
    uint64_t lo = 0, hi = 0;
    if (!bytes || size < 32u)
        return false;
    __builtin_memcpy(&lo, bytes, 8);
    __builtin_memcpy(&hi, bytes + 8, 8);
    return lo == GE1_KEYED_RECORD_MAGIC_LO && hi == GE1_KEYED_RECORD_MAGIC_HI;
}
inline bool ge1_keyed_record_parts(const uint8_t* bytes, uint32_t size, Ge1KeyedParts& out)
{
    static_assert(sizeof(Ge1KeyedJob) == 256, "Ge1KeyedJob layout");
    static_assert(sizeof(Ge1KeyedPass) == 128, "Ge1KeyedPass layout");
    if (!ge1_is_keyed_record(bytes, size) || (size & 15u))
        return false;
    uint32_t hdr[4];
    __builtin_memcpy(hdr, bytes + 16, 16);
    const uint32_t total = hdr[0], passes = hdr[1], trisBytes = hdr[2], s4bBytes = hdr[3];
    if (total != size || passes == 0 || passes > 64u || (trisBytes & 15u) || (s4bBytes & 15u))
        return false;
    const uint64_t fixed = 32ull + 256ull + 128ull * passes + trisBytes + s4bBytes;
    if (fixed + 32ull > size)
        return false;
    out.s4b = s4bBytes ? bytes + 32 + 256 + 128u * passes + trisBytes : nullptr;
    out.s4bBytes = s4bBytes;
    out.job = reinterpret_cast<const Ge1KeyedJob*>(bytes + 32);
    out.pass = reinterpret_cast<const Ge1KeyedPass*>(bytes + 32 + 256);
    out.passes = passes;
    out.tris = bytes + 32 + 256 + 128u * passes;
    out.trisBytes = trisBytes;
    out.compact = bytes + fixed;
    out.compactSize = size - static_cast<uint32_t>(fixed);
    if (!ge1_is_compact_record(out.compact, out.compactSize) || out.job->passes != passes)
        return false;
    uint32_t count = 0;
    __builtin_memcpy(&count, out.compact + 16, 4);
    uint64_t tris = 0;
    for (uint32_t i = 0; i < passes; ++i)
        tris += 3ull * out.pass[i].ntris;
    return count == passes && tris <= trisBytes;
}
// RZV1 S4a: the facts of one compact packet (16-byte tag + n x 32-byte
// vertices), as the keyed record carries them: the GS drawing-kick rule for
// triangle prims (a vertex without ADC, with two queued before it in this
// packet, kicks (v-2, v-1, v); a list restarts its queue every three
// vertices; a fan kicks (0, v-1, v)), then the bounds over the vertices those
// triangles reference, fields parsed as GE1 parses them (ParseCompactXYZF2 /
// FmmPos). Any non-triangle prim, or more than 64 vertices, leaves flags
// bit 0 clear and no triangles. tris gets 3 x ntris u8 indices (capacity
// 3 x 62). Float work assumes IEEE round-to-nearest without flush-to-zero.
inline void ge1_keyed_pass_facts(const uint8_t* pkt, uint32_t size, Ge1KeyedPass& p, uint8_t* tris)
{
    __builtin_memset(&p, 0, sizeof(p));
    const uint32_t nv = (size - 16u) / 32u;
    uint64_t tag = 0;
    __builtin_memcpy(&tag, pkt, 8);
    p.nverts = nv;
    p.prim = static_cast<uint32_t>((tag >> 47) & 0x7ffu);
    const uint32_t type = p.prim & 7u;
    const bool iip = (p.prim >> 3) & 1u;
    auto vert = [&](uint32_t v) {
        Ge1CompactVertex x;
        __builtin_memcpy(&x, pkt + 16 + 32u * v, 32);
        return x;
    };
    for (uint32_t v = 0; v < nv && v < 64u; ++v)
        if (vert(v).W3 & 0x8000u)
            p.adc[v >> 5] |= 1u << (v & 31u);
    if (type < 3u || type > 5u || nv > 64u)
        return;
    p.flags = 1u;
    bool any = false, flat = true, zeq = true;
    uint32_t z0 = 0;
    const float big = __builtin_huge_valf();
    for (int l = 0; l < 4; ++l)
    {
        p.pmin[l] = 0xffffffffu;
        p.cmin[l] = 0xffu;
        p.tmin[l] = big;
        p.tmax[l] = -big;
    }
    auto takeP = [&](const Ge1CompactVertex& x) {
        const uint32_t q[4] = {x.X & 0xffffu, x.Y & 0xffffu, (x.Z >> 4) & 0x00ffffffu, (x.W3 >> 4) & 0xffu};
        for (int l = 0; l < 4; ++l)
        {
            p.pmin[l] = q[l] < p.pmin[l] ? q[l] : p.pmin[l];
            p.pmax[l] = q[l] > p.pmax[l] ? q[l] : p.pmax[l];
        }
        if (!any)
            z0 = q[2];
        else if (q[2] != z0)
            zeq = false;
        any = true;
        float s, t, qf;
        const uint32_t qb = x.Q == 0u ? 0x00800000u : x.Q;
        __builtin_memcpy(&s, &x.S, 4);
        __builtin_memcpy(&t, &x.T, 4);
        __builtin_memcpy(&qf, &qb, 4);
        const float tv[4] = {s / qf, t / qf, qf, qf};
        for (int l = 0; l < 4; ++l)
        {
            if (tv[l] != tv[l])
            {
                p.tnan |= 1u << l;
                continue;
            }
            p.tmin[l] = tv[l] < p.tmin[l] ? tv[l] : p.tmin[l];
            p.tmax[l] = tv[l] > p.tmax[l] ? tv[l] : p.tmax[l];
        }
    };
    auto takeC = [&](const Ge1CompactVertex& x) {
        for (int l = 0; l < 4; ++l)
        {
            const uint8_t c = static_cast<uint8_t>(x.RGBA >> (8 * l));
            p.cmin[l] = c < p.cmin[l] ? c : p.cmin[l];
            p.cmax[l] = c > p.cmax[l] ? c : p.cmax[l];
        }
    };
    uint32_t queued = 0;
    for (uint32_t v = 0; v < nv; ++v)
    {
        ++queued;
        if (type == 3u && queued > 3u)
            queued = 1u;
        const bool adc = (p.adc[v >> 5] >> (v & 31u)) & 1u;
        if (adc || queued < 3u)
            continue;
        const uint32_t a = type == 5u ? 0u : v - 2u, b = v - 1u, c = v;
        uint8_t* t3 = tris + 3u * p.ntris++;
        t3[0] = static_cast<uint8_t>(a);
        t3[1] = static_cast<uint8_t>(b);
        t3[2] = static_cast<uint8_t>(c);
        const Ge1CompactVertex va = vert(a), vb = vert(b), vc = vert(c);
        takeP(va);
        takeP(vb);
        takeP(vc);
        if (va.RGBA != vc.RGBA || vb.RGBA != vc.RGBA)
            flat = false;
        if (iip)
        {
            takeC(va);
            takeC(vb);
        }
        takeC(vc);
    }
    if (flat)
        p.flags |= 2u;
    if (any && zeq)
        p.flags |= 4u;
}
#endif
