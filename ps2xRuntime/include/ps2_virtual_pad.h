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
    };

    // Landscape layout scaled by the window height: D-pad bottom-left, face
    // buttons bottom-right, shoulders in the top corners, Select/Start at
    // the bottom inner corners. On a wide phone this sits in the pillarbox
    // bars beside the 4:3 picture.
    inline Layout makeLayout(float w, float h)
    {
        const float u = h;
        const float cy = 0.60f * u;
        const float off = 0.13f * u;
        const float br = 0.07f * u;
        const float lx = 0.205f * u; // clusters end at 0.405u: inside the 4:3 pillarbox on a 19.5:9 phone
        const float rx = w - 0.205f * u;
        Layout l{};
        l.dpadX = lx;
        l.dpadY = cy;
        l.dpadR = off + br;
        l.buttons = {
            {kUp, lx, cy - off, br, "up"},
            {kDown, lx, cy + off, br, "down"},
            {kLeft, lx - off, cy, br, "left"},
            {kRight, lx + off, cy, br, "right"},
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
