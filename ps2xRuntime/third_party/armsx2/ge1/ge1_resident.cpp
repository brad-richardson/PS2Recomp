// RZV1 S4b (GE1_RESIDENT=1): the resident pool, mirrored slot for slot from the
// runtime's (the runtime ships a slot's inputs only when it (re)loads it), and
// the static-world vertex generator from ge1_resident_gen.inc, checked against
// every vertex of the record's compact record: on the CPU at ingest (the
// compact words, then GSVertex against GE1's own parse with the packet's UV and
// depth clamp from the vendor hook), and on Metal (Mac) per vsync, read back
// and compared the same way. Diagnostic: the S4a path still draws.
#include "ge1_keyed.h"
#include "pcsx2/GS/GS.h"
#include "pcsx2/GS/GSVertexKick.h"
#include "pcsx2/GS/GSVertexKickKernel.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace rzgen
{
#include "ge1_resident_dialect_c.h"
#include "ge1_rz_core.inc"
#include "ge1_resident_gen.inc"
} // namespace rzgen
#undef F2U
#undef U2F
#undef F2I
#undef U2FLT
#undef U32C
#undef I32C
#undef PREC
#undef MSB
#undef UMUL
#undef fma
#undef VARIANT
#undef DEVC
#undef THR

// ge1_resident_metal.mm (Mac); the stubs below elsewhere.
bool ge1rm_init(size_t poolWords);
uint32_t* ge1rm_pool();
bool ge1rm_run(const uint32_t* jobs, size_t jobWords, const uint32_t* items, uint32_t nitems, uint32_t* out, double* gpuMs);
#if !(defined(__APPLE__) && !TARGET_OS_IPHONE)
bool ge1rm_init(size_t) { return false; }
uint32_t* ge1rm_pool() { return nullptr; }
bool ge1rm_run(const uint32_t*, size_t, const uint32_t*, uint32_t, uint32_t*, double*) { return false; }
#endif

