#pragma once

// SET2: in-game settings overlay (design B, Brad 10-12): Select+Start opens a
// minimal paused panel (Mode, Resolution, Perf overlay, On-device upscale,
// Online, Restart).
// Pure logic here (unit-tested by ps2_settings_overlay_tests.cpp); the render
// loop in ps2_runtime.cpp owns pause/draw/restart, and the platform shells
// own the settings file layer + relaunch.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace ps2x::set2
{

// --- Chord: hold back Select so the chord never leaks it to the game ---
//
// Brad 10-12: "select does reset the character" (SET1's probe missed it). A
// Select press is held for kChordWindowMs: if a Start edge arrives inside the
// window it is the chord (open the panel, neither button reaches the game);
// otherwise the Select forwards (the rider reset keeps working, ~150 ms
// later). A Select released inside the window with no Start forwards a short
// pulse so the guest still sees it. Start-first-then-Select is two separate
// presses, never the chord (Start already reached the game).
constexpr int kChordWindowMs = 150;
constexpr int kSelectPulseFrames = 3;

struct ChordState
{
    bool selectWas = false;
    bool startWas = false;
    bool holding = false;    // Select edge seen, window open
    int64_t holdSinceMs = 0; // window start (wall ms)
    bool consumed = false;   // chord fired; strip both until both released
    int pulseLeft = 0;       // early-release forward pulse (host frames)
};

struct ChordOut
{
    bool openEdge = false;    // chord fired this frame: open the panel
    bool stripSelect = false; // strip the Select bit from the game mask
    bool stripStart = false;  // strip the Start bit from the game mask
    bool addSelect = false;   // OR the Select bit into the game mask (pulse)
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
    if (selectEdge)
    {
        st.holding = true;
        st.holdSinceMs = nowMs;
        st.pulseLeft = 0;
    }
    if (st.holding)
    {
        if (!selectDown)
        {
            // Released inside the window with no Start: forward a short
            // pulse so the guest still sees the press.
            st.holding = false;
            st.pulseLeft = kSelectPulseFrames;
        }
        else if (nowMs - st.holdSinceMs >= kChordWindowMs)
        {
            // Window expired first: a lone Select, forwarded from here on
            // (a Start edge on this frame is a plain Start, not the chord).
            st.holding = false;
        }
        else if (startEdge)
        {
            st.holding = false;
            st.consumed = true;
            out.openEdge = true;
            out.stripSelect = true;
            out.stripStart = true;
        }
        else
        {
            out.stripSelect = true;
        }
    }
    else if (!selectDown && st.pulseLeft > 0)
    {
        out.addSelect = true;
        --st.pulseLeft;
    }
    st.selectWas = selectDown;
    st.startWas = startDown;
    return out;
}

// --- Panel navigation (pad edges; touch taps map to the same edges) ---
//
// Rows: Mode, Resolution, Perf, Upscale, Touch, Online, Restart. X changes
// the pending value (Mode/Resolution/Online apply on restart; Perf, Upscale
// and Touch are live). Restart arms on the first X ("one-line confirm") and
// fires on the second. Triangle or the chord closes. Row::Count drives the
// geometry, hit test and draw loops, so appending a row needs no layout
// change.
enum class Row : uint8_t
{
    Mode = 0,
    Res = 1,
    Perf = 2,
    Upscale = 3,
    Touch = 4,
    Online = 5,
    Restart = 6,
    Count = 7
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
    case Row::Online:
        return "Online";
    case Row::Restart:
        return "Restart";
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
enum class Res : uint8_t
{
    Native = 0,  // GE1_UPSCALE=native
    R2x = 1,     // GE1_UPSCALE=2
    RDefault = 2 // no override: the platform default
};
// Touch controls (android-touch-1): Auto shows the overlay only when no
// physical gamepad is present (Odin hides by default); On/Off force it.
// Persisted as PS2X_VIRTUAL_PAD (On="1", Off="0", Auto=key absent).
enum class Touch : uint8_t
{
    Auto = 0,
    On = 1,
    Off = 2
};
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
    Mode mode = Mode::M120; // pending (applied on restart)
    Res res = Res::RDefault; // pending (applied on restart)
    bool perf = false;       // live
    bool upscale = false;    // live (REM3 P11 Phase 2)
    Touch touch = Touch::Auto; // live (android-touch-1)
    bool online = false;     // pending (applied on restart; online-default-1)
    bool restartArmed = false;
};

// SET2 Online row (online-default-1): the panel-owned persistence key.
// Off writes PS2X_NET=0 (settings layer reads last, so it beats the
// preset); on writes PS2X_NET=1. Same truthiness as the net HLE gate:
// set and not starting with '0'.
inline bool onlineEnvOn(const char *value)
{
    return value && *value && value[0] != '0';
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
        st.mode = (st.mode == Mode::M120) ? Mode::M60 : Mode::M120;
        out.action = Action::Changed;
        break;
    case Row::Res:
        st.res = (st.res == Res::Native) ? Res::R2x : (st.res == Res::R2x ? Res::RDefault : Res::Native);
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
        st.touch = (st.touch == Touch::Auto) ? Touch::On : (st.touch == Touch::On ? Touch::Off : Touch::Auto);
        out.action = Action::Changed;
        break;
    case Row::Online:
        st.online = !st.online;
        out.action = Action::Changed;
        break;
    case Row::Restart:
        if (st.restartArmed)
            out.action = Action::DoRestart;
        else
        {
            st.restartArmed = true;
            out.action = Action::ArmRestart;
        }
        break;
    default:
        break;
    }
    return out;
}

