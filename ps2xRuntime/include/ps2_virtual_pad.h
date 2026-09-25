#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

// I26: on-screen virtual controls (layout + hit test; no raylib/SDL here so
// the host suite can test it). The iOS render loop draws the overlay, maps
// the current touches through pressedMask() and publishes the result with
// liveMask(); PSPadBackend::readState ORs it into the keyboard/gamepad
// union, and a PS2X_PAD_SCRIPT press is applied on top of that as before.
// I32: the left D-pad is now a floating analog stick (updateStick tracks
// the anchor across frames; liveStick() carries the left-stick bytes) and
// the D-pad moved to the right side, above the face buttons.
// I34 (Brad): the stick is 1.5x, and the D-pad is 80% of the face-button
// cluster's bounding box, below the cluster, pushed right to the
// no-overlap limit (literal down-right of the cluster can't fit: the
// cluster's right edge is 0.005u from the screen edge).
namespace ps2x::vpad
{
    // PS2 pad button bits (same values as ps2_pad.cpp / Pad.cpp).
    constexpr uint16_t kSelect = 0x0001u;
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
    struct TestTouch
    {
        uint64_t tick;
        float fx, fy;
        uint64_t hold;
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
            char *p = nullptr;
            const char *c = item.c_str();
            t.tick = std::strtoull(c, &p, 10);
            bool ok = p != c && *p == ':';
            if (ok) { c = p + 1; t.fx = std::strtof(c, &p); ok = p != c && *p == ':'; }
            if (ok) { c = p + 1; t.fy = std::strtof(c, &p); ok = p != c && *p == ':'; }
            if (ok) { c = p + 1; t.hold = std::strtoull(c, &p, 10); ok = p != c && *p == '\0' && t.hold > 0; }
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

    // PS2X_VIRTUAL_PAD: unset or anything but "0" = on (iOS sets it from
    // Settings > Virtual controls).
    inline bool enabledFromEnv(const char *value)
    {
        return !(value && value[0] == '0');
    }

    // Pressed bits published by the render thread, read by the pad backend.
    inline std::atomic<uint16_t> &liveMask()
    {
        static std::atomic<uint16_t> mask{0u};
        return mask;
    }
}