namespace
{
constexpr uint32_t kSlotQw = 448; // the largest blob: 1 + 2 + strip table + 3 x 128 scenery qwords
constexpr uint32_t kJobWords = 80;
constexpr uint32_t kItemWords = 16;

struct State
{
    bool on = false, gpu = false, ready = false;
    uint32_t slots = 32768;
    std::vector<uint32_t> poolHeap;
    uint32_t* pool = nullptr;
    std::vector<uint32_t> slotQw;
    // the record in flight (between before and after)
    struct Pending
    {
        uint32_t packet;
        uint32_t item;          // index into items (GPU) or ~0u
        uint32_t words[8];      // the record's compact vertex
        uint32_t gen[8];        // the CPU generator's words
    };
    std::vector<Pending> pending;
    uint32_t pktUv[64];
    int pktClamp[64];
    // the vsync batch
    std::vector<uint32_t> jobs, items, expect, out;
    // counters
    uint64_t records = 0, noBlock = 0, genEmpty = 0, badSlot = 0, unloaded = 0, oversize = 0, badIndex = 0;
    uint64_t verts = 0, unsupported = 0, cpuOk = 0, cpuBad = 0, gsvOk = 0, gsvBad = 0, gpuOk = 0, gpuBad = 0;
    uint64_t loads = 0, vsyncs = 0;
    double gpuMs = 0;
    int dumps = 8;
};
State& st()
{
    static State s;
    return s;
}

void PacketHook(u32 packet, u32 uv, int clamp)
{
    State& s = st();
    if (packet < 64)
    {
        s.pktUv[packet] = uv;
        s.pktClamp[packet] = clamp;
    }
}

void Init()
{
    State& s = st();
    if (s.ready)
        return;
    s.ready = true;
    const char* on = std::getenv("GE1_RESIDENT");
    s.on = on && !std::strcmp(on, "1");
    if (!s.on)
        return;
    if (const char* v = std::getenv("PS2X_SSX3_NATIVE_POOL_SLOTS"))
        s.slots = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
    const size_t words = static_cast<size_t>(s.slots) * kSlotQw * 4u;
    const char* gpu = std::getenv("GE1_RESIDENT_GPU");
    s.gpu = !(gpu && !std::strcmp(gpu, "0")) && ge1rm_init(words);
    if (s.gpu)
        s.pool = ge1rm_pool();
    else
    {
        s.poolHeap.assign(words, 0);
        s.pool = s.poolHeap.data();
    }
    s.slotQw.assign(s.slots, 0);
    GSSetCompactPacketHook(&PacketHook);
    std::fprintf(stderr, "[ge1] resident: on, %u slots x %u qw, gpu=%d\n", s.slots, kSlotQw, s.gpu ? 1 : 0);
}

void Masks(int clamp, uint32_t keep[4], uint32_t sh[4])
{
    GSVector4i k = GSVector4i::xffffffff(), h = GSVector4i::zero();
    if (static_cast<GSLimit24BitDepth>(clamp) != GSLimit24BitDepth::Disabled)
        GSVertexKickKernel::MakeDepthClampMasks(static_cast<GSLimit24BitDepth>(clamp), k, h);
    std::memcpy(keep, &k, 16);
    std::memcpy(sh, &h, 16);
}

void Expected(const uint32_t w[8], uint32_t uv, int clamp, uint32_t m[8])
{
    ::Ge1CompactVertex cv;
    std::memcpy(&cv, w, 32);
    GSVector4i m0, m1;
    GSVertexKernels::ParseCompactXYZF2(&cv, uv, m0, m1);
    if (static_cast<GSLimit24BitDepth>(clamp) != GSLimit24BitDepth::Disabled)
    {
        GSVector4i keep, sh;
        GSVertexKickKernel::MakeDepthClampMasks(static_cast<GSLimit24BitDepth>(clamp), keep, sh);
        m1 = (m1 & keep) | (m1.srl32<8>() & sh);
    }
    std::memcpy(m, &m0, 16);
    std::memcpy(m + 4, &m1, 16);
}

void Dump(const char* what, const uint32_t* a, const uint32_t* b, uint32_t g0, uint32_t g1)
{
    State& s = st();
    if (s.dumps-- <= 0)
        return;
    std::fprintf(stderr, "[ge1] resident %s mismatch kind=%u flags=0x%x pos=%u uv=%u col=%u\n  want", what, (g1 >> 16) & 0xffu,
        g1 >> 24, g0 & 0xffffu, g0 >> 16, g1 & 0xffffu);
    for (int i = 0; i < 8; i++)
        std::fprintf(stderr, " %08x", a[i]);
    std::fprintf(stderr, "\n  got ");
    for (int i = 0; i < 8; i++)
        std::fprintf(stderr, " %08x", b[i]);
    std::fprintf(stderr, "\n");
}

void Print()
{
    State& s = st();
    std::fprintf(stderr,
        "[ge1] resident: records=%llu (no_block=%llu gen_empty=%llu bad_slot=%llu unloaded=%llu oversize=%llu bad_index=%llu) "
        "loads=%llu verts=%llu unsupported=%llu | cpu words ok=%llu bad=%llu | cpu gsvertex ok=%llu bad=%llu | "
        "gpu ok=%llu bad=%llu gpu_ms=%.1f vsyncs=%llu\n",
        (unsigned long long)s.records, (unsigned long long)s.noBlock, (unsigned long long)s.genEmpty,
        (unsigned long long)s.badSlot, (unsigned long long)s.unloaded, (unsigned long long)s.oversize,
        (unsigned long long)s.badIndex, (unsigned long long)s.loads, (unsigned long long)s.verts,
        (unsigned long long)s.unsupported, (unsigned long long)s.cpuOk, (unsigned long long)s.cpuBad,
        (unsigned long long)s.gsvOk, (unsigned long long)s.gsvBad, (unsigned long long)s.gpuOk,
        (unsigned long long)s.gpuBad, s.gpuMs, (unsigned long long)s.vsyncs);
}
} // namespace

bool ge1_resident_on()
{
    Init();
    return st().on;
}

