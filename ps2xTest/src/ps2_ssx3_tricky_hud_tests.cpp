#include "MiniTest.h"
#include "ps2_ssx3_tricky_hud.h"

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
            t.IsTrue(resolveChainB(nullptr, 0) == 0u, "null ram"); });});

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
}
