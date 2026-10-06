#include "MiniTest.h"
#include "ps2_ssx3_tricky_menu.h"
#include "ps2_ssx3_tricky_preview.h"

#include <cstring>
#include <string>
#include <vector>

// TK11: Tricky mode frontend entry. Synthetic RAM with a LOC file laid out as
// on the disc (LOCH header, LOCT {key, index}, LOCL offsets + UTF-16 strings).
namespace
{
using ps2_ssx3_tricky::LocFile;

void put32(std::vector<uint8_t> &ram, uint32_t a, uint32_t v) { std::memcpy(&ram[a], &v, 4); }

// Builds `f` at `base`: entries {key, index}; strings by index (others "").
void buildLoc(std::vector<uint8_t> &ram, uint32_t base, const LocFile &f,
              const std::vector<std::pair<uint32_t, uint32_t>> &entries,
              const std::vector<std::pair<uint32_t, std::string>> &strings)
{
    std::memcpy(&ram[base], "LOCH", 4);
    put32(ram, base + 4, 0x14);
    put32(ram, base + 0xC, 1);
    put32(ram, base + 0x10, f.pool);
    std::memcpy(&ram[base + 0x14], "LOCT", 4);
    put32(ram, base + 0x18, 0x10);
    put32(ram, base + 0x1C, f.id);
    uint32_t p = base + 0x24;
    for (const auto &e : entries)
    {
        put32(ram, p, e.first);
        put32(ram, p + 4, e.second);
        p += 8;
    }
    const uint32_t pool = base + f.pool;
    std::memcpy(&ram[pool], "LOCL", 4);
    put32(ram, pool + 12, f.count);
    uint32_t s = 16 + 4 * f.count; // string area, relative to the pool
    const uint32_t empty = s;
    s += 2;
    for (uint32_t i = 0; i < f.count; ++i)
        put32(ram, pool + 16 + 4 * i, empty);
    for (const auto &kv : strings)
    {
        put32(ram, pool + 16 + 4 * kv.first, s);
        ps2_ssx3_tricky::writeUtf16(ram.data(), pool + s, kv.second.c_str());
        s += 2 * static_cast<uint32_t>(kv.second.size() + 1);
    }
}

std::vector<uint8_t> stockCourseRam()
{
    using namespace ps2_ssx3_course;
    std::vector<uint8_t> ram(kRamSize, 0);
    for (uint32_t i = 0; i < kRows; ++i)
    {
        std::memcpy(&ram[kEventBase + i * kEventStride], &i, 4);
        std::memcpy(&ram[kTopoBase + i * kTopoStride], &i, 4);
        std::memcpy(&ram[kDiscBase + i * kDiscStride], &i, 4);
    }
    std::memcpy(&ram[kEventBase + 4], "Snow Jam", 8);
    const int32_t loc = 0;
    std::memcpy(&ram[kTopoBase + 16], &loc, 4);
    return ram;
}
} // namespace

