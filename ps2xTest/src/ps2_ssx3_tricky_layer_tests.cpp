#include "MiniTest.h"
#include "ps2_ssx3_tricky_layer.h"

// TKL1: Tricky layer seam tests. Pure seams (config, context gate, F7/F8
// validation) plus reducer/lifecycle tests against fake RAM (F9: a missing
// atlas must not silence a burst; F1: quick-load rebuilds aliases).
namespace
{
using ps2_ssx3_tricky_hud::Atlas;
using ps2_ssx3_tricky_hud::kAtlasH;
using ps2_ssx3_tricky_hud::kAtlasW;
using ps2_ssx3_tricky_layer::Config;
using ps2_ssx3_tricky_layer::CourseContext;
using ps2_ssx3_tricky_layer::CourseKind;
using ps2_ssx3_tricky_layer::PresentationPacket;
using ps2_ssx3_tricky_layer::RacePhase;
using ps2_ssx3_tricky_layer::Reducer;

constexpr uint32_t kRamSize = 0x600000u;
constexpr uint32_t kG = 0x1000u, kA = 0x2000u, kB = 0x3000u, kR = 0x5000u;
constexpr uint32_t kMgr = 0x10000u, kApp = 0x11000u, kVt = 0x12000u;

void w32(std::vector<uint8_t> &ram, uint32_t addr, uint32_t v)
{
    std::memcpy(ram.data() + addr, &v, 4);
}

void wf(std::vector<uint8_t> &ram, uint32_t addr, float v)
{
    std::memcpy(ram.data() + addr, &v, 4);
}

void wstr(std::vector<uint8_t> &ram, uint32_t addr, uint32_t width, const char *s)
{
    std::memset(ram.data() + addr, 0, width);
    std::memcpy(ram.data() + addr, s, std::strlen(s));
}

// Fake live race: the HUD chain, meter words, race clock, the app-update
// chain (live) and clear loading slots.
void fakeLive(std::vector<uint8_t> &ram, float fill, int32_t level, uint32_t clock)
{
    using namespace ps2_ssx3_tricky_hud;
    w32(ram, kChainRoot, kG);
    w32(ram, kG + kChainAOff, kA);
    w32(ram, kA + kChainBOff, kB);
    w32(ram, kB + kChainROff, kR);
    wf(ram, kR + kRiderFillOff, fill);
    w32(ram, kR + kRiderUberOff, static_cast<uint32_t>(level));
    w32(ram, kB + kRaceClockOff, clock);
    w32(ram, ps2_ssx3_course::kAppMgrPtr, kMgr);
    w32(ram, kMgr, kApp);
    w32(ram, kApp, kVt);
    w32(ram, kVt + ps2_ssx3_course::kAppUpdateSlot, ps2_ssx3_course::kGameUpdate);
}

void fakeFrozen(std::vector<uint8_t> &ram)
{
    // Menus: no live update (vt slot cleared), clock untouched.
    w32(ram, kVt + ps2_ssx3_course::kAppUpdateSlot, 0u);
}

// One mode over event 0 (name + archive), armed against `ram`, then RAM
// flipped to the mode (Tricky) or left stock by ramToMode.
void armOneMode(std::vector<uint8_t> &ram, bool withAlias)
{
    using namespace ps2_ssx3_course;
    Mode m;
    m.name = "T";
    ModeRow r;
    r.event = 0;
    r.name = "TRICKYNAME";
    r.archive = "TRICKYARCH";
    m.rows.push_back(r);
    if (withAlias)
    {
        ModeAlias a;
        a.disc = "/DISC";
        a.host = "/HOST";
        m.aliases.push_back(a);
    }
    std::vector<Mode> modes;
    modes.push_back(m);
    std::string log;
    armModes(courseModes(), ram.data(), modes, [&](const std::string &s) { log += s; });
}

void ramToMode(std::vector<uint8_t> &ram, bool tricky)
{
    using namespace ps2_ssx3_course;
    const uint32_t name = kEventBase + 0u * kEventStride + 4u;
    const uint32_t arch = kEventBase + 0u * kEventStride + 68u;
    if (tricky)
    {
        wstr(ram, name, 32u, "TRICKYNAME");
        wstr(ram, arch, 16u, "TRICKYARCH");
    }
    else
    {
        wstr(ram, name, 32u, "");
        wstr(ram, arch, 16u, "");
    }
}

void resetLayerForTest()
{
    using namespace ps2_ssx3_tricky_layer;
    Reducer &r = reducer();
    resetRunState(r);
    r.inTricky = false;
    r.atlasTried = false;
    r.atlas = ps2_ssx3_tricky_hud::Atlas{};
    PostQueue &q = postQueue();
    q.head = q.tail = 0u;
    q.count = 0u;
    ps2_ssx3_tricky_song::Player &p = ps2_ssx3_tricky_song::player();
    p.song = ps2_ssx3_tricky_song::Song{};
    p.cmd.store(0u, std::memory_order_relaxed);
    p.cmdEpoch.store(0u, std::memory_order_relaxed);
    p.liveEpoch.store(0u, std::memory_order_relaxed);
    p.suspended.store(false, std::memory_order_relaxed);
    p.resume = 0u;
    p.songPeak = 0;
    p.cur = ps2_ssx3_tricky_song::BurstStats{};
    p.last = ps2_ssx3_tricky_song::BurstStats{};
    p.burstsDone = 0u;
    p.burstsStarted = 0u;
    p.probeLastLeft = 0u;
    p.probeActive = false;
    p.probeMixLogged = true; // quiet: the mix log is proven elsewhere
    ps2_cd_overlay::clearModeAlias();
    ps2_ssx3_course::courseModes() = ps2_ssx3_course::Modes{};
}

Atlas synthAtlas256()
{
    using namespace ps2_ssx3_tricky_hud;
    std::vector<uint8_t> blob;
    const char magic[8] = {'T', 'K', 'H', 'U', 'D', '2', '\0', '\0'};
    blob.insert(blob.end(), magic, magic + 8);
    const uint32_t wh[3] = {256u, 256u, 0u};
    blob.insert(blob.end(), reinterpret_cast<const uint8_t *>(wh),
                reinterpret_cast<const uint8_t *>(wh) + 12);
    blob.insert(blob.end(), 256u * 256u * 4u, 0);
    return parseAtlas(blob.data(), blob.size());
}

std::vector<uint8_t> atlasBlob(uint32_t w, uint32_t h)
{
    std::vector<uint8_t> blob;
    const char magic[8] = {'T', 'K', 'H', 'U', 'D', '2', '\0', '\0'};
    blob.insert(blob.end(), magic, magic + 8);
    const uint32_t wh[3] = {w, h, 0u};
    blob.insert(blob.end(), reinterpret_cast<const uint8_t *>(wh),
                reinterpret_cast<const uint8_t *>(wh) + 12);
    blob.insert(blob.end(), static_cast<size_t>(w) * h * 4u, 0);
    return blob;
}

} // namespace

