// SPDX-License-Identifier: GPL-3.0-or-later
// NRT1: native static world, phase 1: terrain + scenery instances (see ps2_native_world.h).
//
// The terrain model is NRV1's (local/research/NRV1/REPORT.md §2, tooling
// terrain.h / drive_terrain.inc), written from our own disassembly of the
// SSX 3 terrain program (images 0b1975ec / 5d749eaf); no microcode bytes.
// Arithmetic is microVU's at the play settings: AArch64 FPCR round toward
// zero + flush-to-zero, results clamped to +-FLT_MAX, separate multiply and
// add (this file is built with -ffp-contract=off -frounding-math), microVU's
// DIV-by-zero rule and saturating FTOI.
//
// Routing is speculative and exact-only: the model runs first, without side
// effects; a job that would reach the guard-band clipper (0x1cc8, the one VU
// output the model doesn't reproduce) or uses an unmodelled entry runs on
// VU1. A follow-up job (0x8d8/0x910/0x948/0x980) takes the route of its base
// job, because it reads the base job's packets and scratch, which only exist
// in host state when the base was served natively.
#include "ps2_native_world.h"

#include "ps2_fpmode.h"
#include "ps2_telemetry.h"
#include "runtime/ps2_memory.h"
#include "runtime/gs/ge1_gs_api.h"

#define XXH_INLINE_ALL
#include "runtime/third_party/xxhash.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <arm_neon.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

#pragma clang fp contract(off)

