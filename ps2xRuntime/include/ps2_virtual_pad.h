#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

// I26: on-screen virtual controls (layout + hit test; no raylib/SDL here so
// the host suite can test it). The iOS render loop draws the overlay, maps
// the current touches through pressedMask() and publishes the result to the
// IN2 pad latch (ps2_pad_latch.h). PSPadBackend::readState takes the latched mask, and a
// PS2X_PAD_SCRIPT press is applied on top of that as before.
// I32: the left D-pad is now a floating analog stick (updateStick tracks
// the anchor across frames; liveStick() carries the left-stick bytes) and
// the D-pad moved to the right side, above the face buttons.
// I34 (Brad): the stick is 1.5x, and the D-pad is 80% of the face-button
// cluster's bounding box, below the cluster, pushed right to the
// no-overlap limit (literal down-right of the cluster can't fit: the
// cluster's right edge is 0.005u from the screen edge).
// VT1 (Brad): touches carry platform IDs; each touch is classified once at
// touch-down and owns its control until lift-off (updatePad). The stick
// finger never presses buttons; Select/Start/L/R stay on their owner touch;
// face/D-pad keep slide-between within their cluster. The stick anchors at
// the touch-down point anywhere in the left half (minus 1.0x button discs)
// and follows past stickR instead of re-anchoring.
namespace ps2x::vpad
{
    // PS2 pad button bits (same values as ps2_pad.cpp / Pad.cpp).
    constexpr uint16_t kSelect = 0x0001u;
    constexpr uint16_t kL3 = 0x0002u; // IN2: no overlay button; TEST_TAP only
    constexpr uint16_t kR3 = 0x0004u; // IN2: no overlay button; TEST_TAP only
    constexpr uint16_t kStart = 0x0008u;
    constexpr uint16_t kUp = 0x0010u;
    constexpr uint16_t kRight = 0x0020u;
    constexpr uint16_t kDown = 0x0040u;
    constexpr uint16_t kLeft = 0x0080u;
    constexpr uint16_t kL2 = 0x0100u;
    constexpr uint16_t kR2 = 0x0200u;
    constexpr uint16_t kL1 = 0x0400u;
    constexpr uint16_t kR1 = 0x0800u;
    constexpr uint16_t kTriangle = 0x1000u;
    constexpr uint16_t kCircle = 0x2000u;
    constexpr uint16_t kCross = 0x4000u;
    constexpr uint16_t kSquare = 0x8000u;

    struct Button
    {
        uint16_t mask;
        float x, y, r; // centre and radius, window points
        const char *label;
    };

    struct Layout
    {
        std::vector<Button> buttons; // includes the four D-pad arrows (drawn; hit-tested via the D-pad disc)
        float dpadX, dpadY, dpadR;   // D-pad disc: 8-way by angle, dead zone in the middle
        float stickRestX, stickRestY; // floating-stick rest position (drawn dim until touched)
        float stickR;                // stick base radius = max drag deflection, window points
        float stickZoneX;            // stick zone: touches with x < stickZoneX drive the stick
    };

