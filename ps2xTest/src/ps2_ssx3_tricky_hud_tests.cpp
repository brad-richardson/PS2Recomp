#include "MiniTest.h"
#include "ps2_ssx3_tricky_hud.h"

// TK43a: Tricky meter reskin compositor/reader/atlas tests. The atlas is
// synthetic solid-color rects (no game art); rect positions mirror the
// staged trickyhud.bin layout in make_trickyhud_atlas.py.
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
    paintRect(px, ringRect(0), 10); // pale
    paintRect(px, ringRect(1), 20); // gold
    paintRect(px, ringRect(2), 30); // orange
    paintRect(px, ringRect(3), 40); // red
    paintRect(px, silverRingRect(), 50);
    paintRect(px, letterRect(0), 60);
    paintRect(px, letterRect(1), 61);
    paintRect(px, letterRect(2), 62);
    paintRect(px, letterRect(3), 63);
    paintRect(px, letterRect(4), 64);
    paintRect(px, letterRect(5), 65);
    paintRect(px, jewelGreyRect(), 90);
    paintRect(px, jewelRedRect(), 100);
    paintRect(px, pillRect(), 110);
    paintRect(px, pillGreyRect(), 130);
    paintRect(px, snowflakeRect(), 120);
    std::vector<uint8_t> blob;
    const char magic[8] = {'T', 'K', 'H', 'U', 'D', '1', '\0', '\0'};
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
            const char magic[8] = {'T', 'K', 'H', 'U', 'D', '1', '\0', '\0'};
            blob.insert(blob.end(), magic, magic + 8);
            const uint32_t wh[3] = {256u, 256u, 0u};
            blob.insert(blob.end(), reinterpret_cast<const uint8_t *>(wh),
                        reinterpret_cast<const uint8_t *>(wh) + 12);
            blob.insert(blob.end(), 256u * 256u * 4u, 0);
            t.IsTrue(!parseAtlas(nullptr, 0).ok, "null");
            blob[0] = 'X';
            t.IsTrue(!parseAtlas(blob.data(), blob.size()).ok, "bad magic");
            blob[0] = 'T';
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
            composeOverlay(f.data(), 640, 480, a, 0.0f, false, 1000u, 0u);
            for (int i = 0; i < 16; ++i)
                t.IsTrue(pxAt(f, 640, ringCx(), ringCy(i)) == 50, "ring silver");
            t.IsTrue(pxAt(f, 640, 588, 125) == 90, "jewel grey");
            t.IsTrue(pxAt(f, 640, 588, 415) == 130, "grey pill slot");
            t.IsTrue(pxAt(f, 640, 535, 100) == 1, "no splash");
            t.IsTrue(pxAt(f, 640, 600, 20) == 1, "score kept");
            t.IsTrue(pxAt(f, 640, 586, 156) == 26, "pole"); });
        tc.Run("half meter: 8 lit in band order, 8 silver", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_hud;
            Atlas a = synthAtlas();
            auto f = blankFrame(640, 480);
            composeOverlay(f.data(), 640, 480, a, 0.5f, false, 1000u, 0u);
            const uint8_t want[16] = {10, 10, 10, 10, 20, 20, 20, 20, 50, 50, 50, 50, 50, 50, 50, 50};
            for (int i = 0; i < 16; ++i)
                t.IsTrue(pxAt(f, 640, ringCx(), ringCy(i)) == want[i], "ring band"); });
        tc.Run("full meter: bands, red jewel, pill, splash", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_hud;
            Atlas a = synthAtlas();
            auto f = blankFrame(640, 480);
            composeOverlay(f.data(), 640, 480, a, 1.0f, true, 1000u, 1045u);
            const uint8_t want[16] = {10, 10, 10, 10, 20, 20, 20, 20, 30, 30, 30, 30, 40, 40, 40, 40};
            t.IsTrue(pxAt(f, 640, 588, 125) == 120, "splash covers jewel center");
            t.IsTrue(pxAt(f, 640, 535, 100) == 120, "splash");
            t.IsTrue(pxAt(f, 640, 588, ringCy(15)) == 120, "splash covers top ring");
            t.IsTrue(pxAt(f, 640, 588, 410) == 110, "pill");
            auto g = blankFrame(640, 480);
            composeOverlay(g.data(), 640, 480, a, 1.0f, true, 2000u, 1045u);
            for (int i = 0; i < 16; ++i)
                t.IsTrue(pxAt(g, 640, ringCx(), ringCy(i)) == want[i], "ring band");
            t.IsTrue(pxAt(g, 640, 588, 125) == 100, "jewel red after splash");
            t.IsTrue(pxAt(g, 640, 535, 100) == 1, "splash expired"); });
        tc.Run("jewel pulses when full", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_hud;
            Atlas a = synthAtlas();
            auto f = blankFrame(640, 480);
            composeOverlay(f.data(), 640, 480, a, 1.0f, true, 0u, 0u);
            t.IsTrue(pxAt(f, 640, 575, 120) == 100, "bright phase");
            auto g = blankFrame(640, 480);
            composeOverlay(g.data(), 640, 480, a, 1.0f, true, 8u, 0u);
            t.IsTrue(pxAt(g, 640, 575, 120) == 82, "dim phase"); });
        tc.Run("arch letters spaced with arc bottoms", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_hud;
            Atlas a = synthAtlas();
            auto f = blankFrame(640, 480);
            composeOverlay(f.data(), 640, 480, a, 0.0f, false, 1000u, 0u);
            t.IsTrue(pxAt(f, 640, 543, 75) == 60, "T");
            t.IsTrue(pxAt(f, 640, 561, 71) == 61, "R");
            t.IsTrue(pxAt(f, 640, 578, 67) == 62, "I");
            t.IsTrue(pxAt(f, 640, 594, 67) == 63, "C");
            t.IsTrue(pxAt(f, 640, 612, 71) == 64, "K");
            t.IsTrue(pxAt(f, 640, 631, 75) == 65, "Y");
            t.IsTrue(pxAt(f, 640, 578, 56) == 62, "I raised");
            t.IsTrue(pxAt(f, 640, 541, 60) == 1, "T lower, outside slab");
            t.IsTrue(pxAt(f, 640, 551, 75, 1) == 1, "gap between letters");
            t.IsTrue(pxAt(f, 640, 548, 80, 1) == 1, "backing slab dims label gap");
            t.IsTrue(pxAt(f, 640, 520, 80, 1) == 2, "slab ends at x544"); });
        tc.Run("layout scales to export size", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_hud;
            Atlas a = synthAtlas();
            auto f = blankFrame(1280, 960);
            composeOverlay(f.data(), 1280, 960, a, 1.0f, false, 1000u, 0u);
            t.IsTrue(pxAt(f, 1280, 1176, 2 * ringCy(0)) == 10, "bottom ring at 2x");
            t.IsTrue(pxAt(f, 1280, 1176, 2 * ringCy(15)) == 40, "top ring at 2x"); });
        tc.Run("faults draw nothing and never crash", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_hud;
            Atlas bad;
            auto f = blankFrame(640, 480);
            composeOverlay(f.data(), 640, 480, bad, 1.0f, true, 0u, 45u);
            t.IsTrue(pxAt(f, 640, 588, 391) == 1, "bad atlas inert");
            composeOverlay(nullptr, 640, 480, synthAtlas(), 1.0f, true, 0u, 45u);
            t.IsTrue(true, "null frame survives"); });});
}
