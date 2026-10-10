// RZV1 S4b: regenerate one static-world vertex from the resident pool, exactly
// as the runtime's NRT1 model computes it (microVU arithmetic: the `int`
// variant of ge1_rz_core.inc). One source for C (CPU check) and MSL (Metal).
// The dialect prelude defines uint, DEVC (address space of read-only buffers),
// THR (thread-local pointers) and the rz_core macros.
//
// pool:  the slot's input blob, u32 words (qword-major); blob qword 0 is the
//        key header, regions follow (ps2_native_world.cpp keyAppend order).
// job:   80 words: [0..15] M rows (terrain mem[0..3]; scenery V x model),
//        [16..19] scale, [20..23] offset (scenery: w = 1.0), [24..27] RGBA qword,
//        [28..43] uvm (terrain texgen M2 / scenery UV matrix), [44..71] the
//        base job's M, scale, offset, RGBA (terrain follow-ups).
// g0:    pos | uv << 16 (blob qword indices); g1: col | kind << 16 | flags << 24.
// out:   the vertex as the compact record carries it: S, T, RGBA, Q, X, Y, Z, W3.
uint rzg_mat_lane(DEVC uint* m, uint l, uint x, uint y, uint z, uint w)
{
    uint a = vmul(m[l], x);
    a = vadd(a, vmul(m[4u + l], y));
    a = vadd(a, vmul(m[8u + l], z));
    a = vadd(a, vmul(m[12u + l], w));
    return a;
}
uint rzg_itof(uint raw, uint scaleBits)
{
    // ITOFn: the 32-bit lane as a signed int, exactly converted, times 2^-n (exact)
    float f = U2FLT(I32C(raw));
    return i_mul(F2U(f), scaleBits);
}
uint rzg_pack_rgba(uint r, uint g, uint b, uint a)
{
    return (r & 0xffu) | ((g & 0xffu) << 8) | ((b & 0xffu) << 16) | ((a & 0xffu) << 24);
}
void rzg_vertex(DEVC uint* blob, DEVC uint* job, uint g0, uint g1, THR uint* out)
{
    uint pos = g0 & 0xffffu;
    uint uvq = g0 >> 16;
    uint col = g1 & 0xffffu;
    uint kind = (g1 >> 16) & 0xffu;
    uint flags = g1 >> 24;
    bool rewrite = (flags & 1u) != 0u;
    bool rgbaBase = (flags & 2u) != 0u;
    uint uvKind = (flags >> 2) & 7u;
    bool adc = (flags & 32u) != 0u;
    uint p[4];
    uint uv[4];
    if (kind == 0u)
    {
        // terrain: the base job's constants for a 0xc30 rewrite (its XYZ/Q are the base pass's)
        uint cb = rewrite ? 44u : 0u;
        for (uint l = 0u; l < 4u; ++l)
            p[l] = blob[4u * pos + l];
        uint c[4];
        for (uint l = 0u; l < 4u; ++l)
            c[l] = rzg_mat_lane(job + cb, l, p[0], p[1], p[2], p[3]);
        uint q = vdiv(ONEB, c[3]);
        uint s[4];
        for (uint l = 0u; l < 4u; ++l)
        {
            uint acc = vmul(job[cb + 20u + l], ONEB);
            s[l] = vadd(acc, vmul(vmul(c[l], q), job[cb + 16u + l]));
        }
        if (uvKind == 2u || uvKind == 3u)
        {
            uv[0] = rzg_mat_lane(job + 28u, 0u, blob[4u * uvq], blob[4u * uvq + 1u], blob[4u * uvq + 2u], blob[4u * uvq + 3u]);
            uv[1] = rzg_mat_lane(job + 28u, 1u, blob[4u * uvq], blob[4u * uvq + 1u], blob[4u * uvq + 2u], blob[4u * uvq + 3u]);
            uv[2] = ONEB;
            uv[3] = ONEB;
        }
        else
        {
            for (uint l = 0u; l < 4u; ++l)
                uv[l] = blob[4u * uvq + l];
        }
        if (rewrite)
        {
            // ST.w of the base pass is uvA.w * Q with uvA.w = 1.0 (VIF STROW): S,T = uv.xy (1 Q), Q = 1 Q
            uint qb = vmul(ONEB, q);
            out[0] = vmul(uv[0], qb);
            out[1] = vmul(uv[1], qb);
            out[3] = qb;
        }
        else
        {
            out[0] = vmul(uv[0], q);
            out[1] = vmul(uv[1], q);
            out[3] = vmul(uv[2], q);
        }
        uint rb = rgbaBase ? 68u : 24u;
        out[2] = rzg_pack_rgba(job[rb], job[rb + 1u], job[rb + 2u], job[rb + 3u]);
        out[4] = ftoi(s[0], 0x41800000u);
        out[5] = ftoi(s[1], 0x41800000u);
        out[6] = ftoi(s[2], 0x41800000u);
        out[7] = adc ? 0xffffffffu : ftoi(s[3], 0x41800000u);
        return;
    }
    // scenery instance (kind 1 no-clip, 2 clip)
    bool clip = kind == 2u;
    p[0] = rzg_itof(blob[4u * pos], 0x38000000u);
    p[1] = rzg_itof(blob[4u * pos + 1u], 0x38000000u);
    p[2] = rzg_itof(blob[4u * pos + 2u], 0x38000000u);
    p[3] = clip ? blob[4u * pos + 3u] : ONEB;
    uint uvraw[4];
    for (uint l = 0u; l < 4u; ++l)
        uvraw[l] = blob[4u * uvq + l];
    if (uvKind == 5u)
    {
        // format 2: FTOI12(m10 u + m11 v + (m12 + m13)) of the raw UV, zw = 1.0
        uint u0 = rzg_itof(uvraw[0], 0x39800000u);
        uint v0 = rzg_itof(uvraw[1], 0x39800000u);
        uint d[2];
        for (uint l = 0u; l < 2u; ++l)
        {
            uint o = vadd(job[36u + l], job[40u + l]);
            uint a = vmul(job[28u + l], u0);
            a = vadd(a, vmul(job[32u + l], v0));
            a = vadd(a, vmul(o, ONEB));
            d[l] = ftoi(a, 0x45800000u);
        }
        uvraw[0] = d[0];
        uvraw[1] = d[1];
        uvraw[2] = ONEB;
        uvraw[3] = ONEB;
    }
    uv[0] = rzg_itof(uvraw[0], 0x39800000u);
    uv[1] = rzg_itof(uvraw[1], 0x39800000u);
    uv[2] = clip ? uvraw[2] : ONEB;
    uv[3] = clip ? uvraw[3] : ONEB;
    uint c[4];
    for (uint l = 0u; l < 4u; ++l)
    {
        if (clip)
            c[l] = rzg_mat_lane(job, l, p[0], p[1], p[2], p[3]);
        else
        {
            // the viewport folded into each row: row' = scale (.) row + offset (row.w)
            uint r0 = vadd(vmul(job[16u + l], job[l]), vmul(job[20u + l], job[3u]));
            uint r1 = vadd(vmul(job[16u + l], job[4u + l]), vmul(job[20u + l], job[7u]));
            uint r2 = vadd(vmul(job[16u + l], job[8u + l]), vmul(job[20u + l], job[11u]));
            uint r3 = vadd(vmul(job[16u + l], job[12u + l]), vmul(job[20u + l], job[15u]));
            uint a = vmul(r0, p[0]);
            a = vadd(a, vmul(r1, p[1]));
            a = vadd(a, vmul(r2, p[2]));
            a = vadd(a, vmul(r3, p[3]));
            c[l] = a;
        }
    }
    uint q = vdiv(ONEB, c[3]);
    out[0] = vmul(uv[0], q);
    out[1] = vmul(uv[1], q);
    out[3] = vmul(uv[2], q);
    out[2] = rzg_pack_rgba(blob[4u * col], blob[4u * col + 1u], blob[4u * col + 2u], blob[4u * col + 3u]);
    uint t[4];
    for (uint l = 0u; l < 4u; ++l)
        t[l] = vmul(c[l], q);
    if (clip)
    {
        uint s[4];
        for (uint l = 0u; l < 4u; ++l)
            s[l] = vadd(vmul(job[20u + l], ONEB), vmul(t[l], job[16u + l]));
        out[4] = ftoi(s[0], 0x41800000u);
        out[5] = ftoi(s[1], 0x41800000u);
        out[6] = ftoi(s[2], 0x41800000u);
        out[7] = adc ? 0xffffffffu : ftoi(s[3], 0x41800000u);
    }
    else
    {
        out[4] = ftoi(t[0], 0x41800000u);
        out[5] = ftoi(t[1], 0x41800000u);
        out[6] = ftoi(t[2], 0x41800000u);
        out[7] = adc ? 0x0000ffffu : ftoi(t[3], 0x41800000u);
    }
}
// The kick's parse of a compact vertex into GSVertex (ParseCompactXYZF2 + the
// depth clamp), applied to the generated words: what GE1 stores at the slot.
void rzg_gsvertex(THR uint* w, uint uvreg, uint keep0, uint keep1, uint keep2, uint keep3, uint sh0, uint sh1, uint sh2,
                  uint sh3, THR uint* m)
{
    m[0] = w[0];
    m[1] = w[1];
    m[2] = w[2];
    m[3] = w[3] == 0u ? 0x00800000u : w[3];
    uint m1[4];
    m1[0] = (w[4] & 0xffffu) | (w[5] << 16);
    m1[1] = (w[6] >> 4) & 0x00ffffffu;
    m1[2] = uvreg;
    m1[3] = (w[7] >> 4) & 0xffu;
    m[4] = (m1[0] & keep0) | ((m1[0] >> 8) & sh0);
    m[5] = (m1[1] & keep1) | ((m1[1] >> 8) & sh1);
    m[6] = (m1[2] & keep2) | ((m1[2] >> 8) & sh2);
    m[7] = (m1[3] & keep3) | ((m1[3] >> 8) & sh3);
}
