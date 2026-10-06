#include "MiniTest.h"
#include "ps2_ach.h"
#include "ps2_ui_toast.h"

#include "rc_error.h"
#include "rc_runtime.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{

// Save/restore one env key across a case (the suite shares one process).
struct EnvGuard
{
    const char *key;
    bool had;
    std::string saved;
    explicit EnvGuard(const char *k) : key(k)
    {
        const char *v = std::getenv(k);
        had = (v != nullptr);
        if (had)
            saved = v;
    }
    ~EnvGuard()
    {
        if (had)
            ::setenv(key, saved.c_str(), 1);
        else
            ::unsetenv(key);
    }
};

struct EnvHold
{
    EnvGuard ach{"PS2X_ACHIEVEMENTS"};
    EnvGuard overlay{"PS2X_CD_OVERLAY"};
    EnvGuard picker{"PS2X_SSX3_COURSE_PICKER"};
    EnvGuard menu{"PS2X_SSX3_TRICKY_MENU"};
    EnvGuard set{"PS2X_ACH_SET"};
};

void clearAchEnv()
{
    ::unsetenv("PS2X_ACHIEVEMENTS");
    ::unsetenv("PS2X_CD_OVERLAY");
    ::unsetenv("PS2X_SSX3_COURSE_PICKER");
    ::unsetenv("PS2X_SSX3_TRICKY_MENU");
    ::unsetenv("PS2X_ACH_SET");
}

// Synthetic guest memory: zeroed RDRAM + scratch, menu vs race rider words.
struct FakeGuest
{
    std::vector<uint8_t> rdram = std::vector<uint8_t>(ps2_ach::kRdramSize, 0);
    std::vector<uint8_t> scratch = std::vector<uint8_t>(ps2_ach::kScratchSize, 0);
    ps2_ach::MemView view() const { return {rdram.data(), scratch.data()}; }
    void setFloat(uint32_t addr, float v) { std::memcpy(rdram.data() + addr, &v, 4); }
};

struct TriggerCount
{
    int n = 0;
    uint32_t lastId = 0;
};

TriggerCount g_tc;

void RC_CCONV countHandler(const rc_runtime_event_t *e)
{
    if (e && e->type == RC_RUNTIME_EVENT_ACHIEVEMENT_TRIGGERED)
    {
        ++g_tc.n;
        g_tc.lastId = e->id;
    }
}

uint32_t RC_CCONV viewReader(uint32_t address, uint8_t *buffer, uint32_t numBytes, void *ud)
{
    const auto *view = static_cast<const ps2_ach::MemView *>(ud);
    return ps2_ach::readGuestMemory(address, buffer, numBytes, *view);
}

const char *kTestSetJson = R"json({
  "ID": 2928, "Title": "SSX 3 (test)",
  "Achievements": [
    {"ID": 1, "Title": "race started", "Description": "Z over 1000",
     "MemAddr": "fF005409c8>F1000.0", "Points": 1, "Flags": 3},
    {"ID": 2, "Title": "Happiness entered", "Description": "course 14",
     "MemAddr": "0xH5305f0=14", "Points": 1, "Flags": 3},
    {"ID": 3, "Title": "unofficial", "Description": "skip",
     "MemAddr": "0xH5305f0=14", "Points": 1, "Flags": 5},
    {"ID": 101000001, "Title": "Warning: Unknown Emulator", "Description": "skip",
     "MemAddr": "1=1.300.", "Points": 0, "Flags": 3},
    {"ID": 4, "Title": "empty", "Description": "skip", "MemAddr": "", "Points": 0, "Flags": 3}
  ],
  "Leaderboards": [{"ID": 1, "Title": "ignored"}]
})json";

std::string tmpDir()
{
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "ps2x_ach_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir.string();
}

} // namespace