namespace ps2_native_world
{
// NRS1: strip one kick packet (tag verbatim + N x 48-byte regs) to its dense
// form (tag verbatim + N x 32-byte Ge1CompactVertex) at dst. Every byte the
// kick consumes is carried (ST.w0-2, the RGBAQ low bytes, the XYZF2 words);
// only ST.w3 and the RGBAQ upper bytes, which no kick path reads, are dropped.
static void compactPacket(const uint8_t *pkt, uint32_t pktSize, uint8_t *dst)
{
    const uint32_t nv = (pktSize - 16u) / 48u;
    std::memcpy(dst, pkt, 16);
    for (uint32_t i = 0; i < nv; i++)
    {
        const uint32_t *reg = reinterpret_cast<const uint32_t *>(pkt + 16 + 48u * i);
        Ge1CompactVertex v;
        v.S = reg[0];
        v.T = reg[1];
        v.RGBA = (reg[4] & 0xffu) | ((reg[5] & 0xffu) << 8) | ((reg[6] & 0xffu) << 16) |
                 ((reg[7] & 0xffu) << 24);
        v.Q = reg[2];
        v.X = reg[8];
        v.Y = reg[9];
        v.Z = reg[10];
        v.W3 = reg[11];
        std::memcpy(dst + 16 + 32u * i, &v, 32);
    }
}

namespace
{
constexpr uint64_t kTerrainImageA = 0x0B1975EC17164657ull;
constexpr uint64_t kTerrainImageB = 0x5D749EAF98061004ull;
constexpr uint32_t kMaxVerts = 64;   // n = 8: packet A = 4 strips x 16 vertices
constexpr uint32_t kPktQw = 1 + 3 * kMaxVerts;
constexpr uint32_t kPktA = 0x60, kPktB = 0x121; // VU1 qword addresses of the two output packets

// ---------------------------------------------------------------- knobs
struct Knobs
{
    bool terrain = false;
    bool instances = false; // scenery instances (image 2826c443)
    bool check = false;
    bool compact = false; // NRS1: emit compact records (dense vertices, no GIF padding)
    bool keyed = false;   // RZV1 S4a: wrap compact records with key + constants + facts (needs compact)
    uint32_t keyWindow = 8; // S4a pool simulation: a key unused for more than this many ticks is evicted
};
const Knobs &knobs()
{
    static const Knobs k = [] {
        Knobs r;
        if (const char *v = std::getenv("PS2X_SSX3_NATIVE_WORLD"))
        {
            // "1" | "terrain" | "instances" | "terrain,instances"
            const std::string list = v;
            size_t at = 0;
            while (at <= list.size())
            {
                const size_t end = std::min(list.find(',', at), list.size());
                const std::string tok = list.substr(at, end - at);
                if (tok == "1" || tok == "terrain")
                    r.terrain = true;
                else if (tok == "instances")
                    r.instances = true;
                at = end + 1;
            }
        }
        if (const char *v = std::getenv("PS2X_SSX3_NATIVE_WORLD_CHECK"))
            r.check = !std::strcmp(v, "1");
        if (const char *v = std::getenv("PS2X_SSX3_NATIVE_COMPACT"))
            r.compact = !std::strcmp(v, "1");
        if (const char *v = std::getenv("PS2X_SSX3_NATIVE_KEYED"))
            r.keyed = r.compact && !std::strcmp(v, "1");
        if (const char *v = std::getenv("PS2X_SSX3_NATIVE_KEYED_WINDOW"))
            r.keyWindow = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
        if (r.check)
            r.terrain = r.instances = false; // check mode routes nothing
        return r;
    }();
    return k;
}

// ---------------------------------------------------------------- entries
struct Entry
{
    uint16_t pc;
    int8_t k;       // size class from the setup sub (0/2/4 -> n 4/6/8); -1: follow-up (group state)
    uint8_t base;   // 0x9b0 base pass
    uint8_t pass2;  // 1: 0xc30 (ST rewrite, re-kick), 2: 0xd40 (strip re-run with UV set B)
    uint8_t texgen; // 0 none, 1: 0xb60 (UV B = M2 x positions), 2: 0xbc8 (UV B = M2 x UV A)
};
// NRV1-validated entries. Base-only 0x610/0x648/0x680 and the mixed-patch
// entries 0x868/0x8a0 were never seen in the captures and stay on VU1.
constexpr Entry kEntries[] = {
    {0x6b8, 0, 1, 1, 0}, {0x700, 2, 1, 1, 0}, {0x748, 4, 1, 1, 0},
    {0x790, 0, 1, 2, 0}, {0x7d8, 2, 1, 2, 0}, {0x820, 4, 1, 2, 0},
    {0x8d8, -1, 0, 1, 1}, {0x910, -1, 0, 2, 1}, {0x948, -1, 0, 1, 2}, {0x980, -1, 0, 2, 2},
};
constexpr int kNumEntries = sizeof(kEntries) / sizeof(kEntries[0]);
int entryIndex(uint32_t pc)
{
    for (int i = 0; i < kNumEntries; ++i)
        if (kEntries[i].pc == pc)
            return i;
    return -1;
}

// ---------------------------------------------------------------- VU float model
// NEON, one VU instruction per vector op: MUL and ADD stay separate (this file
// is built with -ffp-contract=off: no FMLA), each result clamped as microVU does,
// under the FPCR that runModel() sets (round toward zero, flush to zero).
inline uint32_t fbits(float f)
{
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}
inline float ffrom(uint32_t u)
{
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
// A value the compiler can't fold (x * 1.0 is not an identity under flush-to-zero).
inline float opaque(float x)
{
    __asm__ volatile("" : "+w"(x));
    return x;
}
using F4 = float32x4_t;
// microVU's result clamp: NaN -> +-FLT_MAX by sign, +-inf -> +-FLT_MAX.
inline F4 vclamp4(F4 x)
{
    const F4 hi = vdupq_n_f32(FLT_MAX), lo = vdupq_n_f32(-FLT_MAX);
    const uint32x4_t xu = vreinterpretq_u32_f32(x);
    const F4 nanv = vreinterpretq_f32_u32(vorrq_u32(vandq_u32(xu, vdupq_n_u32(0x80000000u)), vdupq_n_u32(0x7f7fffffu)));
    F4 r = vbslq_f32(vcgtq_f32(x, hi), hi, x);
    r = vbslq_f32(vcltq_f32(x, lo), lo, r);
    return vbslq_f32(vceqq_f32(x, x), r, nanv);
}
inline F4 vmul4(F4 a, F4 b) { return vclamp4(vmulq_f32(a, b)); }
inline F4 vadd4(F4 a, F4 b) { return vclamp4(vaddq_f32(a, b)); }
inline F4 vmadd4(F4 acc, F4 a, F4 b) { return vadd4(acc, vmul4(a, b)); }
// DIV Q = fs / ft; microVU: a zero-exponent divisor gives sign(fs ^ ft) | FLT_MAX.
inline float vdiv(float fs, float ft)
{
    if ((fbits(ft) & 0x7f800000u) == 0)
        return ffrom(((fbits(fs) ^ fbits(ft)) & 0x80000000u) | 0x7f7fffffu);
    const float r = fs / ft;
    if (r != r)
        return (fbits(r) >> 31) ? -FLT_MAX : FLT_MAX;
    return r > FLT_MAX ? FLT_MAX : (r < -FLT_MAX ? -FLT_MAX : r);
}
// FTOI4: multiply by 16 (exact), FCVTZS (toward zero, saturating). Inputs are
// clamped results, so never NaN.
inline uint32x4_t vftoi4(F4 x) { return vreinterpretq_u32_s32(vcvtq_s32_f32(vmulq_n_f32(x, 16.0f))); }

inline F4 ld4(const uint8_t *mem, uint32_t qw)
{
    return vld1q_f32(reinterpret_cast<const float *>(mem + ((qw & 0x3ffu) << 4)));
}
inline uint32_t ldw(const uint8_t *mem, uint32_t qw, int lane)
{
    uint32_t v;
    std::memcpy(&v, mem + ((qw & 0x3ffu) << 4) + 4 * lane, 4);
    return v;
}
inline uint32_t ilw(const uint8_t *mem, uint32_t qw, int lane) { return ldw(mem, qw, lane) & 0xffffu; }
// MULAx / MADDAy / MADDAz / MADDw: every step rounded.
inline F4 mat4(const F4 *m, F4 v)
{
    F4 a = vmul4(m[0], vdupq_laneq_f32(v, 0));
    a = vmadd4(a, m[1], vdupq_laneq_f32(v, 1));
    a = vmadd4(a, m[2], vdupq_laneq_f32(v, 2));
    return vmadd4(a, m[3], vdupq_laneq_f32(v, 3));
}
// CLIPw.xyz flags (bits +x -x +y -y +z -z), |c| against |w|.
inline uint32_t vclip(F4 c)
{
    const F4 w = vdupq_n_f32(std::fabs(vgetq_lane_f32(c, 3)));
    const uint32x4_t gt = vcgtq_f32(c, w), lt = vcltq_f32(c, vnegq_f32(w));
    return (vgetq_lane_u32(gt, 0) & 1u) | (vgetq_lane_u32(lt, 0) & 2u) | (vgetq_lane_u32(gt, 1) & 4u) |
           (vgetq_lane_u32(lt, 1) & 8u) | (vgetq_lane_u32(gt, 2) & 16u) | (vgetq_lane_u32(lt, 2) & 32u);
}
// 0x17e8 / 0x1968 on the last three CLIP sets: any vertex beyond +z, or all
// three outside one plane (FCOR masks), rejects the triangle; anything else
// goes to the clipper and kicks a polygon packet.
inline bool trivialReject(uint32_t hist)
{
    if (hist & 0x010410u)
        return true;
    static constexpr uint32_t kAll[5] = {0xfdf7dfu, 0xff7df7u, 0xffbefbu, 0xffdf7du, 0xffefbeu};
    for (uint32_t m : kAll)
        if (((hist | m) & 0xffffffu) == 0xffffffu)
            return true;
    return false;
}

// One output packet, laid out as in VU1 memory: tag qword, then per vertex
// ST, RGBAQ, XYZF2.
struct Pkt
{
    uint32_t q[kPktQw][4];
    uint32_t nv = 0;
};
struct TerrainConst
{
    F4 m[4], scale, offset;
    uint32x4_t rgba;
};
void loadConst(const uint8_t *mem, TerrainConst &c)
{
    for (int i = 0; i < 4; ++i)
        c.m[i] = ld4(mem, i);
    c.scale = ld4(mem, 4);
    c.offset = ld4(mem, 5);
    c.rgba = vld1q_u32(reinterpret_cast<const uint32_t *>(mem + 6 * 16));
}
// UV source: VU1 memory (VIF-uploaded) or a host array (texgen output).
struct UvSrc
{
    const uint8_t *mem = nullptr;
    uint32_t qw = 0;
    const F4 *local = nullptr;
    F4 at(uint32_t i) const { return local ? local[i] : ld4(mem, qw + i); }
};

// The 0x1368 / 0x15a8 strip loop over nstrips strips of two grid rows.
// writeRgba = false is the 0xd40 variant (the RGBA slot keeps its contents).
// Returns false when a triangle would reach the clipper.
bool strips(const TerrainConst &c, const uint8_t *mem, uint32_t pos, const UvSrc &uv, uint32_t n,
            uint32_t nstrips, bool writeRgba, Pkt &pk)
{
    bool ok = true;
    const float one = opaque(1.0f);
    const F4 acc = vmul4(c.offset, vdupq_n_f32(one)); // MULAw ACC = vf6 * vf0.w
    uint32_t v = 0;
    for (uint32_t s = 0; s < nstrips; ++s)
    {
        uint32_t hist = 0;
        for (uint32_t col = 0; col < n; ++col)
            for (uint32_t r = 0; r < 2; ++r, ++v)
            {
                const uint32_t a = (s + r) * n + col;
                const F4 cl = mat4(c.m, ld4(mem, pos + a));
                const float q = vdiv(one, vgetq_lane_f32(cl, 3));
                const F4 qv = vdupq_n_f32(q);
                const F4 sc = vmadd4(acc, vmul4(cl, qv), c.scale);
                vst1q_f32(reinterpret_cast<float *>(pk.q[1 + 3 * v]), vmul4(uv.at(a), qv));
                if (writeRgba)
                    vst1q_u32(pk.q[2 + 3 * v], c.rgba);
                uint32_t *qx = pk.q[3 + 3 * v];
                vst1q_u32(qx, vftoi4(sc));
                if (col == 0)
                    qx[3] = 0xffffffffu;
                hist = ((hist << 6) | vclip(cl)) & 0xffffffu;
                if (col > 0 && (hist & 0x3ffffu))
                {
                    qx[3] = 0xffffffffu;
                    if (!trivialReject(hist))
                        ok = false;
                }
            }
    }
    return ok;
}
// 0x1ae8: ST.xy = UV.xy * ST.w for nv vertices, UVs in strip order.
void rewriteSt(Pkt &pk, uint32_t nv, uint32_t n, const UvSrc &uv)
{
    for (uint32_t v = 0; v < nv; ++v)
    {
        const uint32_t s = v / (2 * n), rem = v % (2 * n);
        uint32_t *qs = pk.q[1 + 3 * v];
        const F4 st = vmul4(uv.at((s + (rem & 1)) * n + rem / 2), vdupq_n_f32(ffrom(qs[3])));
        qs[0] = vgetq_lane_u32(vreinterpretq_u32_f32(st), 0);
        qs[1] = vgetq_lane_u32(vreinterpretq_u32_f32(st), 1);
    }
}
void setTag(Pkt &pk, uint32_t nloop, const uint8_t *mem, uint32_t yzwQw)
{
    pk.q[0][0] = nloop | 0x8000u;
    pk.q[0][1] = ldw(mem, yzwQw, 1);
    pk.q[0][2] = ldw(mem, yzwQw, 2);
    pk.q[0][3] = ldw(mem, yzwQw, 3);
    pk.nv = nloop;
}
void setTagYzw(Pkt &pk, const uint8_t *mem, uint32_t yzwQw)
{
    pk.q[0][1] = ldw(mem, yzwQw, 1);
    pk.q[0][2] = ldw(mem, yzwQw, 2);
    pk.q[0][3] = ldw(mem, yzwQw, 3);
}

// Host stand-in for what a natively served base job leaves in VU1 memory.
struct Group
{
    bool live = false;   // a terrain base job has run since the last reset
    bool native = false; // ...and was served natively (follow-ups follow)
    uint32_t k = 0, n = 0, baseTop = 0;
    uint32_t clipTagQw = 9; // last writer of qw 452/480: 0x9b0 (mem[9]) or 0xd40 (mem[10])
    Pkt pk[2];
};
// Kicks of one job, snapshotted at kick time, written behind room for the
// native record header so the record needs no second copy.
struct Kicks
{
    static constexpr uint32_t kHdr = 48; // 16-byte signature + a 32-byte table (<= 4 packets)
    std::vector<uint8_t> bytes;          // [kHdr reserved][packet 0][packet 1]...
    uint32_t off[4]{}, size[4]{};        // offsets from body() start
    uint32_t count = 0;
    void clear()
    {
        bytes.resize(kHdr);
        count = 0;
    }
    const uint8_t *body() const { return bytes.data() + kHdr; }
    uint32_t bodySize() const { return static_cast<uint32_t>(bytes.size()) - kHdr; }
    void kick(const Pkt &pk) { kickBytes(pk.q, 16u * (1u + 3u * pk.nv)); }
    void kickBytes(const void *src, uint32_t sz)
    {
        off[count] = bodySize();
        size[count] = sz;
        bytes.resize(bytes.size() + sz);
        std::memcpy(bytes.data() + kHdr + off[count], src, sz);
        ++count;
    }
    // The record (ge1_gs_api.h layout) in place: returns its start inside bytes.
    const uint8_t *record(uint32_t &recSize)
    {
        const uint32_t table = (8u + 4u * count + 15u) & ~15u;
        uint8_t *const start = bytes.data() + kHdr - (16u + table);
        const uint64_t magic[2] = {GE1_NATIVE_RECORD_MAGIC_LO, GE1_NATIVE_RECORD_MAGIC_HI};
        std::memcpy(start, magic, 16);
        std::memset(start + 16, 0, table);
        std::memcpy(start + 16, &count, 4);
        std::memcpy(start + 24, size, 4u * count);
        recSize = 16u + table + bodySize();
        return start;
    }
    // NRS1: the compact record into cbytes (reused across jobs, like bytes):
    // same packets, dense vertices. Same hook, same VU1 writes; only the
    // transport bytes differ.
    std::vector<uint8_t> cbytes;
    const uint8_t *recordCompact(uint32_t &recSize)
    {
        uint32_t dense[4];
        for (uint32_t i = 0; i < count; ++i)
            dense[i] = 16u + 32u * ((size[i] - 16u) / 48u);
        const uint32_t table = (8u + 4u * count + 15u) & ~15u;
        size_t body = 0;
        for (uint32_t i = 0; i < count; ++i)
            body += dense[i];
        cbytes.clear();
        cbytes.resize(16u + table + body);
        const uint64_t magic[2] = {GE1_COMPACT_RECORD_MAGIC_LO, GE1_COMPACT_RECORD_MAGIC_HI};
        std::memcpy(cbytes.data(), magic, 16);
        std::memset(cbytes.data() + 16, 0, table);
        std::memcpy(cbytes.data() + 16, &count, 4);
        std::memcpy(cbytes.data() + 24, dense, 4u * count);
        size_t at = 16u + table;
        for (uint32_t i = 0; i < count; ++i)
        {
            compactPacket(bodyBytes(off[i]), size[i], cbytes.data() + at);
            at += dense[i];
        }
        recSize = static_cast<uint32_t>(cbytes.size());
        return cbytes.data();
    }
    const uint8_t *bodyBytes(uint32_t o) const { return body() + o; }
    // RZV1 S4a: the keyed record into kbytes: header, job, per-packet facts,
    // triangle lists, then the compact record verbatim (built first). The
    // facts' float work runs under IEEE round-to-nearest without flush-to-zero
    // (GE1's own threads), whatever mode the MTVU thread is in.
    std::vector<uint8_t> kbytes;
    const uint8_t *recordKeyed(const Ge1KeyedJob &jobIn, uint32_t &recSize)
    {
        uint32_t csize = 0;
        const uint8_t *crec = recordCompact(csize);
        Ge1KeyedPass pass[4];
        uint8_t tris[4][3 * 64];
        const uint64_t saved = ps2_fpmode::readControl();
        ps2_fpmode::writeControl(saved & ~((uint64_t{3} << 22) | (uint64_t{1} << 24)));
        const uint32_t table = (8u + 4u * count + 15u) & ~15u;
        uint32_t at = 16u + table, triBytes = 0;
        for (uint32_t i = 0; i < count; ++i)
        {
            uint32_t n = 0;
            std::memcpy(&n, crec + 24 + 4 * i, 4);
            ge1_keyed_pass_facts(crec + at, n, pass[i], tris[i]);
            triBytes += 3u * pass[i].ntris;
            at += n;
        }
        ps2_fpmode::writeControl(saved);
        const uint32_t triPadded = (triBytes + 15u) & ~15u;
        const uint32_t total = 32u + 256u + 128u * count + triPadded + csize;
        kbytes.assign(total, 0);
        const uint64_t magic[2] = {GE1_KEYED_RECORD_MAGIC_LO, GE1_KEYED_RECORD_MAGIC_HI};
        const uint32_t hdr[4] = {total, count, triPadded, 0};
        std::memcpy(kbytes.data(), magic, 16);
        std::memcpy(kbytes.data() + 16, hdr, 16);
        Ge1KeyedJob job = jobIn;
        job.passes = count;
        std::memcpy(kbytes.data() + 32, &job, 256);
        std::memcpy(kbytes.data() + 288, pass, 128u * count);
        uint8_t *t = kbytes.data() + 288 + 128u * count;
        for (uint32_t i = 0; i < count; ++i)
        {
            std::memcpy(t, tris[i], 3u * pass[i].ntris);
            t += 3u * pass[i].ntris;
        }
        std::memcpy(kbytes.data() + 288 + 128u * count + triPadded, crec, csize);
        recSize = total;
        return kbytes.data();
    }
};

// Runs one terrain job of the routed set into g in place, and kicks. Returns
// false when the job must run on VU1 (then g's packets are spent: the caller
// marks the group VU1-run, or restores a backup for a clip re-run follow-up).
// noinline: the FPCR write in runModel() must not be reordered with its math.
__attribute__((noinline)) bool model(const uint8_t *mem, const Entry &e, uint32_t top, Group &g, Kicks &kicks)
{
    kicks.clear();
    uint32_t k, n, posTop;
    if (e.k >= 0)
    {
        k = static_cast<uint32_t>(e.k);
        n = 4 + k;
        posTop = top;
    }
    else
    {
        if (!g.live || !g.native)
            return false;
        k = g.k;
        n = g.n;
        posTop = g.baseTop;
    }
    const uint32_t nsA = ilw(mem, 11 + k, 0), nsB = ilw(mem, 11 + k, 1), offB = ilw(mem, 11 + k, 3);
    const uint32_t nlA = ilw(mem, 12 + k, 0), nlB = ilw(mem, 12 + k, 1);
    const uint32_t cntA = ilw(mem, 12 + k, 2), cntB = ilw(mem, 12 + k, 3);
    // Shapes the strip loop and the 4-vertex ST rewrite assume; anything else runs on VU1.
    if (nsA * 2 * n != nlA || nsB * 2 * n != nlB || nlA > kMaxVerts || nlB > kMaxVerts || offB % n ||
        offB + (nsB + 1) * n > n * n || (nsA + 1) * n > n * n)
        return false;
    if (e.texgen && ((nsA + 1) * n > nlA || offB + (nsB + 1) * n > nlA))
        return false; // the texgen loop writes nlA entries (0x1b98)
    if (e.pass2 == 1 && (cntA != 3 * nlA || cntB != 3 * nlB || nlA % 4 || nlB % 4))
        return false;
    TerrainConst c;
    loadConst(mem, c);
    bool ok = true;
    if (e.base)
    {
        // 0x9b0: tags from mem[7], strips with UV set A, kick A, kick B.
        setTag(g.pk[0], nlA, mem, 7);
        setTag(g.pk[1], nlB, mem, 7);
        ok &= strips(c, mem, top, UvSrc{mem, top + 0x40}, n, nsA, true, g.pk[0]);
        kicks.kick(g.pk[0]);
        ok &= strips(c, mem, top + offB, UvSrc{mem, top + 0x40 + offB}, n, nsB, true, g.pk[1]);
        kicks.kick(g.pk[1]);
        g.clipTagQw = 9;
    }
    // UV set B as the pass-2 code reads it: VIF-uploaded, or the texgen output.
    F4 uvb[kMaxVerts];
    UvSrc uvA{mem, top + 0x80}, uvBsrc{mem, top + 0x80 + offB};
    if (e.texgen)
    {
        F4 m2[4];
        for (int i = 0; i < 4; ++i)
            m2[i] = ld4(mem, top + 128 + i);
        const uint32_t src = e.texgen == 1 ? posTop : top + 0x40;
        const float one = opaque(1.0f);
        for (uint32_t i = 0; i < n * n; ++i)
        {
            // MULAx/MADDAy/MADDAz/MADDw.xy; z/w = 1.0 (MAXw.zw)
            const F4 a = mat4(m2, ld4(mem, src + i));
            uvb[i] = vsetq_lane_f32(one, vsetq_lane_f32(one, a, 2), 3);
        }
        uvA = UvSrc{nullptr, 0, uvb};
        uvBsrc = UvSrc{nullptr, 0, uvb + offB};
    }
    if (e.pass2 == 1)
    {
        // 0xc30: tags y/z/w from mem[8], ST rewrite, re-kick A then B.
        setTagYzw(g.pk[0], mem, 8);
        setTagYzw(g.pk[1], mem, 8);
        rewriteSt(g.pk[0], nlA, n, uvA);
        kicks.kick(g.pk[0]);
        rewriteSt(g.pk[1], nlB, n, uvBsrc);
        kicks.kick(g.pk[1]);
    }
    else if (e.pass2 == 2)
    {
        // 0xd40: tags y/z/w from mem[8], strips from the base positions with UV set B.
        setTagYzw(g.pk[0], mem, 8);
        setTagYzw(g.pk[1], mem, 8);
        ok &= strips(c, mem, posTop, uvA, n, nsA, false, g.pk[0]);
        kicks.kick(g.pk[0]);
        ok &= strips(c, mem, posTop + offB, uvBsrc, n, nsB, false, g.pk[1]);
        kicks.kick(g.pk[1]);
        g.clipTagQw = 10;
    }
    if (!ok)
        return false;
    if (e.base)
    {
        g.k = k;
        g.n = n;
        g.baseTop = top;
    }
    return true;
}

// Writes what the group's base job (and follow-ups so far) would have left in
// VU1 memory, so a follow-up can run on VU1 exactly (the unexpected case of a
// follow-up needing the clipper after a natively served base).
void materialize(uint8_t *mem, const Group &g)
{
    auto sw = [&](uint32_t qw, int lane, uint32_t v) { std::memcpy(mem + ((qw & 0x3ffu) << 4) + 4 * lane, &v, 4); };
    sw(39, 0, g.n);
    sw(39, 1, g.k);
    sw(39, 2, g.n * g.n);
    sw(510, 0, g.baseTop);
    sw(41, 0, 0x1e0);
    for (int lane = 1; lane < 4; ++lane)
    {
        sw(452, lane, ldw(mem, g.clipTagQw, lane));
        sw(480, lane, ldw(mem, g.clipTagQw, lane));
    }
    for (int h = 0; h < 2; ++h)
    {
        const uint32_t base = h ? kPktB : kPktA;
        for (uint32_t i = 0; i < 1 + 3 * g.pk[h].nv; ++i)
            std::memcpy(mem + (((base + i) & 0x3ffu) << 4), g.pk[h].q[i], 16);
    }
}

// ---------------------------------------------------------------- scenery instances
// NRV1's model of the scenery-instance program (image 2826c443; REPORT §3,
// tooling scenery.h / drive_scenery.inc), from our own disassembly. Header
// jobs (0x2270 ITOP=0 / 0x2320) compose the instance matrix and draw the first
// chunk; continuations (0x22c8 ITOP=0 / 0x2358) reuse the header's state; an
// ITOP!=0 job on 0x2270/0x22c8 is a second texture pass over the previous
// pass's packet (planar texgen, or the UV region as it stands). Lit instances,
// UV formats other than 0/2, a second pass with clipping and the effect entries
// (0x3a08/0x12b8/0x3bf0/0x2400) run on VU1.
constexpr uint64_t kSceneryImage = 0x2826C44313604279ull;
constexpr uint32_t kMaxSceneVerts = 128;
struct ScenePkt
{
    uint32_t q[1 + 3 * kMaxSceneVerts][4];
    uint32_t nv = 0;
};
// The last pass's output at one TOP buffer (VIF double-buffers TOP), as VU1
// memory would hold it: the packet at TOP+0xdb and, for UV format 2 or a
// texgen pass, the decoded UV region.
struct SceneSlot
{
    bool valid = false;
    uint32_t top = 0;
    ScenePkt pkt;
    bool decoded = false;
    uint32_t uvQw = 0, uvN = 0;
    uint32_t uv[kMaxSceneVerts][4];
};
struct SceneGroup
{
    bool live = false, native = false;
    F4 comp[4];         // mem[6..9]: V x model rows
    F4 m[4];            // mem[10..13]: UV / texgen matrix rows
    uint32_t flags = 0; // mem[34].w
    SceneSlot slot[2];
    SceneSlot *find(uint32_t top)
    {
        for (SceneSlot &sl : slot)
            if (sl.valid && sl.top == top)
                return &sl;
        return nullptr;
    }
    // The slot a pass at `top` writes: its own, else a free one, else the other buffer's.
    SceneSlot &claim(uint32_t top)
    {
        if (SceneSlot *sl = find(top))
            return *sl;
        SceneSlot &sl = !slot[0].valid ? slot[0] : !slot[1].valid ? slot[1] : (slot[0].top == top ? slot[0] : slot[1]);
        sl.valid = false;
        sl.top = top;
        sl.decoded = false;
        return sl;
    }
    void forget(uint32_t top)
    {
        if (SceneSlot *sl = find(top))
            sl->valid = false;
    }
    void forgetAll() { slot[0].valid = slot[1].valid = false; }
};
inline bool sceneEligible(uint32_t fl)
{
    const uint32_t fmt = fl & 0x1e, pass2 = fl & 0x3c0;
    return !(fl & 1) && (fmt == 0 || fmt == 2) && (pass2 == 0 || pass2 == 0x100) && !(pass2 && (fl & 0x20));
}
// ITOFn of lanes [0, lanes) (SCVTF then x 2^-n, exact for 16-bit ints); other lanes keep their raw bits.
inline F4 itofLanes(const uint32_t *q, float scale, int lanes)
{
    const F4 conv = vmulq_n_f32(vcvtq_f32_s32(vld1q_s32(reinterpret_cast<const int32_t *>(q))), scale);
    F4 raw = vld1q_f32(reinterpret_cast<const float *>(q));
    raw = vsetq_lane_f32(vgetq_lane_f32(conv, 0), raw, 0);
    if (lanes > 1)
        raw = vsetq_lane_f32(vgetq_lane_f32(conv, 1), raw, 1);
    if (lanes > 2)
        raw = vsetq_lane_f32(vgetq_lane_f32(conv, 2), raw, 2);
    return raw;
}
inline const uint32_t *qwp(const uint8_t *mem, uint32_t qw)
{
    return reinterpret_cast<const uint32_t *>(mem + ((qw & 0x3ffu) << 4));
}
// FTOI12 of lanes x/y; z = w = 1.0.
inline void ftoi12xy(F4 r, uint32_t out[4])
{
    const uint32x4_t i = vreinterpretq_u32_s32(vcvtq_s32_f32(vmulq_n_f32(r, 4096.0f)));
    out[0] = vgetq_lane_u32(i, 0);
    out[1] = vgetq_lane_u32(i, 1);
    out[2] = out[3] = 0x3f800000u;
}
// 0x1f40 -> 0xbe8, format 2: uv' = FTOI12(m10.xy u + m11.xy v + (m12 + m13).xy), in place.
void sceneUvDecode(const uint8_t *mem, uint32_t chunk, const F4 m[4], SceneSlot &g)
{
    const uint32_t ofs = ilw(mem, chunk + 1, 0), n = ilw(mem, chunk + 1, 2);
    const uint32_t dst = chunk + 2 + ofs + n;
    const float one = opaque(1.0f);
    const F4 o = vadd4(m[2], m[3]);
    for (uint32_t i = 0; i < n; ++i)
    {
        const F4 uv = itofLanes(qwp(mem, dst + i), 1.0f / 4096.0f, 2);
        F4 a = vmul4(m[0], vdupq_laneq_f32(uv, 0));
        a = vmadd4(a, m[1], vdupq_laneq_f32(uv, 1));
        a = vmadd4(a, o, vdupq_n_f32(one));
        ftoi12xy(a, g.uv[i]);
    }
    g.decoded = true;
    g.uvQw = dst;
    g.uvN = n;
}
// The chunk's vertex loop (0x2908 unlit no-clip, 0x2d28 unlit guard-band clip).
// Returns false when a triangle would reach the clipper (0x3198) or the shape is unsupported.
bool sceneVerts(const uint8_t *mem, const SceneGroup &g, const SceneSlot &sl, uint32_t chunk, bool clip, ScenePkt &pk)
{
    const uint32_t ofs = ilw(mem, chunk + 1, 0), n = ilw(mem, chunk + 1, 2);
    if (n > kMaxSceneVerts)
        return false;
    const uint32_t table = chunk + 2, pos = table + ofs, uvq = pos + n, col = pos + 2 * n;
    const float one = opaque(1.0f);
    const F4 scale = ld4(mem, 4);
    const F4 offset = vsetq_lane_f32(one, ld4(mem, 5), 3); // MOVE.w vf6, vf0 (0x2648)
    F4 m[4];
    for (int i = 0; i < 4; ++i)
        m[i] = clip ? g.comp[i] : vmadd4(vmul4(scale, g.comp[i]), offset, vdupq_laneq_f32(g.comp[i], 3));
    const F4 acc = vmul4(offset, vdupq_n_f32(one));
    std::memcpy(pk.q[0], qwp(mem, chunk), 16); // the chunk's GIF tag
    bool ok = true;
    uint32_t vi = 0;
    for (uint32_t t = table; t < pos; ++t)
    {
        const uint32_t cnt = ilw(mem, t, 0);
        uint32_t hist = 0;
        for (uint32_t k = 0; k < cnt; ++k, ++vi)
        {
            if (vi >= n)
                return false;
            F4 p = itofLanes(qwp(mem, pos + vi), 1.0f / 32768.0f, 3);
            if (!clip)
                p = vsetq_lane_f32(one, p, 3); // MADDw ..., vf0.w
            const F4 c = mat4(m, p);
            const float q = vdiv(one, vgetq_lane_f32(c, 3));
            const F4 qv = vdupq_n_f32(q);
            const uint32_t *uvraw = (sl.decoded && sl.uvQw == uvq) ? sl.uv[vi] : qwp(mem, uvq + vi);
            F4 u = itofLanes(uvraw, 1.0f / 4096.0f, 2);
            if (!clip)
                u = vsetq_lane_f32(one, vsetq_lane_f32(one, u, 2), 3); // MAXw.zw vf7, vf0, vf0.w
            vst1q_f32(reinterpret_cast<float *>(pk.q[1 + 3 * vi]), vmul4(u, qv));
            std::memcpy(pk.q[2 + 3 * vi], qwp(mem, col + vi), 16);
            const F4 t4 = vmul4(c, qv);
            uint32_t *qx = pk.q[3 + 3 * vi];
            if (!clip)
            {
                vst1q_u32(qx, vftoi4(t4));
                if (k < 2)
                    qx[3] = 0x0000ffffu; // ISW.w of -1
            }
            else
            {
                vst1q_u32(qx, vftoi4(vmadd4(acc, t4, scale)));
                if (k < 2)
                    qx[3] = 0xffffffffu; // MFIR.w of -1
                hist = ((hist << 6) | vclip(c)) & 0xffffffu;
                if (k >= 2 && (hist & 0x3ffffu))
                {
                    qx[3] = 0xffffffffu;
                    if (!trivialReject(hist))
                        ok = false;
                }
            }
        }
    }
    pk.nv = vi;
    // The kick is the tag's own size: one PACKED tag of ST/RGBAQ/XYZF2, EOP, one loop per vertex.
    const uint32_t lo = pk.q[0][0], nreg = pk.q[0][1] >> 28, flg = (pk.q[0][1] >> 26) & 3u;
    if ((lo & 0x7fffu) != vi || !(lo & 0x8000u) || nreg != 3 || flg != 0)
        return false;
    return ok;
}
// 0x3360 (+ 0x2010 -> 0xce8 planar texgen for format 0x100): ST' = (u, v, 1, 1) x ST.w in place
// in the previous pass's packet; the tag is the chunk's with y from mem[31].y.
bool sceneStPass(const uint8_t *mem, uint32_t chunk, const SceneGroup &grp, SceneSlot &g)
{
    const uint32_t ofs = ilw(mem, chunk + 1, 0), n = ilw(mem, chunk + 1, 2);
    if (n > kMaxSceneVerts || n != g.pkt.nv)
        return false;
    const uint32_t base = chunk + 2 + ofs, uvq = base + n;
    const float one = opaque(1.0f);
    uint32_t gen[kMaxSceneVerts][4];
    const bool texgen = (grp.flags & 0x3c0) == 0x100;
    if (texgen)
        for (uint32_t i = 0; i < n; ++i)
        {
            const F4 p = itofLanes(qwp(mem, base + 2 * n + i), 1.0f / 32768.0f, 3);
            F4 a = vmul4(grp.m[0], vdupq_laneq_f32(p, 0));
            a = vmadd4(a, grp.m[1], vdupq_laneq_f32(p, 1));
            a = vmadd4(a, grp.m[2], vdupq_laneq_f32(p, 2));
            a = vmadd4(a, grp.m[3], vdupq_n_f32(one));
            ftoi12xy(a, gen[i]);
        }
    for (uint32_t k = 0; k < n; ++k)
    {
        const uint32_t *uvraw = texgen ? gen[k] : (g.decoded && g.uvQw == uvq) ? g.uv[k] : qwp(mem, uvq + k);
        F4 u = itofLanes(uvraw, 1.0f / 4096.0f, 2);
        u = vsetq_lane_f32(one, vsetq_lane_f32(one, u, 2), 3);
        uint32_t *qs = g.pkt.q[1 + 3 * k];
        vst1q_f32(reinterpret_cast<float *>(qs), vmul4(u, vdupq_n_f32(ffrom(qs[3]))));
    }
    std::memcpy(g.pkt.q[0], qwp(mem, chunk), 16);
    g.pkt.q[0][1] = ldw(mem, 31, 1); // LQ.y vf31, 31(vi0)
    if (texgen)
    {
        // VU1 memory holds the texgen output in the UV region from here on.
        std::memcpy(g.uv, gen, sizeof(uint32_t) * 4 * n);
        g.decoded = true;
        g.uvQw = uvq;
        g.uvN = n;
    }
    return true;
}
// What a natively served header leaves in VU1 memory that other jobs read:
// written eagerly, so a VU1-run continuation or effect job sees stock values.
void sceneWriteConsts(uint8_t *mem, const SceneGroup &g)
{
    for (int i = 0; i < 4; ++i)
    {
        vst1q_f32(reinterpret_cast<float *>(mem + (6 + i) * 16), g.comp[i]);
        vst1q_f32(reinterpret_cast<float *>(mem + (10 + i) * 16), g.m[i]);
    }
    std::memcpy(mem + 34 * 16 + 12, &g.flags, 4); // ISW.w vi4, 34(vi0)
}
// Before a VU1-run job reads a native pass's output at its TOP buffer.
void sceneMaterialize(uint8_t *mem, const SceneSlot &g)
{
    if (!g.valid)
        return;
    for (uint32_t i = 0; i < 1 + 3 * g.pkt.nv; ++i)
        std::memcpy(mem + (((g.top + 0xdb + i) & 0x3ffu) << 4), g.pkt.q[i], 16);
    if (g.decoded)
        for (uint32_t i = 0; i < g.uvN; ++i)
            std::memcpy(mem + (((g.uvQw + i) & 0x3ffu) << 4), g.uv[i], 16);
}

// ---------------------------------------------------------------- stats
struct EntryStats
{
    std::atomic<uint64_t> routed{0}, fallback{0}, orphan{0}, match{0}, mismatch{0}, fbConfirmed{0}, fbSpurious{0};
};
EntryStats g_stats[kNumEntries];
EntryStats g_sstats[3]; // scenery: header, continuation, second pass
constexpr const char *kSceneKinds[3] = {"hdr", "cont", "pass2"};
std::atomic<uint64_t> g_sceneUnmodelled{0};
std::atomic<uint64_t> g_resumeAfterNative{0}, g_unmodelledTerrain{0}, g_checkSkipped{0};
// RZV1 S4a: the resident-pool simulation (keys at MSCAL, window eviction).
std::atomic<uint64_t> g_keyJobs{0}, g_keyHits{0}, g_keyCollide{0}, g_keyBytesAll{0}, g_keyBytesMiss{0}, g_keyDistinct{0},
    g_keyEvicted{0}, g_keyRecBytes{0}, g_keyCompactBytes{0};
std::atomic<int> g_mismatchDumps{8};

void printStats()
{
    char line[2048];
    int len = std::snprintf(line, sizeof(line), "[nrt1] terrain%s:", knobs().check ? " check" : "");
    for (int i = 0; i < kNumEntries; ++i)
    {
        const EntryStats &s = g_stats[i];
        const uint64_t any = s.routed + s.fallback + s.orphan + s.match + s.mismatch + s.fbConfirmed + s.fbSpurious;
        if (!any)
            continue;
        if (knobs().check)
            len += std::snprintf(line + len, sizeof(line) - len, " %x=m%llu/x%llu/fb%llu/fs%llu", kEntries[i].pc,
                                 (unsigned long long)s.match.load(), (unsigned long long)s.mismatch.load(),
                                 (unsigned long long)s.fbConfirmed.load(), (unsigned long long)s.fbSpurious.load());
        else
            len += std::snprintf(line + len, sizeof(line) - len, " %x=r%llu/fb%llu/o%llu", kEntries[i].pc,
                                 (unsigned long long)s.routed.load(), (unsigned long long)s.fallback.load(),
                                 (unsigned long long)s.orphan.load());
        if (len >= static_cast<int>(sizeof(line)) - 64)
            break;
    }
    std::fprintf(stderr, "%s unmodelled=%llu resume_after_native=%llu check_skipped=%llu\n", line,
                 (unsigned long long)g_unmodelledTerrain.load(), (unsigned long long)g_resumeAfterNative.load(),
                 (unsigned long long)g_checkSkipped.load());
    if (!knobs().instances && !knobs().check)
        return;
    len = std::snprintf(line, sizeof(line), "[nrt1] instances%s:", knobs().check ? " check" : "");
    for (int i = 0; i < 3; ++i)
    {
        const EntryStats &s = g_sstats[i];
        if (knobs().check)
            len += std::snprintf(line + len, sizeof(line) - len, " %s=m%llu/x%llu/fb%llu/fs%llu", kSceneKinds[i],
                                 (unsigned long long)s.match.load(), (unsigned long long)s.mismatch.load(),
                                 (unsigned long long)s.fbConfirmed.load(), (unsigned long long)s.fbSpurious.load());
        else
            len += std::snprintf(line + len, sizeof(line) - len, " %s=r%llu/fb%llu/o%llu", kSceneKinds[i],
                                 (unsigned long long)s.routed.load(), (unsigned long long)s.fallback.load(),
                                 (unsigned long long)s.orphan.load());
    }
    std::fprintf(stderr, "%s vu1_by_flags=%llu\n", line, (unsigned long long)g_sceneUnmodelled.load());
    if (knobs().keyed)
    {
        const uint64_t jobs = g_keyJobs.load(), hits = g_keyHits.load();
        std::fprintf(stderr,
                     "[nrt1] s4a keyed: jobs=%llu hits=%llu (%.2f%%, window %u ticks) distinct=%llu evicted=%llu "
                     "collisions=%llu input_bytes=%llu miss_bytes=%llu record_bytes=%llu compact_bytes=%llu\n",
                     (unsigned long long)jobs, (unsigned long long)hits, jobs ? 100.0 * double(hits) / double(jobs) : 0.0,
                     knobs().keyWindow, (unsigned long long)g_keyDistinct.load(), (unsigned long long)g_keyEvicted.load(),
                     (unsigned long long)g_keyCollide.load(), (unsigned long long)g_keyBytesAll.load(),
                     (unsigned long long)g_keyBytesMiss.load(), (unsigned long long)g_keyRecBytes.load(),
                     (unsigned long long)g_keyCompactBytes.load());
    }
}

// Boots end on SIGTERM (no atexit), so the counts also print every 65,536 terrain events.
std::atomic<uint64_t> g_events{0};
void countEvent()
{
    if ((g_events.fetch_add(1, std::memory_order_relaxed) & 0xffffu) == 0xffffu)
        printStats();
}

void atExit()
{
    printStats();
}
void registerExit()
{
    static std::once_flag once;
    std::call_once(once, [] { std::atexit(atExit); });
}

// ---------------------------------------------------------------- per-job state (MTVU thread)
struct State
{
    uint64_t codeGen = ~0ull, imageHash = 0;
    Group group;
    Group work;
    Kicks kicks;
    bool inTerrainRun = false; // a natively served terrain job since the last non-terrain job
    bool lastNative = false;
    // check mode: the pending comparison for the VU1 run in progress
    bool checkPending = false;
    bool checkServable = false;
    EntryStats *checkStats = nullptr;
    uint32_t checkPc = 0;
    std::vector<uint8_t> capBytes;
    std::vector<uint32_t> capSize;
    bool firstLogged = false;
    SceneGroup scene;
    bool sceneFirstLogged = false;
    // RZV1 S4a: the job being emitted (filled before emitKicks when keyed) and the pool simulation.
    Ge1KeyedJob job{};
    std::vector<uint8_t> keyIn;
    struct PoolEntry
    {
        uint64_t check, lastTick;
    };
    std::unordered_map<uint64_t, PoolEntry> pool;
    uint64_t poolTick = 0;
};
State &state()
{
    static State s;
    return s;
}
std::atomic<bool> g_inTerrainRun{false};
std::atomic<bool> g_inSceneRun{false}; // a natively served instance pass whose follow-ups need host state

uint64_t imageHash(PS2Memory &memory, State &s)
{
    const uint64_t g = memory.getVU1CodeGeneration();
    if (g != s.codeGen)
    {
        s.codeGen = g;
        s.imageHash = ps2x::telemetry::hashBytes(memory.getVU1Code(), PS2_VU1_CODE_SIZE);
    }
    return s.imageHash;
}

// The model under microVU's FP mode, FPCR restored after.
bool runModel(const uint8_t *mem, const Entry &e, uint32_t top, Group &g, Kicks &kicks)
{
    const uint64_t saved = ps2_fpmode::readControl();
    ps2_fpmode::writeControl(ps2_fpmode::ps2Control(saved));
    const bool ok = model(mem, e, top, g, kicks);
    ps2_fpmode::writeControl(saved);
    return ok;
}
} // namespace

void buildRecord(const uint8_t *packets, const uint32_t *sizes, uint32_t count, std::vector<uint8_t> &out)
{
    const uint32_t table = (8u + 4u * count + 15u) & ~15u;
    size_t body = 0;
    for (uint32_t i = 0; i < count; ++i)
        body += sizes[i];
    out.resize(16u + table + body);
    const uint64_t magic[2] = {GE1_NATIVE_RECORD_MAGIC_LO, GE1_NATIVE_RECORD_MAGIC_HI};
    std::memcpy(out.data(), magic, 16);
    std::memset(out.data() + 16, 0, table);
    std::memcpy(out.data() + 16, &count, 4);
    std::memcpy(out.data() + 24, sizes, 4u * count);
    std::memcpy(out.data() + 16 + table, packets, body);
}

// NRS1: the suite's compact-record builder: same packets as buildRecord's,
// dense (compactPacket each). GIF-sized `sizes` in, dense sizes out.
void buildCompactRecord(const uint8_t *packets, const uint32_t *sizes, uint32_t count,
                        std::vector<uint8_t> &out)
{
    uint32_t dense[64];
    for (uint32_t i = 0; i < count; ++i)
        dense[i] = 16u + 32u * ((sizes[i] - 16u) / 48u);
    const uint32_t table = (8u + 4u * count + 15u) & ~15u;
    size_t body = 0;
    for (uint32_t i = 0; i < count; ++i)
        body += dense[i];
    out.resize(16u + table + body);
    const uint64_t magic[2] = {GE1_COMPACT_RECORD_MAGIC_LO, GE1_COMPACT_RECORD_MAGIC_HI};
    std::memcpy(out.data(), magic, 16);
    std::memset(out.data() + 16, 0, table);
    std::memcpy(out.data() + 16, &count, 4);
    std::memcpy(out.data() + 24, dense, 4u * count);
    size_t off = 0, at = 16u + table;
    for (uint32_t i = 0; i < count; ++i)
    {
        compactPacket(packets + off, sizes[i], out.data() + at);
        off += sizes[i];
        at += dense[i];
    }
}

void buildKeyedRecord(const uint8_t *packets, const uint32_t *sizes, uint32_t count, const Ge1KeyedJob &job,
                      std::vector<uint8_t> &out)
{
    Kicks k;
    k.clear();
    size_t off = 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        k.kickBytes(packets + off, sizes[i]);
        off += sizes[i];
    }
    uint32_t size = 0;
    const uint8_t *rec = k.recordKeyed(job, size);
    out.assign(rec, rec + size);
}

bool active()
{
    static const bool a = knobs().terrain || knobs().instances || knobs().check;
    return a;
}

namespace
{
// The job's kicks to the GIF arbiter as one native record (ge1_gs_api.h
// layout) in one PATH1 submission. Consecutive PATH1 packets of one job drain
// together today, so one arbiter entry keeps their place against PATH2/PATH3.
// RZV1 S4a: key the job's input bytes (gathered in keyIn by the caller) and
// run the pool simulation: a hit is a key seen within keyWindow ticks.
void keyJob(PS2Memory &memory, State &s)
{
    Ge1KeyedJob &j = s.job;
    j.inputBytes = static_cast<uint32_t>(s.keyIn.size());
    j.key = XXH3_64bits(s.keyIn.data(), s.keyIn.size());
    j.check = XXH64(s.keyIn.data(), s.keyIn.size(), 0x5334);
    const uint64_t tick = memory.gs().vsyncTick.load(std::memory_order_relaxed);
    if (tick != s.poolTick)
    {
        s.poolTick = tick;
        for (auto it = s.pool.begin(); it != s.pool.end();)
        {
            if (tick - it->second.lastTick > knobs().keyWindow)
            {
                it = s.pool.erase(it);
                g_keyEvicted.fetch_add(1, std::memory_order_relaxed);
            }
            else
                ++it;
        }
    }
    g_keyJobs.fetch_add(1, std::memory_order_relaxed);
    g_keyBytesAll.fetch_add(j.inputBytes, std::memory_order_relaxed);
    auto it = s.pool.find(j.key);
    if (it == s.pool.end())
    {
        s.pool.emplace(j.key, State::PoolEntry{j.check, tick});
        g_keyDistinct.fetch_add(1, std::memory_order_relaxed);
        g_keyBytesMiss.fetch_add(j.inputBytes, std::memory_order_relaxed);
        return;
    }
    if (it->second.check != j.check)
        g_keyCollide.fetch_add(1, std::memory_order_relaxed);
    g_keyHits.fetch_add(1, std::memory_order_relaxed);
    it->second.lastTick = tick;
}
// Appends nqw qwords of VU1 data memory at qw (wrapping like the model's loads).
void keyAppend(State &s, const uint8_t *mem, uint32_t qw, uint32_t nqw)
{
    for (uint32_t i = 0; i < nqw; ++i)
    {
        const uint8_t *q = mem + (((qw + i) & 0x3ffu) << 4);
        s.keyIn.insert(s.keyIn.end(), q, q + 16);
    }
}
void keyHeader(State &s, uint32_t a, uint32_t b)
{
    const uint32_t h[4] = {a, b, 0, 0};
    const uint8_t *p = reinterpret_cast<const uint8_t *>(h);
    s.keyIn.assign(p, p + 16);
}

bool emitKicks(PS2Memory &memory, State &s)
{
    uint32_t recSize = 0;
    // NRS1: the compact record holds the same packets dense; same hook, same
    // VU1-memory writes, so the det hashes (vu1Data included) are unchanged.
    // RZV1 S4a: keyed wraps the same compact record (GE1 ingests it verbatim).
    const uint8_t *rec;
    if (knobs().keyed)
    {
        keyJob(memory, s);
        rec = s.kicks.recordKeyed(s.job, recSize);
        g_keyRecBytes.fetch_add(recSize, std::memory_order_relaxed);
        g_keyCompactBytes.fetch_add(static_cast<uint32_t>(s.kicks.cbytes.size()), std::memory_order_relaxed);
    }
    else
        rec = knobs().compact ? s.kicks.recordCompact(recSize) : s.kicks.record(recSize);
    memory.submitGifPacket(GifPathId::Path1, rec, recSize);
    return true;
}

bool terrainBefore(PS2Memory &memory, State &s, uint32_t startPC, uint32_t top)
{
    if (!knobs().terrain && !knobs().check)
        return false;
    const int ei = entryIndex(startPC);
    if (ei < 0)
    {
        // An unmodelled terrain entry (mixed patches, base-only): VU1, and any
        // later follow-up reads VU1 memory as usual.
        g_unmodelledTerrain.fetch_add(1, std::memory_order_relaxed);
        s.group.live = true;
        s.group.native = false;
        return false;
    }
    const Entry &e = kEntries[ei];
    EntryStats &st = g_stats[ei];
    const uint8_t *mem = memory.getVU1Data();
    if (!e.base && !(s.group.live && s.group.native))
        return false; // follow-up of a VU1-run base: VU1 memory holds its state
    // The model works on the group in place. Only a clip re-run follow-up can
    // fail after touching the base's packets, and its fallback needs them.
    const bool needBackup = !e.base && e.pass2 == 2 && !knobs().check;
    if (needBackup)
        s.work = s.group;
    const bool servable = runModel(mem, e, top, s.group, s.kicks);

    if (knobs().check)
    {
        // Route nothing; compare against the VU1 run in afterVu1().
        s.checkPending = true;
        s.checkServable = servable;
        s.checkStats = &st;
        s.checkPc = startPC;
        s.capBytes.clear();
        s.capSize.clear();
        if (servable)
        {
            s.group.live = true;
            s.group.native = true; // the model follows the group as if it had served it
        }
        else if (e.base)
        {
            s.group.live = true;
            s.group.native = false;
        }
        else
            s.group.native = false; // the model can't follow this group any further
        return false;
    }

    if (!servable)
    {
        if (e.base)
        {
            st.fallback.fetch_add(1, std::memory_order_relaxed);
            countEvent();
            s.group.live = true;
            s.group.native = false;
            return false;
        }
        // A follow-up of a natively served base that the model can't serve:
        // put the base's VU1 memory state in place and let VU1 run it.
        st.orphan.fetch_add(1, std::memory_order_relaxed);
        countEvent();
        materialize(memory.getVU1Data(), s.work);
        s.group.native = false;
        return false;
    }

    s.group.live = true;
    s.group.native = true;
    st.routed.fetch_add(1, std::memory_order_relaxed);
    countEvent();
    s.inTerrainRun = true;
    s.lastNative = true;
    g_inTerrainRun.store(true, std::memory_order_relaxed);
    if (!s.firstLogged)
    {
        s.firstLogged = true;
        std::fprintf(stderr, "[nrt1] first native terrain job pc=0x%x top=%u kicks=%u\n", startPC, top,
                     s.kicks.count);
    }
    if (knobs().keyed)
    {
        // RZV1 S4a: the job's geometry inputs (base positions, UV A, UV B) and constants.
        const uint32_t n = s.group.n, nn = n * n;
        keyHeader(s, 1u, startPC);
        keyAppend(s, mem, s.group.baseTop, nn);
        keyAppend(s, mem, top + 0x40, nn);
        // Texgen follow-ups (0x8d8/0x910/0x948/0x980) generate UV B from M2 (at
        // TOP+0x80, a per-frame constant carried in uvm), so UV B isn't an input.
        if (!e.texgen)
            keyAppend(s, mem, top + 0x80, nn);
        Ge1KeyedJob &j = s.job;
        j = Ge1KeyedJob{};
        j.kind = 1;
        j.pc = startPC;
        j.n = n;
        std::memcpy(j.m, mem, 64);
        std::memcpy(j.scale, mem + 4 * 16, 16);
        std::memcpy(j.offset, mem + 5 * 16, 16);
        std::memcpy(j.rgba, mem + 6 * 16, 16);
        if (e.texgen)
            for (int i = 0; i < 4; ++i)
                std::memcpy(j.uvm + 4 * i, mem + (((top + 128 + i) & 0x3ffu) << 4), 16);
    }
    return emitKicks(memory, s);
}

// One scenery job through the model (FPCR set for it): header, continuation or
// second pass. sl is the TOP slot the job wrote (or would read); nothing is
// marked valid or native here.
bool sceneModel(const uint8_t *mem, SceneGroup &g, uint32_t pc, uint32_t top, bool header, bool second, SceneSlot *&sl)
{
    const uint64_t saved = ps2_fpmode::readControl();
    ps2_fpmode::writeControl(ps2_fpmode::ps2Control(saved));
    bool servable = false;
    sl = nullptr;
    if (header)
    {
        const uint32_t fl = ldw(mem, top + 4, 3) & 0xffffu;
        g.live = true;
        g.native = false;
        g.flags = fl;
        if (sceneEligible(fl))
        {
            F4 v[4];
            for (int i = 0; i < 4; ++i)
                v[i] = ld4(mem, i);
            for (int i = 0; i < 4; ++i)
            {
                g.comp[i] = mat4(v, ld4(mem, top + i));
                g.m[i] = ld4(mem, top + 5 + i);
            }
            sl = &g.claim(top);
            sl->decoded = false; // the VIF uploaded a new chunk into this buffer
            const uint32_t chunk = top + 0x1b;
            if ((fl & 0x1e) == 2)
                sceneUvDecode(mem, chunk, g.m, *sl);
            servable = sceneVerts(mem, g, *sl, chunk, (fl & 0x20) != 0, sl->pkt);
        }
        else
            g_sceneUnmodelled.fetch_add(1, std::memory_order_relaxed);
    }
    else if (!second)
    {
        if (g.live && g.native)
        {
            sl = &g.claim(top);
            sl->decoded = false; // the VIF uploaded a new chunk into this buffer
            if ((g.flags & 0x1e) == 2)
                sceneUvDecode(mem, top, g.m, *sl);
            servable = sceneVerts(mem, g, *sl, top, (g.flags & 0x20) != 0, sl->pkt);
        }
    }
    else if (g.live && g.native && !(g.flags & 0x20))
    {
        sl = g.find(top);
        if (sl)
            servable = sceneStPass(mem, pc == 0x2270 ? top + 0x1b : top, g, *sl);
    }
    ps2_fpmode::writeControl(saved);
    return servable;
}

bool sceneBefore(PS2Memory &memory, State &s, uint32_t pc, uint32_t top, uint32_t itop)
{
    if (!knobs().instances && !knobs().check)
        return false;
    if (pc != 0x2270 && pc != 0x22c8 && pc != 0x2320 && pc != 0x2358)
    {
        s.scene.forget(top); // effect entries: VU1 (the header constants are in VU1 memory)
        return false;
    }
    const bool second = (pc == 0x2270 || pc == 0x22c8) && itop != 0;
    const bool header = !second && (pc == 0x2270 || pc == 0x2320);
    EntryStats &st = g_sstats[header ? 0 : second ? 2 : 1];
    SceneGroup &g = s.scene;
    uint8_t *mem = memory.getVU1Data();
    const bool check = knobs().check;
    s.kicks.clear();
    SceneSlot *sl = nullptr;
    const bool servable = sceneModel(mem, g, pc, top, header, second, sl);

    const bool groupNative = g.live && g.native;
    if (check)
    {
        if (header ? !sceneEligible(g.flags) : !groupNative)
        {
            g.forget(top); // VU1 writes this buffer
            return false;   // VU1's by flags, or VU1 memory holds this instance's state: nothing to compare
        }
        s.checkPending = true;
        s.checkServable = servable;
        s.checkStats = &st;
        s.checkPc = pc | (second ? 0x10000u : 0u);
        s.capBytes.clear();
        s.capSize.clear();
        if (servable)
        {
            sl->valid = true;
            s.kicks.kickBytes(sl->pkt.q, 16u * (1u + 3u * sl->pkt.nv));
            g.native = true;
        }
        else
        {
            if (header)
                g.native = false;
            g.forget(top); // the model can't follow this buffer further
        }
        return false;
    }
    if (!servable)
    {
        if (header)
        {
            if (sceneEligible(g.flags))
                st.fallback.fetch_add(1, std::memory_order_relaxed);
            g.forget(top);
            return false; // VU1 runs the whole instance
        }
        if (!groupNative)
        {
            g.forget(top);
            return false;
        }
        // A native instance's job the model can't serve: VU1 needs what the
        // native passes left at this TOP (the header constants are already written).
        // (A second pass with no slot reads VU1 memory that holds the truth already.)
        (second && sl ? st.orphan : st.fallback).fetch_add(1, std::memory_order_relaxed);
        countEvent();
        if (second && sl)
            sceneMaterialize(mem, *sl);
        g.forget(top); // VU1 memory holds the truth for this buffer again
        return false;
    }
    if (header)
    {
        g.native = true;
        sceneWriteConsts(mem, g);
    }
    sl->valid = true;
    st.routed.fetch_add(1, std::memory_order_relaxed);
    countEvent();
    s.lastNative = true;
    g_inSceneRun.store(true, std::memory_order_relaxed);
    s.kicks.kickBytes(sl->pkt.q, 16u * (1u + 3u * sl->pkt.nv));
    if (!s.sceneFirstLogged)
    {
        s.sceneFirstLogged = true;
        std::fprintf(stderr, "[nrt1] first native instance job pc=0x%x itop=%u top=%u verts=%u\n", pc, itop, top,
                     sl->pkt.nv);
    }
    if (knobs().keyed)
    {
        // RZV1 S4a: the chunk (tag, sizes, strip table, positions, UVs, colours) and the instance constants.
        const uint32_t chunk = (header || (second && pc == 0x2270)) ? top + 0x1b : top;
        const uint32_t ofs = ilw(mem, chunk + 1, 0), n = ilw(mem, chunk + 1, 2);
        keyHeader(s, 2u, pc | (second ? 0x10000u : 0u));
        keyAppend(s, mem, chunk, 2u + ofs + 3u * n);
        Ge1KeyedJob &j = s.job;
        j = Ge1KeyedJob{};
        j.kind = 2;
        j.pc = pc | (second ? 0x10000u : 0u);
        j.n = n;
        j.variant = (g.flags & 0x20u) ? 0u : 1u;
        j.flags = g.flags;
        for (int i = 0; i < 4; ++i)
        {
            vst1q_f32(j.m + 4 * i, g.comp[i]);
            vst1q_f32(j.uvm + 4 * i, g.m[i]);
        }
        std::memcpy(j.scale, mem + 4 * 16, 16);
        std::memcpy(j.offset, mem + 5 * 16, 16);
        j.offset[3] = 1.0f;
    }
    return emitKicks(memory, s);
}
} // namespace

bool beforeVu1(PS2Memory &memory, uint32_t startPC, uint32_t top, uint32_t itop)
{
    if (!active())
        return false;
    registerExit();
    State &s = state();
    s.lastNative = false;
    const uint64_t h = imageHash(memory, s);
    const bool terrainImage = h == kTerrainImageA || h == kTerrainImageB;
    if (!terrainImage)
    {
        s.inTerrainRun = false;
        g_inTerrainRun.store(false, std::memory_order_relaxed);
    }
    if (h != kSceneryImage)
    {
        g_inSceneRun.store(false, std::memory_order_relaxed);
        s.scene.forgetAll(); // other programs write the TOP buffers' areas too
    }
    if (terrainImage)
        return terrainBefore(memory, s, startPC, top);
    if (h == kSceneryImage)
        return sceneBefore(memory, s, startPC, top, itop);
    return false;
}

void afterVu1()
{
    if (!active())
        return;
    State &s = state();
    if (!s.checkPending)
        return;
    s.checkPending = false;
    countEvent();
    EntryStats &st = *s.checkStats;
    const uint32_t ncap = static_cast<uint32_t>(s.capSize.size());
    if (!s.checkServable)
    {
        // The model expected clipper polygons (or an unsupported shape).
        if (ncap > s.kicks.count)
            st.fbConfirmed.fetch_add(1, std::memory_order_relaxed);
        else
            st.fbSpurious.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    bool same = ncap == s.kicks.count;
    uint32_t off = 0, bad = 0;
    for (uint32_t i = 0; same && i < ncap; ++i)
    {
        same = s.capSize[i] == s.kicks.size[i] &&
               !std::memcmp(s.capBytes.data() + off, s.kicks.body() + s.kicks.off[i], s.capSize[i]);
        if (!same)
            bad = i;
        off += s.capSize[i];
    }
    if (same)
    {
        st.match.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    st.mismatch.fetch_add(1, std::memory_order_relaxed);
    if (g_mismatchDumps.fetch_sub(1, std::memory_order_relaxed) > 0)
    {
        std::fprintf(stderr, "[nrt1] check mismatch pc=0x%x kicks model=%u vu1=%u first_bad=%u", s.checkPc,
                     s.kicks.count, ncap, bad);
        if (bad < ncap && bad < s.kicks.count)
        {
            uint32_t o = 0;
            for (uint32_t i = 0; i < bad; ++i)
                o += s.capSize[i];
            const uint32_t sz = s.capSize[bad] < s.kicks.size[bad] ? s.capSize[bad] : s.kicks.size[bad];
            for (uint32_t b = 0; b < sz; b += 4)
            {
                uint32_t a, m;
                std::memcpy(&a, s.capBytes.data() + o + b, 4);
                std::memcpy(&m, s.kicks.body() + s.kicks.off[bad] + b, 4);
                if (a != m)
                {
                    std::fprintf(stderr, " byte %u vu1=%08x model=%08x", b, a, m);
                    break;
                }
            }
            std::fprintf(stderr, " size vu1=%u model=%u", s.capSize[bad], s.kicks.size[bad]);
        }
        std::fprintf(stderr, "\n");
    }
}

void onResume()
{
    if (active() && state().lastNative)
        g_resumeAfterNative.fetch_add(1, std::memory_order_relaxed);
}

void onVu1Packet(const uint8_t *bytes, uint32_t size)
{
    if (!active())
        return;
    State &s = state();
    if (s.checkPending)
    {
        s.capBytes.insert(s.capBytes.end(), bytes, bytes + size);
        s.capSize.push_back(size);
    }
}

std::string saveReady()
{
    if (active() && g_inTerrainRun.load(std::memory_order_relaxed))
        return "nrt1: native terrain run open";
    if (active() && g_inSceneRun.load(std::memory_order_relaxed))
        return "nrt1: native instance pass open";
    return {};
}

void resetForLoad()
{
    if (!active())
        return;
    State &s = state();
    s.group = Group{};
    s.scene.live = s.scene.native = false;
    s.scene.forgetAll();
    g_inSceneRun.store(false, std::memory_order_relaxed);
    s.inTerrainRun = false;
    s.lastNative = false;
    s.checkPending = false;
    g_inTerrainRun.store(false, std::memory_order_relaxed);
}

bool testModelTerrain(const uint8_t *vu1Data, uint32_t startPC, uint32_t top,
                      std::vector<std::vector<uint8_t>> &kicks)
{
    kicks.clear();
    const int ei = entryIndex(startPC);
    if (ei < 0)
        return false;
    State &s = state();
    Kicks k;
    const bool ok = runModel(vu1Data, kEntries[ei], top, s.group, k);
    for (uint32_t i = 0; i < k.count; ++i)
        kicks.emplace_back(k.body() + k.off[i], k.body() + k.off[i] + k.size[i]);
    if (ok)
    {
        s.group.live = true;
        s.group.native = true;
    }
    else if (kEntries[ei].base)
    {
        s.group.live = true;
        s.group.native = false;
    }
    return ok;
}

bool testModelScenery(const uint8_t *vu1Data, uint32_t startPC, uint32_t top, uint32_t itop,
                      std::vector<uint8_t> &packet)
{
    packet.clear();
    SceneGroup &g = state().scene;
    const bool second = (startPC == 0x2270 || startPC == 0x22c8) && itop != 0;
    const bool header = !second && (startPC == 0x2270 || startPC == 0x2320);
    SceneSlot *sl = nullptr;
    const bool ok = sceneModel(vu1Data, g, startPC, top, header, second, sl);
    if (!ok)
    {
        if (header)
            g.native = false;
        g.forget(top);
        return false;
    }
    if (header)
        g.native = true;
    sl->valid = true;
    const uint8_t *b = reinterpret_cast<const uint8_t *>(sl->pkt.q);
    packet.assign(b, b + 16u * (1u + 3u * sl->pkt.nv));
    return true;
}

void testResetGroup()
{
    State &s = state();
    s.group = Group{};
    s.scene.live = s.scene.native = false;
    s.scene.forgetAll();
}
} // namespace ps2_native_world
