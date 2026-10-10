#include "MiniTest.h"
#include "ps2_native_world.h"
#include "runtime/gs/ge1_gs_api.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// NRT1: the terrain model on synthetic VU1 images (no game data). Layout as
// the SSX 3 terrain program reads it (local/research/NRV1/REPORT.md §2).
namespace
{
constexpr uint32_t kTop = 38;

struct Vu1Image
{
    uint8_t mem[16384]{};
    void put(uint32_t qw, float x, float y, float z, float w)
    {
        const float v[4] = {x, y, z, w};
        std::memcpy(mem + qw * 16, v, 16);
    }
    void putw(uint32_t qw, uint32_t x, uint32_t y, uint32_t z, uint32_t w)
    {
        const uint32_t v[4] = {x, y, z, w};
        std::memcpy(mem + qw * 16, v, 16);
    }
};

// RZV1 S4a: an independent reference for the keyed pass facts, computed
// from the GIF form of the packet (48-byte register triples), and a builder
// for hand-made PACKED {ST, RGBAQ, XYZF2} packets.
struct RefVert
{
    float s, t, q;
    uint8_t r, g, b, a;
    uint32_t x, y, z, f;
    bool adc;
};
std::vector<uint8_t> gifPacket(uint32_t prim, const std::vector<RefVert> &vs)
{
    std::vector<uint8_t> p(16 + 48 * vs.size(), 0);
    const uint64_t lo = uint64_t(vs.size()) | (1ull << 15) | (1ull << 46) | (uint64_t(prim) << 47) | (3ull << 60);
    const uint64_t hi = 0x412;
    std::memcpy(p.data(), &lo, 8);
    std::memcpy(p.data() + 8, &hi, 8);
    for (size_t i = 0; i < vs.size(); ++i)
    {
        const RefVert &v = vs[i];
        uint32_t r[12] = {};
        std::memcpy(&r[0], &v.s, 4);
        std::memcpy(&r[1], &v.t, 4);
        std::memcpy(&r[2], &v.q, 4);
        r[4] = v.r;
        r[5] = v.g;
        r[6] = v.b;
        r[7] = v.a;
        r[8] = v.x;
        r[9] = v.y;
        r[10] = v.z << 4;
        r[11] = (v.f << 4) | (v.adc ? 0x8000u : 0u);
        std::memcpy(p.data() + 16 + 48 * i, r, 48);
    }
    return p;
}
bool refFactsMatch(TestCase &t, const std::vector<uint8_t> &g, const Ge1KeyedPass &ps, const uint8_t *tris)
{
    const uint32_t nv = (static_cast<uint32_t>(g.size()) - 16u) / 48u;
    uint64_t lo;
    std::memcpy(&lo, g.data(), 8);
    const uint32_t prim = static_cast<uint32_t>((lo >> 47) & 0x7ff), type = prim & 7u;
    const bool iip = (prim >> 3) & 1u;
    auto reg = [&](uint32_t v, int i) {
        uint32_t x;
        std::memcpy(&x, g.data() + 16 + 48 * v + 4 * i, 4);
        return x;
    };
    uint32_t adc[2] = {0, 0};
    for (uint32_t v = 0; v < nv; ++v)
        if (reg(v, 11) & 0x8000u)
            adc[v / 32] |= 1u << (v % 32);
    bool ok = ps.nverts == nv && ps.prim == prim && ps.adc[0] == adc[0] && ps.adc[1] == adc[1];
    if (type < 3 || type > 5)
        return ok && ps.flags == 0 && ps.ntris == 0;
    std::vector<uint32_t> tri;
    for (uint32_t v = 0; v < nv; ++v)
    {
        const uint32_t pos = type == 3 ? v % 3 : v; // queue position within the list triple / strip
        if ((adc[v / 32] >> (v % 32)) & 1u)
            continue;
        if ((type == 3 && pos == 2) || (type != 3 && v >= 2))
        {
            tri.push_back(type == 5 ? 0 : v - 2);
            tri.push_back(v - 1);
            tri.push_back(v);
        }
    }
    ok = ok && ps.ntris * 3 == tri.size();
    for (size_t i = 0; ok && i < tri.size(); ++i)
        ok = tris[i] == tri[i];
    uint32_t pmin[4] = {~0u, ~0u, ~0u, ~0u}, pmax[4] = {0, 0, 0, 0}, tnan = 0;
    uint8_t cmin[4] = {255, 255, 255, 255}, cmax[4] = {0, 0, 0, 0};
    float tmin[4] = {INFINITY, INFINITY, INFINITY, INFINITY}, tmax[4] = {-INFINITY, -INFINITY, -INFINITY, -INFINITY};
    bool flat = true, zeq = true;
    for (size_t i = 0; i < tri.size(); i += 3)
    {
        auto rgba = [&](uint32_t v) { return (reg(v, 4) & 0xff) | (reg(v, 5) & 0xff) << 8 | (reg(v, 6) & 0xff) << 16 | (reg(v, 7) & 0xff) << 24; };
        if (rgba(tri[i]) != rgba(tri[i + 2]) || rgba(tri[i + 1]) != rgba(tri[i + 2]))
            flat = false;
        for (int k = 0; k < 3; ++k)
        {
            const uint32_t v = tri[i + k];
            const uint32_t pv[4] = {reg(v, 8) & 0xffff, reg(v, 9) & 0xffff, (reg(v, 10) >> 4) & 0xffffff, (reg(v, 11) >> 4) & 0xff};
            for (int l = 0; l < 4; ++l)
            {
                pmin[l] = std::min(pmin[l], pv[l]);
                pmax[l] = std::max(pmax[l], pv[l]);
            }
            if (((reg(v, 10) >> 4) & 0xffffff) != ((reg(tri[0], 10) >> 4) & 0xffffff))
                zeq = false;
            float sf, tf, qf;
            uint32_t qb = reg(v, 2) == 0 ? 0x00800000u : reg(v, 2), sb = reg(v, 0), tb = reg(v, 1);
            std::memcpy(&sf, &sb, 4);
            std::memcpy(&tf, &tb, 4);
            std::memcpy(&qf, &qb, 4);
            const float tv[4] = {sf / qf, tf / qf, qf, qf};
            for (int l = 0; l < 4; ++l)
            {
                if (std::isnan(tv[l]))
                    tnan |= 1u << l;
                else
                {
                    tmin[l] = std::min(tmin[l], tv[l]);
                    tmax[l] = std::max(tmax[l], tv[l]);
                }
            }
            if (iip || k == 2)
                for (int l = 0; l < 4; ++l)
                {
                    const uint8_t c = static_cast<uint8_t>(reg(v, 4 + l));
                    cmin[l] = std::min(cmin[l], c);
                    cmax[l] = std::max(cmax[l], c);
                }
        }
    }
    const uint32_t flags = 1u | (flat ? 2u : 0u) | (!tri.empty() && zeq ? 4u : 0u);
    ok = ok && ps.flags == flags && ps.tnan == tnan && !std::memcmp(ps.pmin, pmin, 16) && !std::memcmp(ps.pmax, pmax, 16) &&
         !std::memcmp(ps.cmin, cmin, 4) && !std::memcmp(ps.cmax, cmax, 4) && !std::memcmp(ps.tmin, tmin, 16) &&
         !std::memcmp(ps.tmax, tmax, 16);
    t.IsTrue(ok, "keyed pass facts == reference");
    return ok;
}
// One keyed record over these GIF packets; checks every pass against the reference.
void checkKeyed(TestCase &t, const std::vector<std::vector<uint8_t>> &k)
{
    std::vector<uint8_t> cat, comp, rec;
    std::vector<uint32_t> sizes;
    for (const auto &p : k)
    {
        cat.insert(cat.end(), p.begin(), p.end());
        sizes.push_back(static_cast<uint32_t>(p.size()));
    }
    const uint32_t count = static_cast<uint32_t>(sizes.size());
    Ge1KeyedJob job{};
    job.key = 0x1122334455667788ull;
    ps2_native_world::buildCompactRecord(cat.data(), sizes.data(), count, comp);
    ps2_native_world::buildKeyedRecord(cat.data(), sizes.data(), count, job, rec);
    t.IsTrue(ge1_is_keyed_record(rec.data(), static_cast<uint32_t>(rec.size())), "signature");
    t.IsTrue(ge1_is_any_native_record(rec.data(), static_cast<uint32_t>(rec.size())), "routed as a native record");
    t.IsFalse(ge1_is_compact_record(rec.data(), static_cast<uint32_t>(rec.size())), "not the compact kind");
    t.Equals(rec.size() % 16, size_t(0), "16-byte aligned");
    Ge1KeyedParts parts;
    t.IsTrue(ge1_keyed_record_parts(rec.data(), static_cast<uint32_t>(rec.size()), parts), "well formed");
    t.Equals(parts.passes, count, "one pass per packet");
    t.Equals(parts.job->key, job.key, "key carried");
    t.Equals(parts.job->passes, count, "job.passes filled");
    t.Equals(parts.compactSize, static_cast<uint32_t>(comp.size()), "compact size");
    t.IsTrue(std::memcmp(parts.compact, comp.data(), comp.size()) == 0, "compact record verbatim");
    const uint8_t *tri = parts.tris;
    for (uint32_t i = 0; i < count; ++i)
    {
        refFactsMatch(t, k[i], parts.pass[i], tri);
        tri += 3 * parts.pass[i].ntris;
    }
    std::vector<uint8_t> bad = rec;
    bad.resize(bad.size() - 16);
    t.IsFalse(ge1_keyed_record_parts(bad.data(), static_cast<uint32_t>(bad.size()), parts), "truncated keyed record rejected");
    bad = rec;
    const uint32_t passesBad = count + 1u;
    std::memcpy(bad.data() + 20, &passesBad, 4);
    t.IsFalse(ge1_keyed_record_parts(bad.data(), static_cast<uint32_t>(bad.size()), parts), "pass count mismatch rejected");
    bad = rec;
    bad[32 + 256 + 128u * count + parts.trisBytes] ^= 1u;
    t.IsFalse(ge1_keyed_record_parts(bad.data(), static_cast<uint32_t>(bad.size()), parts), "embedded magic checked");
}

uint32_t word(const std::vector<uint8_t> &pkt, uint32_t qw, int lane)
{
    uint32_t v;
    std::memcpy(&v, pkt.data() + qw * 16 + 4 * lane, 4);
    return v;
}
float fword(const std::vector<uint8_t> &pkt, uint32_t qw, int lane)
{
    float v;
    std::memcpy(&v, pkt.data() + qw * 16 + 4 * lane, 4);
    return v;
}

// n = 4 patch inside the guard band: identity clip matrix, the game's viewport
// constants, positions (0.1 col, 0.1 row, 0.5, w), UV A (col/4, row/4), UV B (col, row).
Vu1Image patch4(float w = 1.0f)
{
    Vu1Image im;
    im.put(0, 1, 0, 0, 0);
    im.put(1, 0, 1, 0, 0);
    im.put(2, 0, 0, 1, 0);
    im.put(3, 0, 0, 0, 1);
    im.put(4, 1024.0f, -1024.0f, -8388467.5f, 0.0f);
    im.put(5, 2047.5f, 2047.5f, 8388467.5f, 0.0f);
    im.putw(6, 0x80, 0x80, 0x80, 0x80);
    im.putw(7, 0, 0x11110001u, 0x22220002u, 0x33330003u);  // pass-1 tag words y/z/w
    im.putw(8, 0, 0x44440004u, 0x55550005u, 0x66660006u);  // pass-2 tag words
    im.putw(11, 2, 1, 0, 8);   // k = 0: strips A / B, B's first row offset
    im.putw(12, 16, 8, 48, 24); // nloop A / B, pass-2 qword counts
    for (uint32_t r = 0; r < 4; ++r)
        for (uint32_t c = 0; c < 4; ++c)
        {
            im.put(kTop + r * 4 + c, 0.1f * c, 0.1f * r, 0.5f, w);
            im.put(kTop + 0x40 + r * 4 + c, 0.25f * c, 0.25f * r, 1, 1);
            im.put(kTop + 0x80 + r * 4 + c, float(c), float(r), 1, 1);
        }
    return im;
}
// Scenery instance (NRV1 tooling scenery.h layout): identity V and model,
// header at kSTop (flags at TOP+4.w, UV matrix rows at TOP+5..8), chunk at
// TOP+0x1b: GIF tag, (ofs, -, n), one strip of 3, positions (ITOF15 ints),
// UVs (ITOF12 ints), RGBA.
constexpr uint32_t kSTop = 100;
Vu1Image instance3(uint32_t flags, uint32_t w = 0x3f800000u)
{
    Vu1Image im;
    im.put(0, 1, 0, 0, 0);
    im.put(1, 0, 1, 0, 0);
    im.put(2, 0, 0, 1, 0);
    im.put(3, 0, 0, 0, 1);
    im.put(4, 1024.0f, -1024.0f, -8388467.5f, 0.0f);
    im.put(5, 2047.5f, 2047.5f, 8388467.5f, 0.0f);
    for (uint32_t i = 0; i < 4; ++i)
        im.put(kSTop + i, i == 0, i == 1, i == 2, i == 3);
    im.putw(kSTop + 4, 0, 0, 0, flags);
    const uint32_t c = kSTop + 0x1b, pos = c + 3;
    im.putw(c, 3u | 0x8000u, 0x30000000u | 0x01234u, 0x512u, 0); // NLOOP 3, EOP, NREG 3 (ST, RGBAQ, XYZF2)
    im.putw(c + 1, 1, 0, 3, 0);
    im.putw(c + 2, 3, 0, 0, 0);
    im.putw(pos + 0, 0, 0, 0, w);
    im.putw(pos + 1, 16384, 0, 0, w); // x = 0.5
    im.putw(pos + 2, 0, 16384, 0, w); // y = 0.5
    im.putw(pos + 3, 0, 0, 0, 0);
    im.putw(pos + 4, 2048, 1024, 0, 0); // u = 0.5, v = 0.25
    im.putw(pos + 5, 4096, 4096, 0, 0);
    for (uint32_t i = 0; i < 3; ++i)
        im.putw(pos + 6 + i, 0x10 * i, 0x20, 0x30, 0x80);
    return im;
}
} // namespace

