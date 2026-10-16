#include "MiniTest.h"
#include "ps2_ssx3_tricky_list.h"

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
            else if (row == goal.row && goalEvent >= 0)
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
            t.Equals(listGoal(s).row, 0, "default goal is the Race row");
            s.sel = 2;
            t.Equals(listGoal(s).row, 1, "Elysium default is Race");
            listToggle(s);
            t.Equals(listGoal(s).row, 3, "toggle picks the Jam row");
            listToggle(s);
            t.Equals(listGoal(s).row, 1, "toggle flips back");
            s.sel = 0;
            listToggle(s);
            t.Equals(listGoal(s).row, 0, "single-type toggle is a no-op"); });

        tc.Run("list nav wraps; toggle is per-course", [](TestCase &t)
               {
            ListState s;
            listMove(s, true);
            t.Equals(s.sel, 9, "up from 0 wraps to Alaska");
            listMove(s, false);
            t.Equals(s.sel, 0, "down wraps back");
            s.sel = 2;
            listToggle(s);
            s.sel = 5;
            t.Equals(s.opt[5], 0, "Aloha keeps its own option");
            t.Equals(s.opt[2], 1, "Elysium toggle kept"); });

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
            // Goal: Aloha Race, P1 row 2 (event 14). Cursor starts at row 0.
            FakeGuest g;
            g.screen = 2;
            g.peak = 0;
            g.type = 0;
            g.row = 0;
            g.goal = kCourses[5].opts[0];
            g.goalEvent = 14;
            NavState n;
            navStart(n, g.goal, 14, 0);
            uint64_t tick = 0;
            uint16_t lastOut = 0;
            for (int i = 0; i < 600 && n.phase != NavPhase::Done && n.phase != NavPhase::Aborted; ++i)
            {
                NavOut o = navStep(n, g.view(), tick, true, true);
                if (lastOut != 0 && o.mask == 0)
                    g.applyFinished(lastOut);
                lastOut = o.mask;
                ++tick;
            }
            t.Equals(static_cast<int>(n.phase), static_cast<int>(NavPhase::Done), "accepted on row 2");
            t.Equals(g.row, 2, "cursor reached row 2, never the row-3 copy"); });

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
                tick = n.releaseUntil > tick ? n.releaseUntil : tick + 1;
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
                tick = n.releaseUntil > tick ? n.releaseUntil : tick + 1;
            }
            t.Equals(static_cast<int>(n.phase), static_cast<int>(NavPhase::Aborted), "budget aborts"); });

        tc.Run("geometry hits rows, toggle and legends", [](TestCase &t)
               {
            ListGeom g = listLayout(640.0f, 480.0f);
            t.Equals(listHit(g, g.rowX + 4, g.rowY[3] + 2), 3, "row tap");
            t.Equals(listHit(g, g.rowX - 4, g.rowY[3] + 2), -1, "miss left");
            t.Equals(listHit(g, g.selectX + 2, g.selectY + 2), 11, "select legend");
            t.Equals(listHit(g, g.backX + 2, g.backY + 2), 12, "back legend");
            t.Equals(listHit(g, g.toggleX + 2, g.toggleY + 2), 10, "toggle"); }); });
}
