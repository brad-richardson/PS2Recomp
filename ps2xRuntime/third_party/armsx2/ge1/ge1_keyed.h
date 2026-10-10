#pragma once
// RZV1 S4a: the keyed native record ABI on the adapter side (mirror of
// ps2xRuntime/include/runtime/gs/ge1_gs_api.h per the buddy rule). Kept apart
// from ge1_gs.h so the check TU can include the vendor's kick header (which
// declares its own Ge1CompactVertex) without the adapter's twin.
#include <cstdint>
#ifndef GE1_COMPACT_RECORD_MAGIC_LO
#define GE1_COMPACT_RECORD_MAGIC_LO 0x00002A4E52430000ull
#define GE1_COMPACT_RECORD_MAGIC_HI 0x5245434F52440002ull
#endif
#define GE1_KEYED_RECORD_MAGIC_LO 0x00002A4E524B0000ull
#define GE1_KEYED_RECORD_MAGIC_HI 0x5245434F52440003ull
typedef struct Ge1KeyedJob
{
    uint64_t key, check;
    uint32_t kind, pc, n, variant, inputBytes, flags, passes, reserved0;
    float m[16], scale[4], offset[4];
    uint32_t rgba[4];
    float uvm[16];
    uint32_t reserved[8];
} Ge1KeyedJob;
typedef struct Ge1KeyedPass
{
    uint32_t nverts, ntris, prim, flags;
    uint32_t adc[2];
    uint32_t tnan, reserved0;
    uint32_t pmin[4], pmax[4];
    float tmin[4], tmax[4];
    uint8_t cmin[4], cmax[4];
    uint32_t reserved[6];
} Ge1KeyedPass;
// RZV1 S4a: the keyed record's parts (mirror of the runtime's ge1_keyed_record_parts).
struct Ge1KeyedParts
{
    const Ge1KeyedJob* job;
    const Ge1KeyedPass* pass;
    uint32_t passes;
    const uint8_t* tris;
    uint32_t trisBytes;
    const uint8_t* compact;
    uint32_t compactSize;
    const uint8_t* s4b; // RZV1 S4b block (PS2X_SSX3_NATIVE_KEYED=2), else null
    uint32_t s4bBytes;
};
inline bool ge1_keyed_record_parts(const uint8_t* bytes, uint32_t size, Ge1KeyedParts& out)
{
    static_assert(sizeof(Ge1KeyedJob) == 256, "Ge1KeyedJob layout");
    static_assert(sizeof(Ge1KeyedPass) == 128, "Ge1KeyedPass layout");
    uint64_t lo = 0, hi = 0;
    if (!bytes || size < 64u || (size & 15u))
        return false;
    __builtin_memcpy(&lo, bytes, 8);
    __builtin_memcpy(&hi, bytes + 8, 8);
    if (lo != GE1_KEYED_RECORD_MAGIC_LO || hi != GE1_KEYED_RECORD_MAGIC_HI)
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
    __builtin_memcpy(&lo, out.compact, 8);
    __builtin_memcpy(&hi, out.compact + 8, 8);
    if (lo != GE1_COMPACT_RECORD_MAGIC_LO || hi != GE1_COMPACT_RECORD_MAGIC_HI || out.job->passes != passes)
        return false;
    uint32_t count = 0;
    __builtin_memcpy(&count, out.compact + 16, 4);
    uint64_t tris = 0;
    for (uint32_t i = 0; i < passes; ++i)
        tris += 3ull * out.pass[i].ntris;
    return count == passes && tris <= trisBytes;
}
// The check (ge1_s4a_check.cpp): the pass's facts recomputed from the
// compact packet through GE1's parse, compared field for field.
bool ge1_s4a_pass_matches(const uint8_t* pkt, uint32_t size, const Ge1KeyedPass& rec, const uint8_t* recTris);
// RZV1 S4b (GE1_RESIDENT=1, ge1_resident.cpp): the resident pool and the
// vertex generator, checked against the record's compact vertices (CPU every
// record; Metal batched per vsync on the Mac). Called around the compact ingest.
bool ge1_resident_on();
void ge1_resident_before(const Ge1KeyedParts& parts);
void ge1_resident_after();
void ge1_resident_vsync();