// --- Panel geometry + hit test (QSR1 precedent: pure, render loop draws) ---
//
// Hit values: 0..kRowCount-1 = row tap (same as X on that row),
// kRowCount = gear-dot tap (closed panel: open; open panel: close),
// -1 = miss (open panel: tap outside closes).
struct PanelGeom
{
    float px = 0, py = 0, pw = 0, ph = 0; // panel rect
    float rowX = 0, rowW = 0;
    float rowY[kRowCount] = {};
    float rowH = 0;
    float dotX = 0, dotY = 0, dotR = 0; // gear dot (closed state)
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
    // Gear dot top-right: clear of the vpad shoulders (top corners at 0.10u
    // with 0.07u discs).
    g.dotX = w - 0.055f * h;
    g.dotY = 0.20f * h;
    g.dotR = 0.045f * h;
    return g;
}

inline int panelHit(const PanelGeom &g, bool open, float x, float y)
{
    const float ddx = x - g.dotX, ddy = y - g.dotY;
    if (ddx * ddx + ddy * ddy <= g.dotR * g.dotR)
        return kRowCount;
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
           k == "GE1_ONDEVICE_UPSCALE" || // REM3 P11 Phase 2: the live toggle persists under its env name
           k == "PS2X_NET"; // online-default-1: the Online row persists under its env name
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
    std::string s = "# set2: written by the in-game settings panel (Mode/Resolution/Perf/Upscale/Touch/Online).\n"
                    "# Delete lines (or this file) to return to the platform defaults.\n";
    for (const auto &kv : pairs)
        s += kv.first + "=" + kv.second + "\n";
    return s;
}

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

inline const char *resToUpscale(Res r)
{
    switch (r)
    {
    case Res::Native:
        return "native";
    case Res::R2x:
        return "2";
    default:
        return ""; // RDefault: no override line
    }
}

// Projected render height for a pending Res option, from the measured frame
// height and the effective upscale string (set2EffUpscale: "2", "2.5",
// "native", "1x", ...). RDefault shows the measured height itself; Native
// and R2x scale from the implied 1x base (measured / effective, or measured
// when the effective value is "native", which already renders at display
// size). Unparseable scales fall back to measured (neutral). Approximate by
// construction on display-export paths (the output size is setting-blind);
// the RDefault case is always exact.
inline unsigned resProjectedH(Res pending, const std::string &eff, unsigned measuredH)
{
    if (pending == Res::RDefault || measuredH == 0)
        return measuredH;
    float base = static_cast<float>(measuredH);
    if (eff != "native" && eff != "halfnative")
    {
        const float s = std::strtof(eff.c_str(), nullptr);
        if (s > 0.0f)
            base = static_cast<float>(measuredH) / s;
    }
    const float h = (pending == Res::Native) ? base : base * 2.0f;
    return static_cast<unsigned>(h + 0.5f);
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

#if defined(__APPLE__) && !defined(PS2X_IOS)
// Stashed by main() so the panel's Restart can re-exec the runner (Mac only;
// defined in ps2_runtime.cpp).
void set2NoteExecArgs(int argc, char **argv);
#endif

} // namespace ps2x::set2
