#pragma once

// SET2: in-game settings overlay (design B, Brad 10-12): Select+Start opens a
// minimal paused panel (Mode, Resolution, Perf overlay, On-device upscale,
// Restart). set2-polish-1 (Brad 10-16): the chord is the only opener — the
// gear dot is gone. On touch: hold SELECT + tap START on the overlay, or
// two-finger-tap with no overlay up.
// Pure logic here (unit-tested by ps2_settings_overlay_tests.cpp); the render
// loop in ps2_runtime.cpp owns pause/draw/restart, and the platform shells
// own the settings file layer + relaunch.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
// Rows: Mode, Resolution, Perf, Upscale, Touch, Menu button. X changes
// the pending value (Mode/Resolution apply on restart; Perf, Upscale,
// Touch and Menu button are live). There is no Restart row: X on a clean
// Mode/Resolution row applies the change and arms the restart; a second X
// on that row while it still differs from the running value restarts
// (ArmRestart/DoRestart, reusing the old one-line confirm). X anywhere
// else, or moving off the row, disarms. Triangle or the chord closes.
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
    Count = 6
};
static constexpr int kRowCount = static_cast<int>(Row::Count);

// Short row labels (panel-polish-1, Brad 10-15: every label shows in full;
// the draw code sets label left, value right, so no dot leaders truncate).
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
        return "Touch";
    case Row::MenuBtn:
        return "Menu button";
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

// Next choice in height order (wraps). X on the Resolution row steps this.
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

// Menu "..." button (settings-res-1 addendum, Brad 10-17): a small dim
// button at the top middle of the screen; tapping it opens the panel. Shown
// by default on every device (even with a physical controller); only this
// row hides it. Persisted as PS2X_MENU_BUTTON (Hidden="0", Shown=key
// absent, so deleting the line restores the default).
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
        return ""; // Shown: no override line
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
enum class Action : uint8_t
{
    None = 0,
    Changed = 1,  // a pending value changed (redraw)
    ArmRestart = 2,
    DoRestart = 3,
    Close = 4
};

struct NavState
{
    Row sel = Row::Mode;
    Mode mode = Mode::M120;   // pending (applied on restart)
    Res res = Res::Screen;    // pending (applied on restart)
    bool perf = false;        // live
    bool upscale = false;     // live (REM3 P11 Phase 2)
    Touch touch = Touch::Auto; // live (android-touch-1)
    MenuBtn menubtn = MenuBtn::Shown; // live (settings-res-1)
    Mode runMode = Mode::M120; // running values, sampled at open (dirty detect)
    Res runRes = Res::Screen;
    unsigned nativeH = 0; // measured device height px, sampled at open (order + Screen label)
    bool restartArmed = false;
};

inline bool modeDirty(const NavState &st)
{
    return st.mode != st.runMode;
}

