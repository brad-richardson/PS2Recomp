#pragma once
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
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace ps2_ssx3_tricky_list
{

// The LIST knob (default off; also requires the MENU knob armed with Tricky
// mode on). Cached: knobs never change mid-run.
inline bool knob()
{
    static const bool on = [] {
        const char *v = std::getenv("PS2X_SSX3_TRICKY_LIST");
        return v && std::strcmp(v, "1") == 0;
    }();
    return on;
}

// ---- Course table (Tricky order; Part 1 §1 + receipts/ssx3-events-nav.txt)
//
// Each course names its SSX 3 event id and, per event-type option, the
// (peak, mode-type, list row) the navigator must accept. Dual-type courses
// (Elysium, Aloha, Megaplex) offer the race-list row ("Race", nav type 5)
// and the freestyle-list row ("Jam", nav type 6); the default is Race with a
// Left/Right toggle (Part 1 Q2). `tp` is the TP*.SSH preview code, or null
// when the overlay ships none (Megaplex: text card, Part 1 Q3).
struct CourseOption
{
    uint8_t peak; // 0..2 (P1..P3)
    uint8_t type; // 0 Race list, 1 Freestyle list
    uint8_t row;  // cursor row on the Select Event list
};

struct Course
{
    const char *name;
    const char *tp; // TP preview code, or nullptr
    int event;      // SSX 3 event id (both options share it)
    CourseOption opts[2];
    uint8_t nOpts;
    const char *raceLabel; // pane header for opts[0]
    const char *jamLabel;  // pane header for opts[1] (dual courses only)
};

inline constexpr Course kCourses[10] = {
    {"Garibaldi", "GARI", 0, {{0, 0, 0}, {0, 0, 0}}, 1, "Race", nullptr},
    {"Snowdream", "SNOW", 1, {{0, 0, 1}, {0, 0, 0}}, 1, "Race", nullptr},
    {"Elysium Alps", "ELYS", 16, {{2, 0, 1}, {2, 1, 3}}, 2, "Race", "Jam"},
    {"Mesablanca", "MESA", 2, {{1, 0, 0}, {0, 0, 0}}, 1, "Race", nullptr},
    {"Merqury City", "MERQ", 4, {{2, 0, 0}, {0, 0, 0}}, 1, "Race", nullptr},
    {"Aloha Ice Jam", "ALOHA", 14, {{0, 0, 2}, {0, 1, 3}}, 2, "Race", "Jam"},
    {"Pipedream", "PIPE", 11, {{0, 1, 2}, {0, 0, 0}}, 1, "Super Pipe", nullptr},
    {"Untracked", "UNTRACK", 7, {{2, 1, 0}, {0, 0, 0}}, 1, "Slopestyle", nullptr},
    {"Tokyo Megaplex", nullptr, 15, {{1, 0, 2}, {1, 1, 3}}, 2, "Race", "Jam"},
    {"Alaska", "ALASKA", 6, {{1, 1, 0}, {0, 0, 0}}, 1, "Slopestyle", nullptr},
};

inline constexpr uint32_t kCourseCount = 10u;

// ---- List navigation (pad edges and touch taps map to the same steps) ----
struct ListState
{
    uint8_t sel = 0;      // highlighted course 0..9 (kept across re-opens)
    uint8_t opt[10] = {}; // per-course option (0 = Race/first, 1 = Jam)
};

inline void listMove(ListState &s, bool up)
{
    s.sel = static_cast<uint8_t>((s.sel + (up ? kCourseCount - 1u : 1u)) % kCourseCount);
}

inline void listToggle(ListState &s)
{
    if (kCourses[s.sel].nOpts > 1)
        s.opt[s.sel] ^= 1u;
}

inline const CourseOption &listGoal(const ListState &s)
{
    const Course &c = kCourses[s.sel];
    return c.opts[c.nOpts > 1 ? s.opt[s.sel] : 0];
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
inline constexpr uint64_t kGoneDoneTicks = 60u; // a dead Event widget this long after accept = handed off
inline constexpr int kPressBudget = 48;

enum class NavPhase : uint8_t
{
    Idle = 0, // overlay browsing (no driving)
    Sync,     // Triangle back until the Select Peak screen
    ToPeak,   // step the peak, X to accept
    ToMode,   // step the mode type, X to accept
    ToEvent,  // step the event row, X to accept
    Accept,   // wait for the guest to leave the Map screens (loading)
    BackOut,  // user Triangle: back out until the Map screens are left
    Done,     // handed off (Accept) or backed out (BackOut)
    Aborted,  // budget/view failure; pad released, overlay stays open
};

struct NavState
{
    NavPhase phase = NavPhase::Idle;
    CourseOption goal = {};
    int event = -1;
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
    int sub = 0;      // accept-only sub-step (0 step, 1 accept, 2 verify, 3 back)
    uint64_t goneSince = 0; // Accept: first tick the Event widget read dead
    int anchor = 0;   // assumed Peak/Mode cursor (top on entry; the accepted
                      // word after each attempt). The pre-accept words are
                      // stale (m4), so the fallback never trusts them.
    bool handedOff = false; // Done via Accept (race loading), else backed out
};

struct NavOut
{
    uint16_t mask = 0;
    bool done = false;
    bool aborted = false;
};

inline const char *navPhaseName(NavPhase p)
{
    switch (p)
    {
    case NavPhase::Idle:
        return "Idle";
    case NavPhase::Sync:
        return "Sync";
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
    n.event = event;
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

inline void navReset(NavState &n) { n = NavState{}; }

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
    // (BackOut keeps backing out); the overlay re-opens from guest state.
    if (tick < n.lastTick && n.phase != NavPhase::Idle && n.phase != NavPhase::Done &&
        n.phase != NavPhase::Aborted)
    {
        if (n.phase != NavPhase::BackOut)
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
        return abort();
    }
    if (tick < n.settleUntil)
        return out;
    switch (n.phase)
    {
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
                // Mismatch: the accepted word is ground truth for where the
                // cursor was. Re-anchor there and step again. Stagnant (same
                // accepted twice: a no-wrap edge or a lock): force the step
                // away from the edge next time instead of the wrap-shortest.
                {
                    const int acc = v.peakOk ? static_cast<int>(v.peak) : 0;
                    if (v.peakOk && acc == n.lastPos)
                        n.dir = (acc == 0) ? 1 : -1;
                    else
                        n.dir = 0;
                    n.lastPos = acc;
                    n.anchor = acc;
                }
                if (++n.tries >= 8)
                    return abort(); // never converging (or locked)
                n.sub = 3;
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
                return press(kPadCross);
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
        // Accept-only fallback: the pre-accept word is stale (m4), so step
        // the anchor (top on entry, the accepted word after each attempt),
        // accept, and verify. Each attempt re-anchors on ground truth.
        if (n.tries >= 8)
            return abort();
        if (n.sub == 3)
            n.sub = 0; // backed out; step from the anchor
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
            return press(kPadCross);
        }
        if (n.sub == 2)
        {
            // Still on Peak: the accept did not take; retry once, then give up.
            n.sub = 4;
            return press(kPadCross);
        }
        return abort();
    case NavPhase::ToMode:
        if (v.screen == 2)
        {
            if (!modeLive && n.sub >= 2)
            {
                // Fallback accept landed: verify the mode type before
                // driving the Event screen.
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
                // Mismatch: re-anchor on the accepted word and step again,
                // forcing away from the edge on stagnation (same shape).
                {
                    const int acc = v.typeOk ? static_cast<int>(v.type) : 0;
                    if (v.typeOk && acc == n.lastPos)
                        n.dir = (acc == 0) ? 1 : -1;
                    else
                        n.dir = 0;
                    n.lastPos = acc;
                    n.anchor = acc;
                }
                if (++n.tries >= 6)
                    return abort(); // never converging
                n.sub = 3;
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
            // Kicked back to Peak (lock?): re-sync.
            n.phase = NavPhase::ToPeak;
            n.dir = 0;
            n.lastPos = -2;
            n.tries = 0;
            n.sub = 0;
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
            return press(kPadCross);
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
        // Accept-only fallback: same anchor shape as the peak (top on
        // entry = Race, the accepted word after each attempt).
        if (n.tries >= 6)
            return abort();
        if (n.sub == 3)
            n.sub = 0; // backed out; step from the anchor
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
            return press(kPadCross);
        }
        if (n.sub == 2)
        {
            n.sub = 4;
            return press(kPadCross);
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
            return press(kPadCross);
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
            return press(kPadCross);
        }
        n.phase = NavPhase::ToEvent; // knocked elsewhere; re-drive
        return navStep(n, v, tick, peakLive, modeLive);
    case NavPhase::BackOut:
        // One Triangle per evaluation until the Map screens are left.
        return press(kPadTriangle);
    default:
        return abort();
    }
}

// ---- Overlay geometry + hit test (SET2 precedent: pure, render loop draws)
//
// Full-screen list: title bar, 10 course rows (Tricky order) on the left,
// the preview/event pane on the right, the help bar with X/Select and
// Triangle/Previous legends at the foot. Hit values: 0..9 = row tap (same
// as navigating there; a tap on the highlighted row rides), 10 = the
// event-type toggle (dual courses), 11 = the Select legend (ride), 12 = the
// Previous legend (back), -1 = miss.
struct ListGeom
{
    float titleX = 0, titleY = 0, titleW = 0, titleH = 0;
    float rowX = 0, rowW = 0, rowH = 0;
    float rowY[10] = {};
    float paneX = 0, paneY = 0, paneW = 0, paneH = 0;
    float toggleX = 0, toggleY = 0, toggleW = 0, toggleH = 0;
    float selectX = 0, selectY = 0, selectW = 0, selectH = 0;
    float backX = 0, backY = 0, backW = 0, backH = 0;
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
    // Toggle sits on the pane header (dual courses); legends on the help bar.
    g.toggleX = g.paneX + g.paneW * 0.62f;
    g.toggleW = g.paneW * 0.34f;
    g.toggleY = g.paneY + 2.0f;
    g.toggleH = g.rowH * 0.9f;
    g.selectX = w * 0.60f;
    g.selectW = w * 0.18f;
    g.backX = w * 0.80f;
    g.backW = w * 0.14f;
    g.selectY = g.backY = h * 0.905f;
    g.selectH = g.backH = h * 0.06f;
    return g;
}

inline int listHit(const ListGeom &g, float x, float y)
{
    if (x >= g.selectX && x < g.selectX + g.selectW && y >= g.selectY && y < g.selectY + g.selectH)
        return 11;
    if (x >= g.backX && x < g.backX + g.backW && y >= g.backY && y < g.backY + g.backH)
        return 12;
    if (x >= g.toggleX && x < g.toggleX + g.toggleW && y >= g.toggleY && y < g.toggleY + g.toggleH)
        return 10;
    if (x < g.rowX || x >= g.rowX + g.rowW)
        return -1;
    for (int i = 0; i < 10; ++i)
    {
        if (y >= g.rowY[i] && y < g.rowY[i] + g.rowH)
            return i;
    }
    return -1;
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
    navStart(d.nav, goal, event, tick);
}

inline void driveArmBackOut(uint64_t tick)
{
    Drive &d = drive();
    std::lock_guard<std::mutex> lock(d.mu);
    d.armed = true;
    d.backOut = true;
    d.event = -1;
    navBackOut(d.nav, tick);
}

// Render thread: disarm (overlay closed, user took over, or Done consumed).
inline void driveDisarm()
{
    Drive &d = drive();
    std::lock_guard<std::mutex> lock(d.mu);
    d.armed = false;
    d.backOut = false;
    navReset(d.nav);
}

struct DriveProgress
{
    bool driving = false;
    NavPhase phase = NavPhase::Idle;
    bool done = false;
    bool handedOff = false;
    bool aborted = false;
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
    p.presses = d.nav.presses;
    return p;
}

// Game thread (pad-merge point): returns the active-high press mask to OR
// into the pad word, or 0. `tricky` = Tricky mode on (the menu hook's
// mirror); `controller` = the Map controller (0 = unknown).
inline uint16_t driveOnRead(const uint8_t *ram, uint32_t size, uint32_t controller, uint64_t tick, bool tricky,
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
