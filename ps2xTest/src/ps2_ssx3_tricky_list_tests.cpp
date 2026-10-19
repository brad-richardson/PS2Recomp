#include "MiniTest.h"
#include "ps2_ssx3_tricky_list.h"
#include "ps2_tk_font.h"

#include <algorithm>
#include <cstring>
#include <vector>

// tk-menu-1 Part 2: the dedicated Tricky course list + closed-loop navigator.
// Synthetic RAM; the navigator is driven tick by tick against scripted views.
namespace
{
using namespace ps2_ssx3_tricky_list;

void put32(std::vector<uint8_t> &ram, uint32_t a, uint32_t v) { std::memcpy(&ram[a], &v, 4); }

// Stock nav tables (receipts/ssx3-events-nav.txt): the event id per
// (peak, type, row). Race lists repeat the Rival row; freestyle lists repeat
// the Jam row; P3 Race row 3 is the DONOTUSE pad (event 23).
int navEvent(int peak, int type, int row)
{
    static const int kRace[3][4] = {{0, 1, 14, 14}, {2, 3, 15, 15}, {4, 16, 16, 23}};
    static const int kFree[3][5] = {{5, 8, 11, 14, 14}, {6, 9, 12, 15, 15}, {7, 10, 13, 16, 16}};
    static const int kRide[3][8] = {{14, 17, 5, 0, 8, 18, 1, 11},
                                    {19, 2, 9, 12, 15, 20, 6, 3},
                                    {16, 21, 4, 10, 13, 7, 23, 23}};
    if (type == 0)
        return (peak >= 0 && peak < 3 && row >= 0 && row < 4) ? kRace[peak][row] : -1;
    if (type == 1)
        return (peak >= 0 && peak < 3 && row >= 0 && row < 5) ? kFree[peak][row] : -1;
    return (peak >= 0 && peak < 3 && row >= 0 && row < 8) ? kRide[peak][row] : -1;
}

// RAM with the Map globals and one Event-screen list widget ("screen" object
// at `screen`, one widget `nav` with cursor row `row`), backed by the stock
// nav tables above.
std::vector<uint8_t> viewRam(uint32_t sub, uint32_t peak, uint32_t type, uint8_t row, uint32_t controller,
                              uint32_t screen, uint32_t nav)
{
    std::vector<uint8_t> ram(0x800000, 0);
    put32(ram, 0x4a259c, sub);
    put32(ram, 0x4a25a4, peak);
    put32(ram, 0x4a25a0, type);
    put32(ram, controller + 0x2e8, screen);
    put32(ram, screen + 0x38, 0x700000);
    put32(ram, 0x700000, 1);
    put32(ram, screen + 0x3c, 0x701000);
    put32(ram, 0x701000, nav);
    const char *types[] = {"Race", "Freestyle", "Freeride"};
    char name[48];
    std::snprintf(name, sizeof(name), "Peak%u%sLocations", peak + 1, types[type]);
    put32(ram, nav + 0x38, ps2_ssx3_tricky::nameHash(name));
    ram[nav + 0x95] = row;
    const uint32_t counts[] = {4, 5, 8};
    const uint32_t bases[] = {0x4781d0u, 0x4786e0u, 0x478d38u};
    for (uint32_t r = 0; r < counts[type]; ++r)
        put32(ram, bases[type] + peak * counts[type] * 0x6cu + r * 0x6cu,
              static_cast<uint32_t>(navEvent(static_cast<int>(peak), static_cast<int>(type), r)));
    return ram;
}

FeView evView(uint32_t peak, uint32_t type, uint8_t row)
{
    auto ram = viewRam(2, peak, type, row, 0x600000, 0x601000, 0x602000);
    return readView(ram.data(), static_cast<uint32_t>(ram.size()), 0x600000);
}

// P4: scripted guest whose Peak cursor resets to `reset` on every
// triangle-back (the gari5 stock behavior), or persists (`persist`:
// the p4-gari2 shape). Part 4 goal Garibaldi Slopestyle {peak 0,
// type 1, row 3} on donor ev 14; clamps, no wrap (hard model). The
// Event cursor is tracked too: Cross leaves only on the goal row.
struct ResetDrive
{
    int phase = 0;
    bool handedOff = false;
    int acceptedPeak = -1;
};
ResetDrive driveResetPeak(int reset, bool persist = false)
{
    int cursorPeak = reset, acceptedPeak = reset;
    int cursorType = 0, acceptedType = 0;
    int cursorRow = 0;
    int screen = 0;
    bool left = false;
    NavState n;
    navStart(n, kCourses[0].opts[1], 14, 0);
    uint64_t tick = 0;
    uint16_t lastOut = 0;
    const auto view = [&]() {
        FeView v;
        if (left)
            return v;
        auto ram = viewRam(static_cast<uint32_t>(screen), static_cast<uint32_t>(acceptedPeak),
                           static_cast<uint32_t>(acceptedType), static_cast<uint8_t>(cursorRow), 0x600000,
                           0x601000, 0x602000);
        return readView(ram.data(), static_cast<uint32_t>(ram.size()), 0x600000);
    };
    const auto apply = [&](uint16_t mask) {
        // Part 4: the guest wraps (measured live: Down from Peak 1 lands
        // Peak 3); the old clamp was harder than reality.
        if (mask == kPadDown)
        {
            if (screen == 0)
                cursorPeak = (cursorPeak + 1) % 3;
            else if (screen == 1)
                cursorType = cursorType < 1 ? cursorType + 1 : 1;
            else
                cursorRow = (cursorRow + 1) % 5;
        }
        else if (mask == kPadUp)
        {
            if (screen == 0)
                cursorPeak = (cursorPeak + 2) % 3;
            else if (screen == 1)
                cursorType = cursorType > 0 ? cursorType - 1 : 0;
            else
                cursorRow = (cursorRow + 4) % 5;
        }
        else if (mask == kPadCross)
        {
            if (screen == 0)
            {
                acceptedPeak = cursorPeak;
                screen = 1;
            }
            else if (screen == 1)
            {
                acceptedType = cursorType;
                screen = 2;
            }
            else if (cursorRow == 3)
                left = true;
        }
        else if (mask == kPadTriangle && screen == 1)
        {
            screen = 0;
            if (!persist)
                cursorPeak = reset; // P4: stock re-entry resets the cursor
        }
        else if (mask == kPadTriangle && screen == 2)
            screen = 1;
    };
    for (int i = 0; i < 1500 && n.phase != NavPhase::Done && n.phase != NavPhase::Aborted; ++i)
    {
        NavOut o = navStep(n, view(), tick, false, false);
        if (lastOut != 0 && o.mask == 0)
            apply(lastOut);
        lastOut = o.mask;
        ++tick;
    }
    return {static_cast<int>(n.phase), n.handedOff, acceptedPeak};
}

// A scripted Select Peak/Mode/Event guest: directional presses move the
// cursor (wrap over the list), Cross accepts one level down, Triangle backs
// out one level, and Cross on the goal row leaves the Map screens.
struct FakeGuest
{
    int screen = 2; // 0 peak, 1 mode, 2 event
    int peak = 0, type = 0, row = 0;
    int goalEvent = -1;
    CourseOption goal = {};
    bool left = false;
    int acceptsNeeded = 1; // the guest misses this many Event accepts first

    FeView view()
    {
        FeView v;
        if (left)
        {
            v.mapLive = false;
            return v;
        }
        auto ram = viewRam(static_cast<uint32_t>(screen), static_cast<uint32_t>(peak), static_cast<uint32_t>(type),
                           static_cast<uint8_t>(row), 0x600000, 0x601000, 0x602000);
        v = readView(ram.data(), static_cast<uint32_t>(ram.size()), 0x600000);
        return v;
    }

    void applyFinished(uint16_t mask)
    {
        if (left)
            return;
        if (mask == kPadDown || mask == kPadUp)
        {
            const int d = (mask == kPadDown) ? 1 : -1;
            if (screen == 0)
                peak = (peak + d + 3) % 3;
            else if (screen == 1)
                type = (type + d + 3) % 3;
            else
                row = (row + d + 4) % 4;
        }
        else if (mask == kPadCross)
        {
            if (screen == 0)
                screen = 1;
            else if (screen == 1)
                screen = 2;
            else if (row == goal.row && goalEvent >= 0 && --acceptsNeeded <= 0)
                left = true;
        }
        else if (mask == kPadTriangle)
        {
            if (screen > 0)
                --screen;
            else
                left = true;
        }
    }
};

} // namespace

