// RZV1 S4a: GE1_S4A_CHECK's recomputation of one keyed pass (see ge1_gs.cpp
// S4aCheck): the GS drawing-kick rule over the compact packet, each vertex
// parsed by GE1's own ParseCompactXYZF2 (the kick's m0/m1) and FmmPos (the
// vertex trace's {x, y, z, fog}); every field compared bit for bit.
#include "ge1_keyed.h"
#include "pcsx2/GS/GSVertexKick.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

bool ge1_s4a_pass_matches(const uint8_t* pkt, uint32_t size, const Ge1KeyedPass& rec, const uint8_t* recTris)
{
    const uint32_t nv = (size - 16u) / 32u;
    u64 tag = 0;
    std::memcpy(&tag, pkt, 8);
    const u32 prim = static_cast<u32>((tag >> 47) & 0x7ffu);
    const u32 type = prim & 7u;
    const bool iip = (prim >> 3) & 1u;
    if (rec.nverts != nv || rec.prim != prim)
        return false;
    u32 adc[2] = {0, 0};
    GSVector4i m0[64], m1[64];
    for (u32 v = 0; v < nv && v < 64u; v++)
    {
        ::Ge1CompactVertex cv;
        std::memcpy(&cv, pkt + 16 + 32u * v, 32);
        if (cv.W3 & 0x8000u)
            adc[v >> 5] |= 1u << (v & 31u);
        GSVertexKernels::ParseCompactXYZF2(&cv, 0u, m0[v], m1[v]);
    }
    if (adc[0] != rec.adc[0] || adc[1] != rec.adc[1])
        return false;
    if (type < 3u || type > 5u || nv > 64u)
        return rec.flags == 0u && rec.ntris == 0u;
    u32 pmin[4] = {~0u, ~0u, ~0u, ~0u}, pmax[4] = {0, 0, 0, 0}, tnan = 0;
    u8 cmin[4] = {255, 255, 255, 255}, cmax[4] = {0, 0, 0, 0};
    float tmin[4], tmax[4];
    for (int l = 0; l < 4; l++)
    {
        tmin[l] = std::numeric_limits<float>::infinity();
        tmax[l] = -std::numeric_limits<float>::infinity();
    }
    bool any = false, flat = true, zeq = true;
    u32 z0 = 0, ntris = 0, queued = 0;
    u8 tris[3 * 64];
    auto takeP = [&](u32 v) {
        const GSVector4i p = GSVertexKernels::FmmPos(m1[v]);
        for (int l = 0; l < 4; l++)
        {
            const u32 x = p.U32[l];
            pmin[l] = std::min(pmin[l], x);
            pmax[l] = std::max(pmax[l], x);
        }
        if (!any)
            z0 = p.U32[2];
        else if (p.U32[2] != z0)
            zeq = false;
        any = true;
        const float s = m0[v].F32[0], t = m0[v].F32[1], q = m0[v].F32[3];
        const float tv[4] = {s / q, t / q, q, q};
        for (int l = 0; l < 4; l++)
        {
            if (std::isnan(tv[l]))
            {
                tnan |= 1u << l;
                continue;
            }
            tmin[l] = std::min(tmin[l], tv[l]);
            tmax[l] = std::max(tmax[l], tv[l]);
        }
    };
    auto takeC = [&](u32 v) {
        for (int l = 0; l < 4; l++)
        {
            const u8 c = m0[v].U8[8 + l];
            cmin[l] = std::min(cmin[l], c);
            cmax[l] = std::max(cmax[l], c);
        }
    };
    for (u32 v = 0; v < nv; v++)
    {
        queued++;
        if (type == 3u && queued > 3u)
            queued = 1u;
        if (((adc[v >> 5] >> (v & 31u)) & 1u) || queued < 3u)
            continue;
        const u32 a = type == 5u ? 0u : v - 2u, b = v - 1u, c = v;
        tris[3 * ntris] = static_cast<u8>(a);
        tris[3 * ntris + 1] = static_cast<u8>(b);
        tris[3 * ntris + 2] = static_cast<u8>(c);
        ntris++;
        takeP(a);
        takeP(b);
        takeP(c);
        if (m0[a].U32[2] != m0[c].U32[2] || m0[b].U32[2] != m0[c].U32[2])
            flat = false;
        if (iip)
        {
            takeC(a);
            takeC(b);
        }
        takeC(c);
    }
    const u32 flags = 1u | (flat ? 2u : 0u) | (any && zeq ? 4u : 0u);
    if (rec.ntris != ntris || rec.flags != flags || rec.tnan != tnan || std::memcmp(recTris, tris, 3u * ntris))
        return false;
    if (std::memcmp(rec.pmin, pmin, 16) || std::memcmp(rec.pmax, pmax, 16) || std::memcmp(rec.cmin, cmin, 4) ||
        std::memcmp(rec.cmax, cmax, 4) || std::memcmp(rec.tmin, tmin, 16) || std::memcmp(rec.tmax, tmax, 16))
        return false;
    return true;
}