void register_ps2_ach_tests()
{
    MiniTest::Case("Ps2Ach", [](TestCase &tc)
                   {
        tc.Run("knob + tricky predicates read the env", [](TestCase &t)
               {
            EnvHold hold;
            clearAchEnv();
            t.IsFalse(ps2_ach::knobOn(), "knob unset is off");
            t.IsFalse(ps2_ach::trickyActive(), "no tricky knobs");
            ::setenv("PS2X_ACHIEVEMENTS", "1", 1);
            t.IsTrue(ps2_ach::knobOn(), "knob =1 is on");
            ::setenv("PS2X_ACHIEVEMENTS", "0", 1);
            t.IsFalse(ps2_ach::knobOn(), "knob =0 is off");
            ::setenv("PS2X_ACHIEVEMENTS", "yes", 1);
            t.IsFalse(ps2_ach::knobOn(), "knob =yes is off");
            ::unsetenv("PS2X_ACHIEVEMENTS");
            ::setenv("PS2X_CD_OVERLAY", "/x", 1);
            t.IsTrue(ps2_ach::trickyActive(), "overlay dir is tricky");
            ::setenv("PS2X_CD_OVERLAY", "", 1);
            t.IsFalse(ps2_ach::trickyActive(), "empty overlay is not");
            ::unsetenv("PS2X_CD_OVERLAY");
            ::setenv("PS2X_SSX3_COURSE_PICKER", "1", 1);
            t.IsTrue(ps2_ach::trickyActive(), "picker is tricky");
            ::setenv("PS2X_SSX3_COURSE_PICKER", "0", 1);
            t.IsFalse(ps2_ach::trickyActive(), "picker 0 is not");
            ::unsetenv("PS2X_SSX3_COURSE_PICKER");
            ::setenv("PS2X_SSX3_TRICKY_MENU", "1", 1);
            t.IsTrue(ps2_ach::trickyActive(), "tricky menu is tricky");
            ::unsetenv("PS2X_SSX3_TRICKY_MENU"); });

        tc.Run("reader maps RDRAM + scratch, clamps, rejects the rest", [](TestCase &t)
               {
            FakeGuest g;
            g.rdram[0x1234] = 0xAB;
            g.rdram[ps2_ach::kRdramSize - 1] = 0xCD;
            g.scratch[0] = 0xEF;
            const auto view = g.view();
            uint8_t buf[8] = {};
            t.Equals(ps2_ach::readGuestMemory(0x1234, buf, 1, view), 1u, "rdram byte");
            t.IsTrue(buf[0] == 0xAB, "rdram value");
            t.Equals(ps2_ach::readGuestMemory(0x02000000u, buf, 1, view), 1u, "scratch byte");
            t.IsTrue(buf[0] == 0xEF, "scratch value");
            t.Equals(ps2_ach::readGuestMemory(ps2_ach::kRdramSize - 1, buf, 4, view), 1u,
                     "clamped at rdram end");
            t.Equals(ps2_ach::readGuestMemory(0x03000000u, buf, 4, view), 0u, "unmapped reads 0");
            t.Equals(ps2_ach::readGuestMemory(0x02004000u, buf, 1, view), 0u, "past scratch reads 0");
            const ps2_ach::MemView nullView{};
            t.Equals(ps2_ach::readGuestMemory(0x100, buf, 1, nullView), 0u, "null view reads 0"); });

        tc.Run("condition eval: menu RAM quiet, race RAM triggers once", [](TestCase &t)
               {
            FakeGuest menu; // all zeros: Z=0, course=0
            FakeGuest race;
            race.setFloat(0x5409c8u, 91054.8f);
            race.rdram[0x5305f0u] = 14;
            rc_runtime_t rt;
            rc_runtime_init(&rt);
            t.IsTrue(rc_runtime_activate_achievement(&rt, 1, "fF005409c8>F1000.0", nullptr, 0) == RC_OK,
                     "race-started activates");
            t.IsTrue(rc_runtime_activate_achievement(&rt, 2, "0xH5305f0=14", nullptr, 0) == RC_OK,
                     "course-14 activates");
            g_tc = TriggerCount{};
            auto mv = menu.view();
            for (int i = 0; i < 120; ++i)
                rc_runtime_do_frame(&rt, countHandler, viewReader, &mv, nullptr);
            t.Equals(g_tc.n, 0, "0 triggers on menu RAM");
            auto rv = race.view();
            rc_runtime_do_frame(&rt, countHandler, viewReader, &rv, nullptr);
            t.Equals(g_tc.n, 2, "both trigger on the first race frame");
            for (int i = 0; i < 10; ++i)
                rc_runtime_do_frame(&rt, countHandler, viewReader, &rv, nullptr);
            t.Equals(g_tc.n, 2, "no re-trigger while held");
            // rcheevos WAITING semantics (trigger.c): a fresh trigger ignores a
            // true first frame and only arms (WAITING -> ACTIVE) on a false
            // one, so pre-satisfied conditions never fire at load. Every
            // generation below arms on menu RAM first, like production (set
            // loads at boot while conditions are false).
            rc_runtime_t rt2;
            rc_runtime_init(&rt2);
            t.IsTrue(rc_runtime_activate_achievement(&rt2, 1, "fF005409c8>F1000.0", nullptr, 0) == RC_OK,
                     "rt2: activates");
            g_tc = TriggerCount{};
            rc_runtime_do_frame(&rt2, countHandler, viewReader, &mv, nullptr);
            rc_runtime_do_frame(&rt2, countHandler, viewReader, &rv, nullptr);
            t.Equals(g_tc.n, 1, "rt2: fresh runtime fires after arming");
            rc_runtime_destroy(&rt2);
            // Destroy -> init -> activate cycle (what resetForTest does):
            // the second generation must fire too.
            rc_runtime_destroy(&rt);
            rc_runtime_init(&rt);
            t.IsTrue(rc_runtime_activate_achievement(&rt, 1, "fF005409c8>F1000.0", nullptr, 0) == RC_OK,
                     "cycle: race-started re-activates");
            g_tc = TriggerCount{};
            rc_runtime_do_frame(&rt, countHandler, viewReader, &mv, nullptr);
            rc_runtime_do_frame(&rt, countHandler, viewReader, &rv, nullptr);
            t.Equals(g_tc.n, 1, "cycle: fires after re-init");
            rc_runtime_destroy(&rt); });

        tc.Run("condition eval: alt group + ResetIf + hit counts (set patterns)", [](TestCase &t)
               {
            // Real-set idioms, verbatim legacy syntax (cf. 229040 alt groups):
            // core AND one alt, the alt resetting while its byte is clear.
            FakeGuest g;
            rc_runtime_t rt;
            rc_runtime_init(&rt);
            t.IsTrue(rc_runtime_activate_achievement(&rt, 10, "0xH5305f0=14S0xH4a6d54=1_d0xH4a6d54=0",
                                                     nullptr, 0) == RC_OK,
                     "alt+reset activates");
            g_tc = TriggerCount{};
            auto v = g.view();
            rc_runtime_do_frame(&rt, countHandler, viewReader, &v, nullptr); // arm (all false)
            g.rdram[0x5305f0u] = 14;                                         // core true, alt false
            rc_runtime_do_frame(&rt, countHandler, viewReader, &v, nullptr);
            t.Equals(g_tc.n, 0, "core alone does not fire");
            g.rdram[0x4a6d54u] = 1; // alt true
            rc_runtime_do_frame(&rt, countHandler, viewReader, &v, nullptr);
            t.Equals(g_tc.n, 1, "core + alt fires");
            rc_runtime_do_frame(&rt, countHandler, viewReader, &v, nullptr);
            t.Equals(g_tc.n, 1, "held: no re-fire");
            rc_runtime_destroy(&rt);
            // Hit counts: `.2.` needs two consecutive true frames.
            FakeGuest h;
            rc_runtime_t rt2;
            rc_runtime_init(&rt2);
            t.IsTrue(rc_runtime_activate_achievement(&rt2, 11, "0xH5305f0=14.2.", nullptr, 0) == RC_OK,
                     "hit-count activates");
            g_tc = TriggerCount{};
            auto hv = h.view();
            rc_runtime_do_frame(&rt2, countHandler, viewReader, &hv, nullptr); // arm (false)
            h.rdram[0x5305f0u] = 14;
            rc_runtime_do_frame(&rt2, countHandler, viewReader, &hv, nullptr); // hits=1
            t.Equals(g_tc.n, 0, "one true frame is not enough");
            rc_runtime_do_frame(&rt2, countHandler, viewReader, &hv, nullptr); // hits=2
            t.Equals(g_tc.n, 1, "second consecutive true frame fires");
            rc_runtime_destroy(&rt2); });

        tc.Run("stock gate: same eval count per wall second at 60 and 120", [](TestCase &t)
               {
            // Stock 60: 600 VBlanks at divisor 1 over 10 wall s.
            ps2_ach::StockGate g60;
            for (uint64_t tick = 1; tick <= 600; ++tick)
                g60.due(ps2_ach::frameForTick(tick, 1u, 0u, false));
            t.Equals(g60.evals(), 600u, "600 evals at stock 60");
            // Full-120 always: 1200 VBlanks at divisor 2 over 10 wall s.
            ps2_ach::StockGate g120;
            for (uint64_t tick = 1; tick <= 1200; ++tick)
                g120.due(ps2_ach::frameForTick(tick, 2u, 0u, false));
            t.Equals(g120.evals(), 600u, "600 evals at full-120");
            t.Equals(g60.evals(), g120.evals(), "equal per wall second");
            // Events mode via the FH1 accumulator: 300 div-1 (half += 2) +
            // 600 div-2 (half += 1) = 600 stock periods over 10 wall s.
            ps2_ach::StockGate gEv;
            uint64_t half = 0;
            for (int i = 0; i < 300; ++i)
            {
                half += 2u;
                gEv.due(ps2_ach::frameForTick(0u, 1u, half, true));
            }
            for (int i = 0; i < 600; ++i)
            {
                half += 1u;
                gEv.due(ps2_ach::frameForTick(0u, 2u, half, true));
            }
            t.Equals(gEv.evals(), 600u, "600 evals across the enter flip"); });

        tc.Run("stock gate: no double or skip across enter/exit flips", [](TestCase &t)
               {
            ps2_ach::StockGate g;
            uint64_t half = 0;
            std::vector<uint64_t> frames;
            auto feed = [&](uint64_t h)
            {
                const uint64_t f = ps2_ach::frameForTick(0u, 0u, h, true);
                if (g.due(f))
                    frames.push_back(f);
            };
            for (int i = 0; i < 10; ++i)
            {
                half += 2u;
                feed(half);
            }
            for (int i = 0; i < 10; ++i)
            {
                half += 1u;
                feed(half);
            }
            for (int i = 0; i < 10; ++i)
            {
                half += 2u;
                feed(half);
            }
            t.Equals(g.evals(), 25u, "10 + 5 + 10 evals");
            bool stepsOfOne = frames.size() == 25u;
            for (size_t i = 1; i < frames.size() && stepsOfOne; ++i)
                stepsOfOne = (frames[i] == frames[i - 1] + 1u);
            t.IsTrue(stepsOfOne, "frames advance by exactly 1"); });

        tc.Run("set parse: list + dict shapes, filters, malformed", [](TestCase &t)
               {
            std::vector<ps2_ach::Entry> out;
            uint32_t skipped = 0;
            std::string err;
            t.IsTrue(ps2_ach::parseSetJson(kTestSetJson, out, skipped, err), "list shape parses");
            t.Equals(out.size(), 2u, "2 kept");
            t.Equals(skipped, 3u, "3 skipped (flags 5, pseudo id, empty addr)");
            t.IsTrue(out[0].id == 1u && out[0].title == "race started", "entry 1 fields");
            t.IsTrue(out[1].memaddr == "0xH5305f0=14", "entry 2 memaddr");
            const char *dictShape = R"json({"achievements": {
              "7": {"id": 7, "title": "t", "description": "d", "memaddr": "0xH1=1",
                    "points": 5, "flags": 3}}})json";
            t.IsTrue(ps2_ach::parseSetJson(dictShape, out, skipped, err), "dict shape parses");
            t.Equals(out.size(), 1u, "dict entry kept");
            t.IsTrue(out[0].id == 7u && out[0].points == 5u, "dict fields");
            t.IsFalse(ps2_ach::parseSetJson("{nope", out, skipped, err), "malformed fails");
            t.IsFalse(err.empty(), "malformed explains");
            t.IsFalse(ps2_ach::parseSetJson("{\"Achievements\": []}", out, skipped, err) == false,
                      "empty list parses");
            t.Equals(out.size(), 0u, "empty list keeps none"); });

        tc.Run("persistence round-trips, missing is empty", [](TestCase &t)
               {
            ps2_ach::UnlockMap m{{1u, 1728000000ull}, {229040u, 1728000060ull}};
            const std::string rendered = ps2_ach::renderStateJson(m);
            ps2_ach::UnlockMap back;
            t.IsTrue(ps2_ach::parseStateJson(rendered, back), "render parses");
            t.IsTrue(back == m, "round-trip equal");
            const std::string dir = tmpDir();
            const std::string path = dir + "/achievements-state.json";
            t.IsTrue(ps2_ach::saveStateFile(path, m), "save ok");
            ps2_ach::UnlockMap fromFile;
            t.IsTrue(ps2_ach::loadStateFile(path, fromFile), "load ok");
            t.IsTrue(fromFile == m, "file round-trip equal");
            ps2_ach::UnlockMap missing;
            t.IsTrue(ps2_ach::loadStateFile(dir + "/nope.json", missing), "missing loads");
            t.IsTrue(missing.empty(), "missing is empty");
            ps2_ach::UnlockMap bad;
            t.IsFalse(ps2_ach::parseStateJson("{bad", bad), "corrupt fails");
            std::error_code ec;
            std::filesystem::remove_all(dir, ec); });

        tc.Run("shared toast: latest wins, expiry and clear", [](TestCase &t)
               {
            ps2x::ui::clearToastForTest();
            std::string got;
            t.IsFalse(ps2x::ui::pollToast(got), "idle polls false");
            ps2x::ui::toast("hello", 30.0f);
            t.IsTrue(ps2x::ui::pollToast(got), "live polls true");
            t.Equals(got, std::string("hello"), "text round-trips");
            ps2x::ui::toast("newer", 30.0f);
            t.IsTrue(ps2x::ui::pollToast(got), "still live");
            t.Equals(got, std::string("newer"), "latest wins");
            ps2x::ui::toast("expired", 0.0f);
            t.IsFalse(ps2x::ui::pollToast(got), "zero seconds never live");
            ps2x::ui::toast("shown", 30.0f);
            ps2x::ui::toast("", 30.0f);
            t.IsFalse(ps2x::ui::pollToast(got), "empty clears");
            ps2x::ui::clearToastForTest(); });

        tc.Run("toast queue: one at a time, paced by expiry (ACH4)", [](TestCase &t)
               {
            ps2_ach::ToastQueue q;
            std::string text;
            t.IsFalse(q.poll(0, false, text), "empty queue shows nothing");
            q.push("A", "first");
            q.push("B", "");
            q.push("C", "third");
            t.Equals(q.pending(), static_cast<size_t>(3u), "3 pending");
            t.IsTrue(q.poll(1000, false, text), "first shows at once");
            t.Equals(text, std::string("A — first"), "title + desc");
            t.Equals(q.lastShown(), text, "lastShown tracks it");
            t.IsFalse(q.poll(1001, false, text), "next waits while A is held");
            t.IsFalse(q.poll(3999, false, text), "still held at 2999 ms");
            t.IsTrue(q.poll(4000, false, text), "B after A expires");
            t.Equals(text, std::string("B"), "no desc, no dash");
            t.IsFalse(q.due(6999), "C not due before B expires");
            t.IsTrue(q.due(7000), "C due at expiry");
            t.IsTrue(q.poll(7000, false, text) && text == "C — third", "C third, FIFO order");
            t.Equals(q.pending(), static_cast<size_t>(0u), "drained");
            t.IsFalse(q.poll(20000, false, text), "nothing after drain");
            t.IsFalse(q.due(20000), "not due when empty"); });

        tc.Run("toast queue: another toast defers, then the queue resumes (ACH4)", [](TestCase &t)
               {
            ps2_ach::ToastQueue q;
            std::string text;
            q.push("A", "");
            q.push("B", "");
            t.IsTrue(q.poll(0, false, text) && text == "A", "A shows");
            // Quick save interrupts at 1 s and is live until 4 s.
            t.IsFalse(q.poll(3000, true, text), "B waits while Saved is on screen");
            t.IsFalse(q.poll(3999, true, text), "still waiting");
            t.Equals(q.pending(), static_cast<size_t>(1u), "B kept");
            t.IsTrue(q.poll(4000, false, text) && text == "B", "B after Saved clears"); });

        tc.Run("toast queue: > 4 pending coalesce, cap 16 (ACH4)", [](TestCase &t)
               {
            ps2_ach::ToastQueue q;
            std::string text;
            for (int i = 0; i < 4; ++i)
                q.push("T" + std::to_string(i), "d");
            t.IsTrue(q.poll(0, false, text) && text == "T0 — d", "4 pending: individual");
            t.Equals(q.pending(), static_cast<size_t>(3u), "3 left");
            ps2_ach::ToastQueue big;
            for (int i = 0; i < 20; ++i)
                big.push("T" + std::to_string(i), "d");
            t.Equals(big.pending(), static_cast<size_t>(20u), "16 held + 4 counted");
            t.IsTrue(big.poll(0, false, text), "coalesced toast shows");
            t.Equals(text, std::string("Unlocked: T0, T1, T2 +17 more"), "3 titles + count");
            t.Equals(big.pending(), static_cast<size_t>(0u), "coalesced drains all");
            ps2_ach::ToastQueue five;
            for (int i = 0; i < 5; ++i)
                five.push("T" + std::to_string(i), "");
            t.IsTrue(five.poll(0, false, text) && text == "Unlocked: T0, T1, T2 +2 more", "5 pending coalesce");
            five.clear();
            t.IsFalse(five.due(0), "clear empties"); });

        tc.Run("paths: override wins, else beside the card", [](TestCase &t)
               {
            EnvHold hold;
            clearAchEnv();
            t.Equals(ps2_ach::defaultSetPath("/h/Documents/mc0", "/e"),
                     std::string("/h/Documents/achievements-set.json"), "beside mc0");
            t.Equals(ps2_ach::defaultSetPath("", "/e"), std::string("/e/achievements-set.json"),
                     "elf dir fallback");
            t.Equals(ps2_ach::setPathFromEnv("/h/Documents/mc0", "/e"),
                     std::string("/h/Documents/achievements-set.json"), "no override");
            ::setenv("PS2X_ACH_SET", "/tmp/custom.json", 1);
            t.Equals(ps2_ach::setPathFromEnv("/h/Documents/mc0", "/e"), std::string("/tmp/custom.json"),
                     "override wins");
            ::unsetenv("PS2X_ACH_SET");
            t.Equals(ps2_ach::statePathForSet("/h/Documents/achievements-set.json"),
                     std::string("/h/Documents/achievements-state.json"), "state beside set"); });

        tc.Run("runtime: knob off and tricky run 0 evals", [](TestCase &t)
               {
            EnvHold hold;
            FakeGuest race;
            race.setFloat(0x5409c8u, 91054.8f);
            clearAchEnv();
            ps2_ach::resetForTest();
            for (uint64_t tick = 1; tick <= 60; ++tick)
                ps2_ach::onVBlankTick(tick, race.rdram.data(), race.scratch.data());
            t.Equals(ps2_ach::vblanksSeen(), 60u, "vblanks counted");
            t.Equals(ps2_ach::evalsTotal(), 0u, "knob off: 0 evals");
            ::setenv("PS2X_ACHIEVEMENTS", "1", 1);
            ::setenv("PS2X_SSX3_TRICKY_MENU", "1", 1);
            ps2_ach::resetForTest();
            for (uint64_t tick = 1; tick <= 60; ++tick)
                ps2_ach::onVBlankTick(tick, race.rdram.data(), race.scratch.data());
            t.Equals(ps2_ach::evalsTotal(), 0u, "tricky: 0 evals");
            t.IsTrue(ps2_ach::trickyLatched(), "tricky latched");
            t.Equals(ps2_ach::unlocksTotal(), 0u, "tricky: 0 unlocks"); });

        tc.Run("runtime: file set unlocks exactly once and persists", [](TestCase &t)
               {
            EnvHold hold;
            clearAchEnv();
            const std::string dir = tmpDir();
            const std::string setPath = dir + "/achievements-set.json";
            {
                std::ofstream f(setPath, std::ios::binary);
                f << kTestSetJson;
            }
            ::setenv("PS2X_ACH_SET", setPath.c_str(), 1);
            ::setenv("PS2X_ACHIEVEMENTS", "1", 1);
            FakeGuest menu;
            FakeGuest race;
            race.setFloat(0x5409c8u, 91054.8f);
            race.rdram[0x5305f0u] = 14;
            // (One reset per phase: the suite shares one process and the FH
            // mode is cached process-wide, so only the first tick of a phase
            // is guaranteed to evaluate.)
            ps2_ach::resetForTest();
            for (uint64_t tick = 1; tick <= 10; ++tick)
                ps2_ach::onVBlankTick(tick, menu.rdram.data(), menu.scratch.data());
            t.Equals(ps2_ach::unlocksTotal(), 0u, "menu RAM: 0 unlocks");
            t.Equals(ps2_ach::activeCount(), 2u, "2 active from file");
            ps2_ach::resetForTest();
            // Arm on menu RAM first (WAITING -> ACTIVE), then race (fires).
            for (uint64_t tick = 1; tick <= 5; ++tick)
                ps2_ach::onVBlankTick(tick, menu.rdram.data(), menu.scratch.data());
            t.Equals(ps2_ach::unlocksTotal(), 0u, "arming: 0 unlocks");
            for (uint64_t tick = 6; tick <= 25; ++tick)
                ps2_ach::onVBlankTick(tick, race.rdram.data(), race.scratch.data());
            t.Equals(ps2_ach::unlocksTotal(), 2u, "race RAM: both unlock");
            for (uint64_t tick = 26; tick <= 35; ++tick)
                ps2_ach::onVBlankTick(tick, race.rdram.data(), race.scratch.data());
            t.Equals(ps2_ach::unlocksTotal(), 2u, "no re-unlock while held");
            {
                // ACH4: the burst is queued, not collapsed: one shown, one waiting.
                std::string live;
                t.IsTrue(ps2x::ui::pollToast(live), "first unlock toast is live");
                t.IsTrue(live.rfind("race started", 0) == 0 || live.rfind("Happiness entered", 0) == 0,
                         "live toast is an unlock");
                t.Equals(ps2_ach::toastsPending(), static_cast<size_t>(1u), "second unlock still queued");
                ps2x::ui::clearToastForTest();
            }
            ps2_ach::UnlockMap persisted;
            t.IsTrue(ps2_ach::loadStateFile(dir + "/achievements-state.json", persisted),
                     "state file written");
            t.Equals(persisted.size(), 2u, "both persisted");
            // Relaunch: already-unlocked ids never activate, never re-toast.
            ps2_ach::resetForTest();
            for (uint64_t tick = 1; tick <= 10; ++tick)
                ps2_ach::onVBlankTick(tick, race.rdram.data(), race.scratch.data());
            t.Equals(ps2_ach::activeCount(), 0u, "relaunch: 0 active");
            t.Equals(ps2_ach::unlocksTotal(), 0u, "relaunch: 0 unlocks");
            std::error_code ec;
            std::filesystem::remove_all(dir, ec); });
    });
}