void register_ps2_ssx3_tricky_list_tests()
{
    MiniTest::Case("Ps2Ssx3TrickyList", [](TestCase &tc)
                   {
        tc.Run("course table: Tricky order, targets and dual-type defaults", [](TestCase &t)
               {
            t.Equals(kCourseCount, 10u, "ten courses");
            const char *want[] = {"Garibaldi", "Snowdream", "Elysium Alps", "Mesablanca", "Merqury City",
                                  "Aloha Ice Jam", "Pipedream", "Untracked", "Tokyo Megaplex", "Alaska"};
            for (uint32_t i = 0; i < 10; ++i)
                t.Equals(std::strcmp(kCourses[i].name, want[i]), 0, "Tricky order");
            // Part 4: every goal is Peak 1 (fresh-card selectable). Singles
            // keep their native rows; dual-mode Races share P1 Race row 0
            // (ev0), Slopestyles share P1 Free row 3 (ev14).
            for (uint32_t i = 0; i < 10; ++i)
                for (uint32_t m = 0; m < kCourses[i].nOpts; ++m)
                    t.Equals(kCourses[i].opts[m].peak, 0, "all goals Peak 1");
            t.Equals(kCourses[0].opts[0].row, 0, "Garibaldi row 0");
            t.Equals(kCourses[6].opts[0].type, 1, "Pipedream freestyle");
            t.Equals(kCourses[6].opts[0].row, 2, "Pipedream row 2");
            t.Equals(kCourses[9].opts[0].type, 1, "Alaska freestyle");
            // Dual-type courses share one event id; Race is the default.
            t.Equals(kCourses[2].nOpts, 2, "Elysium dual");
            t.Equals(kCourses[2].event, 16, "Elysium event 16 both rows");
            t.Equals(kCourses[2].opts[0].row, 0, "Elysium race row 0");
            t.Equals(kCourses[2].opts[1].row, 3, "Elysium jam row 3");
            t.Equals(kCourses[5].opts[0].row, 2, "Aloha race row 2");
            t.Equals(kCourses[5].opts[1].row, 3, "Aloha jam row 3");
            t.Equals(kCourses[8].nOpts, 2, "Megaplex dual");
            t.IsTrue(kCourses[8].tp == nullptr, "Megaplex has no preview art");
            ListState s;
            t.Equals(listGoal(s).row, 0, "step 0 goal is opts[0]");
            s.sel = 2;
            t.Equals(listGoal(s).row, 0, "Elysium step 0 is the Race row");
            s.step = 1;
            s.mode = 1;
            t.Equals(listGoal(s).row, 3, "step 1 mode 1 is the Slopestyle row");
            s.mode = 0;
            t.Equals(listGoal(s).row, 0, "step 1 mode 0 is Race");
            t.Equals(std::strcmp(kCourses[2].modeName[1], "Slopestyle"), 0, "Jam mode name");
            t.Equals(kCourses[6].nOpts, 1, "Pipedream single-mode"); });

        tc.Run("Part 4 table: remapped goals share two fresh P1 cells", [](TestCase &t)
               {
            // Race options share P1 Race row 0 (loaded ev0); Slopestyle
            // options share P1 Free row 3 (loaded ev14). Each mapped
            // option carries map{loadedEv, ownArchive, ownName}.
            const int raceMapped[] = {2, 3, 4, 8};
            const char *raceArch[] = {"ELYS", "MESA", "MERQ", "MEGA"};
            const char *raceNames[] = {"Elysium Alps", "Mesablanca", "Merqury City", "Tokyo Megaplex"};
            for (int k = 0; k < 4; ++k)
            {
                const int i = raceMapped[k];
                t.Equals(kCourses[i].opts[0].peak, 0, "race donor P1");
                t.Equals(kCourses[i].opts[0].type, 0, "donor race list");
                t.Equals(kCourses[i].opts[0].row, 0, "donor row 0 (ev0)");
                t.Equals(kCourses[i].opts[0].map.event, 0, "loaded ev0");
                t.Equals(std::strcmp(kCourses[i].opts[0].map.archive, raceArch[k]), 0, "world archive");
                t.Equals(std::strcmp(kCourses[i].opts[0].map.name, raceNames[k]), 0, "world name");
            }
            const int jamMapped[] = {0, 1, 2, 3, 4, 7, 8, 9};
            const char *jamArch[] = {"GARI", "SNOW", "ELYS", "MESA", "MERQ", "UNTRACK", "MEGA", "ALASKA"};
            const char *jamNames[] = {"Garibaldi", "Snowdream", "Elysium Alps", "Mesablanca",
                                      "Merqury City", "Untracked", "Tokyo Megaplex", "Alaska"};
            for (int k = 0; k < 8; ++k)
            {
                const int i = jamMapped[k];
                const int opt = (kCourses[i].nOpts == 2) ? 1 : 0;
                t.Equals(kCourses[i].opts[opt].peak, 0, "jam donor P1");
                t.Equals(kCourses[i].opts[opt].type, 1, "donor freestyle list");
                t.Equals(kCourses[i].opts[opt].row, 3, "donor row 3 (ev14)");
                t.Equals(kCourses[i].opts[opt].map.event, 14, "loaded ev14");
                t.Equals(std::strcmp(kCourses[i].opts[opt].map.archive, jamArch[k]), 0, "world archive");
                t.Equals(std::strcmp(kCourses[i].opts[opt].map.name, jamNames[k]), 0, "world name");
            }
            // The five native rows stay unmapped: both Aloha rows, both
            // Garibaldi/Snowdream Race rows, Pipedream.
            t.Equals(kCourses[0].opts[0].map.event, -1, "Garibaldi race native");
            t.Equals(kCourses[1].opts[0].map.event, -1, "Snowdream race native");
            t.Equals(kCourses[5].opts[0].map.event, -1, "Aloha race native");
            t.Equals(kCourses[5].opts[1].map.event, -1, "Aloha jam native");
            t.Equals(kCourses[6].opts[0].map.event, -1, "Pipedream native"); });

        tc.Run("list nav wraps per step; single-mode cursor stays put", [](TestCase &t)
               {
            ListState s;
            listMove(s, true);
            t.Equals(s.sel, 9, "up from 0 wraps to Alaska");
            listMove(s, false);
            t.Equals(s.sel, 0, "down wraps back");
            s.sel = 2;
            s.step = 1;
            listMove(s, false);
            t.Equals(s.mode, 1, "Elysium mode down to Slopestyle");
            listMove(s, false);
            t.Equals(s.mode, 0, "mode wraps over the course's own modes");
            s.sel = 6;
            s.mode = 0;
            listMove(s, false);
            t.Equals(s.sel, 6, "step 1 moves modes, not courses");
            t.Equals(s.mode, 0, "single-mode course has one mode"); });

        tc.Run("readView gates on the Map sub-state and short RAM", [](TestCase &t)
               {
            auto ram = viewRam(2, 1, 0, 0, 0x600000, 0x601000, 0x602000);
            FeView v = readView(ram.data(), static_cast<uint32_t>(ram.size()), 0x600000);
            t.IsTrue(v.mapLive, "Event screen is Map-live");
            t.Equals(v.screen, 2, "screen 2");
            t.Equals(v.row, 0, "cursor row");
            t.Equals(v.event, 2, "cursor event");
            put32(ram, 0x4a259c, 0);
            v = readView(ram.data(), static_cast<uint32_t>(ram.size()), 0x600000);
            t.IsTrue(v.mapLive && v.screen == 0, "Peak screen is Map-live");
            t.Equals(v.row, -1, "no cursor row off the Event screen");
            put32(ram, 0x4a259c, 9);
            v = readView(ram.data(), static_cast<uint32_t>(ram.size()), 0x600000);
            t.IsFalse(v.mapLive, "other screens are not Map-live");
            v = readView(ram.data(), 1024, 0x600000);
            t.IsFalse(v.mapLive, "short RAM rejected"); });

        tc.Run("overlay target stays shut on a direct launch, even with live words", [](TestCase &t)
               {
            t.IsTrue(overlayTarget(true, 0, -1, false, 0, 0, false), "Peak screen opens stock");
            t.IsFalse(overlayTarget(true, 0, -1, false, 0, 0, true), "direct launch suppresses the stale open");
            t.IsFalse(overlayTarget(true, 0, -1, true, 1, 0, true), "suppressed even with a new Map generation");
            t.IsTrue(overlayTarget(true, 0, -1, true, 1, 0, false), "a new Map entry re-opens stock");
            t.IsFalse(overlayTarget(true, 0, -1, true, 0, 0, false), "latched same-gen stays shut");
            t.IsFalse(overlayTarget(true, 2, -1, false, 0, 0, false), "dead Event widget stays shut");
            t.IsFalse(overlayTarget(false, 0, -1, false, 0, 0, false), "non-Map words stay shut"); });

        tc.Run("direct goals resolve Race cells, defer freestyle to the navigator", [](TestCase &t)
               {
            for (int i = 0; i < static_cast<int>(kCourseCount); ++i)
            {
                const Course &c = kCourses[i];
                const DirectGoal goal = directGoalFor(c, c.opts[0]);
                t.IsTrue(goal.ok, std::string("course entry directable: ") + c.name);
                if (goal.ok)
                {
                    const int donor = (c.opts[0].map.event >= 0) ? c.opts[0].map.event : c.event;
                    t.Equals(goal.event, donor, "donor event");
                    t.IsTrue(goal.mapped, "world pinned for the records guard");
                }
            }
            const Course &gari = kCourses[0];
            const DirectGoal g = directGoalFor(gari, gari.opts[0]);
            t.IsTrue(g.ok && g.event == 0, "Garibaldi donor ev0 (probe shape)");
            t.Equals(std::string(g.map.archive), std::string("GARI"), "Garibaldi archive");
            const Course &mesa = kCourses[3];
            const DirectGoal m = directGoalFor(mesa, mesa.opts[0]);
            t.IsTrue(m.ok && m.event == 0, "Mesablanca donor ev0");
            t.Equals(std::string(m.map.archive), std::string("MESA"), "Mesablanca archive");
            const Course &snow = kCourses[1];
            const DirectGoal s = directGoalFor(snow, snow.opts[0]);
            t.IsTrue(s.ok && s.event == 1, "Snowdream native donor ev1");
            const Course &pipe = kCourses[6];
            const DirectGoal p = directGoalFor(pipe, pipe.opts[0]);
            t.IsTrue(p.ok && p.event == 11, "Pipedream Super Pipe directable (donor ev11)");
            t.IsTrue(p.typeArg == 3 && p.rulesArg == 2, "Pipe accept args trial (3,2)");
            const Course &aloha = kCourses[5];
            const DirectGoal a = directGoalFor(aloha, aloha.opts[0]);
            t.IsTrue(a.ok && a.event == 14, "Aloha Race directable (donor ev14)");
            t.IsTrue(a.typeArg == 5 && a.rulesArg == 4, "Aloha accept args trial (5,4)");
            const Course &mega = kCourses[8];
            t.IsTrue(directGoalFor(mega, mega.opts[0]).ok, "Megaplex Race directable (donor ev0)");
            t.Equals(directCourseByCode("MESA"), 3, "archive lookup");
            t.Equals(directCourseByCode("Garibaldi"), 0, "name lookup");
            t.Equals(directCourseByCode("NOPE"), 0, "unknown falls back to Garibaldi");
            t.Equals(directCourseByCode(nullptr), 0, "null falls back to Garibaldi");
            ListState l;
            l.sel = 3;
            directArm(l, true);
            ListState got;
            bool backout = false;
            t.IsTrue(directPeek(got, backout), "peek sees the ask");
            t.Equals(got.sel, static_cast<uint8_t>(3), "list position survives");
            t.IsTrue(backout, "tap asks back out to the menu entry");
            t.IsTrue(directPeek(got, backout), "peek does not consume");
            directDisarm();
            t.IsFalse(directPeek(got, backout), "disarm clears the ask");
            directArm(l, false);
            t.IsTrue(directPeek(got, backout), "probe ask visible");
            t.IsFalse(backout, "probe path launches immediately");
            directDisarm(); });

        tc.Run("navigator rides Mesablanca from the Event screen", [](TestCase &t)
               {
            // Part 4 goal: P1 Race row 0, mapped ev0. The overlay opened
            // on the Event screen, so Sync Triangles back to Peak first.
            FakeGuest g;
            g.screen = 2;
            g.peak = 0;
            g.type = 0;
            g.row = 1;
            g.goal = kCourses[3].opts[0];
            g.goalEvent = 0;
            NavState n;
            navStart(n, g.goal, 0, 0);
            uint64_t tick = 0;
            uint16_t lastOut = 0;
            // Part 2: 4 Cross accepts x 150-tick blind already fill the old cap.
            for (int i = 0; i < 1500 && n.phase != NavPhase::Done && n.phase != NavPhase::Aborted; ++i)
            {
                NavOut o = navStep(n, g.view(), tick, true, true);
                // A press that just finished takes effect in the guest.
                if (lastOut != 0 && o.mask == 0)
                    g.applyFinished(lastOut);
                lastOut = o.mask;
                ++tick;
            }
            t.Equals(static_cast<int>(n.phase), static_cast<int>(NavPhase::Done), "handoff completes");
            t.IsTrue(n.handedOff, "Done via Accept");
            t.Equals(g.peak, 0, "accepted P1");
            t.Equals(g.screen, 2, "accepted from the Event screen"); });

        tc.Run("navigator steps the Event row monotonically and verifies the event", [](TestCase &t)
               {
            // Goal: Aloha Race, P1 row 2 (event 14). Cursor starts at row 0,
            // and the guest misses the first Event accept (retry covers it).
            FakeGuest g;
            g.screen = 2;
            g.peak = 0;
            g.type = 0;
            g.row = 0;
            g.goal = kCourses[5].opts[0];
            g.goalEvent = 14;
            g.acceptsNeeded = 2;
            NavState n;
            navStart(n, g.goal, 14, 0);
            uint64_t tick = 0;
            uint16_t lastOut = 0;
            bool toreDown = false;
            uint64_t restoreAt = 0;
            for (int i = 0; i < 900 && n.phase != NavPhase::Done && n.phase != NavPhase::Aborted; ++i)
            {
                NavOut o = navStep(n, g.view(), tick, true, true);
                if (lastOut != 0 && o.mask == 0)
                {
                    g.applyFinished(lastOut);
                    if (!toreDown && n.phase == NavPhase::Accept && lastOut == kPadCross)
                    {
                        // m6: the Event widget tears down after the accept
                        // (row reads -1 briefly). The retry must not abort.
                        toreDown = true;
                        g.row = 255;
                        restoreAt = tick + 60;
                    }
                }
                if (toreDown && tick >= restoreAt)
                    g.row = 2;
                lastOut = o.mask;
                ++tick;
            }
            t.IsTrue(toreDown, "teardown simulated");
            t.Equals(static_cast<int>(n.phase), static_cast<int>(NavPhase::Done), "retry rides through teardown");
            t.Equals(g.row, 2, "cursor reached row 2, never the row-3 copy"); });

        tc.Run("Accept edge flags apply for mapped goals, restore for native", [](TestCase &t)
               {
            // Part 4: Garibaldi Slopestyle shares P1 Free row 3 (loaded
            // ev14); the Accept edge swaps ev14 to GARI.
            FakeGuest g;
            g.screen = 2;
            g.peak = 0;
            g.type = 0;
            g.row = 0;
            g.goal = kCourses[0].opts[1];
            g.goalEvent = 14;
            NavState n;
            navStart(n, g.goal, 14, 0);
            t.Equals(n.event, 14, "mapped goal verifies the donor event");
            uint64_t tick = 0;
            uint16_t lastOut = 0;
            bool sawApply = false, sawRestore = false;
            for (int i = 0; i < 900 && n.phase != NavPhase::Done && n.phase != NavPhase::Aborted; ++i)
            {
                NavOut o = navStep(n, g.view(), tick, true, true);
                if (o.mapApply)
                    sawApply = true;
                if (o.mapRestore)
                    sawRestore = true;
                if (lastOut != 0 && o.mask == 0)
                    g.applyFinished(lastOut);
                lastOut = o.mask;
                ++tick;
            }
            t.IsTrue(sawApply, "mapped Accept flags apply");
            t.IsFalse(sawRestore, "no restore on a mapped drive");
            t.Equals(static_cast<int>(n.phase), static_cast<int>(NavPhase::Done), "mapped drive hands off");
            // Garibaldi Race: native row restores.
            FakeGuest h;
            h.screen = 2;
            h.peak = 0;
            h.type = 0;
            h.row = 0;
            h.goal = kCourses[0].opts[0];
            h.goalEvent = 0;
            NavState m;
            navStart(m, h.goal, 0, 0);
            t.Equals(m.event, 0, "native goal verifies the course event");
            tick = 0;
            lastOut = 0;
            sawApply = false;
            sawRestore = false;
            for (int i = 0; i < 900 && m.phase != NavPhase::Done && m.phase != NavPhase::Aborted; ++i)
            {
                NavOut o = navStep(m, h.view(), tick, true, true);
                if (o.mapApply)
                    sawApply = true;
                if (o.mapRestore)
                    sawRestore = true;
                if (lastOut != 0 && o.mask == 0)
                    h.applyFinished(lastOut);
                lastOut = o.mask;
                ++tick;
            }
            t.IsFalse(sawApply, "no apply on a native drive");
            t.IsTrue(sawRestore, "native Accept flags restore");
            t.Equals(static_cast<int>(m.phase), static_cast<int>(NavPhase::Done), "native drive hands off"); });

        tc.Run("mapper snapshots, applies and restores on synthetic RAM", [](TestCase &t)
               {
            // Minimal event table: ev6 archive+name fields live.
            const uint32_t size = kMapEventBase + 7u * kMapEventStride + kMapArchiveOff + kMapArchiveLen;
            std::vector<uint8_t> ram(size, 0);
            auto field = [&](int ev, uint32_t off) -> uint8_t * { return ram.data() + kMapEventBase + ev * kMapEventStride + off; };
            std::memcpy(field(6, kMapArchiveOff), "ALASKA", 7);
            std::memcpy(field(6, kMapNameOff), "Alaska", 7);
            MapSpec spec{6, "SNOW", "Snowdream"};
            t.IsTrue(applyCourseMap(ram.data(), size, spec), "apply writes");
            t.Equals(std::strcmp(reinterpret_cast<const char *>(field(6, kMapArchiveOff)), "SNOW"), 0,
                     "archive mapped");
            t.Equals(std::strcmp(reinterpret_cast<const char *>(field(6, kMapNameOff)), "Snowdream"), 0,
                     "name mapped");
            t.IsTrue(applyCourseMap(ram.data(), size, spec), "re-apply idempotent");
            t.IsTrue(restoreCourseMap(ram.data(), size), "restore writes back");
            t.Equals(std::strcmp(reinterpret_cast<const char *>(field(6, kMapArchiveOff)), "ALASKA"), 0,
                     "archive stock again");
            t.Equals(std::strcmp(reinterpret_cast<const char *>(field(6, kMapNameOff)), "Alaska"), 0,
                     "name stock again");
            t.IsTrue(restoreCourseMap(ram.data(), size), "second restore no-op");
            MapSpec bad{23, "SNOW", "Snowdream"};
            t.IsFalse(applyCourseMap(ram.data(), size, bad), "event 23 refused");
            MapSpec empty{6, "", "Snowdream"};
            t.IsFalse(applyCourseMap(ram.data(), size, empty), "empty archive refused");
            t.IsFalse(applyCourseMap(ram.data(), 64, spec), "short RAM refused"); });

        tc.Run("restoreAll puts back every donor mapped this boot", [](TestCase &t)
               {
            // Part 4 shares P1 donor cells across combos: two launches in
            // one boot dirty two records; the menu-return restore must
            // clean both, and clean menus must write nothing.
            // ev1/ev14: no other test maps them, so their snapshots are
            // this test's own clean bytes whatever the run order (the
            // driveOnRead guest leaves nonprintable bytes at ev0's
            // archive field, which mapWrite rightly refuses).
            const uint32_t size = kMapEventBase + 14u * kMapEventStride + kMapArchiveOff + kMapArchiveLen;
            std::vector<uint8_t> ram(size, 0);
            auto field = [&](int ev, uint32_t off) -> uint8_t * { return ram.data() + kMapEventBase + ev * kMapEventStride + off; };
            std::memcpy(field(1, kMapArchiveOff), "SNOW", 5);
            std::memcpy(field(1, kMapNameOff), "Snowdream", 10);
            std::memcpy(field(14, kMapArchiveOff), "ALOHA", 6);
            std::memcpy(field(14, kMapNameOff), "Aloha Ice Jam", 13);
            MapSpec e1{1, "ELYS", "Elysium Alps"};
            MapSpec e14{14, "MEGA", "Tokyo Megaplex"};
            t.IsTrue(applyCourseMap(ram.data(), size, e1), "map ev1");
            t.IsTrue(applyCourseMap(ram.data(), size, e14), "map ev14");
            t.IsTrue(restoreAllCourseMaps(ram.data(), size), "restore all");
            t.Equals(std::strcmp(reinterpret_cast<const char *>(field(1, kMapArchiveOff)), "SNOW"), 0,
                     "ev1 archive stock again");
            t.Equals(std::strcmp(reinterpret_cast<const char *>(field(14, kMapNameOff)), "Aloha Ice Jam"), 0,
                     "ev14 name stock again");
            t.IsTrue(restoreAllCourseMaps(ram.data(), size), "second restore no-op"); });

        tc.Run("preserve-fix-1 boot seeding keeps true stock across Tricky-mapped first maps", [](TestCase &t)
               {
            // Table P D1: the first map used to capture the already-Tricky
            // donor (GARI for event 0, ALOHA for 14), so every later restore
            // reinstalled the donor. Seeding from pristine boot RAM before
            // any mode write keeps TRUE stock. Uses ev2/ev4 (no other test
            // maps them) and resets the process-wide statics afterwards.
            mapState() = MapState{};
            ps2_ssx3_course::courseModes() = ps2_ssx3_course::Modes{};
            const uint32_t size = kMapEventBase + 4u * kMapEventStride + kMapArchiveOff + kMapArchiveLen;
            std::vector<uint8_t> ram(size, 0);
            auto field = [&](int ev, uint32_t off) -> uint8_t * { return ram.data() + kMapEventBase + ev * kMapEventStride + off; };
            std::memcpy(field(2, kMapArchiveOff), "MESA", 5);
            std::memcpy(field(2, kMapNameOff), "Mesablanca", 11);
            std::memcpy(field(4, kMapArchiveOff), "BAM", 4);
            std::memcpy(field(4, kMapNameOff), "Metro City", 11);
            ps2_ssx3_course::Mode mode;
            mode.name = "Tricky";
            ps2_ssx3_course::ModeRow r2;
            r2.event = 2;
            r2.archive = "SNOW";
            r2.name = "Snowdream";
            ps2_ssx3_course::ModeRow r4;
            r4.event = 4;
            r4.archive = "MERQ";
            r4.name = "Merqury City";
            mode.rows.push_back(r2);
            mode.rows.push_back(r4);
            t.IsTrue(ps2_ssx3_course::courseModes().modes.empty(), "modes start disarmed");
            {
                auto sink = [](const std::string &) {};
                t.IsTrue(armModes(ps2_ssx3_course::courseModes(), ram.data(), std::vector<ps2_ssx3_course::Mode>{mode}, sink),
                         "modes arm on pristine RAM");
            }
            seedMapBaselineFromModes(ram.data(), size);
            t.IsTrue(mapState().snapHave[2] && mapState().snapHave[4], "seeded both donors");
            t.Equals(std::strcmp(mapState().snapArchive[2], "MESA"), 0, "ev2 archive true stock");
            t.Equals(std::strcmp(mapState().snapArchive[4], "BAM"), 0, "ev4 archive true stock");
            // The exact D1 order: the menu-entry mapping lands FIRST (donor
            // in RAM), then a second launch maps another donor. Pre-fix the
            // first map's lazy snapshot captures the Tricky donor, so every
            // later restore reinstalls it; seeded baselines keep true stock.
            MapSpec first{2, "SNOW", "Snowdream"};
            MapSpec second{4, "MERQ", "Merqury City"};
            t.IsTrue(applyCourseMap(ram.data(), size, first), "menu-entry launch maps ev2");
            t.IsTrue(applyCourseMap(ram.data(), size, second), "second launch maps ev4");
            t.Equals(std::strcmp(mapState().snapArchive[2], "MESA"), 0, "ev2 snapshot still true stock");
            t.Equals(std::strcmp(mapState().snapArchive[4], "BAM"), 0, "ev4 snapshot still true stock");
            t.IsTrue(restoreAllCourseMaps(ram.data(), size), "restore all");
            t.Equals(std::strcmp(reinterpret_cast<const char *>(field(2, kMapArchiveOff)), "MESA"), 0,
                     "ev2 archive true stock again");
            t.Equals(std::strcmp(reinterpret_cast<const char *>(field(2, kMapNameOff)), "Mesablanca"), 0,
                     "ev2 name true stock again");
            t.Equals(std::strcmp(reinterpret_cast<const char *>(field(4, kMapArchiveOff)), "BAM"), 0,
                     "ev4 archive true stock again");
            mapState() = MapState{};
            ps2_ssx3_course::courseModes() = ps2_ssx3_course::Modes{}; });

        tc.Run("navigator aborts with the pad released when the view goes blind", [](TestCase &t)
               {
            NavState n;
            navStart(n, kCourses[0].opts[0], 0, 0);
            FeView v; // not Map-live on the Sync phase
            v.mapLive = false;
            NavOut o = navStep(n, v, 10, true, true);
            // Sync with no Map screen: abort (the overlay was closed).
            t.IsTrue(o.aborted, "aborts off the Map screens");
            t.Equals(o.mask, 0, "pad released");
            t.Equals(static_cast<int>(n.phase), static_cast<int>(NavPhase::Aborted), "phase Aborted"); });

        tc.Run("a dead Event widget after accept is the handoff", [](TestCase &t)
               {
            // m7: the accept lands, the Event widget tears down (row -1)
            // and the sub word never clears. Sustained death = Done.
            FakeGuest g;
            g.screen = 2;
            g.peak = 0;
            g.type = 0;
            g.row = 0;
            g.goal = kCourses[0].opts[0]; // Garibaldi P1 Race row 0
            g.goalEvent = 0;
            NavState n;
            navStart(n, g.goal, 0, 0);
            uint64_t tick = 0;
            uint16_t lastOut = 0;
            bool toreDown = false;
            for (int i = 0; i < 900 && n.phase != NavPhase::Done && n.phase != NavPhase::Aborted; ++i)
            {
                NavOut o = navStep(n, g.view(), tick, false, false);
                if (lastOut != 0 && o.mask == 0)
                {
                    g.applyFinished(lastOut);
                    if (!toreDown && n.phase == NavPhase::Accept && lastOut == kPadCross)
                    {
                        toreDown = true;
                        g.row = 255; // widget dead, never restored
                    }
                }
                lastOut = o.mask;
                ++tick;
            }
            t.IsTrue(toreDown, "accept fired");
            t.Equals(static_cast<int>(n.phase), static_cast<int>(NavPhase::Done), "Done on sustained death");
            t.IsTrue(n.handedOff, "counts as a handoff"); });

        tc.Run("a slow accept transition is waited out, not aborted", [](TestCase &t)
               {
            // P6 Garibaldi shape (diag nav2): the fallback Cross lands, but
            // the old Peak screen keeps reading for dozens of ticks (+48
            // observed) before the Mode screen appears. An accept must
            // stretch the blind window over the whole transition: a retry
            // mid-flight only restarts the change, while the old short blind
            // evaluated the stale screen, retried once, then gave up
            // (sub==4 abort, "aborted after 2 presses").
            const CourseOption goal = kCourses[0].opts[0]; // Garibaldi P1 Race
            NavState n;
            navStart(n, goal, 0, 0);
            int screen = 0, peak = 0, type = 0, row = 0;
            bool left = false;
            int pending = -1;
            uint64_t flipAt = 0;
            const auto view = [&]() {
                FeView v;
                if (left)
                {
                    v.mapLive = false;
                    return v;
                }
                auto ram = viewRam(static_cast<uint32_t>(screen), static_cast<uint32_t>(peak),
                                   static_cast<uint32_t>(type), static_cast<uint8_t>(row), 0x600000, 0x601000,
                                   0x602000);
                return readView(ram.data(), static_cast<uint32_t>(ram.size()), 0x600000);
            };
            const auto apply = [&](uint16_t mask, uint64_t tick) {
                if (left)
                    return;
                if (mask == kPadDown || mask == kPadUp)
                {
                    const int d = (mask == kPadDown) ? 1 : -1;
                    if (screen == 0)
                        peak = (peak + d + 3) % 3;
                    else if (screen == 1)
                        type = (type + d + 3) % 3;
                    else
                        row = (row + d + 4) % 4;
                }
                else if (mask == kPadCross)
                {
                    if (screen == 2 && row == goal.row)
                    {
                        left = true;
                        pending = -1;
                    }
                    else if (screen < 2)
                    {
                        // The screen change takes 60 ticks; a Cross caught
                        // mid-transition restarts it (the retry never lands).
                        pending = screen + 1;
                        flipAt = tick + 60;
                    }
                }
                else if (mask == kPadTriangle && pending < 0)
                {
                    if (screen > 0)
                        --screen;
                    else
                        left = true;
                }
            };
            uint64_t tick = 0;
            uint16_t lastOut = 0;
            for (int i = 0; i < 1500 && n.phase != NavPhase::Done && n.phase != NavPhase::Aborted; ++i)
            {
                if (pending >= 0 && tick >= flipAt)
                {
                    screen = pending;
                    pending = -1;
                    if (screen == 1)
                        type = 0;
                    if (screen == 2)
                        row = 0;
                }
                NavOut o = navStep(n, view(), tick, false, false);
                if (lastOut != 0 && o.mask == 0)
                    apply(lastOut, tick);
                lastOut = o.mask;
                ++tick;
            }
            t.Equals(static_cast<int>(n.phase), static_cast<int>(NavPhase::Done), "Done, not Aborted");
            t.IsTrue(n.handedOff, "slow transitions still hand off"); });

        tc.Run("setup waits for the Peak entry, then syncs", [](TestCase &t)
               {
            // Production entry is driveArm (phase SetupConfirm); navStart
            // still opens at Sync so the flow tests below are unaffected.
            // Guest: Setup Character reading screen 0, peak word 0; the
            // Continue accept flips the word ~40 ticks later (entry).
            NavState n;
            navStart(n, kCourses[0].opts[0], 0, 0);
            n.phase = NavPhase::SetupConfirm;
            int word = 0;
            uint64_t flipAt = UINT64_MAX;
            const auto view = [&]() {
                auto ram = viewRam(0, static_cast<uint32_t>(word), 0, 0, 0x600000, 0x601000, 0x602000);
                return readView(ram.data(), static_cast<uint32_t>(ram.size()), 0x600000);
            };
            uint64_t tick = 0;
            uint16_t lastOut = 0;
            bool sawCross = false;
            // Settle (150) idles inside Sync, so run to ToPeak, not just
            // past SetupConfirm.
            for (int i = 0; i < 900 && n.phase != NavPhase::ToPeak && n.phase != NavPhase::Aborted; ++i)
            {
                if (tick >= flipAt)
                    word = 1;
                NavOut o = navStep(n, view(), tick, false, false);
                if (o.mask == kPadCross)
                    sawCross = true;
                if (lastOut != 0 && o.mask == 0)
                    flipAt = tick + 40; // Continue accepted; entry follows
                lastOut = o.mask;
                ++tick;
            }
            t.IsTrue(sawCross, "Continue pressed");
            t.Equals(static_cast<int>(n.phase), static_cast<int>(NavPhase::ToPeak), "syncs after entry"); });

        tc.Run("setup skips with no press when the Peak word is already set", [](TestCase &t)
               {
            // Slow tapper: the guest already entered Select Peak (word 1).
            NavState n;
            navStart(n, kCourses[0].opts[0], 0, 0);
            n.phase = NavPhase::SetupConfirm;
            auto ram = viewRam(0, 1, 0, 0, 0x600000, 0x601000, 0x602000);
            FeView v = readView(ram.data(), static_cast<uint32_t>(ram.size()), 0x600000);
            // ToPeak presses on first sight by design (sub0->sub1 accept in
            // one evaluation); the skip is proven by no SetupConfirm try.
            navStep(n, v, 0, false, false);
            t.Equals(n.tries, 0, "no SetupConfirm try spent");
            t.Equals(static_cast<int>(n.phase), static_cast<int>(NavPhase::ToPeak), "straight to ToPeak"); });

        tc.Run("setup gives up after kSetupTries stills", [](TestCase &t)
               {
            // The word never changes (Continue never lands): bounded tries,
            // then abort with the pad released; the user takes over.
            NavState n;
            navStart(n, kCourses[0].opts[0], 0, 0);
            n.phase = NavPhase::SetupConfirm;
            auto ram = viewRam(0, 0, 0, 0, 0x600000, 0x601000, 0x602000);
            FeView v = readView(ram.data(), static_cast<uint32_t>(ram.size()), 0x600000);
            uint64_t tick = 0;
            for (int i = 0; i < 2000 && n.phase != NavPhase::Aborted; ++i)
            {
                navStep(n, v, tick, false, false);
                ++tick;
            }
            t.Equals(static_cast<int>(n.phase), static_cast<int>(NavPhase::Aborted), "aborts");
            t.Equals(n.presses, kSetupTries, "bounded presses"); });

        tc.Run("setup aborts when the words go dark", [](TestCase &t)
               {
            // Backed out to the main menu mid-setup: no blind to wait out on
            // the first evaluation, so abort at once with pad released.
            NavState n;
            navStart(n, kCourses[0].opts[0], 0, 0);
            n.phase = NavPhase::SetupConfirm;
            FeView v; // mapLive false
            NavOut o = navStep(n, v, 0, false, false);
            t.IsTrue(o.aborted, "aborts");
            t.Equals(o.mask, 0, "pad released"); });

        tc.Run("eaten Peak accepts retry tries-bounded, then ride", [](TestCase &t)
               {
            // The Setup->Peak journey eats accepts (no screen change): the
            // navigator retries instead of the two-strike give-up. Guest
            // eats the first Peak accept, then answers (60-tick flips).
            // Part 4: at most one fully-static eat is tolerated now (P6
            // evidence: +48 ticks < one 150-tick retry spacing); a screen
            // that stays static across 3 retries is a refused row and
            // fails fast (next test), because no real transition holds a
            // settled screen still that long.
            const CourseOption goal = kCourses[0].opts[0]; // Garibaldi P1 Race
            NavState n;
            navStart(n, goal, 0, 0);
            int screen = 0, eatsLeft = 1;
            int pending = -1;
            uint64_t flipAt = 0;
            bool left = false;
            const auto view = [&]() {
                FeView v;
                if (left)
                {
                    v.mapLive = false;
                    return v;
                }
                auto ram = viewRam(static_cast<uint32_t>(screen), 0, 0, 0, 0x600000, 0x601000, 0x602000);
                return readView(ram.data(), static_cast<uint32_t>(ram.size()), 0x600000);
            };
            const auto apply = [&](uint16_t mask, uint64_t tick) {
                if (mask != kPadCross)
                    return;
                if (screen == 2)
                {
                    left = true; // goal row 0: the accept leaves the Map screens
                    return;
                }
                if (screen == 0 && eatsLeft > 0)
                {
                    --eatsLeft; // eaten by the transition: no effect
                    return;
                }
                pending = screen + 1;
                flipAt = tick + 60;
            };
            uint64_t tick = 0;
            uint16_t lastOut = 0;
            for (int i = 0; i < 3000 && n.phase != NavPhase::Done && n.phase != NavPhase::Aborted; ++i)
            {
                if (pending >= 0 && tick >= flipAt)
                {
                    screen = pending;
                    pending = -1;
                }
                NavOut o = navStep(n, view(), tick, false, false);
                if (lastOut != 0 && o.mask == 0)
                    apply(lastOut, tick);
                lastOut = o.mask;
                ++tick;
            }
            t.Equals(static_cast<int>(n.phase), static_cast<int>(NavPhase::Done), "rides after retries");
            t.IsTrue(n.handedOff, "handoff");
            t.Equals(n.presses, 4, "1 eaten + Peak + Mode + Event accepts"); });

        tc.Run("a fully static screen aborts fast as refused", [](TestCase &t)
               {
            // Part 4 fail-fast: the row refuses (locked) and no accept
            // will ever land. The view never changes, so the streak
            // fires on the 3rd identical retry read instead of grinding
            // to the tries cap (Round-3 tails: 19 presses / ~50 s).
            NavState n;
            navStart(n, kCourses[0].opts[0], 0, 0);
            auto ram = viewRam(0, 0, 0, 0, 0x600000, 0x601000, 0x602000);
            FeView v = readView(ram.data(), static_cast<uint32_t>(ram.size()), 0x600000);
            uint64_t tick = 0;
            uint64_t abortTick = 0;
            for (int i = 0; i < 4000 && n.phase != NavPhase::Aborted; ++i)
            {
                navStep(n, v, tick, false, false);
                ++tick;
                if (n.phase == NavPhase::Aborted && abortTick == 0)
                    abortTick = tick;
            }
            t.Equals(static_cast<int>(n.phase), static_cast<int>(NavPhase::Aborted), "aborts");
            t.IsTrue(n.refused, "refused flag names the streak abort");
            t.IsTrue(n.presses <= 4, "fails fast (sub-1 + two retries)");
            t.IsTrue(abortTick <= 600, "within ~10 s of ticks, ~5 s live"); });

        tc.Run("moving views still reach the tries cap, not refused", [](TestCase &t)
               {
            // The transition stays in flight: the peak word flips on
            // every issued press but nothing lands. The streak never
            // builds, so the tries cap (not the press budget) still
            // bounds the patience, and refused stays clear.
            NavState n;
            navStart(n, kCourses[0].opts[0], 0, 0);
            uint64_t tick = 0;
            for (int i = 0; i < 6000 && n.phase != NavPhase::Aborted; ++i)
            {
                auto ram = viewRam(0, static_cast<uint32_t>(n.presses % 2), 0, 0, 0x600000, 0x601000,
                                   0x602000);
                FeView v = readView(ram.data(), static_cast<uint32_t>(ram.size()), 0x600000);
                navStep(n, v, tick, false, false);
                ++tick;
            }
            t.Equals(static_cast<int>(n.phase), static_cast<int>(NavPhase::Aborted), "aborts");
            t.IsFalse(n.refused, "cap abort is not a refusal");
            t.Equals(n.tries, kPeakTries, "tries cap binds");
            t.Equals(n.presses, kPeakTries, "one press per try"); });

        tc.Run("back-out Triangles until the Map screens are left", [](TestCase &t)
               {
            NavState n;
            navBackOut(n, 0);
            auto ram = viewRam(2, 0, 0, 0, 0x600000, 0x601000, 0x602000);
            FeView v = readView(ram.data(), static_cast<uint32_t>(ram.size()), 0x600000);
            uint64_t tick = 0;
            bool sawTriangle = false;
            for (int i = 0; i < 60 && n.phase != NavPhase::Done; ++i)
            {
                if (i == 20)
                    v.mapLive = false; // guest left for Setup Character
                NavOut o = navStep(n, v, tick, true, true);
                if (o.mask == kPadTriangle)
                    sawTriangle = true;
                const uint64_t next = std::max(n.releaseUntil, n.blindUntil);
                tick = next > tick ? next : tick + 1;
            }
            t.IsTrue(sawTriangle, "Triangle pressed");
            t.Equals(static_cast<int>(n.phase), static_cast<int>(NavPhase::Done), "Done after leaving");
            t.IsFalse(n.handedOff, "back-out is not a handoff"); });

        tc.Run("back-out completes by press count with the screens still live", [](TestCase &t)
               {
            NavState n;
            navBackOut(n, 0);
            auto ram = viewRam(2, 0, 0, 0, 0x600000, 0x601000, 0x602000);
            FeView v = readView(ram.data(), static_cast<uint32_t>(ram.size()), 0x600000);
            uint64_t tick = 0;
            int triangles = 0;
            for (int i = 0; i < 60 && n.phase != NavPhase::Done; ++i)
            {
                NavOut o = navStep(n, v, tick, true, true);
                if (o.mask == kPadTriangle)
                    ++triangles;
                const uint64_t next = std::max(n.releaseUntil, n.blindUntil);
                tick = next > tick ? next : tick + 1;
            }
            t.Equals(static_cast<int>(n.phase), static_cast<int>(NavPhase::Done), "Done by press count");
            t.Equals(triangles, kBackOutPresses, "bounded Triangles");
            t.Equals(n.presses, kBackOutPresses, "press count binds");
            t.IsFalse(n.handedOff, "counted back-out is not a handoff"); });

        tc.Run("stalled back-out completes via the watchdog", [](TestCase &t)
               {
            driveDisarm();
            driveArmBackOut(0);
            driveWatchdog(0);
            DriveProgress early = driveProgress();
            t.IsTrue(early.driving, "still driving inside the window");
            driveWatchdog(static_cast<uint64_t>(kBackOutStallTicks) - 1u);
            t.IsFalse(driveProgress().done, "no trip before the window");
            driveWatchdog(static_cast<uint64_t>(kBackOutStallTicks));
            DriveProgress p = driveProgress();
            t.IsTrue(p.done, "stalled drive completes");
            t.IsFalse(p.handedOff, "stall is not a handoff");
            t.IsFalse(p.driving, "completed drive no longer drives");
            driveDisarm(); });

        tc.Run("healthy back-out presses never trip the watchdog", [](TestCase &t)
               {
            driveDisarm();
            driveArmBackOut(0);
            auto ram = viewRam(2, 0, 0, 0, 0x600000, 0x601000, 0x602000);
            bool done = false, handedOff = false, aborted = false;
            uint64_t tick = 0;
            for (int i = 0; i < 3; ++i)
            {
                const uint16_t mask = driveOnRead(ram.data(), static_cast<uint32_t>(ram.size()), 0x600000,
                                                  tick, true, true, true, done, handedOff, aborted);
                t.Equals(mask, kPadTriangle, "keeps pressing while live");
                driveWatchdog(tick + 200u);
                t.IsFalse(driveProgress().done, "progress resets the window");
                tick += 100u;
            }
            t.IsFalse(done, "game thread still driving");
            driveDisarm(); });

        tc.Run("driveOnRead arms, drives and reports Done through the shared struct", [](TestCase &t)
               {
            FakeGuest g;
            g.screen = 0;
            g.peak = 1;
            g.type = 0;
            g.row = 0;
            g.goal = kCourses[3].opts[0]; // Mesablanca P1 Race row 0, mapped ev0
            g.goalEvent = 0;
            driveDisarm();
            driveArm(g.goal, 0, 0);
            uint64_t tick = 0;
            uint16_t lastOut = 0;
            bool done = false, handedOff = false, aborted = false;
            for (int i = 0; i < 600 && !done && !aborted; ++i)
            {
                // After the handoff the guest leaves the Map screens: serve
                // a non-Map sub-state (the loading path), not stale Map RAM.
                std::vector<uint8_t> ram =
                    g.left ? std::vector<uint8_t>(0x800000, 0)
                           : viewRam(static_cast<uint32_t>(g.screen), static_cast<uint32_t>(g.peak),
                                     static_cast<uint32_t>(g.type), static_cast<uint8_t>(g.row), 0x600000, 0x601000,
                                     0x602000);
                if (g.left)
                    put32(ram, 0x4a259c, 9);
                const uint16_t mask = driveOnRead(ram.data(), static_cast<uint32_t>(ram.size()), 0x600000, tick,
                                                  true, true, true, done, handedOff, aborted);
                // A finished press takes effect in the guest (same edge as
                // the merge point: the mask drops after hold + gap).
                if (lastOut != 0 && mask == 0)
                    g.applyFinished(lastOut);
                lastOut = mask;
                ++tick;
                if (done && !handedOff)
                    g.left = true; // handoff leaves the Map screens after Done
            }
            t.IsTrue(done, "Done reported");
            t.IsTrue(handedOff, "handoff");
            t.IsFalse(aborted, "no abort");
            DriveProgress p = driveProgress();
            t.IsTrue(p.done, "progress stays Done until disarmed");
            t.IsFalse(p.refused, "success is not a refusal");
            driveDisarm();
            p = driveProgress();
            t.Equals(static_cast<int>(p.phase), static_cast<int>(NavPhase::Idle), "disarm resets"); });

        tc.Run("accept-only Peak falls back to step-accept-verify and retries", [](TestCase &t)
               {
            // The guest reports the ACCEPTED peak/type (cursor invisible).
            // Synthetic P2 Race row 0 goal (Part 4 moved every table goal
            // to P1, so the stepping path needs a non-table goal here):
            // Down+Cross accepts P1 (wrong), Triangle backs out, Down+Cross
            // accepts P2, then Mode, Event. The Peak cursor wraps here
            // (flip coverage is the next test).
            int cursorPeak = 0, acceptedPeak = 0;
            int cursorType = 0, acceptedType = 0;
            int screen = 2, row = 0;
            bool left = false;
            NavState n;
            const CourseOption p2goal{1, 0, 0};
            navStart(n, p2goal, 2, 0);
            uint64_t tick = 0;
            uint16_t lastOut = 0;
            const auto view = [&]() {
                FeView v;
                if (left)
                    return v;
                auto ram = viewRam(static_cast<uint32_t>(screen), static_cast<uint32_t>(acceptedPeak),
                                   static_cast<uint32_t>(acceptedType), static_cast<uint8_t>(row), 0x600000,
                                   0x601000, 0x602000);
                return readView(ram.data(), static_cast<uint32_t>(ram.size()), 0x600000);
            };
            const auto apply = [&](uint16_t mask) {
                if (mask == kPadDown)
                {
                    if (screen == 0)
                        cursorPeak = (cursorPeak + 1) % 3;
                    else if (screen == 1)
                        cursorType = (cursorType + 1) % 2;
                    else
                        row = (row + 1) % 4;
                }
                else if (mask == kPadCross)
                {
                    if (screen == 0)
                    {
                        acceptedPeak = cursorPeak;
                        screen = 1;
                    }
                    else if (screen == 1)
                    {
                        acceptedType = cursorType;
                        screen = 2;
                    }
                    else if (row == 0)
                        left = true;
                }
                else if (mask == kPadTriangle && screen > 0)
                    --screen;
            };
            // P5: the reset-aware fallback spends ~2 extra presses per
            // back-out cycle (accept-first); the product bounds (tries cap,
            // press budget) are unchanged, so only this harness cap grows.
            // Part 2: each Cross accept blinds 150 ticks (slow transitions),
            // so the instant guest needs ~6 accepts' worth of headroom.
            for (int i = 0; i < 3000 && n.phase != NavPhase::Done && n.phase != NavPhase::Aborted; ++i)
            {
                NavOut o = navStep(n, view(), tick, false, false);
                if (lastOut != 0 && o.mask == 0)
                    apply(lastOut);
                lastOut = o.mask;
                ++tick;
            }
            t.Equals(static_cast<int>(n.phase), static_cast<int>(NavPhase::Done), "fallback completes");
            t.IsTrue(n.handedOff, "handoff");
            t.Equals(acceptedPeak, 1, "P2 accepted after one retry");
            t.Equals(n.tries, 0, "tries reset on the verified accept"); });

        tc.Run("accept-only Peak flips direction when the cursor is stuck", [](TestCase &t)
               {
            // No-wrap Peak cursor clamped at P3, starting there; goal
            // Garibaldi P1. Downs never move it, so the fallback flips to
            // Up and walks down to P1. Mode matches (Race), Event row 0.
            int cursorPeak = 2, acceptedPeak = 2;
            int screen = 0, row = 0;
            bool left = false;
            NavState n;
            navStart(n, kCourses[0].opts[0], 0, 0);
            uint64_t tick = 0;
            uint16_t lastOut = 0;
            const auto view = [&]() {
                FeView v;
                if (left)
                    return v;
                auto ram = viewRam(static_cast<uint32_t>(screen), static_cast<uint32_t>(acceptedPeak), 0u,
                                   static_cast<uint8_t>(row), 0x600000, 0x601000, 0x602000);
                return readView(ram.data(), static_cast<uint32_t>(ram.size()), 0x600000);
            };
            const auto apply = [&](uint16_t mask) {
                if (mask == kPadDown)
                    cursorPeak = cursorPeak < 2 ? cursorPeak + 1 : 2;
                else if (mask == kPadUp)
                    cursorPeak = cursorPeak > 0 ? cursorPeak - 1 : 0;
                else if (mask == kPadCross)
                {
                    if (screen == 0)
                    {
                        acceptedPeak = cursorPeak;
                        screen = 1;
                    }
                    else if (screen == 1)
                        screen = 2;
                    else if (row == 0)
                        left = true;
                }
                else if (mask == kPadTriangle && screen > 0)
                    --screen;
            };
            // Part 2: the 150-tick accept blind needs headroom (4 accepts here).
            for (int i = 0; i < 2400 && n.phase != NavPhase::Done && n.phase != NavPhase::Aborted; ++i)
            {
                NavOut o = navStep(n, view(), tick, false, true);
                if (lastOut != 0 && o.mask == 0)
                    apply(lastOut);
                lastOut = o.mask;
                ++tick;
            }
            t.Equals(static_cast<int>(n.phase), static_cast<int>(NavPhase::Done), "flip completes");
            t.Equals(acceptedPeak, 0, "P1 accepted after flipping to Up"); });

        tc.Run("P4 reset cursor at top still rides Garibaldi Slopestyle", [](TestCase &t)
               {
            const ResetDrive d = driveResetPeak(0);
            t.Equals(d.phase, static_cast<int>(NavPhase::Done), "done from top");
            t.IsTrue(d.handedOff, "handoff");
            t.Equals(d.acceptedPeak, 0, "P1 accepted"); });

        tc.Run("P4 reset cursor at Peak 2 still rides Garibaldi Slopestyle", [](TestCase &t)
               {
            const ResetDrive d = driveResetPeak(1);
            t.Equals(d.phase, static_cast<int>(NavPhase::Done), "done from reset value");
            t.IsTrue(d.handedOff, "handoff");
            t.Equals(d.acceptedPeak, 0, "P1 accepted"); });

        tc.Run("P4 reset cursor at last row still rides Garibaldi Slopestyle", [](TestCase &t)
               {
            const ResetDrive d = driveResetPeak(2);
            t.Equals(d.phase, static_cast<int>(NavPhase::Done), "done from last row");
            t.IsTrue(d.handedOff, "handoff");
            t.Equals(d.acceptedPeak, 0, "P1 accepted"); });

        tc.Run("P5 persisting cursor from last row still rides Garibaldi Slopestyle", [](TestCase &t)
               {
            const ResetDrive d = driveResetPeak(2, true);
            t.Equals(d.phase, static_cast<int>(NavPhase::Done), "done despite persist");
            t.IsTrue(d.handedOff, "handoff");
            t.Equals(d.acceptedPeak, 0, "P1 accepted"); });

        tc.Run("press budget aborts a stuck drive", [](TestCase &t)
               {
            NavState n;
            navStart(n, kCourses[4].opts[0], 0, 0); // Merqury P1 Race row 0
            auto ram = viewRam(0, 0, 0, 0, 0x600000, 0x601000, 0x602000);
            FeView v = readView(ram.data(), static_cast<uint32_t>(ram.size()), 0x600000);
            uint64_t tick = 0;
            for (int i = 0; i < 500 && n.phase != NavPhase::Aborted; ++i)
            {
                // The peak never moves (locked): every press is wasted.
                navStep(n, v, tick, true, true);
                const uint64_t next = std::max(n.releaseUntil, n.blindUntil);
                tick = next > tick ? next : tick + 1;
            }
            t.Equals(static_cast<int>(n.phase), static_cast<int>(NavPhase::Aborted), "budget aborts"); });

        tc.Run("filecode rejects unwritten and wild slots", [](TestCase &t)
               {
            t.IsFalse(filecodeOk(0xFFFFFFFFu, 0x2000000u), "FFFFFFFF rejected (no wrap)");
            t.IsFalse(filecodeOk(0u, 0x2000000u), "null rejected");
            t.IsFalse(filecodeOk(0xFFFu, 0x2000000u), "below 0x1000 rejected");
            t.IsFalse(filecodeOk(0x1FFFFFEu, 0x2000000u), "overrun rejected");
            t.IsTrue(filecodeOk(0x1000u, 0x2000000u), "low edge accepted");
            t.IsTrue(filecodeOk(0x1FFFFFCu, 0x2000000u), "high edge accepted");
            std::vector<uint8_t> ram(0x2000000u, 0xFFu);
            // ev 0 -> valid code, ev 1 -> 0xFFFFFFFF, ev 2 -> null.
            auto put32 = [&](uint32_t addr, uint32_t v) { std::memcpy(ram.data() + addr, &v, 4); };
            put32(0x442950u, 0x500000u);
            std::memcpy(ram.data() + 0x500000u, "ARA1", 4);
            put32(0x442954u, 0xFFFFFFFFu);
            put32(0x442958u, 0u);
            t.IsNotNull(readFilecode(ram.data(), 0x2000000u, 0), "valid code read");
            t.IsNull(readFilecode(ram.data(), 0x2000000u, 1), "FFFFFFFF unread");
            t.IsNull(readFilecode(ram.data(), 0x2000000u, 2), "null unread");
            t.Equals(std::strncmp(readFilecode(ram.data(), 0x2000000u, 0), "ARA1", 4), 0,
                     "code bytes match"); });

        tc.Run("geometry hits rows, toggle and legends", [](TestCase &t)
               {
            ListGeom g = listLayout(640.0f, 480.0f);
            t.Equals(listHit(g, g.rowX + 4, g.rowY[3] + 2), 3, "row tap");
            t.Equals(listHit(g, g.rowX - 4, g.rowY[3] + 2), -1, "miss left");
            t.Equals(listHit(g, g.selectX + 2, g.selectY + 2), 11, "select legend");
            t.Equals(listHit(g, g.backX + 2, g.backY + 2), 12, "back legend");
            t.Equals(listHit(g, g.toggleX + 2, g.toggleY + 2), 10, "toggle"); });

        tc.Run("help-bar buttons label the 11/12 zones per step", [](TestCase &t)
               {
            t.IsTrue(std::strcmp(listButtonLabel(0, 11), "Select") == 0, "step0 ride");
            t.IsTrue(std::strcmp(listButtonLabel(0, 12), "Previous") == 0, "step0 back");
            t.IsTrue(std::strcmp(listButtonLabel(1, 11), "Ride") == 0, "step1 ride");
            t.IsTrue(std::strcmp(listButtonLabel(1, 12), "Courses") == 0, "step1 back");
            t.IsNull(listButtonLabel(0, 10), "toggle has no button");
            t.IsNull(listButtonLabel(0, 3), "row has no button");
            t.IsNull(listButtonLabel(0, -1), "miss has no button");
            // The drawn buttons sit on the help bar: below the last course
            // row, inside the screen, select left of back, no row overlap.
            for (float h : {480.0f, 820.0f})
            {
                const float w = h * 4.0f / 3.0f;
                ListGeom g = listLayout(w, h);
                const float rowsEnd = g.rowY[9] + g.rowH;
                t.IsTrue(g.selectY >= rowsEnd, "select below rows");
                t.IsTrue(g.backY >= rowsEnd, "back below rows");
                t.IsTrue(g.selectX + g.selectW <= g.backX, "select left of back");
                t.IsTrue(g.backX + g.backW <= w, "back inside screen");
                t.IsTrue(g.selectY + g.selectH <= h, "bar inside screen");
            } });

        tc.Run("one-tap ride default is Race, toggle cycles dual courses", [](TestCase &t)
               {
            ListState s;
            t.Equals(listDefMode(s), 0, "Garibaldi defaults Race");
            t.IsTrue(std::strcmp(listDefaultModeName(s), "Race") == 0, "Garibaldi default name");
            t.Equals(listDefaultGoal(s).peak, 0, "Garibaldi Race peak");
            t.Equals(listDefaultGoal(s).type, 0, "Garibaldi Race type");
            t.Equals(listDefaultGoal(s).row, 0, "Garibaldi Race row");
            t.Equals(listCycleMode(s), 1, "toggle to Slopestyle");
            t.IsTrue(std::strcmp(listDefaultModeName(s), "Slopestyle") == 0, "toggled name");
            t.Equals(listDefaultGoal(s).type, 1, "Slopestyle type");
            t.Equals(listDefaultGoal(s).row, 3, "Slopestyle row");
            t.Equals(listCycleMode(s), 0, "toggle wraps to Race");
            // Single-mode course: the toggle is a no-op, the goal is fixed.
            ListState p;
            p.sel = 6; // Pipedream, Super Pipe only
            t.Equals(listCycleMode(p), 0, "single stays 0");
            t.IsTrue(std::strcmp(listDefaultModeName(p), "Super Pipe") == 0, "pipe name");
            t.Equals(listDefaultGoal(p).row, 2, "pipe row");
            // Defaults are per course: cycling Garibaldi leaves Snowdream.
            ListState m;
            m.sel = 0;
            listCycleMode(m);
            m.sel = 1;
            t.Equals(listDefMode(m), 0, "Snowdream untouched");
            // Clamp: a stale default index falls back to the first mode.
            m.sel = 6;
            m.modeDef[6] = 5;
            t.Equals(listDefMode(m), 0, "clamped to 0"); });

        // tk-menu-2 shared font module: synthetic TKFONT1 (no game bytes).
        auto tkfontBlob = [](uint32_t w1, uint32_t h1, uint32_t w4, uint32_t h4,
                             const std::vector<ps2_tk_font::Glyph> &gs) {
            std::string b("TKFONT1\0", 8);
            auto u32 = [&](uint32_t v) {
                char tmp[4];
                std::memcpy(tmp, &v, 4);
                b.append(tmp, 4);
            };
            u32(w1);
            u32(h1);
            u32(w4);
            u32(h4);
            u32(static_cast<uint32_t>(gs.size()));
            for (const auto &g : gs)
            {
                char rec[16];
                std::memcpy(rec, &g.code, 2);
                std::memcpy(rec + 2, &g.x, 2);
                std::memcpy(rec + 4, &g.y, 2);
                std::memcpy(rec + 6, &g.w, 2);
                std::memcpy(rec + 8, &g.h, 2);
                std::memcpy(rec + 10, &g.xoff, 2);
                std::memcpy(rec + 12, &g.yoff, 2);
                std::memcpy(rec + 14, &g.adv, 2);
                b.append(rec, 16);
            }
            b.append(w1 * h1 * 4, '\0');
            b.append(w4 * h4 * 4, '\0');
            return b;
        };
        tc.Run("tkfont rejects bad magic and short reads", [&](TestCase &t)
               {
            ps2_tk_font::Baked b;
            t.IsFalse(ps2_tk_font::parseTkFont(nullptr, 0, b), "null rejected");
            const std::string junk("garbage-bytes-here!!!!!!!!", 24);
            t.IsFalse(ps2_tk_font::parseTkFont(reinterpret_cast<const uint8_t *>(junk.data()), junk.size(), b),
                      "bad magic rejected");
            const std::string head("TKFONT1\0", 8);
            t.IsFalse(ps2_tk_font::parseTkFont(reinterpret_cast<const uint8_t *>(head.data()), head.size(), b),
                      "short header rejected"); });
        tc.Run("tkfont parses one glyph and measures", [&](TestCase &t)
               {
            ps2_tk_font::Glyph g;
            g.code = 65;
            g.x = 1;
            g.y = 2;
            g.w = 10;
            g.h = 12;
            g.xoff = 1;
            g.yoff = 2;
            g.adv = 11;
            const std::string blob = tkfontBlob(16, 16, 64, 64, {g});
            ps2_tk_font::Baked b;
            t.IsTrue(ps2_tk_font::parseTkFont(reinterpret_cast<const uint8_t *>(blob.data()), blob.size(), b),
                     "one glyph parses");
            t.Equals(b.glyphs.size(), size_t(1), "glyph count");
            t.Equals(b.px1.size(), size_t(16 * 16 * 4), "1x bytes");
            t.Equals(b.px4.size(), size_t(64 * 64 * 4), "4x bytes");
            t.IsTrue(ps2_tk_font::findGlyph(b, 65) != nullptr, "A found");
            t.IsTrue(ps2_tk_font::findGlyph(b, 66) == nullptr, "B missing");
            const ps2_tk_font::Metrics m = ps2_tk_font::measure(b, "A");
            t.Equals(m.width, 11.0f, "advance width");
            t.Equals(m.top, 2.0f, "ink top");
            t.Equals(m.bot, 14.0f, "ink bottom");
            t.Equals(ps2_tk_font::scaleFor(m, 24.0f), 2.0f, "scale fills band");
            const ps2_tk_font::Metrics e = ps2_tk_font::measure(b, "");
            t.Equals(e.width, 0.0f, "empty width");
            t.Equals(ps2_tk_font::scaleFor(e, 24.0f), 0.0f, "empty scale"); });
        tc.Run("tkfont rejects out-of-atlas cells", [&](TestCase &t)
               {
            ps2_tk_font::Glyph g;
            g.code = 65;
            g.x = 10;
            g.y = 0;
            g.w = 10;
            g.h = 4;
            g.adv = 11;
            const std::string blob = tkfontBlob(16, 16, 64, 64, {g});
            ps2_tk_font::Baked b;
            t.IsFalse(ps2_tk_font::parseTkFont(reinterpret_cast<const uint8_t *>(blob.data()), blob.size(), b),
                      "oob cell rejected"); });
        tc.Run("tkfont uniform scale ignores descenders", [&](TestCase &t)
               {
            ps2_tk_font::Glyph a, y;
            a.code = 65;
            a.x = 0; a.y = 0; a.w = 10; a.h = 12; a.xoff = 0; a.yoff = 2; a.adv = 11;
            y.code = 121;
            y.x = 0; y.y = 0; y.w = 10; y.h = 20; y.xoff = 0; y.yoff = 2; y.adv = 10;
            ps2_tk_font::Glyph h;
            h.code = 72;
            h.x = 0; h.y = 0; h.w = 10; h.h = 12; h.xoff = 0; h.yoff = 2; h.adv = 12;
            const std::string blob = tkfontBlob(16, 32, 64, 128, {a, y, h});
            ps2_tk_font::Baked b;
            t.IsTrue(ps2_tk_font::parseTkFont(reinterpret_cast<const uint8_t *>(blob.data()), blob.size(), b),
                     "three glyphs parse");
            float top = 0.0f, bot = 0.0f;
            ps2_tk_font::refBand(b, top, bot);
            t.Equals(top, 2.0f, "caps top");
            t.Equals(bot, 14.0f, "caps bottom, not the descender");
            const float ku = ps2_tk_font::scaleUniform(b, 48.0f);
            t.Equals(ku, 4.0f, "uniform scale is string-independent");
            const ps2_tk_font::Metrics ma = ps2_tk_font::measure(b, "A");
            const ps2_tk_font::Metrics may = ps2_tk_font::measure(b, "Ay");
            t.IsTrue(ps2_tk_font::scaleFor(may, 40.0f) < ps2_tk_font::scaleFor(ma, 40.0f),
                     "per-string fit shrinks the descender row (the row bug)"); }); });
}
