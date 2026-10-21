#pragma once
#include "ps2_ssx3_fe_launch.h"
// tk-menu-1 Part 2: the dedicated Tricky course list (option (a), Brad 10-15).
//
// A host-drawn, SSX 3-styled overlay listing Tricky's 10 courses in Tricky's
// original order, plus a closed-loop navigator that drives the stock
// Peak -> Mode -> Event screens hidden underneath to hand off to the guest's
// own course-load path. Pure logic here (unit-tested by
// ps2_ssx3_tricky_list_tests.cpp); the render loop in ps2_runtime.cpp owns
// open/close, pad ownership, edge injection, drawing and touch.
//
// Guest-affecting when on (it injects pad edges, the same class as TK11's
// injection); the LIST knob (PS2X_SSX3_TRICKY_LIST, default off) additionally
// requires the MENU knob armed with Tricky mode on. Knob off: nothing here
// runs (one bool check per host frame in the caller).

#include "ps2_ssx3_tricky_menu.h"
#include "ps2_ssx3_tricky_preview.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <mutex>

namespace ps2_ssx3_tricky_list
{

// The LIST knob (lane override, default off) or the MENU knob (release path:
// the RC ships TRICKY_MENU=1 with no LIST row, and the overlay never armed).
// Cached: knobs never change mid-run.
inline bool knob()
{
    static const bool on = [] {
        const char *v = std::getenv("PS2X_SSX3_TRICKY_LIST");
        if (v && std::strcmp(v, "1") == 0)
            return true;
        const char *m = std::getenv("PS2X_SSX3_TRICKY_MENU");
        return m && std::strcmp(m, "1") == 0;
    }();
    return on;
}

// The DIRECT knob (default off until polished; the navigator is the explicit
// off/reference path): overlay taps ride straight into the donor event instead of
// driving the stock screens. Requires Tricky mode, like the overlay itself.
// Cached: knobs never change mid-run.
inline bool directKnob()
{
    static const bool on = [] {
        const char *v = std::getenv("PS2X_SSX3_TRICKY_DIRECT");
        return v && std::strcmp(v, "1") == 0;
    }();
    return on;
}

// ---- Course table (Tricky order; Part 1 §1 + receipts/ssx3-events-nav.txt)
//
// Each course names its SSX 3 event id and, per event-type option, the
// (peak, mode-type, list row) the navigator must accept. Every course is
// dual-mode (Tricky-authentic): the race-list row and the freestyle-list
// row, picked on a second step (Part 2 amendment: no in-row toggle).
// Mode labels are Tricky's own event names (Race / Showoff); the drive
// targets keep the stock SSX 3 rows. The Jam rows' donor events read discipline 6
// (event table 23x8B at ISO 0x435820 = RAM 0x442820: events 14/15/16;
// enum 2 race, 3 slopestyle, 4 big air, 5 halfpipe, 6 backcountry per
// ps2_ssx3_course_manifest.h), but Brad 10-16: they play as trick events,
// so the picker labels them "Slopestyle" (display only; drive targets keep
// the stock event ids). Cross-checks: event 11 reads 5 and its label
// is the stock "Super Pipe" word, event 8 reads 4 and the stock hover pane
// says "Big Air", events 6/7 read 3 "Slopestyle"). `tp` is the TP*.SSH
// preview code, or null when the overlay ships none (Megaplex: text card,
// Part 1 Q3).
// Launch-time world mapping for one mode (Part 3): the navigator drives
// the donor event's (peak, type, row), and at the ToEvent->Accept edge the
// game thread points the donor's world archive+name at the course world
// (design: tk-modes-1 spike 90effd64, TK8 §3a). event < 0 = native row.
struct MapSpec
{
    int8_t event = -1;
    const char *archive = nullptr;
    const char *name = nullptr;
};

struct CourseOption
{
    uint8_t peak; // 0..2 (P1..P3)
    uint8_t type; // 0 Race list, 1 Freestyle list
    uint8_t row;  // cursor row on the Select Event list
    MapSpec map;  // world mapping, or none
};

struct Course
{
    const char *name;
    const char *tp; // TP preview code, or nullptr
    int event;      // SSX 3 event id (both options share it)
    CourseOption opts[2];
    uint8_t nOpts;            // modes on the step-2 list (data-driven)
    const char *modeName[2];  // step-2 labels, SSX 3's own event-type names
};

inline constexpr Course kCourses[10] = {
    // Part 4: every goal is Peak 1 (selectable on a fresh card; P2/P3 rows
    // refuse with LOCKED and the game never advances). All Race options
    // share P1 Race row 0 (ev0, proven fresh by Garibaldi Race); all
    // Slopestyle options share P1 Free row 3 (ev14, proven fresh by Aloha
    // Slopestyle): same (cell, rules-kind) pairings as the validated legs,
    // only the world/name swap target changes. Tricky-authentic modes
    // (local Tricky ISO survey): every course offers Race AND Showoff,
    // opts[0] = Race, opts[1] = Showoff, no navigator fallback in the
    // picker path. Each mapped option carries map{loadedEv, ownArchive,
    // ownName}; native rows stay unmapped. Archive codes verified live
    // (preview course() gate, except MEGA which ships in preset/worlds);
    // names are display strings (<= 31 chars).
    {"Garibaldi", "GARI", 0, {{0, 0, 0}, {0, 1, 3, {14, "GARI", "Garibaldi"}}}, 2, {"Race", "Showoff"}},
    {"Snowdream", "SNOW", 1, {{0, 0, 1}, {0, 1, 3, {14, "SNOW", "Snowdream"}}}, 2, {"Race", "Showoff"}},
    {"Elysium Alps", "ELYS", 16, {{0, 0, 0, {0, "ELYS", "Elysium Alps"}}, {0, 1, 3, {14, "ELYS", "Elysium Alps"}}}, 2, {"Race", "Showoff"}},
    {"Mesablanca", "MESA", 2, {{0, 0, 0, {0, "MESA", "Mesablanca"}}, {0, 1, 3, {14, "MESA", "Mesablanca"}}}, 2, {"Race", "Showoff"}},
    {"Merqury City", "MERQ", 4, {{0, 0, 0, {0, "MERQ", "Merqury City"}}, {0, 1, 3, {14, "MERQ", "Merqury City"}}}, 2, {"Race", "Showoff"}},
    {"Aloha Ice Jam", "ALOHA", 14, {{0, 0, 2}, {0, 1, 3}}, 2, {"Race", "Showoff"}},
    {"Pipedream", "PIPE", 11, {{0, 0, 0}, {0, 1, 2}}, 2, {"Race", "Showoff"}},
    {"Untracked", "UNTRACK", 7, {{0, 0, 0}, {0, 1, 3, {14, "UNTRACK", "Untracked"}}}, 2, {"Race", "Showoff"}},
    {"Tokyo Megaplex", nullptr, 15, {{0, 0, 0, {0, "MEGA", "Tokyo Megaplex"}}, {0, 1, 3, {14, "MEGA", "Tokyo Megaplex"}}}, 2, {"Race", "Showoff"}},
    {"Alaska", "ALASKA", 6, {{0, 0, 0}, {0, 1, 3, {14, "ALASKA", "Alaska"}}}, 2, {"Race", "Showoff"}},
};

inline constexpr uint32_t kCourseCount = 10u;

// ---- Direct-launch goal ---------------------------------------------------
// The guest chain (config + shared loader, no menu walk) rides proven-clean
// donor shells: ev0 for the Race cells (null nav nodes, resolved as race
// type/rules), ev1 for Snowdream Race (its native shell), ev11 (Pipe's own
// freestyle shell) for the Showoff cells with measured args. Donor ev14
// dies in loading on an unhandled-exception loop (0x80000080, probe path),
// so no direct cell uses it; the world pin still loads the course's own
// archive+name.
// Donor bound for direct goals. Mirrors kMapDonorMax (map section below;
// a static_assert there keeps the two in sync).
inline constexpr int kDirectDonorMax = 22;

struct DirectGoal
{
    bool ok = false;   // directable now (cell with known accept args)
    int event = -1;    // donor event to write + queue
    MapSpec map = {};  // world mapping (event < 0 = native row, restore)
    bool mapped = false;
    int typeArg = 0;  // a1 for the 0x144df0 config call (nav node type)
    int rulesArg = 0; // a1 for the 0x1451e8 config call (nav node rules)
};

// Accept args per (type-list, row), read off the static nav tables
// (tk-menu-1 events.py: node[4]=ty, node[8]=words[0]). Refinement: all 10
// courses x {Race, Showoff} direct — Race cells (0,0)/(0,1)/(0,2), Showoff
// cells (1,2) Pipe + (1,3) everywhere else, with measured args. Donor
// ev14 (Aloha/Untracked/Alaska stock shell) dies in loading on an
// unhandled-exception loop (0x80000080, probe path — independent of the
// tap-path load-select repair), so those cells ride proven-clean shells:
// Aloha Race on ev0 (the Race shape), the (1,3) cells on ev11 (Pipe's own
// freestyle shell). World pins stay on the course's own archive+name.
inline bool directCellArgs(int type, int row, int &typeArg, int &rulesArg)
{
    if (type == 0 && (row == 0 || row == 1))
    {
        typeArg = 0;
        rulesArg = 0;
        return true;
    }
    if (type == 0 && row == 2)
    {
        typeArg = 5;
        rulesArg = 4;
        return true; // Aloha Race (donor ev0, world ALOHA)
    }
    if (type == 1 && row == 2)
    {
        typeArg = 3;
        rulesArg = 2;
        return true; // Pipedream Showoff (donor ev11, own event)
    }
    if (type == 1 && row == 3)
    {
        typeArg = 6;
        rulesArg = 5;
        return true; // Showoff (donor ev11) for Gari/Snow/Elys/Mesa/Merq/Aloha/Untrack/Mega/Alaska
    }
    return false;
}

// Direct-launch donor shell: the navigator walks the table's stock donor
// (goal.map / c.event), but the direct chain rides proven-clean shells —
// ev14 dies in loading, so ev14 cells are re-homed and every other cell
// keeps its stock donor. The world pin (archive+name) is untouched: the
// course's own geometry still loads under the clean shell.
inline int directDonorFor(int type, int row, int stockEvent)
{
    if (type == 0 && row == 1)
        return 1; // Snowdream Race keeps its native donor ev1 (proven)
    if (type == 0)
        return 0; // every Race cell rides the ev0 race shell + own world pin
    if (type == 1 && row == 2)
        return 7; // TRIAL (pipe-show-brad): Pipedream Showoff on Untracked's
                  // ev7 slopestyle shell — ev11+PIPE world dies in loading
                  // on Brad's card (0x80000080 loop, tick ~9416, 2/2 runs)
                  // while the same cell is clean on a fresh card
    if (type == 1)
        return 11; // every other Showoff cell rides Pipe's ev11 shell + own world pin
    return stockEvent;
}

inline DirectGoal directGoalFor(const Course &c, const CourseOption &goal)
{
    DirectGoal d;
    // Donor cell + accept args by (type-list, row). Unknown cells fail closed;
    // the DIRECT-on picker never falls back to the navigator.
    if (!directCellArgs(goal.type, goal.row, d.typeArg, d.rulesArg))
        return d;
    // Donor shell: clean-shell override, else the table's stock donor.
    const int stock = (goal.map.event >= 0) ? goal.map.event : c.event;
    d.event = directDonorFor(goal.type, goal.row, stock);
    // Launch-time world pin: the explicit map, else the course's own
    // archive+name on its donor (the proven Garibaldi shape). This also
    // marks the ride for the records guard, like the navigator's edge.
    const char *archive = (goal.map.event >= 0) ? goal.map.archive : c.tp;
    const char *name = (goal.map.event >= 0) ? goal.map.name : c.name;
    if (d.event < 0 || d.event > kDirectDonorMax || !archive || !name)
        return DirectGoal{};
    d.map.event = static_cast<int8_t>(d.event);
    d.map.archive = archive;
    d.map.name = name;
    d.mapped = true;
    d.ok = true;
    return d;
}

// ASCII case-insensitive compare (F09: env values like "gari" must match
// the "GARI" table entries; no <cctype> locale dependence).
inline bool directCodeEqCI(const char *a, const char *b)
{
    if (!a || !b)
        return false;
    while (*a && *b)
    {
        char ca = *a++, cb = *b++;
        if (ca >= 'A' && ca <= 'Z')
            ca = static_cast<char>(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z')
            cb = static_cast<char>(cb + ('a' - 'A'));
        if (ca != cb)
            return false;
    }
    return *a == *b;
}

// Lookup by world archive code (or course name) for the diagnostic course
// selector. Case-insensitive. Null/empty keeps the Garibaldi default; an
// unknown non-empty code returns -1 so the probe caller defers to stock
// instead of silently riding Garibaldi (F09).
inline int directCourseByCode(const char *code)
{
    if (!code || !*code)
        return 0;
    for (int i = 0; i < static_cast<int>(kCourseCount); ++i)
    {
        const Course &c = kCourses[i];
        if (directCodeEqCI(code, c.tp) || directCodeEqCI(code, c.name))
            return i;
        for (uint8_t m = 0; m < c.nOpts; ++m)
            if (directCodeEqCI(code, c.opts[m].map.archive))
                return i;
    }
    return -1;
}

// ---- List navigation (pad edges and touch taps map to the same steps) ----
// Two steps (Part 2 amendment): step 0 picks the course (10 names, Tricky
// order); step 1 picks the mode from the course's own modeName list
// (data-driven: later modes appear with no UI change). Every course is
// dual-mode (Race / Showoff), so step 1 always shows. Triangle steps back.
struct ListState
{
    uint8_t sel = 0;  // highlighted course 0..9 (kept across re-opens)
    uint8_t step = 0; // 0 courses, 1 modes
    uint8_t mode = 0; // highlighted mode 0..nOpts-1 on step 1
    // tricky-picker-2: per-course one-tap ride default (touch). Index 0 =
    // the course's first mode (Race where offered); the touch toggle cycles
    // it. Controller flow (step/mode) is untouched.
    uint8_t modeDef[10] = {};
};

inline void listMove(ListState &s, bool up)
{
    if (s.step == 0)
    {
        s.sel = static_cast<uint8_t>((s.sel + (up ? kCourseCount - 1u : 1u)) % kCourseCount);
        return;
    }
    const uint8_t n = kCourses[s.sel].nOpts;
    s.mode = static_cast<uint8_t>((s.mode + (up ? n - 1u : 1u)) % n);
}

inline const CourseOption &listGoal(const ListState &s)
{
    const Course &c = kCourses[s.sel];
    return c.opts[s.step == 1 ? s.mode : 0];
}

// ---- One-tap ride default (tricky-picker-2, touch) -------------------------
//
// A tap on the highlighted course (or Select) rides immediately with the
// course's default mode instead of opening the mode step. The touch toggle
// (hit 10) cycles the default for dual-mode courses; single-mode courses
// always ride their only mode.
inline const Course &listCourse(const ListState &s)
{
    return kCourses[s.sel < kCourseCount ? s.sel : 0];
}

inline uint8_t listDefMode(const ListState &s)
{
    const Course &c = listCourse(s);
    const uint8_t d = s.modeDef[s.sel < kCourseCount ? s.sel : 0];
    return d < c.nOpts ? d : 0;
}

inline const CourseOption &listDefaultGoal(const ListState &s)
{
    const Course &c = listCourse(s);
    return c.opts[listDefMode(s)];
}

inline const char *listDefaultModeName(const ListState &s)
{
    const Course &c = listCourse(s);
    const char *name = c.modeName[listDefMode(s)];
    return name ? name : "?";
}

// Cycle the selected course's ride default; returns the new default index
// (0 for single-mode courses, which never change).
inline uint8_t listCycleMode(ListState &s)
{
    const Course &c = listCourse(s);
    if (c.nOpts < 2)
        return 0;
    const uint8_t idx = static_cast<uint8_t>(s.sel < kCourseCount ? s.sel : 0);
    s.modeDef[idx] = static_cast<uint8_t>((listDefMode(s) + 1u) % c.nOpts);
    return s.modeDef[idx];
}

// One-tap rule (Brad 10-19): a course tap opens the mode screen when the
// course has more than one mode, else it rides straight away. Pure so the
// render loop and the unit tests share it.
inline bool listCourseOpensModes(uint8_t sel)
{
    return sel < kCourseCount && kCourses[sel].nOpts > 1;
}

// Triangle-back (Brad 10-19: the first press must go back visibly). Step 1
// returns to the course list with the overlay open; step 0 leaves the
// overlay flow (BackOut drive) and the list closes at once — a BackOut that
// stays open reads as an eaten press, and the next Triangle cancels the
// walk instead, so the list never goes back. The done path only latches an
// open overlay, so the caller latches at arm time. Pure so the render loop
// and the unit tests share it.
inline bool listTriangleClosesNow(uint8_t step)
{
    return step != 1;
}

// Render thread: park a tap-to-ride request for the game thread. The overlay
// closes at the tap (host owns it); the game thread queues loading from pad
// glue and keeps directActive set so the overlay cannot reopen mid-flight.
// The stored list position re-derives course + goal on take, so there is a
// single resolution path for taps and the diagnostic course selector.
struct DirectAsk
{
    bool pending = false;
    ListState list = {};
    // Taps can land on any Map screen, but the loading chain is only proven
    // from the main menu (a Setup-Character entry dies mid-load on a null
    // call). With backout set, the tap site arms the proven BackOut drive
    // and the game thread launches once it reports Done; the probe path
    // (already at the menu) launches immediately. frames bounds the wait
    // (~30 s of pad reads); on expiry the ask is dropped and the overlay
    // re-arms so the player can retry by hand.
    bool backout = false;
    bool walked = false; // BackOut done: the Tricky-accept walk is armed or
                         // done; the ask now waits on the main-menu accept hook
    int frames = 0;
};

inline DirectAsk &directAsk()
{
    static DirectAsk a;
    return a;
}

inline std::mutex &directAskMu()
{
    static std::mutex m;
    return m;
}

inline void directArm(const ListState &list, bool backout)
{
    std::lock_guard<std::mutex> lock(directAskMu());
    DirectAsk &a = directAsk();
    a.pending = true;
    a.list = list;
    a.backout = backout;
    a.walked = false;
    a.frames = 0;
}

inline bool directPeek(ListState &list, bool &backout, bool &walked)
{
    std::lock_guard<std::mutex> lock(directAskMu());
    const DirectAsk &a = directAsk();
    if (!a.pending)
        return false;
    list = a.list;
    backout = a.backout;
    walked = a.walked;
    return true;
}

// Game thread: BackOut reached the main menu. The accept walk takes over;
// the ask now waits on the main-menu accept hook (proven handler context).
inline void directSetWalked()
{
    std::lock_guard<std::mutex> lock(directAskMu());
    DirectAsk &a = directAsk();
    a.backout = false;
    a.walked = true;
}

inline int directBump()
{
    std::lock_guard<std::mutex> lock(directAskMu());
    return ++directAsk().frames;
}

inline void directDisarm()
{
    std::lock_guard<std::mutex> lock(directAskMu());
    directAsk().pending = false;
}

// Venue metadata is published with the generic request. The FE consumer
// takes the request first, then this lock; no guest RAM is touched here.
inline MapSpec &directVenue() { static MapSpec m; return m; }
// tk-fe-2: picker-session generation. Bumped at every overlay CLOSE (render
// thread) except the Cross-arm close that carries a fresh park; each mailbox
// request is stamped so the FE consumer (game thread) drops requests from a
// closed session instead of firing them later. Close (not open): the probe
// parks at the main-menu accept BEFORE the first open, so an open-bump would
// orphan every probe park; a close-bump keeps park and consume in one epoch
// while a close-then-reopen still invalidates the stale park.
inline std::atomic<uint64_t> &pickerSession()
{
    static std::atomic<uint64_t> s{0};
    return s;
}
inline void pickerSessionBump() { pickerSession().fetch_add(1u, std::memory_order_acq_rel); }
inline bool requestDirect(const ListState &list)
{
    if (list.sel >= kCourseCount) return false;
    const Course &c = kCourses[list.sel];
    const DirectGoal goal = directGoalFor(c, listGoal(list));
    if (!goal.ok) return false;
    std::lock_guard<std::mutex> lock(directAskMu());
    if (!ps2_ssx3_fe_launch::request(goal.event, goal.typeArg, goal.rulesArg, 1,
                                     pickerSession().load(std::memory_order_acquire)))
        return false;
    directVenue() = goal.map;
    return true;
}
inline MapSpec takeDirectVenue()
{
    std::lock_guard<std::mutex> lock(directAskMu());
    return directVenue();
}

// ---- Guest FE view (read each tick; the navigator derives from this) ----
//
// 0x4a259c: the Map-screen sub-state (hypothesis under test: 0 = Select
// Peak, 1 = Select Mode, 2 = Select Event; anything else = not a Map
// screen). 0x4a25a4 = peak 0..2, 0x4a25a0 = mode type 0..2. The Event cursor
// row comes from the list widget's +0x95 (cursorEvent), cross-checked to the
// expected event id before any accept.
struct FeView
{
    bool mapLive = false;
    int screen = -1; // 0 peak, 1 mode, 2 event, -1 other
    uint32_t peak = 0u;
    uint32_t type = 0u;
    bool peakOk = false;
    bool typeOk = false;
    int row = -1;   // Event-screen cursor row, or -1
    int event = -1; // cursorEvent(), or -1
};

// Event filecode table (lane atlas bake): word 0x442950+ev*4 points at
// a 4-char code. Unwritten slots hold 0xFFFFFFFF, whose +4 wraps to 3 and
// would pass a naive bounds check (m9a SIGSEGV/SIGBUS): compare without
// wrapping, and read the code through the same gate.
inline bool filecodeOk(uint32_t cp, uint32_t size)
{
    return size >= 4u && cp >= 0x1000u && cp <= size - 4u;
}

inline const char *readFilecode(const uint8_t *ram, uint32_t size, int ev)
{
    if (!ram || ev < 0 || ev > 63)
        return nullptr;
    const uint32_t tab = 0x442950u + static_cast<uint32_t>(ev) * 4u;
    if (tab > size - 4u)
        return nullptr;
    uint32_t cp = 0;
    std::memcpy(&cp, ram + tab, 4);
    return filecodeOk(cp, size) ? reinterpret_cast<const char *>(ram + cp) : nullptr;
}

inline FeView readView(const uint8_t *ram, uint32_t size, uint32_t controller)
{
    using namespace ps2_ssx3_tricky;
    FeView v;
    if (!ram || size < 0x4a25a8u)
        return v;
    const uint32_t sub = rd32(ram, 0x4a259cu);
    if (sub > 2u)
        return v;
    v.mapLive = true;
    v.screen = static_cast<int>(sub);
    const uint32_t peak = rd32(ram, 0x4a25a4u);
    const uint32_t type = rd32(ram, 0x4a25a0u);
    v.peakOk = peak <= 2u;
    v.typeOk = type <= 2u;
    v.peak = peak;
    v.type = type;
    if (v.screen == 2)
    {
        const int ev = ps2_ssx3_tricky_preview::cursorEvent(ram, size, controller);
        v.event = ev;
        // Row: re-derive without re-gating (cursorEvent already validated).
        if (ev >= 0)
        {
            const char *types[] = {"Race", "Freestyle", "Freeride"};
            char name[48];
            std::snprintf(name, sizeof(name), "Peak%u%sLocations", peak + 1, types[type]);
            const uint32_t obj = ps2_ssx3_tricky_preview::widget(ram, size, rd32(ram, controller + 0x2e8u), name);
            if (obj)
                v.row = ram[obj + 0x95u];
        }
    }
    return v;
}

// ---- Overlay open gate ----------------------------------------------------
// Direct launches queue loading without any Map fill, leaving stale Map-live
// words that would otherwise open the overlay (and zero the pad) through
// loading and the briefing. The probe sets directActive; a real Map fill or
// a main-menu build clears it, so the stock navigator path is untouched.
inline bool overlayTarget(bool mapLive, int screen, int row, bool latchedOn, uint64_t entries,
                          uint64_t latchGen, bool directActive)
{
    return !directActive && mapLive && (screen != 2 || row >= 0) &&
        (!latchedOn || entries != latchGen);
}

// ---- Closed-loop navigator -----------------------------------------------
// Drives the hidden stock screens to (peak, type, row) and accepts. One
// navStep call per guest tick; the caller holds the returned mask until the
// next tick (tick-keyed presses: no wall clock, so pad-script replays stay
// det). Presses hold kHoldTicks guest ticks, releases last kGapTicks; after
// each screen change the reader settles kSettleTicks before trusting the
// cursor. Anything unexpected (unreadable view, budget spent) aborts with
// the pad released: the overlay stays open and the user picks again.
inline constexpr uint64_t kHoldTicks = 12u; // human-like holds (ONL10 uses 15); 4 proved too short
inline constexpr uint64_t kGapTicks = 6u;
inline constexpr uint64_t kSettleTicks = 10u;
inline constexpr uint64_t kBlindTicks = 30u; // screen transitions may pass through non-Map states
inline constexpr uint64_t kAcceptBlindTicks = 150u; // accept-driven screen changes (Peak->Mode, Mode->Event) outlast the press blind: the old screen still reads at +48 (P6 Garibaldi), so a fallback accept that is still in flight looks exactly like one that did not take
inline constexpr uint64_t kGoneDoneTicks = 60u; // a dead Event widget this long after accept = handed off
inline constexpr int kSetupTries = 4; // Setup-Continue accepts before giving up
inline constexpr uint64_t kSetupBlindTicks = 300u; // Setup->Peak journey per press (traces: the peak word
                                                   // flips ~150 after the Continue accept, the screen follows)
inline constexpr uint64_t kSetupSettleTicks = 150u; // the words lead the screen; settle past the flip
inline constexpr int kPressBudget = 48;
inline constexpr int kBackOutPresses = 8; // BackOut walks at most Select
    // Character -> Select Event -> main menu. The Map-left done-slide cannot
    // see that walk (the sub-state word reads live off-list), so cap the
    // Triangles instead: completing closes the overlay and returns the pad
    // rather than pressing into later menus forever.
inline constexpr int kPeakTries = 16; // accept-only attempts (P3: 8 exhausted on walker-messed states;
                                      // Part 2: the same budget also covers transition-eaten accepts)
inline constexpr int kModeTries = 14; // (P3: 6 exhausted the same way; Part 2: also covers eaten accepts)

enum class NavPhase : uint8_t
{
    Idle = 0, // overlay browsing (no driving)
    Sync,     // Triangle back until the Select Peak screen
    SetupConfirm, // first: leave Setup Character (Continue) and reach Select Peak
    ToPeak,   // step the peak, X to accept
    ToMode,   // step the mode type, X to accept
    ToEvent,  // step the event row, X to accept
    Accept,   // wait for the guest to leave the Map screens (loading)
    BackOut,  // user Triangle: back out until the Map screens are left
    ToTricky, // direct tap: blind Tricky-accept walk on the fresh main menu
    Done,     // handed off (Accept) or backed out (BackOut)
    Aborted,  // budget/view failure; pad released, overlay stays open
};

struct NavState
{
    NavPhase phase = NavPhase::Idle;
    CourseOption goal = {};
    int event = -1; // verified cursor event (donor for mapped goals)
    MapSpec map;    // world mapping to apply at the Accept edge, or none
    uint16_t hold = 0;       // mask held now
    uint64_t holdUntil = 0;  // guest tick (exclusive)
    uint64_t releaseUntil = 0;
    uint64_t settleUntil = 0; // ignore the view below this tick
    uint64_t lastTick = 0;    // tick of the previous step (load = jump back)
    uint64_t blindUntil = 0;  // ignore a non-Map view below this tick
    int presses = 0;
    int dir = 0;      // adaptive step direction (+1/-1), 0 = undecided
    int lastPos = -2; // peak/type/row seen at the last evaluation
    int tries = 0;    // accept-only attempts (ToPeak/ToMode fallback)
    // Part 4 fail-fast: consecutive identical settled retry reads. A real
    // transition always moves something (screen, words, entries); a fully
    // static Peak/Mode screen across retries means the row refuses (locked)
    // and no further accept will land. stillWhere keys the (phase, sub) of
    // the last note, so any branch or phase change restarts the count.
    int stillStreak = 0;
    int stillKey = 0;
    int stillWhere = -1;
    bool refused = false; // streak abort fired (locked row?): toast says so
    int sub = 0;      // accept-only sub-step (0 step, 1 accept, 2 verify, 3 back,
                      // 4 give up, 6 post-back-out accept, 7 directed step)
    uint64_t goneSince = 0; // Accept: first tick the Event widget read dead
    int anchor = 0;   // top on first entry (legacy fast path); after any
                      // back-out it holds the last directed step (0 = none)
                      // for stagnation flips. The pre-accept words are stale
                      // (m4) and the re-entry cursor resets (P4), so the
                      // fallback never trusts a counted position.
    bool handedOff = false; // Done via Accept (race loading), else backed out
    uint32_t setupPeak = 0;  // peak word at arm (Setup reads 0; a change proves Peak entry)
    bool setupArmed = false; // first SetupConfirm evaluation consumed
};

struct NavOut
{
    uint16_t mask = 0;
    bool done = false;
    bool aborted = false;
    bool mapApply = false;   // ToEvent->Accept with a mapped goal: the caller
    bool mapRestore = false; // writes (or restores) before publishing the mask
};

inline const char *navPhaseName(NavPhase p)
{
    switch (p)
    {
    case NavPhase::Idle:
        return "Idle";
    case NavPhase::Sync:
        return "Sync";
    case NavPhase::SetupConfirm:
        return "SetupConfirm";
    case NavPhase::ToPeak:
        return "ToPeak";
    case NavPhase::ToMode:
        return "ToMode";
    case NavPhase::ToEvent:
        return "ToEvent";
    case NavPhase::Accept:
        return "Accept";
    case NavPhase::BackOut:
        return "BackOut";
    case NavPhase::ToTricky:
        return "ToTricky";
    case NavPhase::Done:
        return "Done";
    case NavPhase::Aborted:
        return "Aborted";
    }
    return "?";
}

// Button masks (same values as ps2_virtual_pad.h; kept local so this header
// stays dependency-free for the host suite).
inline constexpr uint16_t kPadUp = 0x0010u;
inline constexpr uint16_t kPadDown = 0x0040u;
inline constexpr uint16_t kPadLeft = 0x0080u;
inline constexpr uint16_t kPadRight = 0x0020u;
inline constexpr uint16_t kPadTriangle = 0x1000u;
inline constexpr uint16_t kPadCross = 0x4000u;

// Starts driving to `goal` (user pressed X on the list).
inline void navStart(NavState &n, const CourseOption &goal, int event, uint64_t tick)
{
    n = NavState{};
    n.phase = NavPhase::Sync;
    n.goal = goal;
    n.map = goal.map;
    n.event = (goal.map.event >= 0) ? goal.map.event : event;
    n.releaseUntil = tick; // evaluate immediately
    n.lastTick = tick;
}

// Starts backing out (user pressed Triangle on the list).
inline void navBackOut(NavState &n, uint64_t tick)
{
    n = NavState{};
    n.phase = NavPhase::BackOut;
    n.releaseUntil = tick;
    n.lastTick = tick;
}

// Direct tap, second leg: the guest is back at the main menu. Clamp the
// cursor to the top with 5 ups (5 rows: wrap or clamp, both land top),
// walk down 3 to the Tricky Courses row, and accept; the main-menu accept
// hook serves the parked ask in the proven handler context. Blind by
// design (the main menu is not a Map screen, so no view check can run or
// complete this). Settles first: BackOut done fires as the Map screens go,
// while the menu is still building presses into it get eaten (tap-gari5:
// cursor reached Tricky but the Cross never accepted). The Cross is
// pressed up to 3 times, blind-spaced: the first accept serves the ask
// and the hook disarms the walk, so later presses only fire while the ask
// is still parked. A walk that still misses leaves the ask parked; the
// serve timeout disarms it and the overlay re-arms.
inline constexpr int kTrickyAcceptPresses = 11;
inline constexpr uint64_t kTrickySettleTicks = 2500;
inline void navToTricky(NavState &n, uint64_t tick)
{
    n = NavState{};
    n.phase = NavPhase::ToTricky;
    n.releaseUntil = tick + kTrickySettleTicks;
    n.lastTick = tick;
}

inline void navReset(NavState &n) { n = NavState{}; }

// Abort on the 3rd identical settled retry read: at the measured 150-tick
// retry spacing that is ~5 s after the loop goes static (vs ~50 s for the
// 16-try cap). Call only where a retry press is actually issued (the
// hold/gap early-returns filter every other evaluation, so each call is one
// guest-visible decision).
inline constexpr int kRefusedStreak = 2;
inline bool stillRefused(NavState &n, const FeView &v)
{
    const int peakW = v.peakOk ? static_cast<int>(v.peak) : 3;
    const int typeW = v.typeOk ? static_cast<int>(v.type) : 3;
    const int rowW = v.row + 1 < 0 ? 0 : (v.row + 1 > 63 ? 63 : v.row + 1);
    const int key = ((((v.screen + 2) * 4 + peakW) * 4 + typeW) * 64 + rowW);
    const int where = static_cast<int>(n.phase) * 8 + n.sub;
    if (n.stillWhere != where || n.stillKey != key)
    {
        n.stillWhere = where;
        n.stillKey = key;
        n.stillStreak = 0;
        return false;
    }
    if (++n.stillStreak >= kRefusedStreak)
    {
        n.refused = true;
        return true;
    }
    return false;
}

inline NavOut navStep(NavState &n, const FeView &v, uint64_t tick, bool peakLive, bool modeLive)
{
    NavOut out;
    if (n.phase == NavPhase::Idle || n.phase == NavPhase::Done || n.phase == NavPhase::Aborted)
        return out;
    const auto abort = [&]() {
        n.phase = NavPhase::Aborted;
        n.hold = 0;
        out.aborted = true;
        return out;
    };
    // Quick-load: the guest tick jumped back. Re-drive from the Sync phase
    // (BackOut keeps backing out, ToTricky keeps walking); the overlay
    // re-opens from guest state.
    if (tick < n.lastTick && n.phase != NavPhase::Idle && n.phase != NavPhase::Done &&
        n.phase != NavPhase::Aborted)
    {
        if (n.phase != NavPhase::BackOut && n.phase != NavPhase::ToTricky)
            n.phase = NavPhase::Sync;
        n.hold = 0;
        n.releaseUntil = 0;
        n.settleUntil = 0;
        n.blindUntil = 0;
        n.dir = 0;
        n.lastPos = -2;
        n.tries = 0;
        n.sub = 0;
        n.anchor = 0;
    }
    n.lastTick = tick;
    // A press in flight: keep holding it for the full hold window (the
    // guest must see the level across several of its own reads).
    if (n.hold && tick < n.holdUntil)
    {
        out.mask = n.hold;
        return out;
    }
    n.hold = 0;
    // Release gap: no mask until the gap expires.
    if (tick < n.releaseUntil)
        return out;
    // Transition blackout: after every press the screens may pass through
    // non-Map or cursorless states (m5: the Event widget reads row=-1 for a
    // dozen ticks after the accept). Never evaluate screen/cursor content
    // inside the blind window; a persistent absence completes below.
    const bool blind = tick < n.blindUntil;
    if (blind && v.mapLive)
        return out;
    const auto press = [&](uint16_t mask) -> NavOut {
        if (++n.presses > kPressBudget)
            return abort();
        n.hold = mask;
        n.holdUntil = tick + kHoldTicks;
        n.releaseUntil = tick + kHoldTicks + kGapTicks;
        n.blindUntil = tick + kHoldTicks + kGapTicks + kBlindTicks;
        out.mask = mask;
        return out;
    };
    // An accept press changes screens: stretch the blind window over the
    // whole transition so a still-in-flight accept is waited out, not
    // retried mid-transition and then given up on (sub==4 abort).
    const auto pressAccept = [&](uint16_t mask) -> NavOut {
        NavOut o = press(mask);
        if (!o.aborted)
            n.blindUntil = tick + kAcceptBlindTicks;
        return o;
    };
    if (!v.mapLive)
    {
        // Screen transitions may pass through non-Map states; only a
        // persistent absence counts. A handoff when we accepted, a close
        // when the user backed out (Setup Character / main menu own the pad
        // again).
        if (tick < n.blindUntil)
            return out;
        if (n.phase == NavPhase::Accept || n.phase == NavPhase::BackOut)
        {
            n.handedOff = (n.phase == NavPhase::Accept);
            n.phase = NavPhase::Done;
            out.done = true;
            return out;
        }
        // The accept walk runs on the main menu (never a Map screen):
        // keep pressing blind; the press count in the switch completes it.
        // Every other off-Map phase aborts here.
        if (n.phase != NavPhase::ToTricky)
            return abort();
    }
    if (tick < n.settleUntil)
        return out;
    switch (n.phase)
    {
    case NavPhase::SetupConfirm:
        // Overlay launch lands the guest in Setup Character (the menu row
        // enters the stock single-event flow), but the Map words read
        // screen 0 there too — a raw Sync would fire into Setup. Press
        // Cross (Continue) and wait for the Peak entry, proven by a
        // peak-word change; an already-entered Peak skips with no press.
        if (!v.mapLive)
        {
            if (tick < n.blindUntil)
                return out;
            return abort();
        }
        if (v.screen == 1 || v.screen == 2)
        {
            // Launched mid-flow (or a stray advance): Sync re-syncs.
            n.phase = NavPhase::Sync;
            return navStep(n, v, tick, peakLive, modeLive);
        }
        if (v.screen != 0)
            return abort();
        if (!n.setupArmed)
        {
            n.setupArmed = true;
            n.setupPeak = v.peakOk ? v.peak : 0u;
            if (n.setupPeak != 0u)
            {
                n.phase = NavPhase::Sync;
                return navStep(n, v, tick, peakLive, modeLive);
            }
        }
        if (v.peakOk && v.peak != n.setupPeak)
        {
            // Peak entry observed. The words lead the screen, so settle
            // past the flip before trusting the cursor.
            n.phase = NavPhase::Sync;
            n.settleUntil = tick + kSetupSettleTicks;
            return navStep(n, v, tick, peakLive, modeLive);
        }
        if (n.tries >= kSetupTries)
            return abort();
        ++n.tries;
        {
            NavOut setup = pressAccept(kPadCross);
            if (!setup.aborted)
                n.blindUntil = tick + kSetupBlindTicks;
            return setup;
        }
    case NavPhase::Sync:
        // Triangle back until the Select Peak screen (at most Event->Mode->
        // Peak). Already there: advance with no press.
        if (v.screen == 0)
        {
            n.phase = NavPhase::ToPeak;
            n.dir = 0;
            n.lastPos = -2;
            n.tries = 0;
            n.sub = 0;
            n.anchor = 0;
            return navStep(n, v, tick, peakLive, modeLive);
        }
        if (v.screen == 1 || v.screen == 2)
            return press(kPadTriangle);
        return abort();
    case NavPhase::ToPeak:
        if (v.screen == 1)
        {
            if (!peakLive && n.sub >= 2)
            {
                // Fallback accept landed: verify the outcome before driving
                // the Mode screen (else a wrong peak rides the wrong list).
                std::fprintf(stderr, "[tklist] tick=%llu nav ToPeak verify screen=%d peakOk=%d peak=%u goal=%u tries=%d sub=%d\n",
                             static_cast<unsigned long long>(tick), v.screen, v.peakOk ? 1 : 0,
                             v.peakOk ? v.peak : 0u, n.goal.peak, n.tries, n.sub);
                if (v.peakOk && v.peak == n.goal.peak)
                {
                    n.phase = NavPhase::ToMode;
                    n.dir = 0;
                    n.lastPos = -2;
                    n.tries = 0;
                    n.sub = 0;
                    n.anchor = 0; // Mode cursor starts at the top (Race)
                    n.settleUntil = tick + kSettleTicks;
                    return out;
                }
                // Mismatch: keep aiming across visits (P5: the cursor may
                // persist with a weird order (p4-gari2 [P1,P3,P2]) or reset
                // (gari5); fresh-aiming every visit oscillates under both).
                // anchor = last directed step (0 = none): flip only when
                // that step left us stagnant; aim fresh only when dir is
                // unset (first aim); otherwise KEEP dir (verify decides).
                {
                    const int acc = v.peakOk ? static_cast<int>(v.peak) : 0;
                    if (n.anchor != 0 && v.peakOk && acc == n.lastPos)
                        n.dir = -n.anchor;
                    else if (n.dir == 0)
                    {
                        const int d = (static_cast<int>(n.goal.peak) - acc + 3) % 3;
                        n.dir = (d == 2) ? -1 : 1;
                    }
                    n.lastPos = acc;
                    n.anchor = 0;
                }
                if (++n.tries >= kPeakTries)
                    return abort(); // never converging (or locked)
                // Post-back-out accepts (sub 6) step next (sub 7); first
                // accepts (sub 2) take the reset value first (sub 3).
                n.sub = (n.sub == 6) ? 7 : 3;
                return press(kPadTriangle);
            }
            // Accepted onto Select Mode (live cursor).
            n.phase = NavPhase::ToMode;
            n.dir = 0;
            n.lastPos = -2;
            n.tries = 0;
            n.sub = 0;
            n.anchor = 0;
            n.settleUntil = tick + kSettleTicks;
            return out;
        }
        if (v.screen != 0)
            return abort();
        if (peakLive)
        {
            if (!v.peakOk)
                return abort();
            if (v.peak == n.goal.peak)
            {
                n.tries = 0;
                n.sub = 0;
                return pressAccept(kPadCross);
            }
            // Adaptive: first step Down; keep the direction while the peak
            // moves, flip when it does not (3 peaks: bounded either way).
            if (n.lastPos != -2 && v.peak == static_cast<uint32_t>(n.lastPos) && n.dir)
                n.dir = -n.dir;
            else if (!n.dir)
                n.dir = 1;
            n.lastPos = static_cast<int>(v.peak);
            return press(n.dir > 0 ? kPadDown : kPadUp);
        }
        // Accept-only fallback: the pre-accept word is stale (m4), and the
        // re-entry cursor resets (P4), so the first visit steps from the
        // top anchor but every back-out accepts the reset value first
        // (sub 3 -> 6), then aims one directed step from ground truth
        // (sub 7 -> 1). Stagnation flips; the tries cap bounds it all.
        if (n.tries >= kPeakTries)
            return abort();
        if (n.sub == 3)
        {
            n.sub = 6;
            return pressAccept(kPadCross);
        }
        if (n.sub == 0)
        {
            if (n.anchor == static_cast<int>(n.goal.peak))
            {
                n.sub = 1;
            }
            else
            {
                // Shortest wrap step from the anchor (3 peaks), or the
                // forced edge step after stagnation.
                int step;
                if (n.dir != 0)
                    step = n.dir;
                else
                {
                    const int d = (static_cast<int>(n.goal.peak) - n.anchor + 3) % 3;
                    step = (d == 2) ? -1 : 1;
                }
                n.anchor = (n.anchor + step + 3) % 3;
                return press(step > 0 ? kPadDown : kPadUp);
            }
        }
        if (n.sub == 1)
        {
            n.sub = 2;
            return pressAccept(kPadCross);
        }
        if (n.sub == 2 || n.sub == 6)
        {
            // Still on Peak: the accept was eaten by a transition or is
            // still in flight. Retry tries-bounded, never the two-strike
            // give-up: a landed accept changes the screen, so a screen-0
            // retry is always safe (the Setup->Peak journey outlasts any
            // fixed blind; the word/entries/object signals all lead it).
            // Part 4: a fully static screen across retries is a refused
            // row (locked), not an eaten accept: fail fast with the
            // overlay left open instead of grinding to the tries cap.
            if (stillRefused(n, v))
                return abort(); // refused (locked?): toast names it
            if (++n.tries >= kPeakTries)
                return abort(); // never converging (or locked)
            n.sub = 2;
            return pressAccept(kPadCross);
        }
        if (n.sub == 7)
        {
            // One directed step from the reset position, then accept.
            const int step = (n.dir != 0) ? n.dir : 1;
            n.anchor = step;
            n.sub = 1;
            return press(step > 0 ? kPadDown : kPadUp);
        }
        return abort();
    case NavPhase::ToMode:
        if (v.screen == 2)
        {
            if (!modeLive && n.sub >= 2)
            {
                // Fallback accept landed: verify the mode type before
                // driving the Event screen.
                std::fprintf(stderr, "[tklist] tick=%llu nav ToMode verify screen=%d typeOk=%d type=%u goal=%u tries=%d sub=%d\n",
                             static_cast<unsigned long long>(tick), v.screen, v.typeOk ? 1 : 0,
                             v.typeOk ? v.type : 0u, n.goal.type, n.tries, n.sub);
                if (v.typeOk && v.type == n.goal.type)
                {
                    n.phase = NavPhase::ToEvent;
                    n.dir = 0;
                    n.lastPos = -2;
                    n.tries = 0;
                    n.sub = 0;
                    n.settleUntil = tick + kSettleTicks;
                    return out;
                }
                // Mismatch: same P5 keep-shape as the peak.
                {
                    const int acc = v.typeOk ? static_cast<int>(v.type) : 0;
                    if (n.anchor != 0 && v.typeOk && acc == n.lastPos)
                        n.dir = -n.anchor;
                    else if (n.dir == 0)
                        n.dir = (static_cast<int>(n.goal.type) > acc) ? 1 : -1;
                    n.lastPos = acc;
                    n.anchor = 0;
                }
                if (++n.tries >= kModeTries)
                    return abort(); // never converging
                n.sub = (n.sub == 6) ? 7 : 3;
                return press(kPadTriangle);
            }
            n.phase = NavPhase::ToEvent;
            n.dir = 0;
            n.lastPos = -2;
            n.tries = 0;
            n.sub = 0;
            n.settleUntil = tick + kSettleTicks;
            return out;
        }
        if (v.screen == 0)
        {
            // Kicked back to Peak (lock?): re-sync. Position unknown, so
            // take the reset value first (sub 3), never the top anchor.
            n.phase = NavPhase::ToPeak;
            n.dir = 0;
            n.lastPos = -2;
            n.tries = 0;
            n.sub = 3;
            n.anchor = 0;
            return navStep(n, v, tick, peakLive, modeLive);
        }
        if (v.screen != 1)
            return abort();
        if (!v.typeOk)
            return abort();
        if (v.type == n.goal.type)
        {
            n.tries = 0;
            n.sub = 0;
            return pressAccept(kPadCross);
        }
        if (modeLive)
        {
            if (n.lastPos != -2 && v.type == static_cast<uint32_t>(n.lastPos) && n.dir)
                n.dir = -n.dir;
            else if (!n.dir)
                n.dir = 1;
            n.lastPos = static_cast<int>(v.type);
            return press(n.dir > 0 ? kPadDown : kPadUp);
        }
        // Accept-only fallback: same P4 shape as the peak (first visit
        // steps from the top anchor; back-outs take the reset first).
        if (n.tries >= kModeTries)
            return abort();
        if (n.sub == 3)
        {
            n.sub = 6;
            return pressAccept(kPadCross);
        }
        if (n.sub == 0)
        {
            if (n.anchor == static_cast<int>(n.goal.type))
            {
                n.sub = 1;
            }
            else
            {
                // Two rows: one step either way lands it (forced edge
                // step after stagnation, else away from the anchor).
                int step;
                if (n.dir != 0)
                    step = n.dir;
                else
                    step = (n.anchor == 0) ? 1 : -1;
                n.anchor = (n.anchor + step + 3) % 3;
                return press(step > 0 ? kPadDown : kPadUp);
            }
        }
        if (n.sub == 1)
        {
            n.sub = 2;
            return pressAccept(kPadCross);
        }
        if (n.sub == 2 || n.sub == 6)
        {
            // Still on Mode: same ate-by-transition patience as the peak.
            // Part 4: static-screen fail-fast, same as ToPeak.
            if (stillRefused(n, v))
                return abort(); // refused (locked?): toast names it
            if (++n.tries >= kModeTries)
                return abort(); // never converging
            n.sub = 2;
            return pressAccept(kPadCross);
        }
        if (n.sub == 7)
        {
            const int step = (n.dir != 0) ? n.dir : 1;
            n.anchor = step;
            n.sub = 1;
            return press(step > 0 ? kPadDown : kPadUp);
        }
        return abort();
    case NavPhase::ToEvent:
        if (v.screen != 2)
        {
            // Kicked back (lock?): re-sync from the top.
            n.phase = NavPhase::Sync;
            n.dir = 0;
            return navStep(n, v, tick, peakLive, modeLive);
        }
        if (v.row < 0 || v.event < 0)
            return abort();
        if (v.row == n.goal.row && v.event == n.event)
        {
            n.phase = NavPhase::Accept;
            n.goneSince = 0u;
            NavOut accept = pressAccept(kPadCross);
            // Map screens live, overlay open, nothing loading: the caller
            // applies (or restores) the world mapping before the mask.
            if (!accept.aborted)
            {
                if (n.map.event >= 0)
                    accept.mapApply = true;
                else
                    accept.mapRestore = true;
            }
            return accept;
        }
        {
            // Monotonic stepping toward the goal row (no wrap assumption);
            // flip once when a press does not move the cursor.
            int want = (v.row < n.goal.row) ? 1 : -1;
            if (n.lastPos != -2 && v.row == n.lastPos)
            {
                if (n.dir && ((n.dir > 0) != (want > 0)))
                    return abort(); // flipped already and still stuck
                want = -want;
                n.dir = want;
            }
            else
                n.dir = want;
            n.lastPos = v.row;
            return press(want > 0 ? kPadDown : kPadUp);
        }
    case NavPhase::Accept:
        // The accept press is in flight (or done); leaving the Map screens
        // completes it (handled above). Still on the Event screen: press
        // again, whether the row reads matched (missed) or not (landed but
        // the widget tore down: the retry advances My Rules, which the flow
        // needs anyway). The press budget bounds this either way. But a
        // landed accept kills the Event widget (m7: row stays -1 while the
        // guest loads and races, the sub word never clears): a dead widget
        // sustained past kGoneDoneTicks IS the handoff.
        if (v.screen == 2)
        {
            if (v.row < 0)
            {
                if (n.goneSince == 0u)
                    n.goneSince = tick;
                if (tick - n.goneSince >= kGoneDoneTicks)
                {
                    n.handedOff = true;
                    n.phase = NavPhase::Done;
                    out.done = true;
                    return out;
                }
            }
            else
                n.goneSince = 0u;
            return pressAccept(kPadCross);
        }
        n.phase = NavPhase::ToEvent; // knocked elsewhere; re-drive
        return navStep(n, v, tick, peakLive, modeLive);
    case NavPhase::BackOut:
        // One Triangle per evaluation until the Map screens are left — or
        // the press count is spent (the done-slide above never fires
        // off-list, so finish the walk by count and let the overlay close).
        if (n.presses >= kBackOutPresses)
        {
            n.handedOff = false;
            n.phase = NavPhase::Done;
            out.done = true;
            return out;
        }
        return press(kPadTriangle);
    case NavPhase::ToTricky:
        // Blind Tricky-accept walk (main menu, no view): clamp top with 5
        // ups, 3 downs to the Tricky Courses row, Cross up to 3 times. The
        // main-menu accept hook serves the parked tap ask and disarms the
        // walk; presses only continue while the ask is still parked, so a
        // served or timed-out ask ends the walk at once.
        if (n.presses >= kTrickyAcceptPresses)
        {
            n.handedOff = false;
            n.phase = NavPhase::Done;
            out.done = true;
            return out;
        }
        {
            ListState parked;
            bool parkedBackout = false, parkedWalked = false;
            if (!directPeek(parked, parkedBackout, parkedWalked))
            {
                n.handedOff = false;
                n.phase = NavPhase::Done;
                out.done = true;
                return out;
            }
        }
        if (n.presses >= kTrickyAcceptPresses - 3)
            return pressAccept(kPadCross);
        if (n.presses >= kTrickyAcceptPresses - 6)
            return press(kPadDown);
        return press(kPadUp);
    default:
        return abort();
    }
}

// ---- Overlay geometry (SET2 precedent: pure, render loop draws) ----------
//
// Full-screen list: title bar, 10 course rows (Tricky order) on the left,
// the preview/event pane on the right, the help bar with the X/Triangle
// pad-key legend at the foot. Controller-only (Brad 10-21, tk-pad-ui-1):
// the touch Select/Back buttons and their hit tests are gone — touch
// players drive the list with the virtual pad, which the render loop draws
// on top. No toggle (removed 10-19).
struct ListGeom
{
    float titleX = 0, titleY = 0, titleW = 0, titleH = 0;
    float rowX = 0, rowW = 0, rowH = 0;
    float rowY[10] = {};
    float paneX = 0, paneY = 0, paneW = 0, paneH = 0;
};

inline ListGeom listLayout(float w, float h)
{
    ListGeom g;
    // Title bar across the top (measured: the stock header band).
    g.titleX = w * 0.06f;
    g.titleW = w * 0.88f;
    g.titleH = h * 0.09f;
    g.titleY = h * 0.04f;
    // Course rows: left column, 10 rows with the stock ~22 px pitch at
    // 640x480 (Part 1 §4).
    g.rowX = w * 0.08f;
    g.rowW = w * 0.48f;
    g.rowH = (h * 0.72f) / 10.0f;
    const float rowsTop = h * 0.16f;
    for (int i = 0; i < 10; ++i)
        g.rowY[i] = rowsTop + i * g.rowH;
    // Preview/event pane: right column.
    g.paneX = w * 0.60f;
    g.paneW = w * 0.32f;
    g.paneY = rowsTop;
    g.paneH = g.rowH * 6.0f;
    return g;
}

// ---- Launch-time world mapping (Part 3) -----------------------------------
//
// Design: tk-modes-1 spike 90effd64 (TK8 §3a mechanism). The navigator
// drives a slopestyle donor event's (peak, type, row); here, at the
// ToEvent->Accept edge, the game thread points the donor's world archive
// (+68, 16 B) and name (+4, 32 B) at the course world, and writes the stock
// fields back on native accepts. Event table base/stride mirror
// ps2_ssx3_course_manifest.h (kEventBase/kEventStride).
//
// Safety: runs only on the game thread at the Accept edge (Map screens
// live, overlay open: no live event, nothing loading) or not at all;
// every address is bounds-checked against the RAM size before any write,
// so validation is all-or-nothing. Snapshot-on-first-apply reads the
// donor's stock fields from live RAM (nothing could have written before),
// keeping every combo pure within a session. Game-thread only (driveOnRead
// holds the drive mutex); knob off: the caller never reaches this.
inline constexpr uint32_t kMapEventBase = 0x43D950u;
inline constexpr uint32_t kMapEventStride = 100u;
inline constexpr uint32_t kMapArchiveOff = 68u;
inline constexpr uint32_t kMapArchiveLen = 16u;
inline constexpr uint32_t kMapNameOff = 4u;
inline constexpr uint32_t kMapNameLen = 32u;
inline constexpr int kMapDonorMax = 22;
static_assert(kDirectDonorMax == kMapDonorMax, "direct donor bound follows the map table");

struct MapState
{
    // Part 4: stock snapshots are per donor event (several donors are
    // mapped across launches in one boot now that goals share P1 cells).
    // snapHave also gates all writes: with no mapped donor the restore
    // paths return before touching RAM (knob-off and picker-unused boots
    // stay write-free).
    bool snapHave[kMapDonorMax + 1] = {};
    char snapArchive[kMapDonorMax + 1][kMapArchiveLen] = {};
    char snapName[kMapDonorMax + 1][kMapNameLen] = {};
    int activeEvent = -1; // donor currently mapped, or -1
};

inline MapState &mapState()
{
    static MapState s;
    return s;
}

inline const uint8_t *mapField(const uint8_t *ram, uint32_t size, int event, uint32_t off, uint32_t len)
{
    if (!ram || event < 0 || event > kMapDonorMax)
        return nullptr;
    const uint32_t addr = kMapEventBase + static_cast<uint32_t>(event) * kMapEventStride + off;
    if (addr > size || len > size - addr)
        return nullptr;
    return ram + addr;
}

inline bool mapPrintable(const char *s, uint32_t maxLen)
{
    if (!s)
        return false;
    for (uint32_t i = 0; i < maxLen; ++i)
    {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == 0)
            return true;
        if (c < 32 || c > 126)
            return false;
    }
    return false; // unterminated within the field
}

inline void mapRead(char *dst, uint32_t len, const uint8_t *src)
{
    std::memcpy(dst, src, len);
    dst[len - 1] = 0;
}

inline bool mapWrite(uint8_t *ram, uint32_t size, int event, uint32_t off, uint32_t len, const char *v)
{
    const uint8_t *dst = mapField(ram, size, event, off, len);
    if (!dst || !v || std::strlen(v) == 0 || std::strlen(v) >= len || !mapPrintable(v, len))
        return false;
    std::memset(ram + (dst - ram), 0, len);
    std::memcpy(ram + (dst - ram), v, std::strlen(v));
    return true;
}

// Applies (spec.event, spec.archive, spec.name) after snapshotting the
// donor's stock fields once. True when both fields hold the new values.
inline bool applyCourseMap(uint8_t *ram, uint32_t size, const MapSpec &spec)
{
    if (spec.event < 0 || spec.event > kMapDonorMax || !spec.archive || !spec.name)
        return false;
    if (std::strlen(spec.archive) == 0 || std::strlen(spec.archive) >= kMapArchiveLen ||
        std::strlen(spec.name) == 0 || std::strlen(spec.name) >= kMapNameLen ||
        !mapPrintable(spec.archive, kMapArchiveLen) || !mapPrintable(spec.name, kMapNameLen))
        return false;
    const uint8_t *aSrc = mapField(ram, size, spec.event, kMapArchiveOff, kMapArchiveLen);
    const uint8_t *nSrc = mapField(ram, size, spec.event, kMapNameOff, kMapNameLen);
    if (!aSrc || !nSrc)
        return false;
    MapState &st = mapState();
    // Backstop only: boot seeding (seedMapBaselineFromModes) captures true
    // stock for every armed mode-row event. This fires solely for events
    // mapped without an armed manifest.
    if (!st.snapHave[spec.event])
    {
        mapRead(st.snapArchive[spec.event], kMapArchiveLen, aSrc);
        mapRead(st.snapName[spec.event], kMapNameLen, nSrc);
        st.snapHave[spec.event] = true;
        std::fprintf(stderr, "[tklist] map ev%d stock archive=\"%s\" name=\"%s\"\n", spec.event,
                     st.snapArchive[spec.event], st.snapName[spec.event]);
    }
    if (st.activeEvent == spec.event)
    {
        char curA[kMapArchiveLen];
        char curN[kMapNameLen];
        mapRead(curA, kMapArchiveLen, aSrc);
        mapRead(curN, kMapNameLen, nSrc);
        if (std::strcmp(curA, spec.archive) == 0 && std::strcmp(curN, spec.name) == 0)
            return true; // already mapped for this course
    }
    if (!mapWrite(ram, size, spec.event, kMapArchiveOff, kMapArchiveLen, spec.archive))
        return false;
    if (!mapWrite(ram, size, spec.event, kMapNameOff, kMapNameLen, spec.name))
        return false;
    st.activeEvent = spec.event;
    std::fprintf(stderr, "[tklist] map ev%d world -> archive=\"%s\" name=\"%s\"\n", spec.event, spec.archive,
                 spec.name);
    return true;
}

// Writes the snapshot back when a mapping is active. True when the donor
// holds stock values (or nothing was ever mapped).
inline bool restoreCourseMap(uint8_t *ram, uint32_t size)
{
    MapState &st = mapState();
    if (st.activeEvent < 0 || st.activeEvent > kMapDonorMax || !st.snapHave[st.activeEvent])
        return true;
    if (!mapWrite(ram, size, st.activeEvent, kMapArchiveOff, kMapArchiveLen, st.snapArchive[st.activeEvent]))
        return false;
    if (!mapWrite(ram, size, st.activeEvent, kMapNameOff, kMapNameLen, st.snapName[st.activeEvent]))
        return false;
    std::fprintf(stderr, "[tklist] map ev%d restored archive=\"%s\" name=\"%s\"\n", st.activeEvent,
                 st.snapArchive[st.activeEvent], st.snapName[st.activeEvent]);
    st.activeEvent = -1;
    return true;
}

// Part 4: restore every donor mapped this boot (main-menu rebuild: a stock
// event must never ride a record dirtied by an earlier picker launch).
// Skips records already holding their stock values, so picker-unused boots
// (no snapshots) and already-clean menus write nothing.
inline bool restoreAllCourseMaps(uint8_t *ram, uint32_t size)
{
    MapState &st = mapState();
    bool ok = true;
    for (int ev = 0; ev <= kMapDonorMax; ++ev)
    {
        if (!st.snapHave[ev])
            continue;
        const uint8_t *aSrc = mapField(ram, size, ev, kMapArchiveOff, kMapArchiveLen);
        const uint8_t *nSrc = mapField(ram, size, ev, kMapNameOff, kMapNameLen);
        if (!aSrc || !nSrc)
        {
            ok = false;
            continue;
        }
        char curA[kMapArchiveLen];
        char curN[kMapNameLen];
        mapRead(curA, kMapArchiveLen, aSrc);
        mapRead(curN, kMapNameLen, nSrc);
        if (std::strcmp(curA, st.snapArchive[ev]) == 0 && std::strcmp(curN, st.snapName[ev]) == 0)
            continue;
        if (!mapWrite(ram, size, ev, kMapArchiveOff, kMapArchiveLen, st.snapArchive[ev]) ||
            !mapWrite(ram, size, ev, kMapNameOff, kMapNameLen, st.snapName[ev]))
        {
            ok = false;
            continue;
        }
        std::fprintf(stderr, "[tklist] map ev%d restored archive=\"%s\" name=\"%s\"\n", ev, st.snapArchive[ev],
                     st.snapName[ev]);
    }
    st.activeEvent = -1;
    return ok;
}

// preserve-fix-1 P11/P12 (Table P D1): seed the per-boot stock baseline for
// every donor event any armed course mode touches, from pristine boot RAM —
// before any mode write. Called once at loadELF after applyFromEnv (which no
// longer writes at boot). After this, restore paths always return TRUE stock,
// never an in-mode snapshot: the first-map lazy snapshot in applyCourseMap
// stays only as a backstop for events mapped without an armed manifest.
inline void seedMapBaselineFromModes(const uint8_t *ram, uint32_t size)
{
    MapState &st = mapState();
    if (!ram)
        return;
    const ps2_ssx3_course::Modes &ms = ps2_ssx3_course::courseModes();
    if (!ms.armed)
        return;
    for (const ps2_ssx3_course::ModeField &f : ms.fields)
    {
        const int ev = f.event;
        if (ev < 0 || ev > kMapDonorMax || st.snapHave[ev])
            continue;
        const uint8_t *aSrc = mapField(ram, size, ev, kMapArchiveOff, kMapArchiveLen);
        const uint8_t *nSrc = mapField(ram, size, ev, kMapNameOff, kMapNameLen);
        if (!aSrc || !nSrc)
            continue;
        mapRead(st.snapArchive[ev], kMapArchiveLen, aSrc);
        mapRead(st.snapName[ev], kMapNameLen, nSrc);
        st.snapHave[ev] = true;
        std::fprintf(stderr, "[tklist] baseline ev%d stock archive=\"%s\" name=\"%s\"\n", ev, st.snapArchive[ev],
                     st.snapName[ev]);
    }
}

// ---- Cross-thread drive (game-thread merge, ONL10 precedent) -------------
//
// The navigator runs at the pad-merge point on the game thread (one call per
// guest pad read, keyed by the guest vsync tick: no wall clock, so replays
// stay det). It ORs its press masks onto the published pad word (active-high
// here; the caller clears the active-low bits). The render thread owns the
// overlay: it arms goals here and reads progress back. One mutex per pad
// read (the qpNav precedent); knob off: the caller never reaches this.
struct Drive
{
    std::mutex mu;
    bool armed = false; // a goal (or back-out) awaits / is driving
    bool backOut = false;
    CourseOption goal = {};
    int event = -1;
    NavState nav; // game-thread-owned while armed (render reads progress)
    int stallPresses = -1; // watchdog cursor: last press count seen + tick
    uint64_t stallSince = 0;
};

inline Drive &drive()
{
    static Drive d;
    return d;
}

// Render thread: arm a ride to `goal` (list X), or a back-out (list Triangle).
inline void driveArm(const CourseOption &goal, int event, uint64_t tick)
{
    Drive &d = drive();
    std::lock_guard<std::mutex> lock(d.mu);
    d.armed = true;
    d.backOut = false;
    d.goal = goal;
    d.event = event;
    d.stallPresses = -1;
    d.stallSince = 0u;
    navStart(d.nav, goal, event, tick);
    // The overlay launches from Setup Character, not Select Peak.
    d.nav.phase = NavPhase::SetupConfirm;
}

inline void driveArmBackOut(uint64_t tick)
{
    Drive &d = drive();
    std::lock_guard<std::mutex> lock(d.mu);
    d.armed = true;
    d.backOut = true;
    d.event = -1;
    d.stallPresses = -1;
    d.stallSince = 0u;
    navBackOut(d.nav, tick);
}

// Game thread: BackOut reached the fresh main menu. Walk down to the
// Tricky Courses row and accept it; the accept hook serves the parked tap.
inline void driveArmToTricky(uint64_t tick)
{
    Drive &d = drive();
    std::lock_guard<std::mutex> lock(d.mu);
    d.armed = true;
    d.backOut = false;
    d.event = -1;
    d.stallPresses = -1;
    d.stallSince = 0u;
    navToTricky(d.nav, tick);
}

// Render thread: disarm (overlay closed, user took over, or Done consumed).
inline void driveDisarm()
{
    Drive &d = drive();
    std::lock_guard<std::mutex> lock(d.mu);
    d.armed = false;
    d.backOut = false;
    d.stallPresses = -1;
    d.stallSince = 0u;
    navReset(d.nav);
}

inline constexpr int kBackOutStallTicks = 300; // a BackOut walk finishes
    // in ~150 ticks; a drive with no new press this long is stalled (the
    // game-thread evals went quiet, e.g. the menu teardown cleared the
    // merge flag), so complete it instead of holding the overlay open.

// Render thread, every frame: complete a stalled BackOut drive. Without
// this the overlay stays open (its close evaluation suspends while
// driving) and the pad never returns, even though the guest left long
// ago. Progress (a new press) restarts the window, so healthy walks never
// trip it; the existing done path closes the overlay on the next read.
inline void driveWatchdog(uint64_t tick)
{
    Drive &d = drive();
    std::lock_guard<std::mutex> lock(d.mu);
    if (!d.armed || !d.backOut || d.nav.phase != NavPhase::BackOut)
        return;
    if (d.nav.presses != d.stallPresses)
    {
        d.stallPresses = d.nav.presses;
        d.stallSince = tick;
        return;
    }
    if (tick - d.stallSince >= static_cast<uint64_t>(kBackOutStallTicks))
    {
        d.nav.handedOff = false;
        d.nav.phase = NavPhase::Done;
    }
}

struct DriveProgress
{
    bool driving = false;
    NavPhase phase = NavPhase::Idle;
    bool done = false;
    bool handedOff = false;
    bool aborted = false;
    bool refused = false; // Part 4: streak abort (locked row?) vs tries cap
    int presses = 0;
};

inline DriveProgress driveProgress()
{
    Drive &d = drive();
    std::lock_guard<std::mutex> lock(d.mu);
    DriveProgress p;
    p.driving = d.armed && d.nav.phase != NavPhase::Done && d.nav.phase != NavPhase::Aborted;
    p.phase = d.nav.phase;
    p.done = d.nav.phase == NavPhase::Done;
    p.handedOff = d.nav.handedOff;
    p.aborted = d.nav.phase == NavPhase::Aborted;
    p.refused = d.nav.refused;
    p.presses = d.nav.presses;
    return p;
}

// Game thread (pad-merge point): returns the active-high press mask to OR
// into the pad word, or 0. `tricky` = Tricky mode on (the menu hook's
// mirror); `controller` = the Map controller (0 = unknown).
inline uint16_t driveOnRead(uint8_t *ram, uint32_t size, uint32_t controller, uint64_t tick, bool tricky,
                            bool peakLive, bool modeLive, bool &done, bool &handedOff, bool &aborted)
{
    done = false;
    handedOff = false;
    aborted = false;
    Drive &d = drive();
    std::lock_guard<std::mutex> lock(d.mu);
    if (!d.armed)
        return 0u;
    if (!tricky)
    {
        d.armed = false;
        navReset(d.nav);
        aborted = true;
        return 0u;
    }
    const FeView v = readView(ram, size, controller);
    const NavOut o = navStep(d.nav, v, tick, peakLive, modeLive);
    // Part 3: world mapping at the Accept edge, before the mask publishes.
    // A refused write disarms (overlay stays open, user retries) instead of
    // riding the wrong world.
    if ((o.mapApply || o.mapRestore) && !o.done && !o.aborted)
    {
        const bool ok = o.mapApply ? applyCourseMap(ram, size, d.nav.map) : restoreCourseMap(ram, size);
        if (!ok)
        {
            std::fprintf(stderr, "[tklist] tick=%llu map %s refused; drive disarmed\n",
                         static_cast<unsigned long long>(tick), o.mapApply ? "apply" : "restore");
            d.armed = false;
            navReset(d.nav);
            aborted = true;
            return 0u;
        }
    }
    if (o.done)
    {
        d.armed = false;
        done = true;
        handedOff = d.nav.handedOff;
        return 0u;
    }
    if (o.aborted)
    {
        d.armed = false;
        aborted = true;
        return 0u;
    }
    return o.mask;
}

} // namespace ps2_ssx3_tricky_list