void ge1_resident_before(const Ge1KeyedParts& p)
{
    State& s = st();
    s.pending.clear();
    for (int i = 0; i < 64; i++)
    {
        s.pktUv[i] = 0;
        s.pktClamp[i] = 0;
    }
    s.records++;
    if (!p.s4b || p.s4bBytes < 128)
    {
        s.noBlock++;
        return;
    }
    uint32_t hdr[4];
    std::memcpy(hdr, p.s4b, 16);
    const uint32_t slot = hdr[1], inQw = hdr[2], nGen = hdr[3];
    if (hdr[0] != 0x31423453u || slot >= s.slots || 128u + 16u * inQw + 8u * nGen > p.s4bBytes)
    {
        s.badSlot++;
        return;
    }
    uint32_t* blob = s.pool + static_cast<size_t>(slot) * kSlotQw * 4u;
    if (inQw)
    {
        s.loads++;
        if (inQw > kSlotQw)
        {
            s.oversize++;
            s.slotQw[slot] = 0;
            return;
        }
        std::memcpy(blob, p.s4b + 128, 16u * inQw);
        s.slotQw[slot] = inQw;
    }
    if (!s.slotQw[slot])
    {
        s.unloaded++;
        return;
    }
    if (!nGen)
    {
        s.genEmpty++;
        return;
    }
    uint32_t job[kJobWords] = {};
    std::memcpy(job, p.job->m, 64);
    std::memcpy(job + 16, p.job->scale, 16);
    std::memcpy(job + 20, p.job->offset, 16);
    std::memcpy(job + 24, p.job->rgba, 16);
    std::memcpy(job + 28, p.job->uvm, 64);
    std::memcpy(job + 44, p.s4b + 16, 112);
    const uint32_t jobIdx = static_cast<uint32_t>(s.jobs.size() / kJobWords);
    if (s.gpu)
        s.jobs.insert(s.jobs.end(), job, job + kJobWords);
    const uint8_t* gen = p.s4b + 128 + 16u * inQw;
    const uint32_t lim = s.slotQw[slot];
    // walk the compact record's packets in order; generation entry k is vertex k
    uint32_t count = 0;
    std::memcpy(&count, p.compact + 16, 4);
    const uint32_t table = (8u + 4u * count + 15u) & ~15u;
    uint32_t at = 16u + table, k = 0;
    for (uint32_t pk = 0; pk < count && pk < 64; pk++)
    {
        uint32_t n = 0;
        std::memcpy(&n, p.compact + 24 + 4 * pk, 4);
        const uint32_t nv = (n - 16u) / 32u;
        for (uint32_t v = 0; v < nv && k < nGen; v++, k++)
        {
            s.verts++;
            uint32_t g[2];
            std::memcpy(g, gen + 8u * k, 8);
            const uint32_t kind = (g[1] >> 16) & 0xffu;
            if (kind == 0xffu)
            {
                s.unsupported++;
                continue;
            }
            const uint32_t pos = g[0] & 0xffffu, uvq = g[0] >> 16, col = g[1] & 0xffffu;
            if (pos >= lim || uvq >= lim || col >= lim)
            {
                s.badIndex++;
                continue;
            }
            State::Pending pe;
            pe.packet = pk;
            std::memcpy(pe.words, p.compact + at + 16 + 32u * v, 32);
            rzgen::rzg_vertex(blob, job, g[0], g[1], pe.gen);
            if (std::memcmp(pe.gen, pe.words, 32))
            {
                s.cpuBad++;
                Dump("cpu words", pe.words, pe.gen, g[0], g[1]);
            }
            else
                s.cpuOk++;
            pe.item = ~0u;
            if (s.gpu)
            {
                pe.item = static_cast<uint32_t>(s.items.size() / kItemWords);
                const uint32_t it[kItemWords] = {slot, jobIdx, g[0], g[1]};
                s.items.insert(s.items.end(), it, it + kItemWords);
                s.expect.insert(s.expect.end(), pe.words, pe.words + 8);
                s.expect.insert(s.expect.end(), 8, 0u);
            }
            s.pending.push_back(pe);
        }
        at += n;
    }
}

void ge1_resident_after()
{
    State& s = st();
    for (const State::Pending& pe : s.pending)
    {
        const uint32_t uv = s.pktUv[pe.packet];
        const int clamp = s.pktClamp[pe.packet];
        uint32_t want[8], got[8], keep[4], sh[4];
        Expected(pe.words, uv, clamp, want);
        Masks(clamp, keep, sh);
        rzgen::rzg_gsvertex(const_cast<uint32_t*>(pe.gen), uv, keep[0], keep[1], keep[2], keep[3], sh[0], sh[1], sh[2], sh[3], got);
        if (std::memcmp(want, got, 32))
        {
            s.gsvBad++;
            Dump("cpu gsvertex", want, got, 0, 0);
        }
        else
            s.gsvOk++;
        if (pe.item != ~0u)
        {
            uint32_t* it = s.items.data() + static_cast<size_t>(pe.item) * kItemWords;
            it[4] = uv;
            std::memcpy(it + 5, keep, 16);
            std::memcpy(it + 9, sh, 16);
            std::memcpy(s.expect.data() + static_cast<size_t>(pe.item) * 16u + 8u, want, 32);
        }
    }
    s.pending.clear();
}

void ge1_resident_vsync()
{
    State& s = st();
    if (!s.on)
        return;
    s.vsyncs++;
    const uint32_t n = static_cast<uint32_t>(s.items.size() / kItemWords);
    if (s.gpu && n)
    {
        s.out.assign(static_cast<size_t>(n) * 16u, 0xcdcdcdcdu);
        double ms = 0;
        if (ge1rm_run(s.jobs.data(), s.jobs.size(), s.items.data(), n, s.out.data(), &ms))
        {
            s.gpuMs += ms;
            for (uint32_t i = 0; i < n; i++)
            {
                const uint32_t* a = s.expect.data() + 16u * i;
                const uint32_t* b = s.out.data() + 16u * i;
                if (std::memcmp(a, b, 64))
                {
                    s.gpuBad++;
                    const uint32_t* it = s.items.data() + static_cast<size_t>(i) * kItemWords;
                    Dump(std::memcmp(a, b, 32) ? "gpu words" : "gpu gsvertex", std::memcmp(a, b, 32) ? a : a + 8,
                        std::memcmp(a, b, 32) ? b : b + 8, it[2], it[3]);
                }
                else
                    s.gpuOk++;
            }
        }
    }
    s.jobs.clear();
    s.items.clear();
    s.expect.clear();
    if ((s.vsyncs % 240u) == 0)
        Print();
}