void register_ps2_native_world_tests()
{
    MiniTest::Case("Ps2NativeWorld", [](TestCase &tc)
                   {
        tc.Run("knobs unset: inactive", [](TestCase &t)
               { t.IsFalse(ps2_native_world::active(), "no PS2X_SSX3_NATIVE_WORLD* in the suite env"); });
        tc.Run("base 0x6b8: four kicks, tags, ADC, colour, pass-2 ST", [](TestCase &t)
               {
            ps2_native_world::testResetGroup();
            Vu1Image im = patch4();
            std::vector<std::vector<uint8_t>> k;
            t.IsTrue(ps2_native_world::testModelTerrain(im.mem, 0x6b8, kTop, k), "servable");
            t.Equals(k.size(), size_t(4), "kicks");
            t.Equals(k[0].size(), size_t(16 + 16 * 48), "A size");
            t.Equals(k[1].size(), size_t(16 + 8 * 48), "B size");
            t.Equals(word(k[0], 0, 0), 16u | 0x8000u, "A nloop | EOP");
            t.Equals(word(k[0], 0, 1), 0x11110001u, "A tag y from mem[7]");
            t.Equals(word(k[2], 0, 1), 0x44440004u, "pass-2 tag y from mem[8]");
            t.Equals(word(k[2], 0, 0), 16u | 0x8000u, "pass-2 keeps nloop");
            // vertex v: ST at qw 1+3v, RGBAQ 2+3v, XYZF2 3+3v
            t.Equals(word(k[0], 3, 3), 0xffffffffu, "strip vertex 0 ADC");
            t.Equals(word(k[0], 6, 3), 0xffffffffu, "strip vertex 1 ADC");
            t.Equals(word(k[0], 9, 3), 0u, "vertex 2 drawn, F = 0");
            t.Equals(word(k[0], 3, 0), 32760u, "x = 2047.5 * 16 at column 0");
            t.Equals(word(k[0], 2, 0), 0x80u, "RGBA from mem[6]");
            t.Equals(fword(k[0], 7, 0), 0.25f, "pass-1 S = UV A (q = 1), vertex 2 = column 1");
            t.Equals(fword(k[2], 7, 0), 1.0f, "pass-2 S = UV B (q = 1)");
            t.Equals(word(k[2], 9, 0), word(k[0], 9, 0), "pass 2 keeps XYZ"); });
        tc.Run("round toward zero: q = 1/3", [](TestCase &t)
               {
            ps2_native_world::testResetGroup();
            Vu1Image im = patch4(3.0f);
            std::vector<std::vector<uint8_t>> k;
            t.IsTrue(ps2_native_world::testModelTerrain(im.mem, 0x6b8, kTop, k), "servable");
            t.Equals(word(k[0], 1, 2), 0x3eaaaaaau, "ST.z = 1 * (1/3) rounded toward zero (nearest: 0x3eaaaaab)"); });
        tc.Run("clipping: all outside +x rejects, one outside needs the clipper, +z rejects", [](TestCase &t)
               {
            std::vector<std::vector<uint8_t>> k;
            ps2_native_world::testResetGroup();
            Vu1Image all = patch4();
            for (uint32_t i = 0; i < 16; ++i)
                all.put(kTop + i, 2.0f, 0.1f, 0.5f, 1.0f);
            t.IsTrue(ps2_native_world::testModelTerrain(all.mem, 0x6b8, kTop, k), "all +x: trivially rejected, servable");
            t.Equals(word(k[0], 9, 3), 0xffffffffu, "rejected triangle gets ADC");
            ps2_native_world::testResetGroup();
            Vu1Image one = patch4();
            one.put(kTop + 1, 2.0f, 0.0f, 0.5f, 1.0f);
            t.IsFalse(ps2_native_world::testModelTerrain(one.mem, 0x6b8, kTop, k), "one vertex +x: clipper polygon, VU1");
            ps2_native_world::testResetGroup();
            Vu1Image pz = patch4();
            pz.put(kTop + 1, 0.0f, 0.0f, 2.0f, 1.0f);
            t.IsTrue(ps2_native_world::testModelTerrain(pz.mem, 0x6b8, kTop, k), "a +z vertex rejects its triangles"); });
        tc.Run("follow-up 0x948 uses the base's packets; without a native base it is VU1's", [](TestCase &t)
               {
            std::vector<std::vector<uint8_t>> base, f;
            ps2_native_world::testResetGroup();
            Vu1Image im = patch4();
            t.IsFalse(ps2_native_world::testModelTerrain(im.mem, 0x948, kTop, f), "no base: VU1");
            t.IsTrue(ps2_native_world::testModelTerrain(im.mem, 0x6b8, kTop, base), "base");
            const uint32_t top2 = 300;
            im.put(top2 + 128, 2, 0, 0, 0); // M2: UV B = 2 x UV A
            im.put(top2 + 129, 0, 2, 0, 0);
            im.put(top2 + 130, 0, 0, 0, 0);
            im.put(top2 + 131, 0, 0, 0, 0);
            for (uint32_t i = 0; i < 16; ++i)
                im.put(top2 + 0x40 + i, 0.25f * (i % 4), 0.25f * (i / 4), 1, 1);
            t.IsTrue(ps2_native_world::testModelTerrain(im.mem, 0x948, top2, f), "follow-up served");
            t.Equals(f.size(), size_t(2), "two kicks");
            t.Equals(fword(f[0], 7, 0), 0.5f, "S = 2 x UV A of column 1");
            t.Equals(word(f[0], 9, 0), word(base[0], 9, 0), "XYZ from the base packet");
            t.Equals(word(f[0], 0, 2), 0x55550005u, "tag z from mem[8]"); });
        tc.Run("2b record: built from a job's kicks, parsed back in order; GIF packets aren't records", [](TestCase &t)
               {
            ps2_native_world::testResetGroup();
            Vu1Image im = patch4();
            std::vector<std::vector<uint8_t>> k;
            t.IsTrue(ps2_native_world::testModelTerrain(im.mem, 0x6b8, kTop, k), "servable");
            std::vector<uint8_t> cat;
            std::vector<uint32_t> sizes;
            for (const auto &p : k)
            {
                cat.insert(cat.end(), p.begin(), p.end());
                sizes.push_back(static_cast<uint32_t>(p.size()));
            }
            std::vector<uint8_t> rec;
            ps2_native_world::buildRecord(cat.data(), sizes.data(), static_cast<uint32_t>(sizes.size()), rec);
            t.IsTrue(ge1_is_native_record(rec.data(), static_cast<uint32_t>(rec.size())), "signature");
            t.Equals(rec.size() % 16, size_t(0), "16-byte aligned");
            std::vector<std::vector<uint8_t>> back;
            t.IsTrue(ge1_native_record_for_each(rec.data(), static_cast<uint32_t>(rec.size()),
                                                [&](const uint8_t *p, uint32_t n) { back.emplace_back(p, p + n); }),
                     "well formed");
            t.IsTrue(back == k, "same packets, same order");
            t.IsFalse(ge1_is_native_record(k[0].data(), static_cast<uint32_t>(k[0].size())), "a GIF packet is not a record");
            rec.resize(rec.size() - 16);
            t.IsFalse(ge1_native_record_for_each(rec.data(), static_cast<uint32_t>(rec.size()),
                                                 [](const uint8_t *, uint32_t) {}),
                      "truncated record rejected"); });
        tc.Run("compact record: dense fields == packet regs, expansion round-trips consumed bytes", [](TestCase &t)
               {
            ps2_native_world::testResetGroup();
            Vu1Image im = patch4();
            std::vector<std::vector<uint8_t>> k;
            t.IsTrue(ps2_native_world::testModelTerrain(im.mem, 0x6b8, kTop, k), "servable");
            std::vector<uint8_t> cat;
            std::vector<uint32_t> sizes;
            for (const auto &p : k)
            {
                cat.insert(cat.end(), p.begin(), p.end());
                sizes.push_back(static_cast<uint32_t>(p.size()));
            }
            std::vector<uint8_t> rec;
            ps2_native_world::buildCompactRecord(cat.data(), sizes.data(), static_cast<uint32_t>(sizes.size()), rec);
            t.IsTrue(ge1_is_compact_record(rec.data(), static_cast<uint32_t>(rec.size())), "signature");
            t.IsFalse(ge1_is_native_record(rec.data(), static_cast<uint32_t>(rec.size())), "not the GIF kind");
            t.Equals(rec.size() % 16, size_t(0), "16-byte aligned");
            std::vector<std::vector<uint8_t>> back;
            t.IsTrue(ge1_compact_record_for_each(rec.data(), static_cast<uint32_t>(rec.size()),
                                                 [&](const uint8_t *p, uint32_t n) { back.emplace_back(p, p + n); }),
                     "well formed");
            t.Equals(back.size(), k.size(), "same packets");
            for (size_t i = 0; i < k.size(); ++i)
            {
                const uint32_t nv = (static_cast<uint32_t>(k[i].size()) - 16u) / 48u;
                t.Equals(back[i].size(), size_t(16 + 32u * nv), "dense size");
                t.IsTrue(std::memcmp(back[i].data(), k[i].data(), 16) == 0, "tag verbatim");
                for (uint32_t v = 0; v < nv; ++v)
                {
                    Ge1CompactVertex d;
                    std::memcpy(&d, back[i].data() + 16 + 32u * v, 32);
                    const uint32_t *reg = reinterpret_cast<const uint32_t *>(k[i].data() + 16 + 48u * v);
                    t.Equals(d.S, reg[0], "S");
                    t.Equals(d.T, reg[1], "T");
                    t.Equals(d.Q, reg[2], "Q");
                    const uint32_t rgba = (reg[4] & 0xffu) | ((reg[5] & 0xffu) << 8) |
                                          ((reg[6] & 0xffu) << 16) | ((reg[7] & 0xffu) << 24);
                    t.Equals(d.RGBA, rgba, "RGBA packed");
                    t.Equals(d.X, reg[8], "X verbatim");
                    t.Equals(d.Y, reg[9], "Y verbatim");
                    t.Equals(d.Z, reg[10], "Z verbatim");
                    t.Equals(d.W3, reg[11], "W3 verbatim");
                }
                // Expansion: the tag verbatim, every consumed byte back.
                std::vector<uint8_t> exp(16 + 48u * nv);
                t.IsTrue(ge1_compact_expand_packet(back[i].data(), static_cast<uint32_t>(back[i].size()),
                                                   exp.data(), static_cast<uint32_t>(exp.size())),
                         "expands");
                t.IsTrue(std::memcmp(exp.data(), k[i].data(), 16) == 0, "tag back");
                for (uint32_t v = 0; v < nv; ++v)
                {
                    const uint32_t *g = reinterpret_cast<const uint32_t *>(k[i].data() + 16 + 48u * v);
                    const uint32_t *e = reinterpret_cast<const uint32_t *>(exp.data() + 16 + 48u * v);
                    t.Equals(e[0], g[0], "ST.w0 back");
                    t.Equals(e[1], g[1], "ST.w1 back");
                    t.Equals(e[2], g[2], "ST.w2 back");
                    t.Equals(e[4] & 0xffu, g[4] & 0xffu, "R back");
                    t.Equals(e[5] & 0xffu, g[5] & 0xffu, "G back");
                    t.Equals(e[6] & 0xffu, g[6] & 0xffu, "B back");
                    t.Equals(e[7] & 0xffu, g[7] & 0xffu, "A back");
                    t.Equals(e[8], g[8], "X back");
                    t.Equals(e[9], g[9], "Y back");
                    t.Equals(e[10], g[10], "Z back");
                    t.Equals(e[11], g[11], "W3 back");
                }
            }
            std::vector<uint8_t> bad = rec;
            bad.resize(bad.size() - 16);
            t.IsFalse(ge1_compact_record_for_each(bad.data(), static_cast<uint32_t>(bad.size()),
                                                  [](const uint8_t *, uint32_t) {}),
                      "truncated compact record rejected"); });
        tc.Run("keyed record (S4a): terrain patch wraps the compact record verbatim, facts == reference", [](TestCase &t)
               {
            ps2_native_world::testResetGroup();
            Vu1Image im = patch4();
            std::vector<std::vector<uint8_t>> k;
            t.IsTrue(ps2_native_world::testModelTerrain(im.mem, 0x6b8, kTop, k), "servable");
            checkKeyed(t, k); });
        tc.Run("keyed record (S4a): scenery header and clip variant, facts == reference", [](TestCase &t)
               {
            for (uint32_t flags : {0u, 0x20u})
            {
                ps2_native_world::testResetGroup();
                Vu1Image im = instance3(flags);
                std::vector<uint8_t> p;
                t.IsTrue(ps2_native_world::testModelScenery(im.mem, 0x2320, kSTop, 0, p), "header served");
                checkKeyed(t, {p});
            } });
        tc.Run("keyed record (S4a): hand-made list/fan/strip/sprite packets, ADC, flat, Q=0, NaN", [](TestCase &t)
               {
            auto V = [](float s, float tt, float q, uint8_t c, uint32_t x, uint32_t y, uint32_t z, bool adc) {
                return RefVert{s, tt, q, c, uint8_t(c + 1), uint8_t(c + 2), 0x80, x, y, z, 7, adc};
            };
            // Triangle list (IIP): the second triple's last vertex has ADC, so one triangle.
            std::vector<RefVert> list = {V(0.5f, 0.25f, 1, 10, 100, 200, 5, false), V(1, 1, 2, 10, 300, 210, 5, false),
                                         V(2, 1, 4, 10, 120, 400, 5, false), V(1, 2, 1, 20, 50, 60, 9, false),
                                         V(1, 2, 1, 20, 70, 80, 9, false), V(1, 2, 1, 20, 90, 99, 9, true)};
            // Fan (IIP), 5 vertices: (0,1,2), (0,2,3), (0,3,4); constant Z.
            std::vector<RefVert> fan;
            for (uint32_t i = 0; i < 5; ++i)
                fan.push_back(V(float(i), float(i) * 0.5f, 1.5f, 30, 1000 + 16 * i, 2000 - 8 * i, 77, false));
            // Strip, flat shaded (IIP=0), varying colour; vertex 0/1 ADC as the VU emits; Q=0 on one vertex; a NaN S.
            std::vector<RefVert> strip;
            for (uint32_t i = 0; i < 6; ++i)
                strip.push_back(V(i == 4 ? NAN : float(i), 1, i == 3 ? 0.0f : 2.0f, uint8_t(40 + 3 * i), 500 + i, 600 - i, 1000 + i, i < 2));
            std::vector<RefVert> sprite = {V(0, 0, 1, 5, 1, 2, 3, false), V(1, 1, 1, 5, 4, 5, 3, true)};
            checkKeyed(t, {gifPacket(3 | 8, list), gifPacket(5 | 8, fan), gifPacket(4, strip), gifPacket(6, sprite)});
            // The same strip with IIP: every referenced vertex's colour counts.
            checkKeyed(t, {gifPacket(4 | 8, strip)}); });
        tc.Run("compact record: scenery packets dense identically", [](TestCase &t)
               {
            ps2_native_world::testResetGroup();
            Vu1Image im = instance3(0);
            std::vector<uint8_t> p;
            t.IsTrue(ps2_native_world::testModelScenery(im.mem, 0x2320, kSTop, 0, p), "header served");
            const uint32_t sz = static_cast<uint32_t>(p.size());
            std::vector<uint8_t> rec;
            ps2_native_world::buildCompactRecord(p.data(), &sz, 1, rec);
            std::vector<std::vector<uint8_t>> back;
            t.IsTrue(ge1_compact_record_for_each(rec.data(), static_cast<uint32_t>(rec.size()),
                                                 [&](const uint8_t *q, uint32_t n) { back.emplace_back(q, q + n); }),
                     "well formed");
            t.Equals(back.size(), size_t(1), "one packet");
            t.IsTrue(std::memcmp(back[0].data(), p.data(), 16) == 0, "tag verbatim");
            Ge1CompactVertex d;
            std::memcpy(&d, back[0].data() + 16, 32);
            t.Equals(d.W3, 0x0000ffffu, "scenery ADC W3 verbatim");
            t.Equals(d.X, 32760u, "vertex 0 x");
            // The clip variant's 0xffffffff ADC word rides the same field.
            ps2_native_world::testResetGroup();
            Vu1Image in = instance3(0x20);
            std::vector<uint8_t> pc;
            t.IsTrue(ps2_native_world::testModelScenery(in.mem, 0x2320, kSTop, 0, pc), "clip variant served");
            const uint32_t szc = static_cast<uint32_t>(pc.size());
            std::vector<uint8_t> recc;
            ps2_native_world::buildCompactRecord(pc.data(), &szc, 1, recc);
            std::vector<std::vector<uint8_t>> backc;
            t.IsTrue(ge1_compact_record_for_each(recc.data(), static_cast<uint32_t>(recc.size()),
                                                 [&](const uint8_t *q, uint32_t n) { backc.emplace_back(q, q + n); }),
                     "well formed");
            std::memcpy(&d, backc[0].data() + 16, 32);
            t.Equals(d.W3, 0xffffffffu, "clip ADC W3 verbatim"); });
        tc.Run("scenery header 0x2320: one packet, ADC, XYZ, ST, RGBA; second pass rewrites ST only", [](TestCase &t)
               {
            ps2_native_world::testResetGroup();
            Vu1Image im = instance3(0);
            std::vector<uint8_t> p, p2;
            t.IsFalse(ps2_native_world::testModelScenery(im.mem, 0x22c8, kSTop, 0, p), "continuation without a native header: VU1");
            t.IsTrue(ps2_native_world::testModelScenery(im.mem, 0x2320, kSTop, 0, p), "header served");
            t.Equals(p.size(), size_t(16 * 10), "tag + 3 vertices");
            t.Equals(word(p, 0, 1), 0x30001234u, "the chunk's own tag");
            t.Equals(word(p, 3, 0), 32760u, "vertex 0 x = 2047.5 * 16");
            t.Equals(word(p, 3, 3), 0x0000ffffu, "strip vertex 0 ADC");
            t.Equals(word(p, 6, 0), 40952u, "vertex 1 x = (1024 * 0.5 + 2047.5) * 16");
            t.Equals(word(p, 9, 3), 16u, "vertex 2 drawn (w = FTOI4(1))");
            t.Equals(fword(p, 4, 0), 0.5f, "vertex 1 S");
            t.Equals(fword(p, 4, 1), 0.25f, "vertex 1 T");
            t.Equals(fword(p, 4, 3), 1.0f, "Q lane = 1 x q");
            t.Equals(word(p, 5, 0), 0x10u, "vertex 1 RGBA raw");
            im.putw(kSTop + 0x1b + 3 + 4, 8192, 0, 0, 0); // pass 2 UVs: vertex 1 u = 2.0
            im.putw(31, 0, 0x12345678u, 0, 0);
            t.IsTrue(ps2_native_world::testModelScenery(im.mem, 0x2270, kSTop, 1, p2), "second pass served");
            t.Equals(fword(p2, 4, 0), 2.0f, "S from the new UVs x ST.w");
            t.Equals(word(p2, 0, 1), 0x12345678u, "tag y from mem[31].y");
            t.Equals(word(p2, 6, 0), word(p, 6, 0), "XYZ kept");
            t.IsFalse(ps2_native_world::testModelScenery(im.mem, 0x2270, kSTop + 0x40, 1, p2), "second pass at another TOP: VU1"); });
        tc.Run("scenery: lit instances run on VU1; clip variant needs the clipper for a partly outside triangle", [](TestCase &t)
               {
            std::vector<uint8_t> p;
            ps2_native_world::testResetGroup();
            Vu1Image lit = instance3(1);
            t.IsFalse(ps2_native_world::testModelScenery(lit.mem, 0x2320, kSTop, 0, p), "flags bit 0 (lit): VU1");
            Vu1Image in = instance3(0x20);
            t.IsTrue(ps2_native_world::testModelScenery(in.mem, 0x2320, kSTop, 0, p), "clip variant, inside: served");
            t.Equals(word(p, 3, 3), 0xffffffffu, "clip variant strip ADC");
            Vu1Image out = instance3(0x20);
            out.putw(kSTop + 0x1b + 3 + 1, 65536, 0, 0, 0x3f800000u); // x = 2.0 > w
            t.IsFalse(ps2_native_world::testModelScenery(out.mem, 0x2320, kSTop, 0, p), "one vertex outside: clipper, VU1"); });
        tc.Run("unsupported shape and unmodelled entries run on VU1", [](TestCase &t)
               {
            std::vector<std::vector<uint8_t>> k;
            ps2_native_world::testResetGroup();
            Vu1Image im = patch4();
            im.putw(12, 15, 8, 45, 24);
            t.IsFalse(ps2_native_world::testModelTerrain(im.mem, 0x6b8, kTop, k), "nloop A != strips x 2n");
            Vu1Image ok = patch4();
            t.IsFalse(ps2_native_world::testModelTerrain(ok.mem, 0x868, kTop, k), "mixed patch entry");
            t.IsFalse(ps2_native_world::testModelTerrain(ok.mem, 0x610, kTop, k), "base-only entry"); });
        // NRS1: optional pair dump for ge1_compact_test's file mode. Test-only:
        // NRS1_DUMP_PAIRS=<path> writes the model packets of the synthetic
        // images above (u32 npackets, then u32 nbytes + bytes each). No game
        // data; the file is regenerated, never committed.
        tc.Run("compact pairs dump (NRS1_DUMP_PAIRS only)", [](TestCase &t)
               {
            const char *dir = std::getenv("NRS1_DUMP_PAIRS");
            if (!dir || !dir[0])
                return;
            std::vector<std::vector<uint8_t>> packets;
            {
                ps2_native_world::testResetGroup();
                Vu1Image im = patch4();
                std::vector<std::vector<uint8_t>> k;
                t.IsTrue(ps2_native_world::testModelTerrain(im.mem, 0x6b8, kTop, k), "servable");
                packets.insert(packets.end(), k.begin(), k.end());
            }
            {
                ps2_native_world::testResetGroup();
                Vu1Image im = instance3(0);
                std::vector<uint8_t> p;
                t.IsTrue(ps2_native_world::testModelScenery(im.mem, 0x2320, kSTop, 0, p), "served");
                packets.push_back(p);
            }
            {
                ps2_native_world::testResetGroup();
                Vu1Image in = instance3(0x20);
                std::vector<uint8_t> p;
                t.IsTrue(ps2_native_world::testModelScenery(in.mem, 0x2320, kSTop, 0, p), "served");
                packets.push_back(p);
            }
            const std::string path = std::string(dir) + "/nrs1-pairs.bin";
            std::FILE *f = std::fopen(path.c_str(), "wb");
            t.IsTrue(f != nullptr, "dump opened");
            if (!f)
                return;
            const uint32_t np = static_cast<uint32_t>(packets.size());
            std::fwrite(&np, 4, 1, f);
            for (const auto &p : packets)
            {
                const uint32_t n = static_cast<uint32_t>(p.size());
                std::fwrite(&n, 4, 1, f);
                std::fwrite(p.data(), 1, n, f);
            }
            std::fclose(f); }); });
}
