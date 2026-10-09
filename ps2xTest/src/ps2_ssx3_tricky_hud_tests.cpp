#include "MiniTest.h"
#include "ps2_ssx3_tricky_hud.h"
#include "runtime/gs/ge1_gs_api.h"
#include <cstddef>
#include <utility>

// TK43a: Tricky meter reskin compositor/reader/atlas tests. The atlas is
// synthetic solid-color rects (no game art); rect positions mirror the
// staged trickyhud.bin layout in make_trickyhud_atlas.py.
// TK43c: red-letter cells, the uber CD tap, the letter machine, the diag
// preset, the fanfare flash.
namespace
{
using ps2_ssx3_tricky_hud::Atlas;
using ps2_ssx3_tricky_hud::Rect;

void paintRect(std::vector<uint8_t> &px, const Rect &r, uint8_t v)
{
    for (int y = r.y; y < r.y + r.h; ++y)
        for (int x = r.x; x < r.x + r.w; ++x)
        {
            uint8_t *d = &px[(static_cast<size_t>(y) * 256u + static_cast<size_t>(x)) * 4u];
            d[0] = d[1] = d[2] = v;
            d[3] = 255;
        }
}

Atlas synthAtlas()
{
    using namespace ps2_ssx3_tricky_hud;
    std::vector<uint8_t> px(256u * 256u * 4u, 0);
    paintRect(px, ringRect(0), 10); // gold
    paintRect(px, ringRect(1), 30); // orange (bands 1 and 2 share the cell)
    paintRect(px, ringRect(3), 40); // red
    paintRect(px, silverRingRect(), 50);
    paintRect(px, poleRect(), 70);
    paintRect(px, letterRect(0), 60);
    paintRect(px, letterRect(1), 61);
    paintRect(px, letterRect(2), 62);
    paintRect(px, letterRect(3), 63);
    paintRect(px, letterRect(4), 64);
    paintRect(px, letterRect(5), 65);
    paintRect(px, litLetterRect(0), 140); // lit red T..Y
    paintRect(px, litLetterRect(1), 141);
    paintRect(px, litLetterRect(2), 142);
    paintRect(px, litLetterRect(3), 143);
    paintRect(px, litLetterRect(4), 144);
    paintRect(px, litLetterRect(5), 145);
    paintRect(px, jewelGreyRect(), 90);
    paintRect(px, jewelRedRect(), 100);
    paintRect(px, pillRect(), 110);
    paintRect(px, pillGreyRect(), 130);
    paintRect(px, snowflakeRect(), 120);
    std::vector<uint8_t> blob;
    const char magic[8] = {'T', 'K', 'H', 'U', 'D', '2', '\0', '\0'};
    blob.insert(blob.end(), magic, magic + 8);
    const uint32_t wh[3] = {256u, 256u, 0u};
    const uint8_t *wbp = reinterpret_cast<const uint8_t *>(wh);
    blob.insert(blob.end(), wbp, wbp + 12);
    blob.insert(blob.end(), px.begin(), px.end());
    return parseAtlas(blob.data(), blob.size());
}

std::vector<uint8_t> blankFrame(int w, int h)
{
    std::vector<uint8_t> f(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
    for (size_t i = 0; i < f.size(); i += 4)
    {
        f[i] = 1;
        f[i + 1] = 2;
        f[i + 2] = 3;
        f[i + 3] = 255;
    }
    return f;
}

uint8_t pxAt(const std::vector<uint8_t> &f, int w, int x, int y, int c = 0)
{
    return f[(static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4u + static_cast<size_t>(c)];
}

int ringCy(int i) { return static_cast<int>(std::lround(395.0f - 16.5f * i)); }
int ringCx() { return 588; }

// THD1: non-grey atlas for the BGRA-lane test (synthAtlas is grey, which is
// swap-invariant and would pass vacuously). Same layout, distinct R/G/B per
// cell; the splash and one letter are semi-transparent to exercise blending.
void paintRectRGBA(std::vector<uint8_t> &px, const Rect &r, uint8_t cr, uint8_t cg, uint8_t cb,
                   uint8_t ca)
{
    for (int y = r.y; y < r.y + r.h; ++y)
        for (int x = r.x; x < r.x + r.w; ++x)
        {
            uint8_t *d = &px[(static_cast<size_t>(y) * 256u + static_cast<size_t>(x)) * 4u];
            d[0] = cr;
            d[1] = cg;
            d[2] = cb;
            d[3] = ca;
        }
}

Atlas tintedAtlas()
{
    using namespace ps2_ssx3_tricky_hud;
    std::vector<uint8_t> px(256u * 256u * 4u, 0);
    paintRectRGBA(px, ringRect(0), 200, 40, 30, 255); // gold band (red-heavy)
    paintRectRGBA(px, ringRect(1), 220, 120, 30, 255); // orange bands 1+2
    paintRectRGBA(px, ringRect(3), 200, 30, 30, 255); // red band
    paintRectRGBA(px, silverRingRect(), 180, 190, 200, 255);
    paintRectRGBA(px, poleRect(), 90, 90, 110, 255);
    for (int i = 0; i < 6; ++i)
    {
        const uint8_t v = static_cast<uint8_t>(60 + i * 10);
        paintRectRGBA(px, letterRect(i), v, 120, static_cast<uint8_t>(200 - i * 5), 255);
        paintRectRGBA(px, litLetterRect(i), 200, static_cast<uint8_t>(30 + i * 5), 40, 255);
    }
    paintRectRGBA(px, letterRect(2), 70, 130, 190, 96); // semi chrome I
    paintRectRGBA(px, jewelGreyRect(), 170, 180, 190, 255);
    paintRectRGBA(px, jewelRedRect(), 210, 40, 50, 255);
    paintRectRGBA(px, pillRect(), 200, 40, 40, 255);
    paintRectRGBA(px, pillGreyRect(), 150, 155, 160, 255);
    paintRectRGBA(px, snowflakeRect(), 218, 123, 31, 128); // semi orange flake
    std::vector<uint8_t> blob;
    const char magic[8] = {'T', 'K', 'H', 'U', 'D', '2', '\0', '\0'};
    blob.insert(blob.end(), magic, magic + 8);
    const uint32_t wh[3] = {256u, 256u, 0u};
    const uint8_t *wbp = reinterpret_cast<const uint8_t *>(wh);
    blob.insert(blob.end(), wbp, wbp + 12);
    blob.insert(blob.end(), px.begin(), px.end());
    return parseAtlas(blob.data(), blob.size());
}

// HUD2: fast deterministic frame fill (xorshift32 per u32; the sweep fills
// thousands of frames, so per-byte LCG is the bottleneck, not the stamps).
void randomFrame(std::vector<uint8_t> &f, uint32_t seed)
{
    uint32_t x = seed != 0u ? seed : 0x9e3779b9u;
    uint32_t *w = reinterpret_cast<uint32_t *>(f.data());
    const size_t n = f.size() / 4u;
    for (size_t i = 0; i < n; ++i)
    {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        w[i] = x;
    }
}

// HUD2: fully random atlas bytes (fixed cells, random content) for maximum
// rounding-path coverage in the cached-vs-direct sweep.
Atlas randomAtlas(uint32_t seed)
{
    using namespace ps2_ssx3_tricky_hud;
    std::vector<uint8_t> px(256u * 256u * 4u);
    uint32_t rng = seed;
    for (size_t i = 0; i < px.size(); ++i)
    {
        rng = rng * 1664525u + 1013904223u;
        px[i] = static_cast<uint8_t>(rng >> 24);
    }
    std::vector<uint8_t> blob;
    const char magic[8] = {'T', 'K', 'H', 'U', 'D', '2', '\0', '\0'};
    blob.insert(blob.end(), magic, magic + 8);
    const uint32_t wh[3] = {256u, 256u, 0u};
    const uint8_t *wbp = reinterpret_cast<const uint8_t *>(wh);
    blob.insert(blob.end(), wbp, wbp + 12);
    blob.insert(blob.end(), px.begin(), px.end());
    return parseAtlas(blob.data(), blob.size());
}
} // namespace

void register_ps2_ssx3_tricky_hud_tests()
{
    MiniTest::Case("Ps2Ssx3TrickyHud", [](TestCase &tc)
                   {
        tc.Run("atlas parse accepts good, refuses bad", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_hud;
            Atlas a = synthAtlas();
            t.IsTrue(a.ok, "synthetic atlas parses");
            t.IsTrue(a.w == 256 && a.h == 256, "dims");
            std::vector<uint8_t> blob;
            const char magic[8] = {'T', 'K', 'H', 'U', 'D', '2', '\0', '\0'};
            blob.insert(blob.end(), magic, magic + 8);
            const uint32_t wh[3] = {256u, 256u, 0u};
            blob.insert(blob.end(), reinterpret_cast<const uint8_t *>(wh),
                        reinterpret_cast<const uint8_t *>(wh) + 12);
            blob.insert(blob.end(), 256u * 256u * 4u, 0);
            t.IsTrue(!parseAtlas(nullptr, 0).ok, "null");
            blob[0] = 'X';
            t.IsTrue(!parseAtlas(blob.data(), blob.size()).ok, "bad magic");
            blob[0] = 'T';
            blob[5] = '1';
            t.IsTrue(!parseAtlas(blob.data(), blob.size()).ok, "TKHUD1 refused");
            blob[5] = '2';
            t.IsTrue(!parseAtlas(blob.data(), 100).ok, "truncated");
            blob[9] = 0; // w = 0x100 LE; clear the high byte -> w = 0
            t.IsTrue(!parseAtlas(blob.data(), blob.size()).ok, "zero width"); });});

    MiniTest::Case("Ps2Ssx3TrickyHudReader", [](TestCase &tc)
                   {
        tc.Run("reader accepts valid, refuses invalid", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_hud;
            std::vector<uint8_t> ram(0x10000, 0);
            const uint32_t slot = 0x100u, R = 0x1000u;
            std::memcpy(&ram[slot], &R, 4);
            auto setWords = [&](float fill, int32_t level) {
                std::memcpy(&ram[R + kRiderFillOff], &fill, 4);
                std::memcpy(&ram[R + kRiderUberOff], &level, 4);
            };
            setWords(0.5f, 0);
            MeterFrame f = readMeterFrame(ram.data(), ram.size(), slot);
            t.IsTrue(f.ok && f.fill == 0.5f && f.level == 0, "valid half");
            setWords(1.0f, 10);
            f = readMeterFrame(ram.data(), ram.size(), slot);
            t.IsTrue(f.ok, "valid full/10");
            setWords(std::nanf(""), 0);
            t.IsTrue(!readMeterFrame(ram.data(), ram.size(), slot).ok, "NaN fill");
            setWords(-0.1f, 0);
            f = readMeterFrame(ram.data(), ram.size(), slot);
            t.IsTrue(f.ok && f.fill == 0.0f, "negative fill clamps, still draws");
            setWords(1.1f, 0);
            f = readMeterFrame(ram.data(), ram.size(), slot);
            t.IsTrue(f.ok && f.fill == 1.0f, "fill > 1 clamps");
            setWords(0.5f, -1);
            t.IsTrue(!readMeterFrame(ram.data(), ram.size(), slot).ok, "level -1");
            setWords(0.5f, 11);
            t.IsTrue(!readMeterFrame(ram.data(), ram.size(), slot).ok, "level 11");
            setWords(0.5f, 0);
            const uint32_t zero = 0;
            std::memcpy(&ram[slot], &zero, 4);
            t.IsTrue(!readMeterFrame(ram.data(), ram.size(), slot).ok, "null rider");
            std::memcpy(&ram[slot], &R, 4);
            t.IsTrue(!readMeterFrame(ram.data(), ram.size(), 0xffffffu).ok, "slot OOB");
            t.IsTrue(!readMeterFrame(nullptr, 0, slot).ok, "null ram");
            const uint32_t far = 0x1fffff0u;
            std::memcpy(&ram[slot], &far, 4);
            t.IsTrue(!readMeterFrame(ram.data(), ram.size(), slot).ok, "rider OOB"); });});