    // Landscape layout scaled by the window height (I32/I34): floating
    // analog stick on the left (resting where the I26 D-pad was), D-pad on
    // the right below the face buttons, face buttons bottom-right,
    // shoulders in the top corners, Select/Start at the bottom inner
    // corners. On a wide phone the stick and face buttons sit in the
    // pillarbox bars beside the 4:3 picture; the I34 D-pad overlaps the
    // picture's bottom-right (the right column is full).
    inline Layout makeLayout(float w, float h)
    {
        const float u = h;
        const float cy = 0.60f * u;
        const float off = 0.13f * u;
        const float br = 0.07f * u;
        const float lx = 0.205f * u; // clusters end at 0.405u: inside the 4:3 pillarbox on a 19.5:9 phone
        const float rx = w - 0.205f * u;
        // I34: D-pad overall (disc diameter) = 80% of the face-cluster
        // bounding box (2*(off+br) = 0.40u); centre below the cluster,
        // pushed right to the no-overlap limit (see the I34 layout test).
        const float dpadR = 0.16f * u;
        const float dcx = w - 0.532f * u;
        const float dcy = 0.78f * u;
        const float dOff = dpadR * (11.0f / 19.0f); // arrow bbox == disc diameter, as I32
        const float dBr = dpadR * (8.0f / 19.0f);
        Layout l{};
        l.dpadX = dcx;
        l.dpadY = dcy;
        l.dpadR = dpadR;
        l.stickRestX = lx;
        l.stickRestY = cy;
        l.stickR = 0.15f * u; // I34: 1.5x (base, knob, drag and re-anchor scale; dead zone stays 10%)
        l.stickZoneX = 0.5f * w;
        l.buttons = {
            {kUp, dcx, dcy - dOff, dBr, "up"},
            {kDown, dcx, dcy + dOff, dBr, "down"},
            {kLeft, dcx - dOff, dcy, dBr, "left"},
            {kRight, dcx + dOff, dcy, dBr, "right"},
            {kTriangle, rx, cy - off, br, "triangle"},
            {kCross, rx, cy + off, br, "cross"},
            {kSquare, rx - off, cy, br, "square"},
            {kCircle, rx + off, cy, br, "circle"},
            {kL2, 0.10f * u, 0.10f * u, 0.07f * u, "L2"},
            {kL1, 0.28f * u, 0.10f * u, 0.07f * u, "L1"},
            {kR1, w - 0.28f * u, 0.10f * u, 0.07f * u, "R1"},
            {kR2, w - 0.10f * u, 0.10f * u, 0.07f * u, "R2"},
            {kSelect, 0.33f * u, 0.92f * u, 0.055f * u, "SELECT"},
            {kStart, w - 0.33f * u, 0.92f * u, 0.055f * u, "START"},
        };
        return l;
    }

    inline bool isDpad(uint16_t mask)
    {
        return (mask & (kUp | kDown | kLeft | kRight)) != 0u;
    }

    inline bool isFace(uint16_t mask)
    {
        return (mask & (kTriangle | kCircle | kCross | kSquare)) != 0u;
    }

