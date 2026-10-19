#pragma once

// SET2: in-game settings overlay (design B, Brad 10-12): Select+Start opens a
// minimal paused panel (Mode, Resolution, Perf overlay, On-device upscale,
// Restart). set2-polish-1 (Brad 10-16): the chord is the only opener — the
// gear dot is gone. On touch: hold SELECT + tap START on the overlay, or
// two-finger-tap with no overlay up.
// Pure logic here (unit-tested by ps2_settings_overlay_tests.cpp); the render
// loop in ps2_runtime.cpp owns pause/draw/restart, and the platform shells
// own the settings file layer + relaunch.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "ps2_public_tier.h" // resTierDefault runs the public device-tier rule

namespace ps2x::set2
{

// --- Chord: hold back Select so the chord never leaks it to the game ---
//
// Brad 10-12: "select does reset the character" (SET1's probe missed it). A
// Select press is held for kChordWindowMs: if a Start edge arrives inside the
// window it is the chord (open the panel, neither button reaches the game);
// otherwise the first button forwards (a lone Select keeps the rider reset
// working, ~200 ms later when held; a lone Start keeps pause working the
// same way; quick taps of either forward on release, undelayed).
// A first button released inside the window with no second forwards a short
// pulse so the guest still sees it. Either order arms the window
// (chord-sym-1, Brad 10-17: a physical two-thumb Select+Start lands
// Start-first about half the time, and Start-first could never complete
// the old Select-only hold, so the panel opened unreliably). The window is
// 200 ms, not 150: the 60 Hz poll quantizes each edge by up to a frame, so
// a true 140 ms two-thumb skew could already miss a 150 ms window.
constexpr int kChordWindowMs = 200;
constexpr int kSelectPulseFrames = 3;

struct ChordState
{
    bool selectWas = false;
    bool startWas = false;
    bool holding = false;    // first edge seen, window open
    bool heldStart = false;  // which button opened the window (else Select)
    int64_t holdSinceMs = 0; // window start (wall ms)
    bool consumed = false;   // chord fired; strip both until both released
    int pulseLeft = 0;       // early-release forward pulse (host frames)
    bool pulseStart = false; // the pulse is for Start (else Select)
};

struct ChordOut
{
    bool openEdge = false;    // chord fired this frame: open the panel
    bool stripSelect = false; // strip the Select bit from the game mask
    bool stripStart = false;  // strip the Start bit from the game mask
    bool addSelect = false;   // OR the Select bit into the game mask (pulse)
    bool addStart = false;    // OR the Start bit into the game mask (pulse)
};

inline ChordOut chordStep(ChordState &st, bool selectDown, bool startDown, int64_t nowMs)
{
    ChordOut out;
    if (st.consumed)
    {
        // Chord engaged (DS1 precedent): neither button reaches the game
        // until both are released.
        out.stripSelect = selectDown;
        out.stripStart = startDown;
        if (!selectDown && !startDown)
            st.consumed = false;
        st.holding = false;
        st.pulseLeft = 0;
        st.selectWas = selectDown;
        st.startWas = startDown;
        return out;
    }
    const bool selectEdge = selectDown && !st.selectWas;
    const bool startEdge = startDown && !st.startWas;
    if (!st.holding && (selectEdge || startEdge))
    {
        // Idle: either edge opens the hold (Select wins exact ties, the
        // old path). The other edge inside the window completes the chord.
        st.holding = true;
        st.heldStart = startEdge && !selectEdge;
        st.holdSinceMs = nowMs;
        st.pulseLeft = 0;
    }
    if (st.holding)
    {
        const bool heldDown = st.heldStart ? startDown : selectDown;
        const bool otherDown = st.heldStart ? selectDown : startDown;
        const bool otherWas = st.heldStart ? st.selectWas : st.startWas;
        if (!heldDown)
        {
            // First button released inside the window with no second:
            // forward a short pulse so the guest still sees the press.
            st.holding = false;
            st.pulseLeft = kSelectPulseFrames;
            st.pulseStart = st.heldStart;
        }
        else if (nowMs - st.holdSinceMs >= kChordWindowMs)
        {
            // Window expired first: a lone press, forwarded from here on
            // (a second-button edge on this frame is plain input, not the
            // chord).
            st.holding = false;
        }
        else if (otherDown && !otherWas)
        {
            st.holding = false;
            st.consumed = true;
            out.openEdge = true;
            out.stripSelect = true;
            out.stripStart = true;
        }
        else if (st.heldStart)
        {
            out.stripStart = true;
        }
        else
        {
            out.stripSelect = true;
        }
    }
    else if (st.pulseLeft > 0 && (st.pulseStart ? !startDown : !selectDown))
    {
        if (st.pulseStart)
            out.addStart = true;
        else
            out.addSelect = true;
        --st.pulseLeft;
    }
    st.selectWas = selectDown;
    st.startWas = startDown;
    return out;
}

// --- Panel navigation (pad edges; touch taps map to the same edges) ---
//
// Rows: Mode, Resolution, Perf, Upscale, On-screen controls, Menu button,
// Hold Start, Apply, Cancel. Nothing applies on the row: Left/Right and X
// only STAGE into NavState (settings-ui-1, Brad 10-19). The bottom Apply
// row confirms everything at once like the in-game SSX 3 settings: live
// rows apply without a restart, Mode/Resolution restart (label switches
// to "Apply & restart"). Cancel, Triangle, the chord, or a tap outside
// closes and discards staged changes.
// Row::Count drives the geometry, hit test and draw loops, so appending
// a row needs no layout change.
enum class Row : uint8_t
{
    Mode = 0,
    Res = 1,
    Perf = 2,
    Upscale = 3,
    Touch = 4,
    MenuBtn = 5,
    HoldStart = 6,
    Apply = 7,
    Cancel = 8,
    Count = 9
};
static constexpr int kRowCount = static_cast<int>(Row::Count);

// Short row labels (panel-polish-1, Brad 10-15: every label shows in full;
// the draw code sets label left, value right, so no dot leaders truncate).
// "On-screen controls" (settings-ui-1, Brad 10-19) is the longest at 18.
inline const char *rowLabel(Row r)
{
    switch (r)
    {
    case Row::Mode:
        return "Mode";
    case Row::Res:
        return "Resolution";
    case Row::Perf:
        return "Perf";
    case Row::Upscale:
        return "Upscale";
    case Row::Touch:
        return "On-screen controls";
    case Row::MenuBtn:
        return "Menu button";
    case Row::HoldStart:
        return "Hold Start";
    case Row::Apply:
        return "Apply";
    case Row::Cancel:
        return "Cancel";
    default:
        return "";
    }
}

// One-line help per row, shown at the bottom while the row is selected
// (settings-ui-1, Brad 10-19: every row gets one). All ASCII so no font
// substitution can turn a blurb into '?'.
inline const char *rowBlurb(Row r)
{
    switch (r)
    {
    case Row::Mode:
        return "Guest update rate (needs restart)";
    case Row::Res:
        return "Render height (needs restart)";
    case Row::Perf:
        return "FPS + frame-time overlay";
    case Row::Upscale:
        return "Device upscaler on/off";
    case Row::Touch:
        return "Auto hides them when a controller is connected";
    case Row::MenuBtn:
        return "Select + Start opens settings";
    case Row::HoldStart:
        return "Hold START 0.6s opens settings";
    case Row::Apply:
        return "Apply staged changes; closing discards them.";
    case Row::Cancel:
        return "Discard staged changes and close.";
    default:
        return "";
    }
}

// Nearest common render-height label for a real frame height in pixels
// ("480p" .. "2160p"; "" when h == 0, so the caller can fall back). Ties
// keep the smaller step. The input is measured pixels (the presented frame
// on GL/iOS, the queued AHB on Android-VK), never 448 x scale.
inline std::string resHeightLabel(unsigned h)
{
    if (h == 0)
        return "";
    static const unsigned kSteps[] = {480u, 720u, 960u, 1080u, 1440u, 2160u};
    unsigned best = kSteps[0];
    unsigned bestD = (h > best) ? h - best : best - h;
    for (size_t i = 1; i < sizeof(kSteps) / sizeof(kSteps[0]); ++i)
    {
        const unsigned c = kSteps[i];
        const unsigned d = (h > c) ? h - c : c - h;
        if (d < bestD)
        {
            bestD = d;
            best = c;
        }
    }
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%up", best);
    return std::string(buf);
}

enum class Mode : uint8_t
{
    M120 = 0,
    M60 = 1
};
// Resolution choices (settings-res-1, Brad 10-17): 1x, 2x, Screen, 3x, 4x,
// ordered by resulting height (Screen sits where the device height falls).
// Screen is the device's display resolution (GE1_UPSCALE=native), never the
// PS2's. 1x is never a default. The platform default is preselected and
// marked (Odin/Android = Screen, iOS = 2x); below-native stays bench-only
// (there is no below-native choice).
enum class Res : uint8_t
{
    R1x = 0,   // GE1_UPSCALE=1
    R2x = 1,   // GE1_UPSCALE=2
    Screen = 2, // GE1_UPSCALE=native (device display resolution)
    R3x = 3,   // GE1_UPSCALE=3
    R4x = 4    // GE1_UPSCALE=4
};
static constexpr int kResCount = 5;

// PS2 base height (lines) x scale: the fixed multiplier heights.
constexpr unsigned kResBaseH = 448u;

inline unsigned resScale(Res r)
{
    switch (r)
    {
    case Res::R1x:
        return 1u;
    case Res::R2x:
        return 2u;
    case Res::R3x:
        return 3u;
    case Res::R4x:
        return 4u;
    default:
        return 0u; // Screen: see resHeight
    }
}

// Resulting height in lines for a choice. Multipliers are fixed
// (448 x scale); Screen is the measured device height in pixels.
inline unsigned resHeight(Res r, unsigned nativeH)
{
    if (r == Res::Screen)
        return nativeH;
    return kResBaseH * resScale(r);
}

// Within ~5% of a multiplier's height: |a-b| <= 5% of b.
inline bool resNearHeight(unsigned a, unsigned b)
{
    if (b == 0)
        return false;
    const unsigned d = (a > b) ? a - b : b - a;
    return d * 20u <= b;
}

// Resolution choices ordered by resulting height, ascending. Screen sits
// where the device height falls; when it lands within ~5% of a
// multiplier both show, Screen after the multiplier.
inline std::array<Res, 5> resOrder(unsigned nativeH)
{
    const Res mults[4] = {Res::R1x, Res::R2x, Res::R3x, Res::R4x};
    std::array<Res, 5> out{};
    int n = 0;
    bool placed = false;
    for (int i = 0; i < 4; ++i)
    {
        const unsigned mh = resHeight(mults[i], nativeH);
        if (!placed && resNearHeight(nativeH, mh))
        {
            // Within ~5%: both show, Screen after the multiplier.
            out[static_cast<size_t>(n++)] = mults[i];
            out[static_cast<size_t>(n++)] = Res::Screen;
            placed = true;
            continue;
        }
        if (!placed && nativeH < mh)
        {
            out[static_cast<size_t>(n++)] = Res::Screen;
            placed = true;
        }
        out[static_cast<size_t>(n++)] = mults[i];
    }
    if (!placed)
        out[static_cast<size_t>(n++)] = Res::Screen;
    return out;
}

// Next choice in height order (wraps). Right/X on the Resolution row
// steps this; Left steps resPrev (settings-ui-1, Brad 10-19).
inline Res resNext(Res cur, unsigned nativeH)
{
    const auto order = resOrder(nativeH);
    for (size_t i = 0; i < order.size(); ++i)
    {
        if (order[i] == cur)
            return order[(i + 1) % order.size()];
    }
    return cur;
}

// Previous choice in height order (wraps). Left on the Resolution row
// (pad Left, left half of the row on touch) steps this.
inline Res resPrev(Res cur, unsigned nativeH)
{
    const auto order = resOrder(nativeH);
    for (size_t i = 0; i < order.size(); ++i)
    {
        if (order[i] == cur)
            return order[(i + order.size() - 1) % order.size()];
    }
    return cur;
}

// (resTierDefault/resDefaultRow are defined below resFromSetting: they map
// the tier rule's render string onto the rows.)

// Touch controls (android-touch-1): Auto shows the overlay only when no
// physical gamepad is present (Odin hides by default); On/Off force it.
// Persisted as PS2X_VIRTUAL_PAD (On="1", Off="0", Auto=key absent).
enum class Touch : uint8_t
{
    Auto = 0,
    On = 1,
    Off = 2
};

// android-touch-1: Touch row <-> PS2X_VIRTUAL_PAD. On="1", Off="0",
// Auto=absent (upsert drops empty). Unset or anything but "0"/"1" reads
// as Auto, so a hand-edited file can't brick the overlay.
inline Touch touchFromSetting(const std::string &val)
{
    if (val == "1")
        return Touch::On;
    if (val == "0")
        return Touch::Off;
    return Touch::Auto;
}

inline const char *touchToSetting(Touch t)
{
    switch (t)
    {
    case Touch::On:
        return "1";
    case Touch::Off:
        return "0";
    default:
        return ""; // Auto: no override line
    }
}

inline const char *touchName(Touch t)
{
    switch (t)
    {
    case Touch::On:
        return "on";
    case Touch::Off:
        return "off";
    default:
        return "auto";
    }
}

// On-screen-controls cycle, both directions with wrap (settings-ui-1,
// Brad 10-19): Right/X steps touchNext, Left steps touchPrev.
inline Touch touchNext(Touch t)
{
    switch (t)
    {
    case Touch::Auto:
        return Touch::On;
    case Touch::On:
        return Touch::Off;
    default:
        return Touch::Auto;
    }
}

inline Touch touchPrev(Touch t)
{
    switch (t)
    {
    case Touch::Auto:
        return Touch::Off;
    case Touch::Off:
        return Touch::On;
    default:
        return Touch::Auto;
    }
}

// Menu "..." button (settings-res-1 addendum, Brad 10-17): a small dim
// button at the top middle of the screen; tapping it opens the panel. Shown
// by default on every device (even with a physical controller); only this
// row hides it. Persisted as PS2X_MENU_BUTTON (Hidden="0", Shown="1":
// config-fix-1 item 10 persists explicit values so a lower-priority base
// "0" cannot reappear through a removed line).
enum class MenuBtn : uint8_t
{
    Shown = 0,
    Hidden = 1
};

inline MenuBtn menuBtnFromSetting(const std::string &val)
{
    if (val == "0")
        return MenuBtn::Hidden;
    return MenuBtn::Shown;
}

inline const char *menuBtnToSetting(MenuBtn b)
{
    switch (b)
    {
    case MenuBtn::Hidden:
        return "0";
    default:
        return "1"; // Shown: explicit, wins over a base-layer "0"
    }
}

inline const char *menuBtnName(MenuBtn b)
{
    switch (b)
    {
    case MenuBtn::Hidden:
        return "hidden";
    default:
        return "shown";
    }
}

// The button shows whenever the row says Shown. It does NOT follow the
// Touch row (correction, Brad 10-17): with a physical pad and Touch=Auto
// the pad hides but the button still shows.
inline bool menuButtonShown(MenuBtn b)
{
    return b == MenuBtn::Shown;
}

// shortcuts-1: Input marker row <-> PS2X_INPUT_MARKER. On="1", Off="0"
// (config-fix-1 item 10: explicit, so a lower-priority base "1" cannot
// reappear through a removed line).
// Only the literal "1" arms (same strict check as lim1Apply); unset or
// anything else reads as Off, so a hand-edited file can't arm it. The
// code default is off; the row shows the effective env at open (explicit
// env wins: the Android/iOS settings layers never overwrite an explicit
// PS2X_INPUT_MARKER).
inline bool markerFromSetting(const std::string &val)
{
    return val == "1";
}

inline const char *markerToSetting(bool b)
{
    return b ? "1" : "0"; // Off: explicit, wins over a base-layer "1"
}

inline const char *markerName(bool b)
{
    return b ? "on" : "off";
}

inline bool ondeviceUpscaleOn(const char *val)
{
    return !val || !*val || std::strcmp(val, "0") != 0;
}

// hold-start-1: Hold Start row <-> PS2X_HOLD_START. On=default (absent),
// Off="0". Anything but the literal "0" reads as On, so a hand-edited
// file can't strand the user without the hold opener (the chord and the
// "..." button keep working either way). Live like Touch/Menu button:
// no restart, persisted immediately on change.
inline bool holdStartFromSetting(const std::string &val)
{
    return val != "0";
}

inline const char *holdStartToSetting(bool b)
{
    return b ? "" : "0"; // On: no override line (default); Off: "0"
}

inline const char *holdStartName(bool b)
{
    return b ? "on" : "off";
}

// hold-start-1: single-Start hold opener. While a lone Start is held the
// guest must not see it (pause would open ~one tap late otherwise); on
// release before the threshold the guest gets a normal short Start pulse
// (at least one guest frame via the latch); at the threshold the panel
// opens and Start is never sent. Select present cancels to the chord
// (the two-thumb path owns any Select+Start). Pure step function,
// unit-tested alongside chordStep; the render loop merges its strip/add/
// open with the chord's (OR open/strip, Hold owns addStart when enabled
// so a tap pulses once, not twice). Test spans (PS2X_SET2_TEST_PAD)
// drive the same host union, so this path is automated (and covers the
// chord hook too).
constexpr int kHoldStartMs = 600;
constexpr int kHoldStartPulseFrames = 3;

struct HoldStartState
{
    bool holding = false;   // lone Start tracked, threshold not yet hit
    int64_t holdSinceMs = 0; // Start edge wall time
    bool fired = false;     // opened, suppress until Start releases
    int pulseLeft = 0;      // release pulse frames remaining
};

struct HoldStartOut
{
    bool openEdge = false;   // open the panel this frame
    bool stripStart = false; // strip Start from the guest mask
    bool addStart = false;   // OR Start into the guest mask (pulse)
};

inline HoldStartOut holdStartStep(HoldStartState &st, bool selectDown, bool startDown, int64_t nowMs,
                                  bool enabled)
{
    HoldStartOut out;
    if (!enabled)
    {
        st.holding = false;
        st.fired = false;
        st.pulseLeft = 0;
        return out;
    }
    if (st.fired)
    {
        // Opened: never send Start; wait for the release to re-arm.
        if (startDown)
            out.stripStart = true;
        else
        {
            st.fired = false;
            st.holding = false;
            st.pulseLeft = 0;
        }
        return out;
    }
    if (selectDown)
    {
        // Any Select: the chord owns Select+Start; drop the lone hold
        // (and any armed pulse) so a tap pulses exactly once via chord.
        st.holding = false;
        st.pulseLeft = 0;
        return out;
    }
    if (startDown)
    {
        if (!st.holding && st.pulseLeft == 0)
        {
            st.holding = true;
            st.holdSinceMs = nowMs;
        }
        if (st.holding)
        {
            if (nowMs - st.holdSinceMs >= kHoldStartMs)
            {
                st.holding = false;
                st.fired = true;
                out.openEdge = true;
                out.stripStart = true;
            }
            else
            {
                out.stripStart = true;
            }
        }
        else if (st.pulseLeft > 0)
        {
            // Start re-pressed mid-pulse: restart the hold, drop the pulse.
            st.pulseLeft = 0;
            st.holding = true;
            st.holdSinceMs = nowMs;
            out.stripStart = true;
        }
        return out;
    }
    // Start up.
    if (st.holding)
    {
        // Released before the threshold: forward a short Start pulse so
        // the guest still sees the tap (pause keeps working).
        st.holding = false;
        st.pulseLeft = kHoldStartPulseFrames;
        return out;
    }
    if (st.pulseLeft > 0)
    {
        out.addStart = true;
        --st.pulseLeft;
    }
    return out;
}
enum class Action : uint8_t
{
    None = 0,
    Changed = 1, // a value was staged (redraw; nothing applied yet)
    DoRestart = 2, // Apply with a restart-needing change staged
    Apply = 3, // Apply with live-only changes staged (no restart)
    Close = 4 // close (discards staged changes by construction)
};

struct NavState
{
    Row sel = Row::Mode;
    // Staged values (settings-ui-1, Brad 10-19): Left/Right and X only
    // write these. Nothing applies until the Apply row confirms; closing
    // (Cancel, Triangle, chord, tap outside) drops NavState, so staged
    // changes are discarded without a write.
    Mode mode = Mode::M120;
    Res res = Res::Screen;
    bool perf = false;
    bool upscale = false; // REM3 P11 Phase 2 on-device upscale
    Touch touch = Touch::Auto; // android-touch-1
    MenuBtn menubtn = MenuBtn::Shown; // settings-res-1
    bool holdStart = true; // hold-start-1: default on
    // Not staged (no row since settings-ui-1 dropped it): round-trips the
    // settings file so the PS2X_INPUT_MARKER env knob keeps working.
    bool marker = false; // shortcuts-1: dev option, default off
    // Running values, sampled at open (dirty detect + close discards).
    Mode runMode = Mode::M120;
    Res runRes = Res::Screen;
    bool runPerf = false;
    bool runUpscale = false;
    Touch runTouch = Touch::Auto;
    MenuBtn runMenubtn = MenuBtn::Shown;
    bool runHoldStart = true;
    bool runMarker = false;
    unsigned nativeH = 0; // measured device height px, sampled at open (order + Screen label)
};

inline bool modeDirty(const NavState &st)
{
    return st.mode != st.runMode;
}

inline bool resDirty(const NavState &st)
{
    return st.res != st.runRes;
}

inline bool perfDirty(const NavState &st)
{
    return st.perf != st.runPerf;
}

inline bool upscaleDirty(const NavState &st)
{
    return st.upscale != st.runUpscale;
}

inline bool touchDirty(const NavState &st)
{
    return st.touch != st.runTouch;
}

inline bool menubtnDirty(const NavState &st)
{
    return st.menubtn != st.runMenubtn;
}

inline bool holdStartDirty(const NavState &st)
{
    return st.holdStart != st.runHoldStart;
}

// Staged changes needing a restart (Mode/Resolution). LIVE-RES SLOT: when
// Resolution applies live, drop resDirty() here and push the staged value
// through m_gs.SetUpscale at apply time; the Apply label, dirty tagging
// and persistence below follow automatically.
inline bool restartDirty(const NavState &st)
{
    return modeDirty(st) || resDirty(st);
}

inline bool anyStaged(const NavState &st)
{
    return modeDirty(st) || resDirty(st) || perfDirty(st) || upscaleDirty(st) ||
           touchDirty(st) || menubtnDirty(st) || holdStartDirty(st);
}

inline int stagedCount(const NavState &st)
{
    return (modeDirty(st) ? 1 : 0) + (resDirty(st) ? 1 : 0) + (perfDirty(st) ? 1 : 0) +
           (upscaleDirty(st) ? 1 : 0) + (touchDirty(st) ? 1 : 0) + (menubtnDirty(st) ? 1 : 0) +
           (holdStartDirty(st) ? 1 : 0);
}

// Apply row label switches when a restart-needing change is staged
// (settings-ui-1, Brad 10-19: reads "Apply & restart" when needed).
inline const char *applyLabel(const NavState &st)
{
    return restartDirty(st) ? "Apply & restart" : "Apply";
}

inline std::string applyValue(const NavState &st)
{
    const int n = stagedCount(st);
    if (n == 0)
        return "no changes";
    return std::to_string(n) + (n == 1 ? " change" : " changes");
}

inline const char *cancelValue(const NavState &st)
{
    return anyStaged(st) ? "discard changes" : "close";
}

struct NavOut
{
    Action action = Action::None;
};

// Close-press overhang: buttons down at close stay stripped from the game
// mask until each releases (else the closing Triangle/chord would fire in
// the game on resume). New presses pass (they were never in the mask).
struct SwallowState
{
    uint16_t mask = 0;
};

inline uint16_t swallowStep(SwallowState &st, uint16_t unionMask)
{
    st.mask = static_cast<uint16_t>(st.mask & unionMask);
    return st.mask;
}

// One step of panel input. Up/Down move (wrapping over all rows);
// Left/Right and Cross only STAGE a value (settings-ui-1, Brad 10-19:
// never apply or restart on a row). Left steps back, Right/Cross steps
// forward (binary rows toggle either way). Apply confirms: DoRestart when
// a restart-needing change is staged, Apply for live-only stages, None
// when clean. Cancel/Cancel-tap, Triangle and the chord Close (the caller
// drops NavState, so staged changes are discarded without a write).
inline NavOut navStep(NavState &st, bool upEdge, bool downEdge, bool crossEdge, bool triangleEdge,
                      bool chordEdge, bool leftEdge = false, bool rightEdge = false)
{
    NavOut out;
    if (chordEdge || triangleEdge)
    {
        out.action = Action::Close;
        return out;
    }
    if (upEdge || downEdge)
    {
        const int n = static_cast<int>(Row::Count);
        int sel = static_cast<int>(st.sel);
        if (sel < 0 || sel >= n)
            sel = 0;
        sel = upEdge ? (sel - 1 + n) % n : (sel + 1) % n;
        st.sel = static_cast<Row>(sel);
        return out;
    }
    if (st.sel == Row::Apply)
    {
        // Left/Right on Apply do nothing; only the confirm applies.
        if (!crossEdge || !anyStaged(st))
            return out;
        out.action = restartDirty(st) ? Action::DoRestart : Action::Apply;
        return out;
    }
    if (st.sel == Row::Cancel)
    {
        // Left/Right on Cancel do nothing; the confirm discards + closes.
        if (crossEdge)
            out.action = Action::Close;
        return out;
    }
    if (!crossEdge && !leftEdge && !rightEdge)
        return out;
    switch (st.sel)
    {
    case Row::Mode:
        st.mode = (st.mode == Mode::M120) ? Mode::M60 : Mode::M120;
        out.action = Action::Changed;
        break;
    case Row::Res:
        st.res = leftEdge ? resPrev(st.res, st.nativeH) : resNext(st.res, st.nativeH);
        out.action = Action::Changed;
        break;
    case Row::Perf:
        st.perf = !st.perf;
        out.action = Action::Changed;
        break;
    case Row::Upscale:
        st.upscale = !st.upscale;
        out.action = Action::Changed;
        break;
    case Row::Touch:
        st.touch = leftEdge ? touchPrev(st.touch) : touchNext(st.touch);
        out.action = Action::Changed;
        break;
    case Row::MenuBtn:
        st.menubtn = (st.menubtn == MenuBtn::Shown) ? MenuBtn::Hidden : MenuBtn::Shown;
        out.action = Action::Changed;
        break;
    case Row::HoldStart:
        st.holdStart = !st.holdStart;
        out.action = Action::Changed;
        break;
    default:
        break;
    }
    return out;
}

// --- Panel geometry + hit test (QSR1 precedent: pure, render loop draws) ---
//
// Hit values: 0..kRowCount-1 = row tap (same as X on that row),
// -1 = miss (open panel: tap outside closes; closed panel: no tap target —
// the Select+Start chord, or a two-finger tap with no touch overlay, opens).
struct PanelGeom
{
    float px = 0, py = 0, pw = 0, ph = 0; // panel rect
    float rowX = 0, rowW = 0;
    float rowY[kRowCount] = {};
    float rowH = 0;
};

inline PanelGeom panelLayout(float w, float h)
{
    PanelGeom g;
    g.pw = w * 0.62f;
    g.ph = h * 0.72f;
    g.px = (w - g.pw) * 0.5f;
    g.py = (h - g.ph) * 0.5f;
    g.rowX = g.px + 22.0f;
    g.rowW = g.pw - 44.0f;
    // Title block, then kRowCount even rows (the draw code uses these rects).
    const float titleH = h / 26.0f + 22.0f;
    g.rowH = (g.ph - titleH - 34.0f) / static_cast<float>(kRowCount);
    for (int i = 0; i < kRowCount; ++i)
        g.rowY[i] = g.py + titleH + i * g.rowH;
    return g;
}

inline int panelHit(const PanelGeom &g, bool open, float x, float y)
{
    // set2-polish-1 (Brad 10-16): no gear dot — a closed panel has no row
    // tap target (the "..." menu button is hit-tested separately), so any
    // closed-panel row tap misses.
    if (!open)
        return -1;
    if (x < g.rowX || x > g.rowX + g.rowW)
        return -1;
    for (int i = 0; i < kRowCount; ++i)
    {
        if (y >= g.rowY[i] && y < g.rowY[i] + g.rowH)
            return i;
    }
    return -1;
}

// --- settings.env: the user settings layer, read last at boot ---
//
// Android: <files dir>/settings.env. iOS: <Documents>/settings.env. The Mac
// has no settings file (Mode/Resolution apply through the relaunch env).
// Allowlisted to the keys the panel owns; anything else is ignored.
inline bool settingsKeyAllowed(const char *key)
{
    if (!key)
        return false;
    const std::string k(key);
    return k == "GE1_UPSCALE" || k == "PS2X_SETTINGS_PERF" ||
           k == "PS2X_VIRTUAL_PAD" || // android-touch-1: Touch row persists under its env name
           k == "PS2X_MENU_BUTTON" || // settings-res-1: Menu button row persists under its env name
           k == "PS2X_INPUT_MARKER" || // shortcuts-1: Input marker row persists under its env name
           k == "PS2X_HOLD_START" || // hold-start-1: Hold Start row persists under its env name
           k == "GE1_ONDEVICE_UPSCALE"; // REM3 P11 Phase 2: the live toggle persists under its env name
}

using SettingsPairs = std::vector<std::pair<std::string, std::string>>;

// Parse KEY=VALUE lines ('#' comments, surrounding whitespace trimmed).
// Same shape as ps2_env_file.h's parse (kept local so this header stays
// dependency-free for the host suite).
inline SettingsPairs parseSettingsEnv(const std::string &content)
{
    SettingsPairs out;
    size_t pos = 0;
    const auto trim = [](std::string s) {
        size_t a = 0;
        while (a < s.size() && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r'))
            ++a;
        size_t b = s.size();
        while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r'))
            --b;
        return s.substr(a, b - a);
    };
    while (pos <= content.size())
    {
        size_t end = content.find('\n', pos);
        if (end == std::string::npos)
            end = content.size();
        std::string line = trim(content.substr(pos, end - pos));
        if (!line.empty() && line[0] != '#')
        {
            const size_t eq = line.find('=');
            if (eq != std::string::npos)
            {
                std::string key = trim(line.substr(0, eq));
                std::string val = trim(line.substr(eq + 1));
                if (!key.empty() && settingsKeyAllowed(key.c_str()))
                    out.emplace_back(key, val);
            }
        }
        if (end == content.size())
            break;
        pos = end + 1;
    }
    return out;
}

inline std::string formatSettingsEnv(const SettingsPairs &pairs)
{
    std::string s = "# set2: written by the in-game settings panel (Mode/Resolution/Perf/Upscale/On-screen controls/Menu button/Hold Start).\n"
                    "# Delete lines (or this file) to return to the platform defaults.\n";
    for (const auto &kv : pairs)
        s += kv.first + "=" + kv.second + "\n";
    return s;
}

// What Apply persists: every STAGED row (settings-ui-1, Brad 10-19). The
// single persistence point for both the live apply and the restart apply
// (the restart path writes this file, then relaunches). Pure so the write
// path is unit-tested (Brad 10-17 iPhone bug: a stale choice must not
// survive the restart — the file must carry exactly what the rows show).
inline SettingsPairs settingsForApply(const SettingsPairs &pairs, const NavState &st);

// Upsert one key (empty val removes it). Keeps file order, appends new keys.
inline SettingsPairs upsertSettings(SettingsPairs cur, const std::string &key, const std::string &val)
{
    SettingsPairs out;
    bool done = false;
    for (const auto &kv : cur)
    {
        if (kv.first == key)
        {
            if (!val.empty() && !done)
            {
                out.emplace_back(key, val);
                done = true;
            }
            // else: drop (remove, or a duplicate)
        }
        else
            out.push_back(kv);
    }
    if (!val.empty() && !done)
        out.emplace_back(key, val);
    return out;
}

// Resolution row <-> GE1_UPSCALE. Every choice writes an explicit line
// (there is no default-clearing choice any more); a stored legacy value
// ("2"/"native"/unset) loads via resFromSetting, unset -> platform default.
inline const char *resToUpscale(Res r)
{
    switch (r)
    {
    case Res::Screen:
        return "native";
    case Res::R1x:
        return "1";
    case Res::R2x:
        return "2";
    case Res::R3x:
        return "3";
    case Res::R4x:
        return "4";
    default:
        return "native";
    }
}

inline SettingsPairs settingsForApply(const SettingsPairs &pairs, const NavState &st)
{
    SettingsPairs out = upsertSettings(pairs, "GE1_UPSCALE", resToUpscale(st.res));
    out = upsertSettings(out, "PS2X_SETTINGS_PERF", st.perf ? "1" : "0");
    out = upsertSettings(out, "PS2X_INPUT_MARKER", markerToSetting(st.marker));
    out = upsertSettings(out, "PS2X_VIRTUAL_PAD", touchToSetting(st.touch));
    out = upsertSettings(out, "GE1_ONDEVICE_UPSCALE", st.upscale ? "1" : "0");
    out = upsertSettings(out, "PS2X_MENU_BUTTON", menuBtnToSetting(st.menubtn));
    out = upsertSettings(out, "PS2X_HOLD_START", holdStartToSetting(st.holdStart));
    return out;
}

inline Res resFromSetting(const std::string &val, Res platformDefault)
{
    if (val == "native")
        return Res::Screen;
    if (val == "1")
        return Res::R1x;
    if (val == "2")
        return Res::R2x;
    if (val == "3")
        return Res::R3x;
    if (val == "4")
        return Res::R4x;
    return platformDefault; // unset or junk: the platform default
}

// Default Resolution row: the '(default)' marker and the preselect come
// from the existing device rules, never a panel-side hardcode
// (settings-res-1 correction 4, Brad 10-17). Public builds follow the
// public device-tier rule (ps2_public_tier.h, decision 'Public-build
// defaults by device' 10-13): capable (iOS, or Android with >= 5 GiB
// reported RAM) gets Screen on a 1080p panel, otherwise 2x; every other
// device gets 1x. The tier logic itself is untouched: resTierDefault runs
// the rule's choose() and maps its render string onto the rows. Play
// builds keep their env rules (Android/Odin = Screen, iOS = 2x); anything
// else (Mac/dev) shows Screen.
inline Res resTierDefault(bool isiOS, std::uint64_t ramBytes, int panelW, int panelH)
{
    ps2x::public_tier::Input in{};
    in.ios = isiOS;
    in.ramBytes = ramBytes;
    in.panelW = panelW;
    in.panelH = panelH;
    const ps2x::public_tier::Choice c = ps2x::public_tier::choose(in);
    return resFromSetting(c.render, Res::Screen);
}

inline Res resDefaultRow(bool publicApp, bool isiOS, std::uint64_t ramBytes, int panelW, int panelH)
{
    if (publicApp)
        return resTierDefault(isiOS, ramBytes, panelW, panelH);
    return isiOS ? Res::R2x : Res::Screen;
}

// Row value text: the multiplier first, then the resulting height
// (PS2 base 448 lines x scale); Screen shows the measured device height
// rounded the same way resHeightLabel rounds. The platform default option
// is marked. The panel font is raylib's default (U+0000..U+00FF), so the
// multiply sign (U+00D7, UTF-8 C3 97) and middle dot (U+00B7, UTF-8 C2 B7)
// render; "..." and the em dash do not (the menu button and hints use
// ASCII). settings-ui-1 (Brad 10-19 "?"): the dot was a lone 0xB7 byte,
// which is NOT valid UTF-8, so raylib decoded it as '?'. Both separators
// below are proper two-byte UTF-8 now.
inline std::string resOptionLabel(Res r, unsigned nativeH, Res platformDefault)
{
    std::string s;
    if (r == Res::Screen)
    {
        const std::string h = resHeightLabel(nativeH);
        s = h.empty() ? "Screen" : "Screen \xc2\xb7 " + h;
    }
    else
    {
        const unsigned mult = resScale(r);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%u\xc3\x97 \xc2\xb7 %up", mult, kResBaseH * mult);
        s = buf;
    }
    if (r == platformDefault)
        s += " (default)";
    return s;
}

// Shared settings-style constants (settings-ui-1, Brad 10-19): the panel
// and the resume prompt read the same palette, fonts and spacing here —
// never copied literals at the draw sites. Raylib-agnostic RGBA so this
// header stays dependency-free for the host suite; the render loop
// converts to raylib Color at the draw site.
struct Rgba
{
    uint8_t r = 0, g = 0, b = 0, a = 255;
};
constexpr Rgba kSet2Bg{8, 12, 20, 232};
constexpr Rgba kSet2Border{255, 140, 40, 255};
constexpr Rgba kSet2Title{255, 170, 80, 255};
constexpr Rgba kSet2Text{230, 235, 245, 255};
constexpr Rgba kSet2TextSel{255, 200, 130, 255};
constexpr Rgba kSet2SelBg{255, 140, 40, 48};
constexpr Rgba kSet2Hint{150, 160, 175, 255};
constexpr Rgba kSet2Btn{38, 48, 62, 255};
constexpr Rgba kSet2BtnSel{153, 84, 24, 255};
constexpr Rgba kSet2Pill{0, 0, 0, 120};

inline int set2TitleFont(float h)
{
    return std::max(16, static_cast<int>(h / 26.0f));
}

inline int set2RowFont(float h)
{
    return std::max(12, static_cast<int>(h / 34.0f));
}

struct MenuButtonGeom
{
    float x = 0, y = 0, w = 0, h = 0; // hit rect, window points
};

// Small "..." at the top middle. The hit rect is thumb-sized; the
// drawn glyph is smaller and centred (the render loop draws the text).
// safeTop clears the display cutout (settings-ui-1, Brad 10-19: on an
// iPhone the button sat at 2% under the Dynamic Island/safe area); the
// perf overlay already offsets the same way via set2SafePad.
inline MenuButtonGeom menuButtonLayout(float sw, float sh, float safeTop = 0.0f)
{
    MenuButtonGeom g;
    const float hit = sh * 0.075f; // ~81 px on a 1080p panel
    g.w = hit * 1.6f;
    g.h = hit;
    g.x = (sw - g.w) * 0.5f;
    g.y = safeTop + sh * 0.02f;
    return g;
}

inline bool menuButtonHit(const MenuButtonGeom &g, float x, float y)
{
    return x >= g.x && x <= g.x + g.w && y >= g.y && y <= g.y + g.h;
}

// Touch halves (settings-ui-1, Brad 10-19): a tap left of the row middle
// steps back, right of it steps forward. The draw wraps multi-value rows
// in "< >" arrows as the affordance; the halves are the tap zones.
inline bool rowTapBack(const PanelGeom &g, float x)
{
    return x < g.rowX + g.rowW * 0.5f;
}

#if defined(__APPLE__) && !defined(PS2X_IOS)
// Stashed by main() so the panel's Restart can re-exec the runner (Mac only;
// defined in ps2_runtime.cpp).
void set2NoteExecArgs(int argc, char **argv);
#endif

} // namespace ps2x::set2