    MiniTest::Case("Ps2Ssx3TrickyHudForce", [](TestCase &tc)
                   {
        tc.Run("diag force parses fill,level, clamps, refuses junk", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_hud;
            ForceValue v = parseForce("1,1");
            t.IsTrue(v.ok && v.fill == 1.0f && v.level == 1, "full");
            v = parseForce("0.56,0");
            t.IsTrue(v.ok && v.fill == 0.56f && v.level == 0, "half");
            v = parseForce("0,0");
            t.IsTrue(v.ok && v.fill == 0.0f && v.level == 0, "empty");
            v = parseForce("2,99");
            t.IsTrue(v.ok && v.fill == 1.0f && v.level == 10, "clamps high");
            v = parseForce("-1,-5");
            t.IsTrue(v.ok && v.fill == 0.0f && v.level == 0, "clamps low");
            t.IsTrue(!parseForce(nullptr).ok, "null");
            t.IsTrue(!parseForce("").ok, "empty");
            t.IsTrue(!parseForce("x").ok, "junk");
            t.IsTrue(!parseForce("1").ok, "no level");
            t.IsTrue(!parseForce("1,1x").ok, "trailing junk");
            t.IsTrue(!parseForce("nan,0").ok, "NaN"); });});

    MiniTest::Case("Ps2Ssx3TrickyHudCompose", [](TestCase &tc)
                   {
        tc.Run("empty meter: all silver, grey jewel, no pill/splash", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_hud;
            Atlas a = synthAtlas();
            auto f = blankFrame(640, 480);
            composeOverlay(f.data(), 640, 480, a, 0.0f, false, 1000u, 0u, 0, 0u);
            for (int i = 0; i < 16; ++i)
                t.IsTrue(pxAt(f, 640, ringCx(), ringCy(i)) == 50, "ring silver");
            t.IsTrue(pxAt(f, 640, 588, 125) == 90, "jewel grey");
            t.IsTrue(pxAt(f, 640, 588, 415) == 130, "grey pill slot");
            t.IsTrue(pxAt(f, 640, 500, 150) == 1, "no splash");
            t.IsTrue(pxAt(f, 640, 600, 20) == 1, "score kept");
            t.IsTrue(pxAt(f, 640, 586, 156) == 70, "pole"); });
        tc.Run("half meter: 8 lit in band order, 8 silver", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_hud;
            Atlas a = synthAtlas();
            auto f = blankFrame(640, 480);
            composeOverlay(f.data(), 640, 480, a, 0.5f, false, 1000u, 0u, 0, 0u);
            const uint8_t want[16] = {10, 10, 10, 10, 30, 30, 30, 30, 50, 50, 50, 50, 50, 50, 50, 50};
            for (int i = 0; i < 16; ++i)
                t.IsTrue(pxAt(f, 640, ringCx(), ringCy(i)) == want[i], "ring band"); });
        tc.Run("full meter: bands, red jewel, pill, splash", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_hud;
            Atlas a = synthAtlas();
            auto f = blankFrame(640, 480);
            composeOverlay(f.data(), 640, 480, a, 1.0f, true, 1000u, 1045u, 0, 0u);
            const uint8_t want[16] = {10, 10, 10, 10, 30, 30, 30, 30, 30, 30, 30, 30, 40, 40, 40, 40};
            t.IsTrue(pxAt(f, 640, 588, 125) == 120, "splash covers jewel center");
            t.IsTrue(pxAt(f, 640, 535, 150) == 120, "splash");
            t.IsTrue(pxAt(f, 640, 630, 150) == 120, "splash right arm");
            t.IsTrue(pxAt(f, 640, 588, ringCy(15)) == 120, "splash covers top ring");
            t.IsTrue(pxAt(f, 640, 500, 110) == 1, "nothing above flake top");
            t.IsTrue(pxAt(f, 640, 588, 410) == 110, "pill");
            auto g = blankFrame(640, 480);
            composeOverlay(g.data(), 640, 480, a, 1.0f, true, 2000u, 1045u, 0, 0u);
            for (int i = 0; i < 16; ++i)
                t.IsTrue(pxAt(g, 640, ringCx(), ringCy(i)) == want[i], "ring band");
            t.IsTrue(pxAt(g, 640, 588, 125) == 100, "jewel red after splash");
            t.IsTrue(pxAt(g, 640, 535, 150) == 1, "splash expired"); });
        tc.Run("jewel pulses when full", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_hud;
            Atlas a = synthAtlas();
            auto f = blankFrame(640, 480);
            composeOverlay(f.data(), 640, 480, a, 1.0f, true, 0u, 0u, 0, 0u);
            t.IsTrue(pxAt(f, 640, 575, 120) == 100, "bright phase");
            auto g = blankFrame(640, 480);
            composeOverlay(g.data(), 640, 480, a, 1.0f, true, 8u, 0u, 0, 0u);
            t.IsTrue(pxAt(g, 640, 575, 120) == 82, "dim phase"); });
        tc.Run("arch letters native on the recording arc", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_hud;
            Atlas a = synthAtlas();
            auto f = blankFrame(640, 480);
            composeOverlay(f.data(), 640, 480, a, 0.0f, false, 1000u, 0u, 0, 0u);
            // Raw cell values (no tint): T 527,94 R 550,83 I 565,72 C 579,79
            // K 595,73 Y 607,94, each at its native cell size.
            t.IsTrue(pxAt(f, 640, 540, 105) == 60, "T");
            t.IsTrue(pxAt(f, 640, 558, 95) == 61, "R");
            t.IsTrue(pxAt(f, 640, 570, 80) == 62, "I");
            t.IsTrue(pxAt(f, 640, 588, 90) == 63, "C");
            t.IsTrue(pxAt(f, 640, 605, 80) == 64, "K");
            t.IsTrue(pxAt(f, 640, 625, 110) == 65, "Y");
            t.IsTrue(pxAt(f, 640, 570, 74) == 62, "I raised");
            t.IsTrue(pxAt(f, 640, 540, 90) == 1, "T starts lower");
            t.IsTrue(pxAt(f, 640, 563, 78, 1) == 2, "gap shows smear, no panel"); });
        tc.Run("label smear covers band, feathers, spares surroundings", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_hud;
            Atlas a = synthAtlas();
            std::vector<uint8_t> f(static_cast<size_t>(640) * 480u * 4u);
            for (int y = 0; y < 480; ++y)
                for (int x = 0; x < 640; ++x)
                {
                    uint8_t *d = &f[(static_cast<size_t>(y) * 640u + static_cast<size_t>(x)) * 4u];
                    if (x <= 534) // sky left
                    {
                        d[0] = 200;
                        d[1] = 0;
                        d[2] = 0;
                    }
                    else if (x >= 630) // sky right
                    {
                        d[0] = 0;
                        d[1] = 0;
                        d[2] = 200;
                    }
                    else // the "label"
                    {
                        d[0] = 0;
                        d[1] = 200;
                        d[2] = 0;
                    }
                    d[3] = 255;
                }
            composeOverlay(f.data(), 640, 480, a, 0.0f, false, 1000u, 0u, 0, 0u);
            // (545,70): left of R, above T, t=9/92 -> (180,0,20), label green gone.
            t.IsTrue(pxAt(f, 640, 545, 70) == 180, "smear mid r");
            t.IsTrue(pxAt(f, 640, 545, 70, 1) == 0, "smear mid g");
            t.IsTrue(pxAt(f, 640, 545, 70, 2) == 20, "smear mid b");
            t.IsTrue(pxAt(f, 640, 530, 60) == 200, "left of smear kept");
            t.IsTrue(pxAt(f, 640, 635, 60, 2) == 200, "right of smear kept");
            t.IsTrue(pxAt(f, 640, 582, 20, 1) == 200, "above smear kept");
            // (582,50): top feather row a=1/3 over green -> (33,133,33).
            t.IsTrue(pxAt(f, 640, 582, 50) == 33, "feather r");
            t.IsTrue(pxAt(f, 640, 582, 50, 1) == 133, "feather g");
            t.IsTrue(pxAt(f, 640, 582, 50, 2) == 33, "feather b"); });
        tc.Run("coil cover hides the stock meter behind the rings", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_hud;
            Atlas a = synthAtlas();
            std::vector<uint8_t> f(static_cast<size_t>(640) * 480u * 4u);
            for (int y = 0; y < 480; ++y)
                for (int x = 0; x < 640; ++x)
                {
                    uint8_t *d = &f[(static_cast<size_t>(y) * 640u + static_cast<size_t>(x)) * 4u];
                    d[0] = x <= 551 ? 200 : 0;              // sky left (cover x0-2 = 551)
                    d[1] = x > 551 && x < 610 ? 200 : 0;    // the stock coil (green)
                    d[2] = x >= 610 ? 200 : 0;              // sky right (cover x1+2 = 610)
                    d[3] = 255;
                }
            composeOverlay(f.data(), 640, 480, a, 0.0f, false, 1000u, 0u, 0, 0u);
            // (570,386): ring gap (ring 0 y389, ring 1 ends y385), off the pole;
            // t = 17/55 -> (138,0,62), stock green gone.
            t.IsTrue(pxAt(f, 640, 570, 386) == 138, "gap r");
            t.IsTrue(pxAt(f, 640, 570, 386, 1) == 0, "gap g");
            t.IsTrue(pxAt(f, 640, 570, 386, 2) == 62, "gap b");
            // (556,300): left of the rings (x563), t = 3/55 -> (189,0,11).
            t.IsTrue(pxAt(f, 640, 556, 300) == 189, "left sliver r");
            t.IsTrue(pxAt(f, 640, 556, 300, 1) == 0, "left sliver g");
            t.IsTrue(pxAt(f, 640, 552, 300, 1) == 200, "left of cover kept");
            t.IsTrue(pxAt(f, 640, 580, 450, 1) == 200, "below cover kept");
            t.IsTrue(pxAt(f, 640, ringCx(), ringCy(0)) == 50, "rings draw over the cover"); });
        tc.Run("layout scales to export size", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_hud;
            Atlas a = synthAtlas();
            auto f = blankFrame(1280, 960);
            composeOverlay(f.data(), 1280, 960, a, 1.0f, false, 1000u, 0u, 0, 0u);
            t.IsTrue(pxAt(f, 1280, 1176, 2 * ringCy(0)) == 10, "bottom ring at 2x");
            t.IsTrue(pxAt(f, 1280, 1176, 2 * ringCy(15)) == 40, "top ring at 2x"); });
        tc.Run("faults draw nothing and never crash", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_hud;
            Atlas bad;
            auto f = blankFrame(640, 480);
            composeOverlay(f.data(), 640, 480, bad, 1.0f, true, 0u, 45u, 0, 0u);
            t.IsTrue(pxAt(f, 640, 588, 391) == 1, "bad atlas inert");
            composeOverlay(nullptr, 640, 480, synthAtlas(), 1.0f, true, 0u, 45u, 0, 0u);
            t.IsTrue(true, "null frame survives"); });});

    MiniTest::Case("Ps2Ssx3TrickyHudLetters", [](TestCase &tc)
                   {
        tc.Run("uber post filter matches accepted 0x2133 posts only", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_hud;
            t.IsTrue(kUberSpeechEvent == 0x2133u, "Arcade_Uber event");
            t.IsTrue(kSpeechPostFunc == 0x2b1458u, "post function");
            t.IsTrue(isUberPost(0x2133u, 1u), "accepted uber post");
            t.IsTrue(isUberPost(0x2133u, 0xffffffu), "nonzero v0 counts");
            t.IsTrue(!isUberPost(0x2133u, 0u), "refused post ignored");
            t.IsTrue(!isUberPost(0x20a7u, 1u), "Power_Ups post ignored");
            t.IsTrue(!isUberPost(0x2145u, 1u), "Icons post ignored");
            t.IsTrue(!isUberPost(0u, 1u), "null event ignored"); });
        tc.Run("letter machine lights, flashes, resets", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_hud;
            LetterState st;
            updateLetters(st, 3u, 100u);
            t.IsTrue(st.lit == 3 && st.seen == 3u && st.flashUntil == 0u, "3 posts light 3");
            updateLetters(st, 3u, 200u);
            t.IsTrue(st.lit == 3 && st.flashUntil == 0u, "no posts, steady");
            updateLetters(st, 6u, 300u);
            t.IsTrue(st.lit == 6 && st.flashUntil == 300u + kLetterFlashTicks, "6th opens flash");
            updateLetters(st, 7u, 310u);
            t.IsTrue(st.lit == 6 && st.seen == 7u, "mid-flash post consumed, not lit");
            updateLetters(st, 7u, 300u + kLetterFlashTicks - 1u);
            t.IsTrue(st.lit == 6, "flash holds");
            updateLetters(st, 7u, 300u + kLetterFlashTicks);
            t.IsTrue(st.lit == 0 && st.flashUntil == 0u, "reset after flash");
            updateLetters(st, 8u, 400u);
            t.IsTrue(st.lit == 1, "spelling restarts"); });
        tc.Run("chain resolves the player rider, fails closed", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_hud;
            std::vector<uint8_t> ram(0x600000, 0);
            auto w32 = [&](uint32_t a, uint32_t v) { std::memcpy(&ram[a], &v, 4); };
            auto wf = [&](uint32_t a, float v) { std::memcpy(&ram[a], &v, 4); };
            // G=0x1000 -> A=0x2000 -> B=0x3000 -> R=0x5000.
            // (The root is addressed through the RDRAM mask, as in the game.)
            w32(kChainRoot & kRamMask, 0x1000u);
            w32(0x1000u + kChainAOff, 0x2000u);
            w32(0x2000u + kChainBOff, 0x3000u);
            w32(0x3000u + kChainROff, 0x5000u);
            wf(0x5000u + kRiderFillOff, 0.5f);
            w32(0x5000u + kRiderUberOff, 3u);
            t.IsTrue(resolveChainR(ram.data(), ram.size()) == 0x5000u, "full chain");
            MeterFrame f = readMeterFrameAt(ram.data(), ram.size(), 0x5000u);
            t.IsTrue(f.ok && f.fill == 0.5f && f.level == 3, "direct read");
            // Each broken hop fails closed.
            w32(0x2000u + kChainBOff, 0u);
            t.IsTrue(resolveChainR(ram.data(), ram.size()) == 0u, "null B");
            w32(0x2000u + kChainBOff, 0x3000u);
            w32(0x1000u + kChainAOff, 0u);
            t.IsTrue(resolveChainR(ram.data(), ram.size()) == 0u, "null A");
            w32(0x1000u + kChainAOff, 0x2000u);
            w32(kChainRoot & kRamMask, 0u);
            t.IsTrue(resolveChainR(ram.data(), ram.size()) == 0u, "null root");
            std::vector<uint8_t> empty(0x10000, 0);
            t.IsTrue(resolveChainR(empty.data(), empty.size()) == 0u, "short ram");
            t.IsTrue(resolveChainR(nullptr, 0) == 0u, "null ram"); });
        tc.Run("diag letters preset parses 0..5", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_hud;
            t.IsTrue(parseLettersPreset("0") == 0, "zero");
            t.IsTrue(parseLettersPreset("5") == 5, "five");
            t.IsTrue(parseLettersPreset(nullptr) == -1, "null");
            t.IsTrue(parseLettersPreset("") == -1, "empty");
            t.IsTrue(parseLettersPreset("6") == -1, "six refused");
            t.IsTrue(parseLettersPreset("-1") == -1, "negative refused");
            t.IsTrue(parseLettersPreset("x") == -1, "junk");
            t.IsTrue(parseLettersPreset("5x") == -1, "trailing junk"); });
        tc.Run("lit letters draw red, rest chrome", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_hud;
            Atlas a = synthAtlas();
            auto f = blankFrame(640, 480);
            composeOverlay(f.data(), 640, 480, a, 0.0f, false, 1000u, 0u, 3, 0u);
            t.IsTrue(pxAt(f, 640, 540, 105) == 140, "T red");
            t.IsTrue(pxAt(f, 640, 558, 95) == 141, "R red");
            t.IsTrue(pxAt(f, 640, 570, 80) == 142, "I red");
            t.IsTrue(pxAt(f, 640, 588, 90) == 63, "C chrome");
            t.IsTrue(pxAt(f, 640, 605, 80) == 64, "K chrome");
            t.IsTrue(pxAt(f, 640, 625, 110) == 65, "Y chrome"); });
        tc.Run("fanfare flash blinks all red then all chrome", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_hud;
            Atlas a = synthAtlas();
            // (1024>>3)&1 == 0: red phase. (1032>>3)&1 == 1: chrome phase.
            auto f = blankFrame(640, 480);
            composeOverlay(f.data(), 640, 480, a, 0.0f, false, 1024u, 0u, 6, 2000u);
            t.IsTrue(pxAt(f, 640, 540, 105) == 140, "red phase T");
            t.IsTrue(pxAt(f, 640, 625, 110) == 145, "red phase Y");
            auto g = blankFrame(640, 480);
            composeOverlay(g.data(), 640, 480, a, 0.0f, false, 1032u, 0u, 6, 2000u);
            t.IsTrue(pxAt(g, 640, 540, 105) == 60, "chrome phase T");
            t.IsTrue(pxAt(g, 640, 625, 110) == 65, "chrome phase Y"); });});

    // TK44: race-only visibility. The overlay draws only while the HUD race
    // time [B+0xc] advances (GO..finish); a freeze older than the grace hides
    // (pause, results, menus, pre-race card).
    MiniTest::Case("Ps2Ssx3TrickyHudRace", [](TestCase &tc)
                   {
        tc.Run("race clock shows on advance, hides on freeze", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_hud;
            RaceClock st;
            t.IsTrue(!updateRaceClock(st, 100u, 1000u), "init hides until an advance is seen");
            t.IsTrue(!updateRaceClock(st, 100u, 1001u), "frozen clock never shows");
            t.IsTrue(!updateRaceClock(st, 100u, 2000u), "frozen clock never shows, late");
            t.IsTrue(updateRaceClock(st, 101u, 2001u), "first advance shows");
            t.IsTrue(updateRaceClock(st, 102u, 2002u), "steady advance shows");
            t.IsTrue(updateRaceClock(st, 102u, 2003u), "1 frozen tick within grace shows");
            t.IsTrue(updateRaceClock(st, 102u, 2004u), "grace edge shows");
            t.IsTrue(!updateRaceClock(st, 102u, 2005u), "freeze past grace hides");
            t.IsTrue(!updateRaceClock(st, 102u, 3000u), "long freeze hides");
            t.IsTrue(updateRaceClock(st, 103u, 3001u), "resume shows at once");
            t.IsTrue(updateRaceClock(st, 1u, 3002u), "gate restart (backward) shows");
            t.IsTrue(!updateRaceClock(st, 1u, 3005u), "freeze after restart hides"); });
        tc.Run("race clock opens the adopt window at boundaries only", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_hud;
            RaceClock st;
            updateRaceClock(st, 300u, 7000u);
            t.IsTrue(st.adoptUntil == 0u, "init opens no window");
            updateRaceClock(st, 301u, 7001u);
            t.IsTrue(st.adoptUntil == 0u, "forward bump opens no window");
            t.IsTrue(updateRaceClock(st, 0u, 7452u), "restart still races");
            t.IsTrue(st.adoptUntil == 7452u + kGoAdoptTicks, "backward jump opens the window");
            const uint64_t w = st.adoptUntil;
            updateRaceClock(st, 1u, 7453u);
            t.IsTrue(st.adoptUntil == w, "quick GO after restart keeps the window");
            updateRaceClock(st, 2u, 7454u);
            t.IsTrue(st.adoptUntil == w, "steady advance keeps the window");
            // GO from a frozen 0 (gari race 2: frozen at 0 for 1100+ ticks).
            RaceClock g;
            updateRaceClock(g, 0u, 6200u);
            updateRaceClock(g, 0u, 7387u);
            t.IsTrue(g.adoptUntil == 0u, "freeze opens no window");
            t.IsTrue(updateRaceClock(g, 1u, 7388u), "GO races");
            t.IsTrue(g.adoptUntil == 7388u + kGoAdoptTicks, "GO from a frozen 0 opens the window");
            // A mid-race 0->1 (never frozen) is not a boundary.
            RaceClock m;
            updateRaceClock(m, 0u, 100u);
            updateRaceClock(m, 1u, 101u);
            t.IsTrue(m.adoptUntil == 0u, "unfrozen 0->1 opens no window"); });
        tc.Run("race chain B resolves, fails closed", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_hud;
            std::vector<uint8_t> ram(0x600000, 0);
            auto w32 = [&](uint32_t a, uint32_t v) { std::memcpy(&ram[a], &v, 4); };
            w32(kChainRoot & kRamMask, 0x1000u);
            w32(0x1000u + kChainAOff, 0x2000u);
            w32(0x2000u + kChainBOff, 0x3000u);
            t.IsTrue(resolveChainB(ram.data(), ram.size()) == 0x3000u, "B resolves");
            w32(0x2000u + kChainBOff, 0u);
            t.IsTrue(resolveChainB(ram.data(), ram.size()) == 0u, "null B");
            w32(0x2000u + kChainBOff, 0x3000u);
            w32(0x1000u + kChainAOff, 0u);
            t.IsTrue(resolveChainB(ram.data(), ram.size()) == 0u, "null A");
            std::vector<uint8_t> empty(0x10000, 0);
            t.IsTrue(resolveChainB(empty.data(), empty.size()) == 0u, "short ram");
            t.IsTrue(resolveChainB(nullptr, 0) == 0u, "null ram"); });
        tc.Run("replay state resolves, fails closed", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_hud;
            std::vector<uint8_t> ram(0x600000, 0);
            auto w32 = [&](uint32_t a, uint32_t v) { std::memcpy(&ram[a], &v, 4); };
            w32(kChainRoot & kRamMask, 0x1000u);
            w32(0x1000u + kChainAOff, 0x2000u);
            w32(0x2000u + kChainReplayOff, 0x4000u);
            uint32_t s = 99u;
            t.IsTrue(readReplayState(ram.data(), ram.size(), s) && s == 0u, "live race records (0)");
            w32(0x4000u, 1u);
            t.IsTrue(readReplayState(ram.data(), ram.size(), s) && s == 1u, "auto replay (1)");
            w32(0x2000u + kChainReplayOff, 0u);
            t.IsTrue(!readReplayState(ram.data(), ram.size(), s), "null replay");
            w32(0x1000u + kChainAOff, 0u);
            t.IsTrue(!readReplayState(ram.data(), ram.size(), s), "null A");
            t.IsTrue(!readReplayState(nullptr, 0, s), "null ram"); });});

    // TK43e: the region refactor. composeOverlay draws the region path, so
    // the region must contain every draw (and the smear's edge samples) and
    // everything outside it must be untouched.
    MiniTest::Case("Ps2Ssx3TrickyHudRegion", [](TestCase &tc)
                   {
        tc.Run("region contains every draw at 1x and 2x", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_hud;
            const int kArch[6][2] = {{527, 94}, {550, 83}, {565, 72}, {579, 79}, {595, 73}, {607, 94}};
            const int sizes[3][2] = {{640, 480}, {1280, 960}, {1920, 1080}};
            for (int pass = 0; pass < 3; ++pass)
            {
                const int fw = sizes[pass][0];
                const int fh = sizes[pass][1];
                Layout L;
                L.sx = static_cast<float>(fw) / 640.0f;
                L.sy = static_cast<float>(fh) / 480.0f;
                L.fw = fw;
                const Rect r = hudRegionRect(fw, fh);
                auto inside = [&](int x, int y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; };
                char name[64];
                // Pole, rings (centers +/- half size), jewel.
                std::snprintf(name, sizeof(name), "pole %dx%d", fw, fh);
                t.IsTrue(inside(L.X(584), L.Y(140)) && inside(L.X(589), L.Y(400)), name);
                for (int i = 0; i < 16; ++i)
                {
                    const float cy = 395.0f - static_cast<float>(i) * 16.5f;
                    std::snprintf(name, sizeof(name), "ring %d %dx%d", i, fw, fh);
                    t.IsTrue(inside(L.X(563), L.Y(cy - 6.0f)) && inside(L.X(612), L.Y(cy + 6.0f)), name);
                }
                std::snprintf(name, sizeof(name), "jewel %dx%d", fw, fh);
                t.IsTrue(inside(L.X(568), L.Y(107)) && inside(L.X(608), L.Y(143)), name);
                // Smear rect plus its 2 px edge samples.
                std::snprintf(name, sizeof(name), "smear %dx%d", fw, fh);
                t.IsTrue(inside(L.X(536) - 2, L.Y(49)) && inside(L.X(628) + 2, L.Y(101)), name);
                // TR3 coil cover plus its 2 px edge samples.
                const Rect cc = coilCoverRect(L);
                std::snprintf(name, sizeof(name), "coil cover %dx%d", fw, fh);
                t.IsTrue(inside(cc.x - 2, cc.y) && inside(cc.x + cc.w + 2, cc.y + cc.h - 1), name);
                // Arch letters at native size.
                for (int i = 0; i < 6; ++i)
                {
                    const Rect d = letterRect(i);
                    std::snprintf(name, sizeof(name), "arch %d %dx%d", i, fw, fh);
                    t.IsTrue(inside(L.X(kArch[i][0]), L.Y(kArch[i][1])) &&
                                 inside(L.X(kArch[i][0] + d.w), L.Y(kArch[i][1] + d.h)),
                             name);
                }
                // Pill and splash.
                std::snprintf(name, sizeof(name), "pill+splash %dx%d", fw, fh);
                t.IsTrue(inside(L.X(571), L.Y(400)) && inside(L.X(605), L.Y(430)) &&
                             inside(L.X(481), L.Y(114)) && inside(L.X(637), L.Y(230)),
                         name);
            } });
        tc.Run("compose leaves outside-region pixels untouched", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_hud;
            Atlas a = synthAtlas();
            const int sizes[3][2] = {{640, 480}, {1280, 960}, {1920, 1080}};
            for (int pass = 0; pass < 3; ++pass)
            {
                const int fw = sizes[pass][0];
                const int fh = sizes[pass][1];
                std::vector<uint8_t> f(static_cast<size_t>(fw) * static_cast<size_t>(fh) * 4u);
                uint32_t rng = 0x12345678u;
                for (size_t i = 0; i < f.size(); ++i)
                {
                    rng = rng * 1664525u + 1013904223u;
                    f[i] = static_cast<uint8_t>(rng >> 24);
                }
                for (size_t i = 3; i < f.size(); i += 4)
                    f[i] = 255;
                const std::vector<uint8_t> before = f;
                // Full meter + splash live + letters + fanfare: every draw on.
                composeOverlay(f.data(), fw, fh, a, 1.0f, true, 1024u, 2000u, 6, 2000u);
                const Rect r = hudRegionRect(fw, fh);
                bool outsideSame = true;
                bool insideMoved = false;
                for (int y = 0; y < fh && outsideSame; ++y)
                    for (int x = 0; x < fw; ++x)
                    {
                        const size_t o = (static_cast<size_t>(y) * fw + x) * 4u;
                        const bool in = x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
                        if (in)
                            insideMoved = insideMoved || std::memcmp(&f[o], &before[o], 4u) != 0;
                        else if (std::memcmp(&f[o], &before[o], 4u) != 0)
                        {
                            outsideSame = false;
                            break;
                        }
                    }
                char name[64];
                std::snprintf(name, sizeof(name), "outside untouched %dx%d", fw, fh);
                t.IsTrue(outsideSame, name);
                std::snprintf(name, sizeof(name), "inside drew %dx%d", fw, fh);
                t.IsTrue(insideMoved, name);
            } });
        tc.Run("region path equals the wrapper", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_hud;
            Atlas a = synthAtlas();
            const int fw = 640, fh = 480;
            std::vector<uint8_t> f(static_cast<size_t>(fw) * fh * 4u);
            uint32_t rng = 0xdeadbeeFu;
            for (size_t i = 0; i < f.size(); ++i)
            {
                rng = rng * 1664525u + 1013904223u;
                f[i] = static_cast<uint8_t>(rng >> 24);
            }
            std::vector<uint8_t> g = f;
            composeOverlay(f.data(), fw, fh, a, 0.73f, true, 77u, 90u, 4, 0u);
            const Rect r = hudRegionRect(fw, fh);
            std::vector<uint8_t> tmp(static_cast<size_t>(r.w) * r.h * 4u);
            for (int y = 0; y < r.h; ++y)
                std::memcpy(&tmp[static_cast<size_t>(y) * r.w * 4u],
                            &g[(static_cast<size_t>(r.y + y) * fw + r.x) * 4u],
                            static_cast<size_t>(r.w) * 4u);
            composeHudInto(tmp.data(), r.w, r.h, r.x, r.y, fw, fh, a, 0.73f, true, 77u, 90u, 4, 0u);
            for (int y = 0; y < r.h; ++y)
                std::memcpy(&g[(static_cast<size_t>(r.y + y) * fw + r.x) * 4u],
                            &tmp[static_cast<size_t>(y) * r.w * 4u], static_cast<size_t>(r.w) * 4u);
            t.IsTrue(f == g, "manual region path matches"); });
        tc.Run("cached stamps equal direct compose at 1x, 2x and 1080p", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_hud;
            Atlas a = synthAtlas();
            const int sizes[3][2] = {{640, 480}, {1280, 960}, {1920, 1080}};
            const float fills[3] = {0.0f, 0.53f, 1.0f};
            for (int s = 0; s < 3; ++s)
            {
                const int fw = sizes[s][0], fh = sizes[s][1];
                HudSprites ss;
                t.IsTrue(buildHudSprites(ss, a, fw, fh), "sprites build");
                for (int v = 0; v < 3; ++v)
                {
                    const float fill = fills[v];
                    const bool full = v == 2;
                    // tick 8u = dim pulse phase; splash live; 4 letters.
                    const uint64_t tick = 8u, splash = 90u;
                    std::vector<uint8_t> f(static_cast<size_t>(fw) * fh * 4u);
                    uint32_t rng = 0x51ab1eFu + static_cast<uint32_t>(s * 16 + v);
                    for (size_t i = 0; i < f.size(); ++i)
                    {
                        rng = rng * 1664525u + 1013904223u;
                        f[i] = static_cast<uint8_t>(rng >> 24);
                    }
                    std::vector<uint8_t> g = f;
                    composeOverlay(f.data(), fw, fh, a, fill, full, tick, splash, 4, 0u);
                    const Rect r = hudRegionRect(fw, fh);
                    std::vector<uint8_t> tmp(static_cast<size_t>(r.w) * r.h * 4u);
                    for (int y = 0; y < r.h; ++y)
                        std::memcpy(&tmp[static_cast<size_t>(y) * r.w * 4u],
                                    &g[(static_cast<size_t>(r.y + y) * fw + r.x) * 4u],
                                    static_cast<size_t>(r.w) * 4u);
                    stampHudInto(tmp.data(), r.w, r.h, r.x, r.y, ss, fill, full, tick, splash, 4, 0u);
                    for (int y = 0; y < r.h; ++y)
                        std::memcpy(&g[(static_cast<size_t>(r.y + y) * fw + r.x) * 4u],
                                    &tmp[static_cast<size_t>(y) * r.w * 4u],
                                    static_cast<size_t>(r.w) * 4u);
                    char name[64];
                    std::snprintf(name, sizeof(name), "cached==direct %dx%d v%d", fw, fh, v);
                    t.IsTrue(f == g, name);
                }
            } });
        tc.Run("direct strided stamps equal the temp path", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_hud;
            Atlas a = synthAtlas();
            const int sizes[2][2] = {{640, 480}, {1920, 1080}};
            const int stridePad[2] = {0, 64};
            for (int s = 0; s < 2; ++s)
            {
                const int fw = sizes[s][0], fh = sizes[s][1];
                const int stride = fw + stridePad[s];
                const size_t strideBytes = static_cast<size_t>(stride) * 4u;
                HudSprites ss;
                t.IsTrue(buildHudSprites(ss, a, fw, fh), "sprites build");
                // Full meter + splash live + letters + flash: every draw on.
                std::vector<uint8_t> f(static_cast<size_t>(stride) * fh * 4u);
                uint32_t rng = 0x77aa11u + static_cast<uint32_t>(s);
                for (size_t i = 0; i < f.size(); ++i)
                {
                    rng = rng * 1664525u + 1013904223u;
                    f[i] = static_cast<uint8_t>(rng >> 24);
                }
                std::vector<uint8_t> g = f;
                // Temp path (Part 1): region copy out/in with stride.
                const Rect r = hudRegionRect(fw, fh);
                std::vector<uint8_t> tmp(static_cast<size_t>(r.w) * r.h * 4u);
                for (int y = 0; y < r.h; ++y)
                    std::memcpy(&tmp[static_cast<size_t>(y) * r.w * 4u],
                                &f[static_cast<size_t>(r.y + y) * strideBytes + static_cast<size_t>(r.x) * 4u],
                                static_cast<size_t>(r.w) * 4u);
                stampHudInto(tmp.data(), r.w, r.h, r.x, r.y, ss, 1.0f, true, 8u, 90u, 4, 90u);
                for (int y = 0; y < r.h; ++y)
                    std::memcpy(&f[static_cast<size_t>(r.y + y) * strideBytes + static_cast<size_t>(r.x) * 4u],
                                &tmp[static_cast<size_t>(y) * r.w * 4u], static_cast<size_t>(r.w) * 4u);
                stampHudDirect(g.data(), strideBytes, r, ss, 1.0f, true, 8u, 90u, 4, 90u);
                char name[64];
                std::snprintf(name, sizeof(name), "direct==temp stride=%d", stride);
                t.IsTrue(f == g, name);
            } });});

    MiniTest::Case("Ps2Ssx3TrickyHudBgra", [](TestCase &tc)
                   {
        tc.Run("THD1 swapped stamp equals RGBA stamp with R/B swapped", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_hud;
            Atlas a = tintedAtlas();
            t.IsTrue(a.ok, "tinted atlas parses");
            const int sizes[2][2] = {{640, 480}, {1920, 1080}};
            const int stridePad[2] = {0, 64};
            struct State
            {
                float fill;
                bool full;
                uint64_t tick, splash;
                int lit;
                uint64_t flash;
            };
            // empty; partial + letters; full dim-jewel + splash + chrome flash;
            // full bright-jewel + splash + red flash.
            const State states[4] = {{0.0f, false, 8u, 0u, 0, 0u},
                                     {0.53f, false, 100u, 0u, 3, 0u},
                                     {1.0f, true, 8u, 90u, 4, 90u},
                                     {1.0f, true, 16u, 90u, 5, 90u}};
            for (int s = 0; s < 2; ++s)
                for (int q = 0; q < 2; ++q)
                {
                    const int fw = sizes[s][0], fh = sizes[s][1];
                    const int stride = fw + stridePad[q];
                    const size_t strideBytes = static_cast<size_t>(stride) * 4u;
                    HudSprites ss; // one shared cache serves both lanes
                    t.IsTrue(buildHudSprites(ss, a, fw, fh), "sprites build");
                    const Rect r = hudRegionRect(fw, fh);
                    for (int v = 0; v < 4; ++v)
                    {
                        const State &st = states[v];
                        std::vector<uint8_t> rgba(static_cast<size_t>(stride) * fh * 4u);
                        uint32_t rng = 0x1ed51u + static_cast<uint32_t>(s * 64 + q * 16 + v);
                        for (size_t i = 0; i < rgba.size(); ++i)
                        {
                            rng = rng * 1664525u + 1013904223u;
                            rgba[i] = static_cast<uint8_t>(rng >> 24);
                        }
                        // BGRA background: the same bytes with R/B swapped.
                        std::vector<uint8_t> bgra = rgba;
                        for (size_t i = 0; i < bgra.size(); i += 4)
                            std::swap(bgra[i], bgra[i + 2]);
                        const std::vector<uint8_t> before = rgba;
                        stampHudDirect<false>(rgba.data(), strideBytes, r, ss, st.fill, st.full,
                                              st.tick, st.splash, st.lit, st.flash);
                        stampHudDirect<true>(bgra.data(), strideBytes, r, ss, st.fill, st.full,
                                             st.tick, st.splash, st.lit, st.flash);
                        for (size_t i = 0; i < bgra.size(); i += 4)
                            std::swap(bgra[i], bgra[i + 2]);
                        char name[96];
                        std::snprintf(name, sizeof(name), "bgra==swapped rgba %dx%d pad=%d v%d",
                                      fw, fh, stridePad[q], v);
                        t.IsTrue(rgba == bgra, name);
                        bool drew = false;
                        for (size_t i = 0; i < rgba.size(); i += 4)
                        {
                            if (std::memcmp(&rgba[i], &before[i], 4u) != 0)
                            {
                                drew = true;
                                break;
                            }
                        }
                        std::snprintf(name, sizeof(name), "state drew %dx%d pad=%d v%d", fw, fh,
                                      stridePad[q], v);
                        t.IsTrue(drew, name);
                    }
                } });});

    MiniTest::Case("Ps2Ssx3TrickyHudCache", [](TestCase &tc)
                   {
        tc.Run("HUD2 cached layer equals direct stamp across the visual-key space", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_hud;
            Atlas atlases[2] = {tintedAtlas(), randomAtlas(0xc0ffeeu)};
            t.IsTrue(atlases[0].ok && atlases[1].ok, "atlases parse");
            struct State
            {
                float fill;
                bool full;
                uint64_t tick, splash, flash;
                int lit;
            };
            // Full sweep at 640x480 (816 states: coils 0..16 x full x tick
            // phase x splash x letters x flash); one shared cache also locks
            // the rebuild-on-key-change count.
            std::vector<State> sweep;
            for (int c = 0; c <= 16; ++c)
            {
                const float fill = c < 16 ? (static_cast<float>(c) + 0.5f) / 16.0f : 1.0f;
                for (int f = 0; f < 2; ++f)
                    for (int tp = 0; tp < 2; ++tp)
                    {
                        const uint64_t tick = tp == 0 ? 0u : 8u;
                        for (int sp = 0; sp < 2; ++sp)
                            for (int li = 0; li < 3; ++li)
                                for (int fl = 0; fl < 2; ++fl)
                                    sweep.push_back({fill, f != 0, tick, sp != 0 ? tick + 50u : 0u,
                                                     fl != 0 ? tick + 50u : 0u,
                                                     li == 0 ? 0 : (li == 1 ? 3 : 6)});
                    }
            }
            // Sampled states for the 1080p legs (every draw on/off corners).
            const State sampled[8] = {{0.0f, false, 0u, 0u, 0u, 0},
                                      {0.03f, false, 100u, 0u, 0u, 1},
                                      {0.5f, false, 8u, 0u, 0u, 3},
                                      {0.97f, false, 100u, 0u, 0u, 5},
                                      {1.0f, true, 0u, 50u, 50u, 6},
                                      {1.0f, true, 8u, 90u, 90u, 4},
                                      {0.73f, true, 100u, 0u, 200u, 2},
                                      {0.25f, true, 108u, 200u, 0u, 6}};
            for (int a = 0; a < 2; ++a)
            {
                const Atlas &atlas = atlases[a];
                HudSprites ss;
                t.IsTrue(buildHudSprites(ss, atlas, 640, 480), "direct sprites build");
                HudCache cache;
                const Rect r = hudRegionRect(640, 480);
                const size_t strideBytes = 640u * 4u;
                uint64_t expectRebuilds = 0u;
                HudVisualKey lastKey;
                bool haveKey = false;
                int drew = 0;
                for (size_t v = 0; v < sweep.size(); ++v)
                {
                    const State &st = sweep[v];
                    const HudVisualKey key =
                        visualKeyFor(&atlas, 640, 480, st.fill, st.full, st.tick, st.splash,
                                     st.lit, st.flash);
                    if (!haveKey || key != lastKey)
                    {
                        ++expectRebuilds;
                        lastKey = key;
                        haveKey = true;
                    }
                    t.IsTrue(ensureHudLayer(cache, atlas, 640, 480, st.fill, st.full, st.tick,
                                            st.splash, st.lit, st.flash),
                             "layer ensures");
                    // Frames per key: 3 on every 4th state (the live-smear
                    // proof: same layer, different backgrounds), else 1. One
                    // background per (state, frame), shared by both lanes.
                    const int frames = (v % 4u == 0u) ? 3 : 1;
                    for (int fr = 0; fr < frames; ++fr)
                    {
                        std::vector<uint8_t> bg(640u * 480u * 4u);
                        randomFrame(bg, 0x5eed1u +
                                            static_cast<uint32_t>(a * 1000003u + v * 101u +
                                                                  fr * 1009u));
                        for (int lane = 0; lane < 2; ++lane)
                        {
                            std::vector<uint8_t> f = bg;
                            std::vector<uint8_t> g = bg;
                            if (lane == 0)
                            {
                                stampHudDirect<false>(f.data(), strideBytes, r, ss, st.fill,
                                                      st.full, st.tick, st.splash, st.lit,
                                                      st.flash);
                                stampHudCached<false>(g.data(), strideBytes, r, cache.layer);
                            }
                            else
                            {
                                stampHudDirect<true>(f.data(), strideBytes, r, ss, st.fill,
                                                     st.full, st.tick, st.splash, st.lit,
                                                     st.flash);
                                stampHudCached<true>(g.data(), strideBytes, r, cache.layer);
                            }
                            if (f != bg)
                                ++drew;
                            if (f != g)
                            {
                                char name[128];
                                std::snprintf(name, sizeof(name),
                                              "MISMATCH atlas=%d lane=%d v=%zu fill=%.4f full=%d "
                                              "tick=%llu splash=%llu lit=%d flash=%llu fr=%d",
                                              a, lane, v, static_cast<double>(st.fill),
                                              st.full ? 1 : 0,
                                              static_cast<unsigned long long>(st.tick),
                                              static_cast<unsigned long long>(st.splash), st.lit,
                                              static_cast<unsigned long long>(st.flash), fr);
                                t.IsTrue(false, name);
                                return;
                            }
                        }
                    }
                }
                char name[96];
                std::snprintf(name, sizeof(name), "rebuilds==key changes atlas=%d (%llu)", a,
                              static_cast<unsigned long long>(cache.rebuilds));
                t.IsTrue(cache.rebuilds == expectRebuilds, name);
                std::snprintf(name, sizeof(name), "all states drew atlas=%d (%d)", a, drew);
                t.IsTrue(drew > 0, name);
                // 1080p sampled legs, tight + padded stride, both lanes.
                const int pads[2] = {0, 64};
                for (int q = 0; q < 2; ++q)
                {
                    const int stride = 1920 + pads[q];
                    const size_t sb = static_cast<size_t>(stride) * 4u;
                    HudSprites ss1080;
                    t.IsTrue(buildHudSprites(ss1080, atlas, 1920, 1080), "1080p sprites build");
                    HudCache c1080;
                    const Rect r1080 = hudRegionRect(1920, 1080);
                    for (int v = 0; v < 8; ++v)
                    {
                        const State &st = sampled[v];
                        t.IsTrue(ensureHudLayer(c1080, atlas, 1920, 1080, st.fill, st.full,
                                                st.tick, st.splash, st.lit, st.flash),
                                 "1080p layer ensures");
                        for (int fr = 0; fr < 3; ++fr)
                        {
                            std::vector<uint8_t> bg(static_cast<size_t>(stride) * 1080u * 4u);
                            randomFrame(bg, 0x9e3779b9u +
                                                static_cast<uint32_t>(a * 7919u + q * 104729u +
                                                                      v * 1299709u + fr * 1009u));
                            for (int lane = 0; lane < 2; ++lane)
                            {
                                std::vector<uint8_t> f = bg;
                                std::vector<uint8_t> g = bg;
                                if (lane == 0)
                                {
                                    stampHudDirect<false>(f.data(), sb, r1080, ss1080, st.fill,
                                                          st.full, st.tick, st.splash, st.lit,
                                                          st.flash);
                                    stampHudCached<false>(g.data(), sb, r1080, c1080.layer);
                                }
                                else
                                {
                                    stampHudDirect<true>(f.data(), sb, r1080, ss1080, st.fill,
                                                         st.full, st.tick, st.splash, st.lit,
                                                         st.flash);
                                    stampHudCached<true>(g.data(), sb, r1080, c1080.layer);
                                }
                                if (f != g)
                                {
                                    char nm[128];
                                    std::snprintf(nm, sizeof(nm),
                                                  "1080p MISMATCH atlas=%d pad=%d lane=%d v=%d "
                                                  "fr=%d",
                                                  a, pads[q], lane, v, fr);
                                    t.IsTrue(false, nm);
                                    return;
                                }
                            }
                        }
                    }
                }
            } });
        tc.Run("HUD2 recorded ride packets equal direct", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_hud;
            Atlas atlas = tintedAtlas();
            struct State
            {
                float fill;
                bool full;
                uint64_t tick, splash, flash;
                int lit;
            };
            // Recorded on a Mac Garibaldi ride (HUD2 gari-rec, [hud2-record]
            // transitions, one per lit-coil band seen, mid-occurrence tick).
            // The scripted AP line never fills the meter (no full/letters on
            // this ride, the THD1 gap); the sweep above covers those states.
            const State rec[13] = {
                {0.0312f, false, 11470u, 0u, 0u, 0}, {0.1026f, false, 10613u, 0u, 0u, 0},
                {0.1377f, false, 10192u, 0u, 0u, 0}, {0.2494f, false, 10006u, 0u, 0u, 0},
                {0.2812f, false, 9625u, 0u, 0u, 0},  {0.3646f, false, 8987u, 0u, 0u, 0},
                {0.4051f, false, 8501u, 0u, 0u, 0},  {0.4630f, false, 7806u, 0u, 0u, 0},
                {0.6094f, false, 7383u, 0u, 0u, 0},  {0.6316f, false, 7117u, 0u, 0u, 0},
                {0.7430f, false, 7447u, 0u, 0u, 0},  {0.7502f, false, 6910u, 0u, 0u, 0},
                {0.8919f, false, 6993u, 0u, 0u, 0},
            };
            const int sizes[2][2] = {{640, 480}, {1920, 1080}};
            for (int s = 0; s < 2; ++s)
            {
                const int fw = sizes[s][0], fh = sizes[s][1];
                const size_t sb = static_cast<size_t>(fw) * 4u;
                HudSprites ss;
                t.IsTrue(buildHudSprites(ss, atlas, fw, fh), "sprites build");
                HudCache c;
                const Rect r = hudRegionRect(fw, fh);
                for (int v = 0; v < 13; ++v)
                {
                    const State &st = rec[v];
                    t.IsTrue(ensureHudLayer(c, atlas, fw, fh, st.fill, st.full, st.tick,
                                            st.splash, st.lit, st.flash),
                             "layer ensures");
                    for (int fr = 0; fr < 2; ++fr)
                    {
                        std::vector<uint8_t> bg(static_cast<size_t>(fw) * fh * 4u);
                        randomFrame(bg, 0xdec0deu +
                                            static_cast<uint32_t>(s * 131u + v * 1009u + fr));
                        for (int lane = 0; lane < 2; ++lane)
                        {
                            std::vector<uint8_t> f = bg;
                            std::vector<uint8_t> g = bg;
                            if (lane == 0)
                            {
                                stampHudDirect<false>(f.data(), sb, r, ss, st.fill, st.full,
                                                      st.tick, st.splash, st.lit, st.flash);
                                stampHudCached<false>(g.data(), sb, r, c.layer);
                            }
                            else
                            {
                                stampHudDirect<true>(f.data(), sb, r, ss, st.fill, st.full,
                                                     st.tick, st.splash, st.lit, st.flash);
                                stampHudCached<true>(g.data(), sb, r, c.layer);
                            }
                            if (f != g)
                            {
                                char nm[96];
                                std::snprintf(nm, sizeof(nm),
                                              "recorded MISMATCH %dx%d lane=%d v=%d fr=%d", fw,
                                              fh, lane, v, fr);
                                t.IsTrue(false, nm);
                                return;
                            }
                        }
                    }
                }
            } });
        tc.Run("HUD2 ensure rebuilds only on visual-key change", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_hud;
            Atlas atlas = tintedAtlas();
            HudCache c;
            // (fill, full, tick, splash, letters, flash, fw, fh)
            t.IsTrue(ensureHudLayer(c, atlas, 640, 480, 0.5f, false, 100u, 0u, 2, 0u), "first");
            t.IsTrue(c.rebuilds == 1u, "one rebuild");
            // Same key through different raw values: no rebuild.
            t.IsTrue(ensureHudLayer(c, atlas, 640, 480, 0.52f, false, 101u, 0u, 2, 0u), "same key");
            t.IsTrue(c.rebuilds == 1u, "still one");
            t.IsTrue(ensureHudLayer(c, atlas, 640, 480, 0.52f, false, 101u, 50u, 2, 100u),
                     "windows still off");
            t.IsTrue(c.rebuilds == 1u, "still one (windows)");
            // Each key field flips exactly one rebuild.
            t.IsTrue(ensureHudLayer(c, atlas, 640, 480, 0.0f, false, 101u, 50u, 2, 100u), "lit");
            t.IsTrue(c.rebuilds == 2u, "lit rebuilds");
            t.IsTrue(ensureHudLayer(c, atlas, 640, 480, 0.0f, true, 101u, 50u, 2, 100u), "full");
            t.IsTrue(c.rebuilds == 3u, "full rebuilds");
            t.IsTrue(ensureHudLayer(c, atlas, 640, 480, 0.0f, true, 104u, 50u, 2, 100u), "dim");
            t.IsTrue(c.rebuilds == 4u, "dim rebuilds");
            t.IsTrue(ensureHudLayer(c, atlas, 640, 480, 0.0f, true, 104u, 500u, 2, 100u),
                     "splash on");
            t.IsTrue(c.rebuilds == 5u, "splash rebuilds");
            t.IsTrue(ensureHudLayer(c, atlas, 640, 480, 0.0f, true, 104u, 500u, 5, 100u),
                     "letters");
            t.IsTrue(c.rebuilds == 6u, "letters rebuild");
            t.IsTrue(ensureHudLayer(c, atlas, 640, 480, 0.0f, true, 104u, 500u, 5, 500u),
                     "flash on");
            t.IsTrue(c.rebuilds == 7u, "flash rebuilds");
            t.IsTrue(ensureHudLayer(c, atlas, 640, 480, 0.0f, true, 112u, 500u, 5, 500u),
                     "flash phase");
            t.IsTrue(c.rebuilds == 8u, "flash phase rebuilds");
            t.IsTrue(ensureHudLayer(c, atlas, 1920, 1080, 0.0f, true, 112u, 500u, 5, 500u),
                     "resize");
            t.IsTrue(c.rebuilds == 9u, "resize rebuilds");
            // Refusals fail closed and keep the last good layer.
            Atlas bad;
            const uint64_t before = c.rebuilds;
            t.IsTrue(!ensureHudLayer(c, bad, 640, 480, 0.5f, false, 100u, 0u, 2, 0u), "bad atlas");
            t.IsTrue(c.has, "keeps last good layer");
            t.IsTrue(ensureHudLayer(c, atlas, 1920, 1080, 0.0f, true, 112u, 500u, 5, 500u),
                     "same key serves");
            t.IsTrue(c.rebuilds == before, "no rebuild on same key");
            // A new atlas object rebuilds (art identity is in the key).
            Atlas atlas2 = tintedAtlas();
            t.IsTrue(ensureHudLayer(c, atlas2, 1920, 1080, 0.0f, true, 112u, 500u, 5, 500u),
                     "new atlas");
            t.IsTrue(c.rebuilds == before + 1u, "new atlas rebuilds"); });
        tc.Run("HUD3 scene model equals direct on sweep", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_hud;
            Atlas atlas = tintedAtlas();
            const int fw = 640, fh = 480;
            const size_t sb = static_cast<size_t>(fw) * 4u;
            HudSprites ss;
            t.IsTrue(buildHudSprites(ss, atlas, fw, fh), "sprites build");
            const Rect r = hudRegionRect(fw, fh);
            // fill bands x full x dim phase x splash x letters x flash phase.
            const float fills[6] = {0.0f, 0.03f, 0.30f, 0.55f, 0.80f, 1.0f};
            int checked = 0;
            for (int fi = 0; fi < 6; ++fi)
                for (int full = 0; full < 2; ++full)
                    for (int ph = 0; ph < 2; ++ph)
                        for (int sp = 0; sp < 2; ++sp)
                            for (int li = 0; li < 7; li += 3)
                                for (int fl = 0; fl < 3; ++fl)
                                {
                                    const uint64_t tick = 100u + static_cast<uint64_t>(ph * 8);
                                    const uint64_t splash = sp ? tick + 50u : 0u;
                                    const uint64_t flash =
                                        fl == 0 ? 0u : (fl == 1 ? tick + 50u : tick + 8u);
                                    const bool bfull = full != 0;
                                    const HudVisualKey key = visualKeyFor(
                                        &atlas, fw, fh, fills[fi], bfull, tick, splash, li, flash);
                                    HudScene sc;
                                    if (!buildHudScene(sc, ss, key))
                                    {
                                        t.IsTrue(false, "scene builds");
                                        return;
                                    }
                                    std::vector<uint8_t> bg(static_cast<size_t>(fw) * fh * 4u);
                                    randomFrame(bg, 0x6e3d01u +
                                                        static_cast<uint32_t>(checked * 7919u));
                                    for (int lane = 0; lane < 2; ++lane)
                                    {
                                        std::vector<uint8_t> f = bg;
                                        std::vector<uint8_t> g = bg;
                                        if (lane == 0)
                                        {
                                            stampHudDirect<false>(f.data(), sb, r, ss, fills[fi],
                                                                  bfull, tick, splash, li, flash);
                                            execHudSceneModel<false>(g.data(), sb, r, sc, atlas);
                                        }
                                        else
                                        {
                                            stampHudDirect<true>(f.data(), sb, r, ss, fills[fi],
                                                                 bfull, tick, splash, li, flash);
                                            execHudSceneModel<true>(g.data(), sb, r, sc, atlas);
                                        }
                                        if (f != g)
                                        {
                                            char nm[128];
                                            std::snprintf(nm, sizeof(nm),
                                                          "sweep MISMATCH lane=%d fi=%d full=%d "
                                                          "ph=%d sp=%d li=%d fl=%d",
                                                          lane, fi, full, ph, sp, li, fl);
                                            t.IsTrue(false, nm);
                                            return;
                                        }
                                    }
                                    ++checked;
                                }
            t.IsTrue(checked == 6 * 2 * 2 * 2 * 3 * 3, "sweep covered"); });
        tc.Run("HUD3 scene model equals direct at 1080p and 2x", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_hud;
            Atlas atlas = tintedAtlas();
            struct State
            {
                float fill;
                bool full;
                uint64_t tick, splash, flash;
                int lit;
            };
            // HUD2's 13 recorded ride packets (gari-rec [hud2-record]
            // transitions) plus full-state coverage the scripted ride never
            // reaches (full/letters/splash/flash).
            const State rec[13] = {
                {0.0312f, false, 11470u, 0u, 0u, 0}, {0.1026f, false, 10613u, 0u, 0u, 0},
                {0.1377f, false, 10192u, 0u, 0u, 0}, {0.2494f, false, 10006u, 0u, 0u, 0},
                {0.2812f, false, 9625u, 0u, 0u, 0},  {0.3646f, false, 8987u, 0u, 0u, 0},
                {0.4051f, false, 8501u, 0u, 0u, 0},  {0.4630f, false, 7806u, 0u, 0u, 0},
                {0.6094f, false, 7383u, 0u, 0u, 0},  {0.6316f, false, 7117u, 0u, 0u, 0},
                {0.7430f, false, 7447u, 0u, 0u, 0},  {0.7502f, false, 6910u, 0u, 0u, 0},
                {0.8919f, false, 6993u, 0u, 0u, 0},
            };
            const State extra[6] = {
                {1.0f, true, 8u, 90u, 90u, 6},   // full + splash + fanfare
                {1.0f, true, 16u, 0u, 0u, 6},    // full, dim phase 0
                {1.0f, true, 24u, 0u, 0u, 6},    // full, dim phase 1
                {0.5f, false, 8u, 0u, 200u, 3},  // mid + letters + flash
                {0.0f, false, 100u, 0u, 0u, 0},  // empty meter
                {0.97f, true, 104u, 500u, 0u, 5}, // full edge + splash, no flash
            };
            const int sizes[2][2] = {{1920, 1080}, {3840, 2160}};
            for (int s = 0; s < 2; ++s)
            {
                const int fw = sizes[s][0], fh = sizes[s][1];
                HudSprites sprites;
                t.IsTrue(buildHudSprites(sprites, atlas, fw, fh), "sprites build");
                const Rect r = hudRegionRect(fw, fh);
                for (int grp = 0; grp < 2; ++grp)
                {
                    const State *states = grp == 0 ? rec : extra;
                    const int nst = grp == 0 ? 13 : 6;
                    for (int v = 0; v < nst; ++v)
                    {
                        const State &st = states[v];
                        const HudVisualKey key =
                            visualKeyFor(&atlas, fw, fh, st.fill, st.full, st.tick,
                                         st.splash, st.lit, st.flash);
                        HudScene sc;
                        if (!buildHudScene(sc, sprites, key))
                        {
                            t.IsTrue(false, "scene builds");
                            return;
                        }
                        for (int fr = 0; fr < 2; ++fr)
                        {
                            // Padded stride on the second frame (AHB rows are
                            // stride-padded): the model must match there too.
                            const size_t pad = fr == 0 ? 0u : 64u;
                            const size_t sb = static_cast<size_t>(fw) * 4u + pad;
                            std::vector<uint8_t> bg(sb * static_cast<size_t>(fh));
                            randomFrame(bg, 0xbeef01u + static_cast<uint32_t>(
                                                              s * 100003u + grp * 1009u +
                                                              v * 101u + fr));
                            for (int lane = 0; lane < 2; ++lane)
                            {
                                std::vector<uint8_t> f = bg;
                                std::vector<uint8_t> g = bg;
                                if (lane == 0)
                                {
                                    stampHudDirect<false>(f.data(), sb, r, sprites, st.fill,
                                                          st.full, st.tick, st.splash, st.lit,
                                                          st.flash);
                                    execHudSceneModel<false>(g.data(), sb, r, sc, atlas);
                                }
                                else
                                {
                                    stampHudDirect<true>(f.data(), sb, r, sprites, st.fill,
                                                         st.full, st.tick, st.splash, st.lit,
                                                         st.flash);
                                    execHudSceneModel<true>(g.data(), sb, r, sc, atlas);
                                }
                                if (f != g)
                                {
                                    char nm[128];
                                    std::snprintf(nm, sizeof(nm),
                                                  "1080p/2x MISMATCH %dx%d lane=%d grp=%d "
                                                  "v=%d fr=%d",
                                                  fw, fh, lane, grp, v, fr);
                                    t.IsTrue(false, nm);
                                    return;
                                }
                            }
                        }
                    }
                }
            } });
        tc.Run("HUD3 scene rebuilds only on visual-key change", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_hud;
            Atlas atlas = tintedAtlas();
            HudSprites sprites;
            t.IsTrue(buildHudSprites(sprites, atlas, 1920, 1080), "sprites build");
            const HudVisualKey k1 =
                visualKeyFor(&atlas, 1920, 1080, 0.5f, false, 100u, 0u, 2, 0u);
            const HudVisualKey k2 =
                visualKeyFor(&atlas, 1920, 1080, 0.52f, false, 101u, 0u, 2, 0u);
            t.IsTrue(k1 == k2, "same key across raw values");
            HudScene s1, s2;
            t.IsTrue(buildHudScene(s1, sprites, k1), "scene 1 builds");
            t.IsTrue(buildHudScene(s2, sprites, k2), "scene 2 builds");
            t.IsTrue(s1.nquads == s2.nquads && s1.nsmears == s2.nsmears, "same counts");
            t.IsTrue(std::memcmp(s1.quads, s2.quads, sizeof(s1.quads)) == 0, "same quads");
            t.IsTrue(std::memcmp(s1.smears, s2.smears, sizeof(s1.smears)) == 0,
                     "same smears");
            // A coil flip changes the scene (one ring quad's cell).
            const HudVisualKey k3 =
                visualKeyFor(&atlas, 1920, 1080, 0.0f, false, 101u, 0u, 2, 0u);
            HudScene s3;
            t.IsTrue(buildHudScene(s3, sprites, k3), "scene 3 builds");
            t.IsTrue(std::memcmp(s1.quads, s3.quads, sizeof(s1.quads)) != 0,
                     "coil flip changes quads"); });
        tc.Run("HUD2 GL cached compose equals direct compose", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_hud;
            Atlas atlas = tintedAtlas();
            struct State
            {
                float fill;
                bool full;
                uint64_t tick, splash, flash;
                int lit;
            };
            const State states[4] = {{0.0f, false, 100u, 0u, 0u, 0},
                                     {0.53f, false, 8u, 0u, 0u, 3},
                                     {1.0f, true, 8u, 90u, 90u, 4},
                                     {0.73f, true, 100u, 0u, 200u, 2}};
            const int sizes[2][2] = {{640, 480}, {1920, 1080}};
            for (int s = 0; s < 2; ++s)
            {
                const int fw = sizes[s][0], fh = sizes[s][1];
                HudCache c;
                for (int v = 0; v < 4; ++v)
                {
                    const State &st = states[v];
                    t.IsTrue(ensureHudLayer(c, atlas, fw, fh, st.fill, st.full, st.tick,
                                            st.splash, st.lit, st.flash),
                             "layer ensures");
                    // Same layer, two backgrounds (the live-smear proof).
                    for (int fr = 0; fr < 2; ++fr)
                    {
                        std::vector<uint8_t> f(static_cast<size_t>(fw) * fh * 4u);
                        uint32_t rng = 0x6c311u + static_cast<uint32_t>(s * 31u + v * 101u + fr);
                        for (size_t i = 0; i < f.size(); ++i)
                        {
                            rng = rng * 1664525u + 1013904223u;
                            f[i] = static_cast<uint8_t>(rng >> 24);
                        }
                        std::vector<uint8_t> g = f;
                        composeOverlay(f.data(), fw, fh, atlas, st.fill, st.full, st.tick,
                                       st.splash, st.lit, st.flash);
                        composeOverlayCached(g.data(), fw, fh, c.layer);
                        if (f != g)
                        {
                            char nm[96];
                            std::snprintf(nm, sizeof(nm), "GL MISMATCH %dx%d v=%d fr=%d", fw, fh,
                                          v, fr);
                            t.IsTrue(false, nm);
                            return;
                        }
                    }
                }
            } });});

    // HUD4: the GE1 scene blob layout. The adapter (ge1_gs.h) and the vendor
    // (GSDeviceVK HudSceneBlob) mirror Ge1HudScene byte for byte; both check
    // magic + exact size fail-closed, and this test pins the runtime half.
    MiniTest::Case("Ps2Ssx3TrickyHudGe1Abi", [](TestCase &tc)
                   {
        tc.Run("HUD4 Ge1HudScene layout matches the vendor mirror", [](TestCase &t)
               {
            t.IsTrue(sizeof(Ge1HudScene) == 1016u, "blob is 1016 bytes");
            t.IsTrue(offsetof(Ge1HudScene, regionX) == 8u, "region at 8");
            t.IsTrue(offsetof(Ge1HudScene, nsmears) == 24u, "nsmears at 24");
            t.IsTrue(offsetof(Ge1HudScene, nquads) == 76u, "nquads at 76");
            t.IsTrue(offsetof(Ge1HudScene, quadSrcX) == 80u, "quads at 80");
            t.IsTrue(offsetof(Ge1HudScene, quadDim) == 912u, "dim at 912");
            t.IsTrue(GE1_HUD_SCENE_MAGIC == 0x44554847u, "magic HUDG");
            t.IsTrue(GE1_HUD_SCENE_VERSION == 1u, "version 1");
            t.IsTrue(GE1_HUD_SCENE_MAX_QUADS == 26, "26 quads");
            Ge1HudScene s{};
            s.magic = GE1_HUD_SCENE_MAGIC;
            s.version = GE1_HUD_SCENE_VERSION;
            t.IsTrue(s.magic == 0x44554847u && s.version == 1u, "magic/version assign"); });});}