void register_ps2_ssx3_tricky_menu_tests()
{
    MiniTest::Case("Ps2Ssx3TrickyMenu", [](TestCase &tc)
                   {
        tc.Run("TK52: highlighted event follows the nav cursor across peaks and event types", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_preview;
            using namespace ps2_ssx3_tricky;
            auto ram = stockCourseRam();
            constexpr uint32_t controller = 0x600000, screen = 0x601000, count = 0x602000;
            constexpr uint32_t objects = 0x603000, nav = 0x604000;
            put32(ram, controller + 0x2e8, screen);
            put32(ram, screen + 0x38, count);
            put32(ram, count, 1);
            put32(ram, screen + 0x3c, objects);
            put32(ram, objects, nav);
            put32(ram, 0x4a259c, 2);
            put32(ram, 0x535c08, 22); // accepted event is deliberately different
            const char *names[] = {"Peak1RaceLocations", "Peak2FreestyleLocations", "Peak3FreerideLocations"};
            const uint32_t bases[] = {0x4781d0, 0x4786e0 + 0x21c, 0x478d38 + 2 * 0x360};
            for (uint32_t type = 0; type < 3; ++type)
            {
                put32(ram, 0x4a25a0, type);
                put32(ram, 0x4a25a4, type);
                put32(ram, nav + 0x38, nameHash(names[type]));
                put32(ram, bases[type], 0);
                put32(ram, bases[type] + 0x6c, 7);
                ram[nav + 0x95] = 0;
                t.Equals(cursorEvent(ram.data(), ram.size(), controller), 0, "first highlighted row");
                ram[nav + 0x95] = 1;
                t.Equals(cursorEvent(ram.data(), ram.size(), controller), 7, "cursor moved, accepted event unchanged");
                ram[nav + 0x95] = 255;
                t.Equals(cursorEvent(ram.data(), ram.size(), controller), -1, "invalid row rejected");
            }
            put32(ram, count, 0xffffffff);
            t.Equals(cursorEvent(ram.data(), ram.size(), controller), -1, "bad widget count rejected");
            put32(ram, 0x4a259c, 1);
            t.Equals(cursorEvent(ram.data(), ram.size(), controller), -1, "Select Goal stays stock");
            t.Equals(cursorEvent(ram.data(), 1024, controller), -1, "short RAM rejected before globals");
            t.IsFalse(range(0xfffffffc, 16, ram.size()), "overflowing pointer rejected"); });

        tc.Run("TK52: course gate excludes stock rows, other substitutions and Megaplex", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky_preview;
            auto ram = stockCourseRam();
            t.IsFalse(course(ram.data(), ram.size(), 0), "stock archive");
            std::memcpy(ram.data() + ps2_ssx3_course::kEventBase + 68, "GARI", 5);
            t.IsTrue(course(ram.data(), ram.size(), 0), "Garibaldi substitution");
            std::memcpy(ram.data() + ps2_ssx3_course::kEventBase + 15 * 100 + 68, "MEGA", 5);
            t.IsFalse(course(ram.data(), ram.size(), 15), "Megaplex excluded");
            t.IsFalse(course(ram.data(), ram.size(), -1), "no cursor");
            t.IsFalse(course(ram.data(), 1024, 0), "short RAM"); });

        tc.Run("name hash matches the LUI/LOC hashes (func_317618)", [](TestCase &t)
               {
            using ps2_ssx3_tricky::nameHash;
            t.Equals(nameHash("5online"), 0x0C653025u, "5online");
            t.Equals(nameHash("2quick play"), 0x09A46569u, "2quick play");
            t.Equals(nameHash("Peak1RaceLoc0"), 0x0AF9F920u, "Peak1RaceLoc0 (FE:Map row widget)");
            t.Equals(nameHash("Map"), 0x5380u, "Map"); });

        tc.Run("Online accept takes the Single Event state; other rows are untouched", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky;
            bool online = false;
            t.Equals(onMainMenuAccept(nameHash("5online"), kStateOnline, online), kStateSingleEvent, "Online -> 9");
            t.IsTrue(online, "online flagged");
            t.Equals(onMainMenuAccept(nameHash("2quick play"), 9u, online), 9u, "Single Event unchanged");
            t.IsFalse(online, "not online");
            t.Equals(onMainMenuAccept(nameHash("3multi play"), 0x16u, online), 0x16u, "Multi Play unchanged"); });

        tc.Run("label: finds CMNAMER, repoints Online to the donor string, idempotent", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky;
            std::vector<uint8_t> ram(ps2_ssx3_course::kRamSize, 0);
            const uint32_t base = 0x6A1230;
            buildLoc(ram, base, kCmnAmer, {{0x11111111u, 3u}, {kKeyOnline, kOnlineIndex}, {0x22222222u, kDonorIndex}},
                     {{kOnlineIndex, "Online"},
                      {kDonorIndex, "The SSX Lobby Server could not be located.  Please check your network settings."}});
            t.Equals(findLoc(ram.data(), ps2_ssx3_course::kRamSize, kCmnAmer), base, "found");
            t.Equals(findLoc(ram.data(), ps2_ssx3_course::kRamSize, kFeAmer), 0u, "other file not matched");
            std::string msg;
            t.Equals(patchLabel(ram.data(), ps2_ssx3_course::kRamSize, base, msg), 1, msg.c_str());
            const uint32_t e = loctEntry(ram.data(), base, kCmnAmer, kKeyOnline);
            t.Equals(ps2_ssx3_tricky::rd32(ram.data(), e + 4), kDonorIndex, "Online now reads the donor index");
            t.Equals(utf16At(ram.data(), ps2_ssx3_course::kRamSize, locString(ram.data(), base, kCmnAmer, kDonorIndex)),
                     std::string("Tricky Courses"), "label text");
            t.Equals(utf16At(ram.data(), ps2_ssx3_course::kRamSize, locString(ram.data(), base, kCmnAmer, kOnlineIndex)),
                     std::string("Online"), "the Online string itself is untouched");
            t.Equals(patchLabel(ram.data(), ps2_ssx3_course::kRamSize, base, msg), 0, "second call: already patched"); });

        tc.Run("label + help: entries holding string addresses (the loader's form in RAM)", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky;
            std::vector<uint8_t> ram(ps2_ssx3_course::kRamSize, 0);
            const uint32_t base = 0xAB0000;
            buildLoc(ram, base, kCmnAmer, {{kKeyOnline, kOnlineIndex}},
                     {{kOnlineIndex, "Online"}, {kDonorIndex, "The SSX Lobby Server could not be located."}});
            const uint32_t e = loctEntry(ram.data(), base, kCmnAmer, kKeyOnline);
            put32(ram, e + 4, locString(ram.data(), base, kCmnAmer, kOnlineIndex)); // resolved as by the loader
            std::string msg;
            t.Equals(patchLabel(ram.data(), ps2_ssx3_course::kRamSize, base, msg), 1, msg.c_str());
            const uint32_t donor = locString(ram.data(), base, kCmnAmer, kDonorIndex);
            t.Equals(ps2_ssx3_tricky::rd32(ram.data(), e + 4), donor, "entry now holds the donor's address");
            t.Equals(utf16At(ram.data(), ps2_ssx3_course::kRamSize, donor), std::string("Tricky Courses"), "label");
            t.Equals(patchLabel(ram.data(), ps2_ssx3_course::kRamSize, base, msg), 0, "idempotent");
            const uint32_t fb = 0xC00000;
            buildLoc(ram, fb, kFeAmer, {{kKeyOnlineHelp, kOnlineHelpIndex}},
                     {{kOnlineHelpIndex, "Compete against SSX 3 players around the world."}});
            const uint32_t he = loctEntry(ram.data(), fb, kFeAmer, kKeyOnlineHelp);
            put32(ram, he + 4, locString(ram.data(), fb, kFeAmer, kOnlineHelpIndex));
            t.Equals(patchHelp(ram.data(), ps2_ssx3_course::kRamSize, fb, msg), 1, msg.c_str()); });

        tc.Run("label: refuses a different CMNAMER without writing", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky;
            std::vector<uint8_t> ram(ps2_ssx3_course::kRamSize, 0);
            const uint32_t base = 0x600000;
            buildLoc(ram, base, kCmnAmer, {{kKeyOnline, kOnlineIndex}},
                     {{kOnlineIndex, "Online"}, {kDonorIndex, "Some other text that is long"}});
            const std::vector<uint8_t> before(ram.begin() + base, ram.begin() + base + 0x10000);
            std::string msg;
            t.Equals(patchLabel(ram.data(), ps2_ssx3_course::kRamSize, base, msg), -1, "refused");
            t.IsTrue(std::equal(before.begin(), before.end(), ram.begin() + base), "nothing written");
            buildLoc(ram, base, kCmnAmer, {{kKeyOnline, 12u}},
                     {{kOnlineIndex, "Online"}, {kDonorIndex, "The SSX Lobby Server could not be located."}});
            t.Equals(patchLabel(ram.data(), ps2_ssx3_course::kRamSize, base, msg), -1, "unexpected index refused"); });

        tc.Run("help: Online help line rewritten in place", [](TestCase &t)
               {
            using namespace ps2_ssx3_tricky;
            std::vector<uint8_t> ram(ps2_ssx3_course::kRamSize, 0);
            const uint32_t base = 0x7B0000;
            const std::string stock = "Compete against SSX 3 players around the world.";
            buildLoc(ram, base, kFeAmer, {{kKeyOnlineHelp, kOnlineHelpIndex}},
                     {{kOnlineHelpIndex, stock}, {kOnlineHelpIndex + 1, "Next string"}});
            std::string msg;
            t.Equals(findLoc(ram.data(), ps2_ssx3_course::kRamSize, kFeAmer), base, "found");
            t.Equals(patchHelp(ram.data(), ps2_ssx3_course::kRamSize, base, msg), 1, msg.c_str());
            const uint32_t s = locString(ram.data(), base, kFeAmer, kOnlineHelpIndex);
            t.Equals(utf16At(ram.data(), ps2_ssx3_course::kRamSize, s), std::string(kHelp), "help text");
            t.Equals(utf16At(ram.data(), ps2_ssx3_course::kRamSize, locString(ram.data(), base, kFeAmer, kOnlineHelpIndex + 1)),
                     std::string("Next string"), "next string intact");
            t.Equals(patchHelp(ram.data(), ps2_ssx3_course::kRamSize, base, msg), 0, "idempotent"); });

        tc.Run("mode: picker entry 1 on, entry 0 off, refused while an event is live", [](TestCase &t)
               {
            using namespace ps2_ssx3_course;
            auto ram = stockCourseRam();
            std::vector<Block> b;
            std::string err, msg;
            Picker &p = picker();
            p = Picker{};
            t.IsFalse(ps2_ssx3_tricky::modeSet(ram.data(), true, msg), "no picker: no write");
            t.IsFalse(msg.empty(), "no picker: says why");
            t.IsTrue(parse("event = 0\npicker = 49:Garibaldi\n", b, err), err.c_str());
            t.IsTrue(armPicker(p, ram.data(), b, [](const std::string &) {}), "armed");
            auto loc0 = [&] { int32_t v = 0; std::memcpy(&v, &ram[kTopoBase + 16], 4); return v; };
            auto name0 = [&] { return std::string(reinterpret_cast<const char *>(&ram[kEventBase + 4])); };
            t.IsTrue(ps2_ssx3_tricky::modeSet(ram.data(), true, msg), msg.c_str());
            t.Equals(loc0(), 49, "Tricky on: location 49");
            t.Equals(name0(), std::string("Garibaldi"), "Tricky on: name");
            t.IsFalse(ps2_ssx3_tricky::modeSet(ram.data(), true, msg), "already on: no-op");
            // Live event: [0x4A5B64] -> mgr, [mgr] -> app, [app] -> vtable, vtable+0x34 = 0x2306B8.
            const uint32_t mgr = 0x500000, app = 0x500100, vt = 0x500200, upd = kGameUpdate;
            std::memcpy(&ram[kAppMgrPtr], &mgr, 4);
            std::memcpy(&ram[mgr], &app, 4);
            std::memcpy(&ram[app], &vt, 4);
            std::memcpy(&ram[vt + kAppUpdateSlot], &upd, 4);
            t.IsFalse(ps2_ssx3_tricky::modeSet(ram.data(), false, msg), "refused in an event");
            t.Equals(msg, std::string("mode: not now, an event is live"), "why");
            t.Equals(loc0(), 49, "still on");
            const uint32_t zero = 0;
            std::memcpy(&ram[vt + kAppUpdateSlot], &zero, 4);
            t.IsTrue(ps2_ssx3_tricky::modeSet(ram.data(), false, msg), msg.c_str());
            t.Equals(loc0(), 0, "Tricky off: stock location");
            t.Equals(name0(), std::string("Snow Jam"), "Tricky off: stock name");
            p = Picker{}; });

        tc.Run("mode: TK9 course mode 1 on, Stock off, across two modes", [](TestCase &t)
               {
            using namespace ps2_ssx3_course;
            auto ram = stockCourseRam();
            std::memcpy(&ram[kEventBase + 68], "BAM", 3);
            std::memcpy(&ram[0x47BDB0], "data/ui/courspic.big", 21);
            std::vector<Block> b;
            std::vector<Mode> m;
            std::string err, msg;
            t.IsTrue(parse("mode = Tricky\nrow = 0:GARI:Garibaldi\n"
                           "poke = 0x47BDB0:data/ui/courspic.big:data/ui/courspit.big\n"
                           "mode = Two\nrow = 0:-:Other\n",
                           b, err, &m),
                     err.c_str());
            Modes &ms = courseModes();
            t.IsTrue(armModes(ms, ram.data(), m, [](const std::string &) {}), "armed");
            auto str = [&](uint32_t a) { return std::string(reinterpret_cast<const char *>(&ram[a])); };
            t.IsTrue(ps2_ssx3_tricky::modeSet(ram.data(), true, msg), msg.c_str());
            t.Equals(modeCurrent(ms, ram.data()), static_cast<size_t>(1), "mode 1");
            t.Equals(str(kEventBase + 4), std::string("Garibaldi"), "name");
            t.Equals(str(kEventBase + 68), std::string("GARI"), "archive");
            t.Equals(str(0x47BDB0), std::string("data/ui/courspit.big"), "poke");
            t.IsFalse(ps2_ssx3_tricky::modeSet(ram.data(), true, msg), "already on");
            std::string step;
            t.Equals(modeNext(ms, ram.data(), step), 2, "chord to mode Two");
            t.IsTrue(ps2_ssx3_tricky::modeSet(ram.data(), true, msg), "from Two back to Tricky (via Stock)");
            t.Equals(modeCurrent(ms, ram.data()), static_cast<size_t>(1), "mode 1 again");
            t.IsTrue(ps2_ssx3_tricky::modeSet(ram.data(), false, msg), msg.c_str());
            t.Equals(modeCurrent(ms, ram.data()), static_cast<size_t>(0), "Stock");
            t.Equals(str(kEventBase + 4), std::string("Snow Jam"), "stock name");
            t.Equals(str(kEventBase + 68), std::string("BAM"), "stock archive");
            t.Equals(str(0x47BDB0), std::string("data/ui/courspic.big"), "poke undone");
            ms = Modes{}; });
    });
}
