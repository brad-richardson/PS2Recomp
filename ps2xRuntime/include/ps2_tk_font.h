#pragma once
// tk-menu-2: shared FEFONT module (Brad 10-16 amendment).
//
// Primary: the game's own FEFONT as the game displays it — glyph metrics
// from the user's disc DATA/FONTS/FEFONT.SFN plus the atlas pixels the
// guest uploads, baked offline into TKFONT1 by tools/ssx3/build_tk_font.py
// (generator code only; no pixels or metric tables in git). The bake holds
// the stock 1x raster (upscale off) and the raster-edge 4x (upscale on,
// what the stock menus show).
// Fallback: Neuropol CC0 (ps2xRuntime/resources/fonts/Neuropol-CC0.otf)
// before any staged file exists. raylib's built-in font is the logged last
// resort only.
//
// Pure logic here (no raylib): TKFONT1 parse + SFN-unit layout, unit-tested
// beside ps2_ssx3_tricky_list_tests. The raylib texture upload + quad draw
// live with the caller (ps2_runtime.cpp overlay today).
// SET2 hook points (follow-up, NOT converted here): the settings panel row
// draw in ps2_runtime.cpp (set2 panel block) can call tkMeasure/tkDraw
// through the same loaded instance once its strings are staged.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ps2_tk_font
{

struct Glyph
{
    uint16_t code = 0;
    uint16_t x = 0, y = 0, w = 0, h = 0; // 1x cell (4x cell = *4)
    int16_t xoff = 0, yoff = 0;
    uint16_t adv = 0;
};

// SFN-unit metrics of one string (same layout math the bake preview uses:
// pen advances by advance*k; the ink band is [top, bot) in yoff units).
struct Metrics
{
    float width = 0.0f; // sum of advances, SFN units
    float top = 0.0f, bot = 10.0f;
    bool anyInk = false;
};

struct Baked
{
    bool ok = false;
    uint32_t w1 = 0, h1 = 0, w4 = 0, h4 = 0;
    std::vector<Glyph> glyphs;
    std::string px1; // w1*h1 RGBA (stock raster)
    std::string px4; // w4*h4 RGBA (raster-edge upscale)
};

inline const Glyph *findGlyph(const Baked &b, uint32_t code)
{
    for (const Glyph &g : b.glyphs)
        if (g.code == code)
            return &g;
    return nullptr;
}

// Parse a TKFONT1 blob. Returns false on any shape error (never trusts the
// file; the staged bin is lane scratch, not a trust boundary, but a short
// read must not become an OOB).
inline bool parseTkFont(const uint8_t *d, size_t n, Baked &out)
{
    out = Baked();
    if (!d || n < 28 || std::memcmp(d, "TKFONT1\0", 8) != 0)
        return false;
    auto u32 = [&](size_t o) -> uint32_t {
        uint32_t v = 0;
        if (o + 4 <= n)
            std::memcpy(&v, d + o, 4);
        return v;
    };
    const uint32_t w1 = u32(8), h1 = u32(12), w4 = u32(16), h4 = u32(20);
    const uint32_t ng = u32(24);
    if (w1 == 0 || h1 == 0 || w4 == 0 || h4 == 0 || ng == 0 || ng > 1024)
        return false;
    if (w1 > 2048 || h1 > 2048 || w4 > 8192 || h4 > 8192)
        return false;
    const size_t head = 28 + static_cast<size_t>(ng) * 16;
    const size_t px1 = static_cast<size_t>(w1) * h1 * 4;
    const size_t px4 = static_cast<size_t>(w4) * h4 * 4;
    if (head > n || px1 > n - head || px4 > n - head - px1)
        return false;
    out.w1 = w1;
    out.h1 = h1;
    out.w4 = w4;
    out.h4 = h4;
    out.glyphs.reserve(ng);
    for (uint32_t i = 0; i < ng; ++i)
    {
        const size_t o = 28 + static_cast<size_t>(i) * 16;
        Glyph g;
        uint16_t code, x, y, w, h, adv;
        int16_t xoff, yoff;
        std::memcpy(&code, d + o, 2);
        std::memcpy(&x, d + o + 2, 2);
        std::memcpy(&y, d + o + 4, 2);
        std::memcpy(&w, d + o + 6, 2);
        std::memcpy(&h, d + o + 8, 2);
        std::memcpy(&xoff, d + o + 10, 2);
        std::memcpy(&yoff, d + o + 12, 2);
        std::memcpy(&adv, d + o + 14, 2);
        g.code = code;
        g.x = x;
        g.y = y;
        g.w = w;
        g.h = h;
        g.xoff = xoff;
        g.yoff = yoff;
        g.adv = adv;
        if (g.x + g.w > w1 || g.y + g.h > h1)
            return false;
        out.glyphs.push_back(g);
    }
    out.px1.assign(reinterpret_cast<const char *>(d + head), px1);
    out.px4.assign(reinterpret_cast<const char *>(d + head + px1), px4);
    out.ok = true;
    return true;
}

inline Metrics measure(const Baked &b, const char *s)
{
    Metrics m;
    if (!s)
        return m;
    float top = 1e9f, bot = -1e9f;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(s); *p; ++p)
    {
        const Glyph *g = findGlyph(b, *p);
        if (!g)
            continue;
        m.width += g->adv;
        if (g->w > 1)
        {
            if (g->yoff < top)
                top = g->yoff;
            if (g->yoff + g->h > bot)
                bot = g->yoff + g->h;
        }
    }
    if (bot > top)
    {
        m.top = top;
        m.bot = bot;
        m.anyInk = true;
    }
    return m;
}

// Scale so the ink band fills targetH px.
inline float scaleFor(const Metrics &m, float targetH)
{
    const float band = m.bot - m.top;
    return (m.anyInk && band > 0 && targetH > 0) ? targetH / band : 0.0f;
}

} // namespace ps2_tk_font
