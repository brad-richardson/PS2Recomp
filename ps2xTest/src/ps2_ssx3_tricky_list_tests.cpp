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
// triangle-back (the gari5 stock behavior). Goal Garibaldi Slopestyle
// {peak 1, type 1, row 0} on donor ev 6; clamps, no wrap (hard model).
struct ResetDrive
{
    int phase = 0;
    bool handedOff = false;
    int acceptedPeak = -1;
};
ResetDrive driveResetPeak(int reset)
{
    int cursorPeak = reset, acceptedPeak = reset;
    int cursorType = 0, acceptedType = 0;
    int screen = 0;
    bool left = false;
    NavState n;
    navStart(n, kCourses[0].opts[1], 6, 0);
    uint64_t tick = 0;
    uint16_t lastOut = 0;
    const auto view = [&]() {
        FeView v;
        if (left)
            return v;
        auto ram = viewRam(static_cast<uint32_t>(screen), static_cast<uint32_t>(acceptedPeak),
                           static_cast<uint32_t>(acceptedType), 0, 0x600000, 0x601000, 0x602000);
        return readView(ram.data(), static_cast<uint32_t>(ram.size()), 0x600000);
    };
    const auto apply = [&](uint16_t mask) {
        if (mask == kPadDown)
        {
            if (screen == 0)
                cursorPeak = cursorPeak < 2 ? cursorPeak + 1 : 2;
            else if (screen == 1)
                cursorType = cursorType < 1 ? cursorType + 1 : 1;
        }
        else if (mask == kPadUp)
        {
            if (screen == 0)
                cursorPeak = cursorPeak > 0 ? cursorPeak - 1 : 0;
            else if (screen == 1)
                cursorType = cursorType > 0 ? cursorType - 1 : 0;
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
            else
                left = true;
        }
        else if (mask == kPadTriangle && screen == 1)
        {
            screen = 0;
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
            // Single-type targets (peak, type, row) from ssx3-events-nav.txt.
            t.Equals(kCourses[0].opts[0].peak, 0, "Garibaldi P1");
            t.Equals(kCourses[0].opts[0].row, 0, "Garibaldi row 0");
            t.Equals(kCourses[3].opts[0].peak, 1, "Mesablanca P2");
            t.Equals(kCourses[4].opts[0].peak, 2, "Merqury P3");
            t.Equals(kCourses[6].opts[0].type, 1, "Pipedream freestyle");
            t.Equals(kCourses[6].opts[0].row, 2, "Pipedream row 2");
            t.Equals(kCourses[7].opts[0].peak, 2, "Untracked P3");
            t.Equals(kCourses[9].opts[0].type, 1, "Alaska freestyle");
            // Dual-type courses share one event id; Race is the default.
            t.Equals(kCourses[2].nOpts, 2, "Elysium dual");
            t.Equals(kCourses[2].event, 16, "Elysium event 16 both rows");
            t.Equals(kCourses[2].opts[0].row, 1, "Elysium race row 1");
            t.Equals(kCourses[2].opts[1].row, 3, "Elysium jam row 3");
            t.Equals(kCourses[5].opts[0].row, 2, "Aloha race row 2");
            t.Equals(kCourses[5].opts[1].row, 3, "Aloha jam row 3");
            t.Equals(kCourses[8].nOpts, 2, "Megaplex dual");
            t.IsTrue(kCourses[8].tp == nullptr, "Megaplex has no preview art");
            ListState s;
            t.Equals(listGoal(s).row, 0, "step 0 goal is opts[0]");
            s.sel = 2;
            t.Equals(listGoal(s).row, 1, "Elysium step 0 is the Race row");
            s.step = 1;
            s.mode = 1;
            t.Equals(listGoal(s).row, 3, "step 1 mode 1 is the Slopestyle row");
            s.mode = 0;
            t.Equals(listGoal(s).row, 1, "step 1 mode 0 is Race");
            t.Equals(std::strcmp(kCourses[2].modeName[1], "Slopestyle"), 0, "Jam mode name");
            t.Equals(kCourses[6].nOpts, 1, "Pipedream single-mode"); });

        tc.Run("Part 3 table: four Slopestyle modes mapped onto ev6", [](TestCase &t)
               {
            const int mapped[] = {0, 1, 3, 4};
            const char *arch[] = {"GARI", "SNOW", "MESA", "MERQ"};
            const char *names[] = {"Garibaldi", "Snowdream", "Mesablanca", "Merqury City"};
            for (int k = 0; k < 4; ++k)
            {
                const int i = mapped[k];
                t.Equals(kCourses[i].nOpts, 2, "mapped course dual-mode");
                t.Equals(std::strcmp(kCourses[i].modeName[1], "Slopestyle"), 0, "second mode label");
                t.Equals(kCourses[i].opts[1].peak, 1, "donor P2");
                t.Equals(kCourses[i].opts[1].type, 1, "donor freestyle");
                t.Equals(kCourses[i].opts[1].row, 0, "donor row 0 (ev6)");
                t.Equals(kCourses[i].opts[1].map.event, 6, "donor ev6");
                t.Equals(std::strcmp(kCourses[i].opts[1].map.archive, arch[k]), 0, "world archive");
                t.Equals(std::strcmp(kCourses[i].opts[1].map.name, names[k]), 0, "world name");
                t.Equals(kCourses[i].opts[0].map.event, -1, "race row native");
            }
            const int native[] = {2, 5, 6, 7, 8, 9};
            for (int k = 0; k < 6; ++k)
                for (int m = 0; m < 2; ++m)
                    t.Equals(kCourses[native[k]].opts[m].map.event, -1, "Jam/single rows unmapped"); });

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

        tc.Run("navigator rides Mesablanca from the Event screen", [](TestCase &t)
               {
            // Goal: P2 Race row 0 (event 2). The overlay opened on the Event
            // screen, so Sync Triangles back to Peak first.
            FakeGuest g;
            g.screen = 2;
            g.peak = 0;
            g.type = 0;
            g.row = 1;
            g.goal = kCourses[3].opts[0];
            g.goalEvent = 2;
            NavState n;
            navStart(n, g.goal, 2, 0);
            uint64_t tick = 0;
            uint16_t lastOut = 0;
            for (int i = 0; i < 600 && n.phase != NavPhase::Done && n.phase != NavPhase::Aborted; ++i)
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
            t.Equals(g.peak, 1, "accepted P2");
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
            // Garibaldi Slopestyle: donor ev6 at P2 Free row 0.
            FakeGuest g;
            g.screen = 2;
            g.peak = 0;
            g.type = 0;
            g.row = 0;
            g.goal = kCourses[0].opts[1];
            g.goalEvent = 6;
            NavState n;
            navStart(n, g.goal, 0, 0);
            t.Equals(n.event, 6, "mapped goal verifies the donor event");
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

        tc.Run("driveOnRead arms, drives and reports Done through the shared struct", [](TestCase &t)
               {
            FakeGuest g;
            g.screen = 0;
            g.peak = 1;
            g.type = 0;
            g.row = 0;
            g.goal = kCourses[3].opts[0]; // Mesablanca P2 Race row 0
            g.goalEvent = 2;
            driveDisarm();
            driveArm(g.goal, 2, 0);
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
            driveDisarm();
            p = driveProgress();
            t.Equals(static_cast<int>(p.phase), static_cast<int>(NavPhase::Idle), "disarm resets"); });

        tc.Run("accept-only Peak falls back to step-accept-verify and retries", [](TestCase &t)
               {
            // The guest reports the ACCEPTED peak/type (cursor invisible).
            // Goal Merqury P3 Race row 0: Down+Cross accepts P2 (wrong),
            // Triangle backs out, Down+Cross accepts P3, then Mode, Event.
            // The Peak cursor wraps here (flip coverage is the next test).
            int cursorPeak = 0, acceptedPeak = 0;
            int cursorType = 0, acceptedType = 0;
            int screen = 2, row = 0;
            bool left = false;
            NavState n;
            navStart(n, kCourses[4].opts[0], 4, 0);
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
            for (int i = 0; i < 900 && n.phase != NavPhase::Done && n.phase != NavPhase::Aborted; ++i)
            {
                NavOut o = navStep(n, view(), tick, false, false);
                if (lastOut != 0 && o.mask == 0)
                    apply(lastOut);
                lastOut = o.mask;
                ++tick;
            }
            t.Equals(static_cast<int>(n.phase), static_cast<int>(NavPhase::Done), "fallback completes");
            t.IsTrue(n.handedOff, "handoff");
            t.Equals(acceptedPeak, 2, "P3 accepted after one retry");
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
            for (int i = 0; i < 1200 && n.phase != NavPhase::Done && n.phase != NavPhase::Aborted; ++i)
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
            t.Equals(d.acceptedPeak, 1, "P2 accepted"); });

        tc.Run("P4 reset cursor at Peak 2 still rides Garibaldi Slopestyle", [](TestCase &t)
               {
            const ResetDrive d = driveResetPeak(1);
            t.Equals(d.phase, static_cast<int>(NavPhase::Done), "done from reset value");
            t.IsTrue(d.handedOff, "handoff");
            t.Equals(d.acceptedPeak, 1, "P2 accepted"); });

        tc.Run("P4 reset cursor at last row still rides Garibaldi Slopestyle", [](TestCase &t)
               {
            const ResetDrive d = driveResetPeak(2);
            t.Equals(d.phase, static_cast<int>(NavPhase::Done), "done from last row");
            t.IsTrue(d.handedOff, "handoff");
            t.Equals(d.acceptedPeak, 1, "P2 accepted"); });

        tc.Run("press budget aborts a stuck drive", [](TestCase &t)
               {
            NavState n;
            navStart(n, kCourses[4].opts[0], 4, 0); // Merqury P3
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
