#include "MiniTest.h"
#include "ps2_native_world.h"
#include "runtime/gs/ge1_gs_api.h"

#include <cstdint>
#include <cstring>
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
            t.IsFalse(ps2_native_world::testModelTerrain(ok.mem, 0x610, kTop, k), "base-only entry"); }); });
}