void register_ps2_ssx3_tricky_layer_tests();

void register_ps2_ssx3_tricky_layer_tests()
{
    MiniTest::Case("Ps2Ssx3TrickyLayerConfig", [](TestCase &tc)
                   {
        tc.Run("config parses every knob, off unless exactly 1", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_layer;
            Config c = parseConfig(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
            t.IsTrue(!c.hud && !c.gems && c.artPath.empty() && c.songPath.empty() && c.gemsTable.empty() &&
                         !c.forced.ok && c.lettersPreset == -1 && !c.apPtrSet,
                     "all unset");
            c = parseConfig("1", "/a.bin", "1,1", "3", "/s.wav", "1", "/g.tsv", "0x53ff4c");
            t.IsTrue(c.hud && c.gems, "hud+gems on");
            t.IsTrue(c.artPath == "/a.bin" && c.songPath == "/s.wav" && c.gemsTable == "/g.tsv", "paths");
            t.IsTrue(c.forced.ok && c.forced.fill == 1.0f && c.forced.level == 1, "force");
            t.IsTrue(c.lettersPreset == 3, "preset");
            t.IsTrue(c.apPtrSet && c.apPtrAddr == 0x53ff4cu, "ap ptr");
            t.IsTrue(!parseConfig("0", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr).hud,
                     "0 is off");
            t.IsTrue(!parseConfig("yes", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr).hud,
                     "yes is off");
            t.IsTrue(!parseConfig("1", nullptr, nullptr, nullptr, nullptr, "0", nullptr, nullptr).gems,
                     "gems 0 is off");
            c = parseConfig("1", "", nullptr, "9", "", nullptr, "", "");
            t.IsTrue(c.artPath.empty() && c.songPath.empty() && c.gemsTable.empty() &&
                         c.lettersPreset == -1 && !c.apPtrSet,
                     "empty means unset"); }); });

    MiniTest::Case("Ps2Ssx3TrickyLayerContext", [](TestCase &tc)
                   {
        tc.Run("context gate: kind, phase, feature policy", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_layer;
            CourseContext cx = deriveContext(0u, true, false, true, 7u, 100u);
            t.IsTrue(cx.kind == CourseKind::Stock && cx.phase == RacePhase::Frontend, "stock");
            t.IsTrue(!contextAllowsHud(cx) && !contextAllowsGems(cx) && !contextSuspendsSong(cx),
                     "stock allows nothing, suspends nothing");
            t.IsTrue(cx.raceEpoch == 7u && cx.tick == 100u, "epoch+tick carried");
            cx = deriveContext(1u, true, false, true, 0u, 0u);
            t.IsTrue(cx.kind == CourseKind::Tricky && cx.phase == RacePhase::Racing, "racing");
            t.IsTrue(contextAllowsHud(cx) && contextAllowsGems(cx) && !contextSuspendsSong(cx),
                     "racing draws + sounds");
            cx = deriveContext(1u, true, false, false, 0u, 0u);
            t.IsTrue(cx.phase == RacePhase::Paused, "frozen live event is paused");
            t.IsTrue(!contextAllowsHud(cx) && contextSuspendsSong(cx), "paused suspends");
            cx = deriveContext(1u, false, false, true, 0u, 0u);
            t.IsTrue(cx.phase == RacePhase::Frontend, "menus are frontend even with a running clock");
            t.IsTrue(!contextAllowsHud(cx) && contextSuspendsSong(cx), "frontend suspends");
            cx = deriveContext(1u, true, true, true, 0u, 0u);
            t.IsTrue(cx.phase == RacePhase::Loading, "loading wins over racing");
            t.IsTrue(!contextAllowsHud(cx) && contextSuspendsSong(cx), "loading suspends");
            cx = deriveContext(2u, true, false, true, 3u, 9u);
            t.IsTrue(cx.kind == CourseKind::Tricky && cx.modeIndex == 2u && cx.raceEpoch == 3u, "mode 2");
            t.IsTrue(std::strcmp(phaseName(RacePhase::Paused), "Paused") == 0, "phase name"); }); });

    MiniTest::Case("Ps2Ssx3TrickyLayerF7", [](TestCase &tc)
                   {
        tc.Run("guest pointers: segment, alignment, span, no wrap", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_hud;
            const size_t n = 0x10000u;
            uint32_t off = 0u;
            t.IsTrue(validateGuestPtr(0x1000u, 4u, n, off) && off == 0x1000u, "low ok");
            t.IsTrue(validateGuestPtr(0x80001000u, 4u, n, off) && off == 0x1000u, "kseg0 mirrors");
            t.IsTrue(validateGuestPtr(0xA0001000u, 4u, n, off) && off == 0x1000u, "kseg1 mirrors");
            t.IsTrue(!validateGuestPtr(0u, 4u, n, off), "null refused");
            t.IsTrue(!validateGuestPtr(0x1001u, 4u, n, off), "misaligned refused");
            t.IsTrue(!validateGuestPtr(0x10000000u, 4u, n, off), "seg 1 refused");
            t.IsTrue(!validateGuestPtr(0x70000000u, 4u, n, off), "scratchpad refused");
            t.IsTrue(!validateGuestPtr(0xC0001000u, 4u, n, off), "kseg2 refused");
            t.IsTrue(!validateGuestPtr(0xFFFFFFFFu, 4u, n, off), "all-ones refused");
            t.IsTrue(!validateGuestPtr(0xFFFFFu, 4u, n, off), "span past ram refused");
            t.IsTrue(!validateGuestPtr(0xFFFFFFFCu, 8u, n, off), "wrap into range refused");
            t.IsTrue(!validateGuestPtr(0x1000u, 0u, n, off), "empty span refused");
            t.IsTrue(!validateGuestPtr(0x1000u, n + 1u, n, off), "oversize span refused");
            // The validated lane reads through base+off in one step.
            std::vector<uint8_t> ram(n, 0);
            const uint32_t v = 0x12345678u;
            std::memcpy(ram.data() + 0x2000u, &v, 4);
            uint32_t out = 0u;
            t.IsTrue(readGuestU32(ram.data(), n, 0x1FFCu, 4u, out) && out == v, "base+off reads");
            t.IsTrue(!readGuestU32(ram.data(), n, 0xFFFFFFFCu, 8u, out), "wrapping base refused");
            float f = 0.0f;
            t.IsTrue(readGuestF32(ram.data(), n, 0x1FFCu, 4u, f), "f32 reads");
            t.IsTrue(writeGuestU32(ram.data(), n, 0x1FFCu, 8u, 0xABu), "u32 writes");
            t.IsTrue(!writeGuestU32(ram.data(), n, 0xC0000000u, 0u, 1u), "bad seg write refused");
            // One boundary predicate for the reducer and the gems poll.
            t.IsTrue(raceBoundaryCrossed(1u, 100u, 2u, 100u), "new rider");
            t.IsTrue(raceBoundaryCrossed(1u, 100u, 1u, 50u), "rewound clock");
            t.IsTrue(!raceBoundaryCrossed(1u, 100u, 1u, 100u), "steady");
            t.IsTrue(!raceBoundaryCrossed(1u, 100u, 1u, 101u), "advancing"); }); });

    MiniTest::Case("Ps2Ssx3TrickyLayerF8", [](TestCase &tc)
                   {
        tc.Run("atlas layout: exact 256, every rect inside", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_hud;
            t.IsTrue(atlasLayoutValid(256, 256), "256x256 fits");
            t.IsTrue(!atlasLayoutValid(1, 1), "1x1 cannot fit");
            t.IsTrue(!atlasLayoutValid(230, 256), "pole cell overflows");
            t.IsTrue(!atlasLayoutValid(256, 100), "short height fails");
            t.IsTrue(!atlasLayoutValid(0, 0), "zero fails");
            t.IsTrue(!atlasLayoutValid(-1, 256), "negative fails");
            t.IsTrue(synthAtlas256().ok, "synth 256 parses");
            std::vector<uint8_t> tiny = atlasBlob(1u, 1u);
            t.IsTrue(!parseAtlas(tiny.data(), tiny.size()).ok, "TKA1's 1x1 counterexample refused");
            std::vector<uint8_t> big = atlasBlob(512u, 512u);
            t.IsTrue(!parseAtlas(big.data(), big.size()).ok, "512 refused (rects assume 256)");
            std::vector<uint8_t> good = atlasBlob(256u, 256u);
            t.IsTrue(parseAtlas(good.data(), good.size()).ok, "256 accepted");
            t.IsTrue(!parseAtlas(good.data(), 100u).ok, "truncated refused");
            t.IsTrue(!loadAtlasFile("/nonexistent-tkl1/x.bin").ok, "missing file");
            t.IsTrue(!loadAtlasFile(nullptr).ok, "null path"); }); });

    MiniTest::Case("Ps2Ssx3TrickyLayerSong", [](TestCase &tc)
                   {
        tc.Run("suspend freezes, cancel retires, resume is exact", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_song;
            resetLayerForTest();
            Player &p = player();
            p.song.frames = 4u;
            p.song.pcm = {100, 200, 300, 400, 500, 600, 700, 800};
            p.song.ok = true;
            startBurst(10u);
            uint64_t c = p.cmd.load(std::memory_order_relaxed);
            t.IsTrue((c & 0xffffffffull) == kBurstFrames, "burst opens");
            setSuspended(true);
            int16_t out[4] = {0, 0, 0, 0};
            mixInto(out, 2u);
            t.IsTrue(out[0] == 0 && out[1] == 0, "suspended mixes nothing");
            c = p.cmd.load(std::memory_order_relaxed);
            t.IsTrue((c & 0xffffffffull) == kBurstFrames, "suspended consumes nothing");
            setSuspended(false);
            mixInto(out, 2u);
            t.IsTrue(out[0] == 100 && out[1] == 200, "resume continues at the cursor");
            c = p.cmd.load(std::memory_order_relaxed);
            t.IsTrue((c & 0xffffffffull) == kBurstFrames - 2u, "consumed two");
            cancelBurst();
            t.IsTrue(p.cmd.load(std::memory_order_relaxed) == 0u, "cancel zeroes");
            // A command relabelled from a retired generation is dropped.
            p.cmd.store((0ull << 32) | 4u, std::memory_order_relaxed);
            p.cmdEpoch.store(p.liveEpoch.load(std::memory_order_relaxed) - 1u,
                             std::memory_order_relaxed);
            int16_t out2[8] = {0, 0, 0, 0, 0, 0, 0, 0};
            mixInto(out2, 4u);
            t.IsTrue(out2[0] == 0 && p.cmd.load(std::memory_order_relaxed) == 0u, "stale dropped");
            resetLayerForTest(); }); });

    MiniTest::Case("Ps2Ssx3TrickyLayerReducer", [](TestCase &tc)
                   {
        tc.Run("F9: letters and bursts proceed without art, draw stays off", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_layer;
            using namespace ps2_ssx3_tricky_hud;
            resetLayerForTest();
            setConfigForTest(parseConfig("1", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr));
            std::vector<uint8_t> ram(kRamSize, 0);
            armOneMode(ram, false); // stock strings are zeros
            ramToMode(ram, true);
            fakeLive(ram, 0.0f, 0, 100u);
            PresentationPacket p;
            onVBlankTick(ram.data(), ram.size(), 1000u); // clock seen, not yet racing
            t.IsTrue(latestPacket(p) && !p.draw && !p.racing, "first sample hidden");
            fakeLive(ram, 0.0f, 0, 101u);
            onVBlankTick(ram.data(), ram.size(), 1001u);
            t.IsTrue(latestPacket(p) && !p.draw, "no art, no draw");
            // A post lights a letter even though nothing draws.
            noteUberPost(1001u);
            fakeLive(ram, 0.0f, 0, 102u);
            onVBlankTick(ram.data(), ram.size(), 1002u);
            t.IsTrue(latestPacket(p) && p.litLetters == 1, "letter lights without art");
            // The empty->full edge bursts without art (the song is staged
            // directly: initFromEnv's file shape is proven elsewhere).
            ps2_ssx3_tricky_song::Player &pl = ps2_ssx3_tricky_song::player();
            pl.song.frames = 4u;
            pl.song.pcm = {100, 200, 300, 400, 500, 600, 700, 800};
            pl.song.ok = true;
            fakeLive(ram, 1.0f, 1, 103u);
            onVBlankTick(ram.data(), ram.size(), 1003u);
            t.IsTrue((pl.cmd.load(std::memory_order_relaxed) & 0xffffffffull) ==
                         ps2_ssx3_tricky_song::kBurstFrames,
                     "edge bursts without art");
            t.IsTrue(latestPacket(p) && !p.draw && p.splashUntil == 1003u + 45u, "splash set, not drawn");
            resetLayerForTest();
            clearConfigForTest(); });
        tc.Run("draws while racing, hides in pause, suspends the song", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_layer;
            using namespace ps2_ssx3_tricky_hud;
            resetLayerForTest();
            setConfigForTest(parseConfig("1", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr));
            std::vector<uint8_t> ram(kRamSize, 0);
            armOneMode(ram, false);
            ramToMode(ram, true);
            Reducer &r = reducer();
            r.atlas = synthAtlas256();
            r.atlasTried = true;
            fakeLive(ram, 0.5f, 0, 100u);
            onVBlankTick(ram.data(), ram.size(), 2000u);
            fakeLive(ram, 0.5f, 0, 101u);
            onVBlankTick(ram.data(), ram.size(), 2001u);
            PresentationPacket p;
            t.IsTrue(latestPacket(p) && p.draw && p.fill == 0.5f && !p.full, "racing draws");
            t.IsTrue(!ps2_ssx3_tricky_song::player().suspended.load(), "racing sounds");
            // Freeze the clock (pause): hidden within the grace, suspended.
            fakeLive(ram, 0.5f, 0, 101u);
            onVBlankTick(ram.data(), ram.size(), 2002u);
            onVBlankTick(ram.data(), ram.size(), 2003u);
            onVBlankTick(ram.data(), ram.size(), 2004u);
            onVBlankTick(ram.data(), ram.size(), 2005u);
            t.IsTrue(latestPacket(p) && !p.draw && !p.racing, "pause hides");
            t.IsTrue(ps2_ssx3_tricky_song::player().suspended.load(), "pause suspends");
            // Resume: shown and sounding again, same epoch.
            const uint64_t e = epoch();
            fakeLive(ram, 0.5f, 0, 102u);
            onVBlankTick(ram.data(), ram.size(), 2006u);
            t.IsTrue(latestPacket(p) && p.draw, "resume draws");
            t.IsTrue(!ps2_ssx3_tricky_song::player().suspended.load(), "resume sounds");
            t.IsTrue(epoch() == e, "no boundary across a pause");
            // Menus with a still-running clock: hidden, suspended.
            fakeFrozen(ram);
            onVBlankTick(ram.data(), ram.size(), 2007u);
            t.IsTrue(latestPacket(p) && !p.draw, "menus hide");
            t.IsTrue(ps2_ssx3_tricky_song::player().suspended.load(), "menus suspend");
            resetLayerForTest();
            clearConfigForTest(); });
        tc.Run("race boundary resets run state and bumps the epoch", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_layer;
            using namespace ps2_ssx3_tricky_hud;
            resetLayerForTest();
            setConfigForTest(parseConfig("1", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr));
            std::vector<uint8_t> ram(kRamSize, 0);
            armOneMode(ram, false);
            ramToMode(ram, true);
            Reducer &r = reducer();
            r.atlas = synthAtlas256();
            r.atlasTried = true;
            ps2_ssx3_tricky_song::Player &pl = ps2_ssx3_tricky_song::player();
            pl.song.frames = 4u;
            pl.song.pcm.assign(8u, 100);
            pl.song.ok = true;
            fakeLive(ram, 0.0f, 0, 100u);
            onVBlankTick(ram.data(), ram.size(), 3000u);
            fakeLive(ram, 1.0f, 1, 101u);
            noteUberPost(3000u);
            onVBlankTick(ram.data(), ram.size(), 3001u); // racing, T lit, burst open
            PresentationPacket p;
            t.IsTrue(latestPacket(p) && p.draw && p.litLetters == 1, "lit before the retry");
            t.IsTrue((pl.cmd.load(std::memory_order_relaxed) & 0xffffffffull) != 0u, "burst open");
            t.IsTrue(p.splashUntil == 3001u + 45u, "splash set");
            const uint64_t e = epoch();
            // Retry: the clock rewinds. Letters, latches and splash reset;
            // the guest's re-init writes adopt silently (no second burst).
            fakeLive(ram, 1.0f, 1, 5u);
            onVBlankTick(ram.data(), ram.size(), 3002u);
            t.IsTrue(epoch() == e + 1u, "boundary bumps the epoch");
            t.IsTrue(latestPacket(p) && p.litLetters == 0 && p.splashUntil == 0u, "letters+splash reset");
            t.IsTrue(pl.cmd.load(std::memory_order_relaxed) == 0u, "in-flight burst cancelled");
            // A post after the boundary starts a fresh spelling.
            noteUberPost(3002u);
            fakeLive(ram, 0.2f, 0, 6u);
            onVBlankTick(ram.data(), ram.size(), 3003u);
            t.IsTrue(latestPacket(p) && p.litLetters == 1, "fresh spelling after the boundary");
            resetLayerForTest();
            clearConfigForTest(); });
        tc.Run("leaving Tricky cancels the burst and retires the epoch", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_layer;
            using namespace ps2_ssx3_tricky_hud;
            resetLayerForTest();
            setConfigForTest(parseConfig("1", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr));
            std::vector<uint8_t> ram(kRamSize, 0);
            armOneMode(ram, false);
            ramToMode(ram, true);
            Reducer &r = reducer();
            r.atlas = synthAtlas256();
            r.atlasTried = true;
            ps2_ssx3_tricky_song::Player &pl = ps2_ssx3_tricky_song::player();
            pl.song.frames = 4u;
            pl.song.pcm.assign(8u, 100);
            pl.song.ok = true;
            fakeLive(ram, 0.0f, 0, 100u);
            onVBlankTick(ram.data(), ram.size(), 4000u);
            fakeLive(ram, 1.0f, 1, 101u);
            onVBlankTick(ram.data(), ram.size(), 4001u); // edge -> burst open
            t.IsTrue((pl.cmd.load(std::memory_order_relaxed) & 0xffffffffull) != 0u, "burst open");
            const uint64_t e = epoch();
            ramToMode(ram, false); // back to Stock
            onVBlankTick(ram.data(), ram.size(), 4002u);
            t.IsTrue(pl.cmd.load(std::memory_order_relaxed) == 0u, "exit cancels");
            t.IsTrue(epoch() == e + 1u, "exit retires the epoch");
            PresentationPacket p;
            t.IsTrue(latestPacket(p) && !p.draw && p.epoch == e + 1u, "exit publishes hidden");
            resetLayerForTest();
            clearConfigForTest(); }); });

    MiniTest::Case("Ps2Ssx3TrickyLayerLoad", [](TestCase &tc)
                   {
        tc.Run("F1: load rebuilds aliases from restored mode", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_layer;
            resetLayerForTest();
            setConfigForTest(parseConfig("1", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr));
            std::vector<uint8_t> ram(kRamSize, 0);
            armOneMode(ram, true);
            // Tricky rows restored, pending list cleared (the F1 desync):
            // the load rebuilds the mode's aliases before any CD read.
            ramToMode(ram, true);
            t.IsTrue(ps2_cd_overlay::pendingAliases().empty(), "desync set up");
            const uint64_t e = epoch();
            onStateLoaded(ram.data(), ram.size(), 5000u);
            const auto got = ps2_cd_overlay::pendingAliases();
            t.IsTrue(got.size() == 1u && got[0].first == "/DISC" && got[0].second == "/HOST",
                     "aliases rebuilt");
            t.IsTrue(epoch() == e + 1u, "load retires the epoch");
            // Stock rows restored with a stale pending list: cleared.
            ps2_cd_overlay::setModeAliases({{"/DISC", "/HOST"}});
            ramToMode(ram, false);
            onStateLoaded(ram.data(), ram.size(), 5001u);
            t.IsTrue(ps2_cd_overlay::pendingAliases().empty(), "stock clears");
            resetLayerForTest();
            clearConfigForTest(); }); });
}