    // Buttons held by the given touch points (bit set = pressed). A D-pad
    // touch picks up to two directions by angle (45-degree diagonals); other
    // buttons take any touch within 1.2x their radius.
    inline uint16_t pressedMask(const Layout &l, const float *xs, const float *ys, int n)
    {
        uint16_t mask = 0u;
        for (int i = 0; i < n; ++i)
        {
            const float dx = xs[i] - l.dpadX;
            const float dy = ys[i] - l.dpadY;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d <= l.dpadR * 1.15f)
            {
                if (d < 0.2f * l.dpadR)
                {
                    continue; // dead zone
                }
                // tan(67.5 deg) ~ 2.414: an axis counts when the touch is
                // within 67.5 degrees of it, so diagonals press two arrows.
                const float ax = std::fabs(dx);
                const float ay = std::fabs(dy);
                if (ax * 2.414f >= ay)
                    mask |= dx < 0.0f ? kLeft : kRight;
                if (ay * 2.414f >= ax)
                    mask |= dy < 0.0f ? kUp : kDown;
                continue;
            }
            for (const Button &b : l.buttons)
            {
                if (isDpad(b.mask))
                    continue;
                const float bx = xs[i] - b.x;
                const float by = ys[i] - b.y;
                if (bx * bx + by * by <= (1.2f * b.r) * (1.2f * b.r))
                {
                    mask |= b.mask;
                }
            }
        }
        return mask;
    }

    // I32: floating analog stick. The render loop carries one StickState
    // across frames; each frame updateStick() folds the current touches in.
    // The first touch in the stick zone (left of stickZoneX) that presses
    // no button anchors the stick; while a touch stays near the anchor it
    // drives the vector (touch - anchor) / stickR, clamped to magnitude 1
    // with a ~10% dead zone. A touch far from the anchor re-anchors (a new
    // touch-down); no touch in the zone releases back to centre. Touches on
    // buttons (L1/L2/Select on the left) are never stick touches, so stick
    // + button multi-touch works. Coordinates are screen points: +x right,
    // +y down, matching the gamepad axes (down is positive).
    struct StickState
    {
        bool active = false;
        float ax = 0.0f, ay = 0.0f; // anchor (touch-down point), window points
    };

    struct StickVec
    {
        bool active = false;
        float ax = 0.0f, ay = 0.0f; // anchor: the drawn base centre
        float x = 0.0f, y = 0.0f;   // normalized deflection, -1..1
    };

    inline StickVec updateStick(StickState &st, const Layout &l, const float *xs, const float *ys, int n)
    {
        StickVec out;
        const float trackR = 2.5f * l.stickR; // same finger if within this of the anchor
        int first = -1;                       // first stick-candidate touch this frame
        int best = -1;                        // candidate nearest the anchor, within trackR
        float bestD2 = trackR * trackR;
        for (int i = 0; i < n; ++i)
        {
            if (xs[i] >= l.stickZoneX)
                continue;
            if (pressedMask(l, &xs[i], &ys[i], 1) != 0u)
                continue; // on a button or the D-pad disc: not a stick touch
            if (first < 0)
                first = i;
            if (st.active)
            {
                const float dx = xs[i] - st.ax;
                const float dy = ys[i] - st.ay;
                const float d2 = dx * dx + dy * dy;
                if (d2 <= bestD2)
                {
                    bestD2 = d2;
                    best = i;
                }
            }
        }
        if (st.active && best >= 0)
        {
            out.active = true;
            out.ax = st.ax;
            out.ay = st.ay;
            float vx = (xs[best] - st.ax) / l.stickR;
            float vy = (ys[best] - st.ay) / l.stickR;
            const float m = std::sqrt(vx * vx + vy * vy);
            if (m > 1.0f)
            {
                vx /= m;
                vy /= m;
            }
            if ((m > 1.0f ? 1.0f : m) < 0.10f)
            {
                vx = 0.0f;
                vy = 0.0f;
            }
            out.x = vx;
            out.y = vy;
            return out;
        }
        if (first >= 0)
        {
            st.active = true;
            st.ax = xs[first];
            st.ay = ys[first];
            out.active = true;
            out.ax = st.ax;
            out.ay = st.ay;
            return out; // centred until the finger moves
        }
        st.active = false;
        return out; // released: recentred
    }

    // VT1: touch ownership by platform ID. Each touch is classified once at
    // touch-down and owns its control until lift-off:
    // - starts on a button/D-pad: owns that control. Face buttons and the
    //   D-pad recompute from the current position each frame (slide-between
    //   within their cluster). Select/Start/L1/L2/R1/R2 stay pressed on the
    //   owner touch until lift, wherever it slides.
    // - otherwise in the stick zone (x < stickZoneX, outside the 1.0x button
    //   discs): the stick finger until lift, never pressing any button.
    // - otherwise: inert until lift (never presses, never steals the stick).
    // Only one stick finger at a time; a second stick-zone touch stays inert
    // until it lifts and re-touches. No re-anchor: a drag past stickR slides
    // the anchor along (follow mode), so the stick never goes dead.
    struct TouchPoint
    {
        int64_t id;
        float x, y; // window points
    };

    enum class TouchClass
    {
        Inert,
        Stick,
        Dpad,
        Face,
        Button, // Select/Start/L1/L2/R1/R2: fixed mask until lift
    };

    struct TouchOwner
    {
        int64_t id;
        TouchClass cls;
        uint16_t mask = 0u; // Button: the owned button; Face/Dpad: unused
    };

    struct PadState
    {
        StickState stick; // anchor; active = stick owned
        int64_t stickId = -1;
        std::vector<TouchOwner> owners; // non-stick touches by ID, incl. inert
    };

    struct PadFrame
    {
        uint16_t pressed = 0u;
        StickVec stick;
    };

    // Non-D-pad button whose disc (radius * scale) contains (x, y), or 0.
    inline uint16_t buttonAt(const Layout &l, float x, float y, float scale)
    {
        for (const Button &b : l.buttons)
        {
            if (isDpad(b.mask))
                continue;
            const float dx = x - b.x;
            const float dy = y - b.y;
            const float rr = scale * b.r;
            if (dx * dx + dy * dy <= rr * rr)
                return b.mask;
        }
        return 0u;
    }

    // D-pad directions for (x, y) by angle (same geometry as pressedMask).
    inline uint16_t dpadMaskAt(const Layout &l, float x, float y)
    {
        const float dx = x - l.dpadX;
        const float dy = y - l.dpadY;
        const float d = std::sqrt(dx * dx + dy * dy);
        if (d > l.dpadR * 1.15f || d < 0.2f * l.dpadR)
            return 0u;
        uint16_t mask = 0u;
        const float ax = std::fabs(dx);
        const float ay = std::fabs(dy);
        if (ax * 2.414f >= ay)
            mask |= dx < 0.0f ? kLeft : kRight;
        if (ay * 2.414f >= ax)
            mask |= dy < 0.0f ? kUp : kDown;
        return mask;
    }

    inline bool dpadDiscAt(const Layout &l, float x, float y)
    {
        const float dx = x - l.dpadX;
        const float dy = y - l.dpadY;
        return std::sqrt(dx * dx + dy * dy) <= l.dpadR * 1.15f;
    }

    // VT3 (Brad): shoulder band. A touch-down in the stick zone above the
    // shoulder discs' bottom edge (+ half a radius margin) belongs to the
    // nearest shoulder button, never the stick: near-misses on L1/L2 on a
    // phone used to anchor the floating stick at the top of the screen (or
    // go inert under an owned stick). The y-band rule (rather than a 1.6x
    // disc) was chosen so there are no gaps: corners and the strip between
    // the shoulders also belong to a shoulder. The stick may only start
    // below the band. PS2X_VPAD_SHOULDER_BAND=0 restores VT1 (1.0x discs).
    inline float shoulderBandBottom(const Layout &l)
    {
        float bottom = -1.0f, rmax = 0.0f;
        for (const Button &b : l.buttons)
        {
            if (b.mask != kL1 && b.mask != kL2)
                continue;
            bottom = bottom < b.y + b.r ? b.y + b.r : bottom;
            rmax = rmax < b.r ? b.r : rmax;
        }
        if (bottom < 0.0f)
            return -1.0f; // no shoulders in the layout: no band
        return bottom + 0.5f * rmax;
    }

    // Nearest shoulder for a band touch-down: L1/L2, plus Select/Start only
    // if one sits up in the band (none does on the current layout).
    inline uint16_t nearestShoulder(const Layout &l, float x, float y, float bandBottom)
    {
        uint16_t best = 0u;
        float bestD2 = 0.0f;
        for (const Button &b : l.buttons)
        {
            const bool shoulder = b.mask == kL1 || b.mask == kL2;
            const bool topMenu =
                (b.mask == kSelect || b.mask == kStart) && b.y < bandBottom;
            if (!shoulder && !topMenu)
                continue;
            const float dx = x - b.x;
            const float dy = y - b.y;
            const float d2 = dx * dx + dy * dy;
            if (best == 0u || d2 < bestD2)
            {
                best = b.mask;
                bestD2 = d2;
            }
        }
        return best;
    }

    inline PadFrame updatePad(PadState &st, const Layout &l, const TouchPoint *ts, int n,
                              bool shoulderBand = true)
    {
        PadFrame out;
        // Drop lifted owners.
        if (st.stickId != -1)
        {
            bool present = false;
            for (int i = 0; i < n && !present; ++i)
                present = ts[i].id == st.stickId;
            if (!present)
            {
                st.stickId = -1;
                st.stick.active = false;
            }
        }
        for (size_t i = 0; i < st.owners.size();)
        {
            bool present = false;
            for (int j = 0; j < n && !present; ++j)
                present = ts[j].id == st.owners[i].id;
            if (present)
                ++i;
            else
                st.owners.erase(st.owners.begin() + static_cast<ptrdiff_t>(i));
        }
        // Classify new touch-downs.
        for (int i = 0; i < n; ++i)
        {
            if (ts[i].id == st.stickId)
                continue;
            bool known = false;
            for (const TouchOwner &o : st.owners)
            {
                if (o.id == ts[i].id)
                {
                    known = true;
                    break;
                }
            }
            if (known)
                continue;
            TouchOwner o{ts[i].id, TouchClass::Inert, 0u};
            if (ts[i].x < l.stickZoneX)
            {
                // VT3 shoulder band: above the shoulder discs (+ margin) a
                // touch-down belongs to the nearest shoulder, never the
                // stick, so the stick only ever starts below the band.
                const float bandBottom = shoulderBand ? shoulderBandBottom(l) : -1.0f;
                const uint16_t bandHit =
                    (bandBottom >= 0.0f && ts[i].y < bandBottom)
                        ? nearestShoulder(l, ts[i].x, ts[i].y, bandBottom)
                        : 0u;
                if (bandHit != 0u)
                {
                    o.cls = TouchClass::Button;
                    o.mask = bandHit;
                    st.owners.push_back(o);
                    continue;
                }
                // Stick zone: buttons claim only their drawn disc (1.0x);
                // the 1.0x-1.2x ring belongs to the stick.
                const uint16_t hit = buttonAt(l, ts[i].x, ts[i].y, 1.0f);
                if (hit != 0u)
                {
                    o.cls = isFace(hit) ? TouchClass::Face : TouchClass::Button;
                    o.mask = hit;
                    st.owners.push_back(o);
                }
                else if (st.stickId == -1)
                {
                    st.stickId = ts[i].id;
                    st.stick.active = true;
                    st.stick.ax = ts[i].x;
                    st.stick.ay = ts[i].y;
                }
                else
                {
                    st.owners.push_back(o); // second stick finger: inert
                }
            }
            else
            {
                if (dpadDiscAt(l, ts[i].x, ts[i].y))
                {
                    o.cls = TouchClass::Dpad;
                    st.owners.push_back(o);
                }
                else
                {
                    const uint16_t hit = buttonAt(l, ts[i].x, ts[i].y, 1.2f);
                    if (hit != 0u)
                    {
                        o.cls = isFace(hit) ? TouchClass::Face : TouchClass::Button;
                        o.mask = hit;
                    }
                    st.owners.push_back(o);
                }
            }
        }
        // Pressed mask from owners' current positions. VT3 slide-on (IN3's
        // (b)): an Inert owner whose position enters a Button-class disc
        // becomes that Button until lift (same scale as touch-down
        // classification: 1.0x in the stick zone, 1.2x on the right).
        for (size_t oi = 0; oi < st.owners.size(); ++oi)
        {
            TouchOwner &o = st.owners[oi];
            const TouchPoint *cur = nullptr;
            for (int i = 0; i < n; ++i)
            {
                if (ts[i].id == o.id)
                {
                    cur = &ts[i];
                    break;
                }
            }
            if (!cur)
                continue;
            if (shoulderBand && o.cls == TouchClass::Inert)
            {
                const float scale = (cur->x < l.stickZoneX) ? 1.0f : 1.2f;
                const uint16_t slide = buttonAt(l, cur->x, cur->y, scale);
                if (slide != 0u && !isDpad(slide) && !isFace(slide))
                {
                    o.cls = TouchClass::Button;
                    o.mask = slide;
                }
            }
            switch (o.cls)
            {
            case TouchClass::Button:
                out.pressed = static_cast<uint16_t>(out.pressed | o.mask);
                break;
            case TouchClass::Face:
            {
                const uint16_t hit = buttonAt(l, cur->x, cur->y, 1.2f);
                if (hit != 0u && isFace(hit))
                    out.pressed = static_cast<uint16_t>(out.pressed | hit);
                break;
            }
            case TouchClass::Dpad:
                out.pressed = static_cast<uint16_t>(out.pressed | dpadMaskAt(l, cur->x, cur->y));
                break;
            default:
                break;
            }
        }
        // Stick vector with follow: past stickR the anchor slides along.
        if (st.stickId != -1)
        {
            const TouchPoint *cur = nullptr;
            for (int i = 0; i < n; ++i)
            {
                if (ts[i].id == st.stickId)
                {
                    cur = &ts[i];
                    break;
                }
            }
            if (cur)
            {
                float vx = (cur->x - st.stick.ax) / l.stickR;
                float vy = (cur->y - st.stick.ay) / l.stickR;
                float m = std::sqrt(vx * vx + vy * vy);
                if (m > 1.0f)
                {
                    // Follow: anchor slides so the finger stays at full deflection.
                    st.stick.ax = cur->x - (vx / m) * l.stickR;
                    st.stick.ay = cur->y - (vy / m) * l.stickR;
                    vx /= m;
                    vy /= m;
                    m = 1.0f;
                }
                if (m < 0.10f)
                {
                    vx = 0.0f;
                    vy = 0.0f;
                }
                out.stick.active = true;
                out.stick.ax = st.stick.ax;
                out.stick.ay = st.stick.ay;
                out.stick.x = vx;
                out.stick.y = vy;
            }
        }
        return out;
    }

    // Left-stick bytes for the pad state (data[6] = LX, data[7] = LY,
    // 0x80 centred), the same bytes the physical-gamepad path writes.
    // Rounded (not truncated) so a full drag lands exactly on 0xFF/0x01.
    inline void stickBytes(const StickVec &v, uint8_t &lx, uint8_t &ly)
    {
        lx = static_cast<uint8_t>(128 + std::lround(v.x * 127.0f));
        ly = static_cast<uint8_t>(128 + std::lround(v.y * 127.0f));
    }

    // Stick vector published by the render thread (low byte LX, high byte
    // LY); kStickNoOverride while the overlay is off or hidden behind a
    // physical controller. PSPadBackend::readState applies it to
    // data[6..7] on top of the keyboard/gamepad values.
    constexpr uint16_t kStickNoOverride = 0xFFFFu;
    inline std::atomic<uint16_t> &liveStick()
    {
        static std::atomic<uint16_t> stick{kStickNoOverride};
        return stick;
    }

    // DEV-ONLY PS2X_VPAD_TEST_STICK="lx,ly" (floats in -1..1, clamped):
    // injects a stick vector instead of the touch-driven one, for runs
    // without a touch screen (the iOS Simulator here has no GUI to drag).
    // Raw, no dead zone, so the bytes are exactly predictable.
    inline bool parseTestStick(const char *spec, float &lx, float &ly)
    {
        if (!spec || !*spec)
            return false;
        char *p = nullptr;
        lx = std::strtof(spec, &p);
        if (p == spec || *p != ',')
            return false;
        const char *c = p + 1;
        ly = std::strtof(c, &p);
        if (p == c || *p != '\0')
            return false;
        lx = lx < -1.0f ? -1.0f : (lx > 1.0f ? 1.0f : lx);
        ly = ly < -1.0f ? -1.0f : (ly > 1.0f ? 1.0f : ly);
        return true;
    }

    // DEV-ONLY PS2X_VPAD_TEST_TOUCHES="tick:fx:fy:holdTicks,..." : synthetic
    // touches (guest vsync tick, window fractions) fed through the overlay's
    // hit test, for testing without a touch screen (the iOS Simulator here has
    // no GUI to click). Malformed items are skipped.
    // VT1: optional 5th field ":id" pins the synthetic touch ID (same ID
    // across successive 1-tick items = a slide path); without it the ID is
    // the item index, so overlapping 4-field items stay distinct touches.
    // Synthetic IDs are offset by kTestTouchIdBase to avoid colliding with
    // platform finger IDs.
    constexpr int64_t kTestTouchIdBase = 0x1000000LL;
    struct TestTouch
    {
        uint64_t tick;
        float fx, fy;
        uint64_t hold;
        int64_t id = -1; // -1 = auto (item index)
    };

    inline std::vector<TestTouch> parseTestTouches(const char *spec)
    {
        std::vector<TestTouch> out;
        if (!spec)
            return out;
        const std::string text(spec);
        size_t begin = 0;
        while (begin < text.size())
        {
            size_t end = text.find(',', begin);
            if (end == std::string::npos)
                end = text.size();
            const std::string item = text.substr(begin, end - begin);
            TestTouch t{};
            t.id = -1;
            char *p = nullptr;
            const char *c = item.c_str();
            t.tick = std::strtoull(c, &p, 10);
            bool ok = p != c && *p == ':';
            if (ok) { c = p + 1; t.fx = std::strtof(c, &p); ok = p != c && *p == ':'; }
            if (ok) { c = p + 1; t.fy = std::strtof(c, &p); ok = p != c && *p == ':'; }
            if (ok)
            {
                c = p + 1;
                t.hold = std::strtoull(c, &p, 10);
                ok = p != c && t.hold > 0 && (*p == '\0' || *p == ':');
                if (ok && *p == ':')
                {
                    c = p + 1;
                    t.id = std::strtoll(c, &p, 10);
                    ok = p != c && *p == '\0' && t.id >= 0;
                }
            }
            if (ok)
                out.push_back(t);
            begin = end + 1;
        }
        return out;
    }

    // Active synthetic touches at `tick`, scaled to window points.
    inline int activeTestTouches(const std::vector<TestTouch> &touches, uint64_t tick, float w, float h,
                                 float *xs, float *ys, int n, int max)
    {
        for (const TestTouch &t : touches)
        {
            if (n < max && tick >= t.tick && tick < t.tick + t.hold)
            {
                xs[n] = t.fx * w;
                ys[n] = t.fy * h;
                ++n;
            }
        }
        return n;
    }

    // VT1: ID-carrying variant for updatePad(). One TouchPoint per active
    // item; same-ID items overlapping on one tick collapse to the first, so
    // a slide path (successive 1-tick items, same ID) yields one touch.
    inline int activeTestTouchesWithIds(const std::vector<TestTouch> &touches, uint64_t tick, float w, float h,
                                         TouchPoint *ts, int n, int max)
    {
        for (size_t k = 0; k < touches.size() && n < max; ++k)
        {
            const TestTouch &t = touches[k];
            if (!(tick >= t.tick && tick < t.tick + t.hold))
                continue;
            const int64_t id = kTestTouchIdBase + (t.id >= 0 ? t.id : static_cast<int64_t>(k));
            bool dup = false;
            for (int j = 0; j < n; ++j)
            {
                if (ts[j].id == id)
                {
                    dup = true;
                    break;
                }
            }
            if (dup)
                continue;
            ts[n].id = id;
            ts[n].x = t.fx * w;
            ts[n].y = t.fy * h;
            ++n;
        }
        return n;
    }

    // IN2 DEV-ONLY PS2X_VPAD_TEST_TAP="ms:button:dur_ms,..." : synthetic
    // button taps on the WALL clock (ms since the render loop's first
    // frame, the padlatch::wallMs epoch), ORed into the published vpad
    // mask. Unlike PS2X_VPAD_TEST_TOUCHES (guest-vsync clocked), a tap
    // shorter than one guest frame lands between two guest reads, which
    // is the lost-tap repro (pre-latch the guest never sees it).
    // Button names match the pad script. Malformed items are skipped.
    struct TestTap
    {
        uint64_t atMs;
        uint16_t mask;
        uint64_t durMs;
    };

    inline uint16_t testTapButtonMask(const std::string &name)
    {
        if (name == "select") return kSelect;
        if (name == "l3") return kL3;
        if (name == "r3") return kR3;
        if (name == "start") return kStart;
        if (name == "up") return kUp;
        if (name == "right") return kRight;
        if (name == "down") return kDown;
        if (name == "left") return kLeft;
        if (name == "l2") return kL2;
        if (name == "r2") return kR2;
        if (name == "l1") return kL1;
        if (name == "r1") return kR1;
        if (name == "triangle") return kTriangle;
        if (name == "circle") return kCircle;
        if (name == "cross") return kCross;
        if (name == "square") return kSquare;
        return 0u;
    }

    inline std::vector<TestTap> parseTestTap(const char *spec)
    {
        std::vector<TestTap> out;
        if (!spec)
            return out;
        const std::string text(spec);
        size_t begin = 0;
        while (begin < text.size())
        {
            size_t end = text.find(',', begin);
            if (end == std::string::npos)
                end = text.size();
            const std::string item = text.substr(begin, end - begin);
            const size_t c1 = item.find(':');
            const size_t c2 = (c1 == std::string::npos) ? std::string::npos : item.find(':', c1 + 1);
            bool ok = c1 != std::string::npos && c2 != std::string::npos &&
                      item.find(':', c2 + 1) == std::string::npos;
            TestTap t{};
            if (ok)
            {
                char *p = nullptr;
                const char *c = item.c_str();
                t.atMs = std::strtoull(c, &p, 10);
                ok = p != c && *p == ':';
                if (ok)
                {
                    t.mask = testTapButtonMask(item.substr(c1 + 1, c2 - c1 - 1));
                    ok = t.mask != 0u;
                }
                if (ok)
                {
                    t.durMs = std::strtoull(item.c_str() + c2 + 1, &p, 10);
                    ok = p != item.c_str() + c2 + 1 && *p == '\0' && t.durMs > 0;
                }
            }
            if (ok)
                out.push_back(t);
            begin = end + 1;
        }
        return out;
    }

    // Union of taps active at `wallMs` (atMs <= wallMs < atMs + durMs).
    inline uint16_t activeTestTap(const std::vector<TestTap> &taps, uint64_t wallMs)
    {
        uint16_t mask = 0u;
        for (const TestTap &t : taps)
        {
            if (wallMs >= t.atMs && wallMs < t.atMs + t.durMs)
                mask = static_cast<uint16_t>(mask | t.mask);
        }
        return mask;
    }

    // PS2X_VIRTUAL_PAD: unset or anything but "0" = on (iOS sets it from
    // Settings > Virtual controls).
    inline bool enabledFromEnv(const char *value)
    {
        return !(value && value[0] == '0');
    }
    // QSR1: touch quick-save menu geometry (pure; the render loop draws it
    // and hit-tests before the guest pad, so menu touches never reach the
    // game). A dot top-centre plus an expanded Save/Load/Retry row under it.
    // Top-centre: the right column is full (R1/R2 shoulders at 0.10u, the
    // face cluster's right edge 0.005u from the screen edge), so a corner
    // dot would cover R2/L2. Hit values: 0 = none, 1 = dot,
    // 2/3/4 = save/load/retry.
    struct Qsr1MenuGeom
    {
        float dotX, dotY, dotR;
        float rowY, rowR, rowX[3];
    };

    inline Qsr1MenuGeom qsr1MenuLayout(float w, float h)
    {
        Qsr1MenuGeom g{};
        g.dotX = w * 0.5f;
        g.dotY = 0.10f * h;
        g.dotR = 0.045f * h;
        g.rowY = 0.24f * h;
        g.rowR = 0.06f * h;
        g.rowX[0] = w * 0.5f - 0.16f * h;
        g.rowX[1] = w * 0.5f;
        g.rowX[2] = w * 0.5f + 0.16f * h;
        return g;
    }

    inline int qsr1MenuHit(const Qsr1MenuGeom &g, bool expanded, float x, float y)
    {
        const float ddx = x - g.dotX, ddy = y - g.dotY;
        if (ddx * ddx + ddy * ddy <= g.dotR * g.dotR)
            return 1;
        if (!expanded)
            return 0;
        for (int i = 0; i < 3; ++i)
        {
            const float dx = x - g.rowX[i], dy = y - g.rowY;
            if (dx * dx + dy * dy <= g.rowR * g.rowR)
                return 2 + i;
        }
        return 0;
    }

    // QSR1 slot-dir rule (pure; unit-tested). iOS Documents wins; otherwise
    // the memory card's parent dir — never elfDirectory, which on Mac det
    // boots is the pinned CD tree (PI1: read-only). Last resort (no usable
    // card path): elfDirectory, matching DS1.
    inline std::string qsr1SlotDirFor(const char *iosDocs, const std::string &mcRoot,
                                      const std::string &elfDir)
    {
        namespace fs = std::filesystem;
        if (iosDocs && iosDocs[0] != '\0')
            return (fs::path(iosDocs) / "states").string();
        const fs::path card(mcRoot);
        if (!mcRoot.empty() && card.has_parent_path())
            return (card.parent_path() / "states").string();
        return (fs::path(elfDir) / "states").string();
    }
}