inline bool resDirty(const NavState &st)
{
    return st.res != st.runRes;
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

inline NavOut navStep(NavState &st, bool upEdge, bool downEdge, bool crossEdge, bool triangleEdge,
                      bool chordEdge)
{
    NavOut out;
    if (chordEdge || triangleEdge)
    {
        out.action = Action::Close;
        st.restartArmed = false;
        return out;
    }
    if (upEdge || downEdge)
    {
        const int n = static_cast<int>(Row::Count);
        int sel = static_cast<int>(st.sel);
        sel = upEdge ? (sel - 1 + n) % n : (sel + 1) % n;
        st.sel = static_cast<Row>(sel);
        st.restartArmed = false; // navigating away disarms the confirm
        return out;
    }
    if (!crossEdge)
        return out;
    switch (st.sel)
    {
    case Row::Mode:
        // A second X while still dirty restarts; otherwise toggle (toggling
        // back to the running value disarms when nothing else is dirty).
        if (st.restartArmed && modeDirty(st))
        {
            out.action = Action::DoRestart;
            break;
        }
        st.mode = (st.mode == Mode::M120) ? Mode::M60 : Mode::M120;
        out.action = Action::Changed;
        st.restartArmed = modeDirty(st) || resDirty(st);
        break;
    case Row::Res:
        if (st.restartArmed && resDirty(st))
        {
            out.action = Action::DoRestart;
            break;
        }
        st.res = resNext(st.res, st.nativeH);
        out.action = Action::Changed;
        st.restartArmed = modeDirty(st) || resDirty(st);
        break;
    case Row::Perf:
        st.perf = !st.perf;
        out.action = Action::Changed;
        st.restartArmed = false;
        break;
    case Row::Upscale:
        st.upscale = !st.upscale;
        out.action = Action::Changed;
        st.restartArmed = false;
        break;
    case Row::Touch:
        st.touch = (st.touch == Touch::Auto) ? Touch::On : (st.touch == Touch::On ? Touch::Off : Touch::Auto);
        out.action = Action::Changed;
        st.restartArmed = false;
        break;
    case Row::MenuBtn:
        st.menubtn = (st.menubtn == MenuBtn::Shown) ? MenuBtn::Hidden : MenuBtn::Shown;
        out.action = Action::Changed;
        st.restartArmed = false;
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
    std::string s = "# set2: written by the in-game settings panel (Mode/Resolution/Perf/Upscale/Touch/Menu button).\n"
                    "# Delete lines (or this file) to return to the platform defaults.\n";
    for (const auto &kv : pairs)
        s += kv.first + "=" + kv.second + "\n";
    return s;
}

// What the restart persists: the PENDING Resolution choice (never the
// effective env) plus the live Perf flag. Pure so the write path is
// unit-tested (Brad 10-17 iPhone bug: a stale choice must not survive the
// restart — the file must carry exactly what the rows show).
inline SettingsPairs settingsForRestart(const SettingsPairs &pairs, Res res, bool perf);

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

inline SettingsPairs settingsForRestart(const SettingsPairs &pairs, Res res, bool perf)
{
    SettingsPairs out = upsertSettings(pairs, "GE1_UPSCALE", resToUpscale(res));
    out = upsertSettings(out, "PS2X_SETTINGS_PERF", perf ? "1" : "0");
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
// multiply sign (xD7) and middle dot (xB7) render; "..." and the em dash do
// not (the menu button and restart hint use ASCII).
inline std::string resOptionLabel(Res r, unsigned nativeH, Res platformDefault)
{
    std::string s;
    if (r == Res::Screen)
    {
        const std::string h = resHeightLabel(nativeH);
        s = h.empty() ? "Screen" : "Screen \xb7 " + h;
    }
    else
    {
        const unsigned mult = resScale(r);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%u\xc3\x97 \xb7 %up", mult, kResBaseH * mult);
        s = buf;
    }
    if (r == platformDefault)
        s += " (default)";
    return s;
}

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

struct MenuButtonGeom
{
    float x = 0, y = 0, w = 0, h = 0; // hit rect, window points
};

// Small dim "..." at the top middle. The hit rect is thumb-sized; the
// drawn glyph is smaller and centred (the render loop draws the text).
inline MenuButtonGeom menuButtonLayout(float sw, float sh)
{
    MenuButtonGeom g;
    const float hit = sh * 0.075f; // ~81 px on a 1080p panel
    g.w = hit * 1.6f;
    g.h = hit;
    g.x = (sw - g.w) * 0.5f;
    g.y = sh * 0.02f;
    return g;
}

inline bool menuButtonHit(const MenuButtonGeom &g, float x, float y)
{
    return x >= g.x && x <= g.x + g.w && y >= g.y && y <= g.y + g.h;
}

#if defined(__APPLE__) && !defined(PS2X_IOS)
// Stashed by main() so the panel's Restart can re-exec the runner (Mac only;
// defined in ps2_runtime.cpp).
void set2NoteExecArgs(int argc, char **argv);
#endif

} // namespace ps2x::set2
