#pragma once

// LOD1: draw distance knob PS2X_SSX3_LOD_SCALE=<f> (default off).
//
// SSX 3 has one view distance: the game camera's far value ([cam+8], 30000
// for one view and 20000 for split screen, 0x22e3b8; every camera mode is
// built with 30000, 0x15f938/0x1622b0/0x169418; blended and clamped per
// frame by 0x15e668). The render start path hands {fov, near, far} to the
// renderer's projection setter 0x376c58 ($f12, $f13, $f14; vtable slot
// 0x49332c; callers 0x22b274, 0x22b310, 0x2d9258, 0x1a25b8). Its far plane
// is what the instance and terrain visibility test (VU0 microprogram 0xdb8
// via 0x229fc8 / 0x22a128) clips against, so objects appear when they
// cross it.
//
// With the knob on, the wrapper multiplies $f14 by the scale at the setter's
// entry and calls the original. Only the renderer's copy changes: the game
// camera, the world streaming radius (1.5 x [cam+8], 0x15ec98 -> 0x3a9658)
// and the fog table (0x36aba0) keep their stock values. The draw lists,
// the VU0/VU1 inputs and the GS packets change (render-side guest RAM), so
// the det hash differs with the knob on; gameplay state does not read them
// [inferred; the LOD1 report checks the rider path]. Knob off: nothing is
// wrapped.
//
// TLS1: PS2X_SSX3_TRICKY_LOD_SCALE=<f> (same range and refusal rules) applies
// only while the current course is a Tricky course (the Tricky layer's own
// predicate: course modes armed and modeCurrent != 0). LOD_SCALE keeps its
// meaning (all courses); where both are set the Tricky value wins on Tricky
// courses. Both unset = today.

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace ps2_ssx3_lod
{
inline constexpr uint32_t kSetPerspective = 0x376c58u;
inline constexpr float kMinScale = 0.25f;
inline constexpr float kMaxScale = 8.0f;

// 0 = off (unset, empty, "0", "off" or "1"). A value outside
// [kMinScale, kMaxScale] or not a number returns -1 (the caller refuses).
inline float parseScale(const char *v) noexcept
{
    if (!v || !*v || std::strcmp(v, "0") == 0 || std::strcmp(v, "off") == 0)
        return 0.0f;
    char *end = nullptr;
    const float s = std::strtof(v, &end);
    if (end == v || *end != '\0' || !std::isfinite(s) || s < kMinScale || s > kMaxScale)
        return -1.0f;
    return s == 1.0f ? 0.0f : s;
}

inline float scale() noexcept
{
    static const float s = parseScale(std::getenv("PS2X_SSX3_LOD_SCALE"));
    return s;
}

// TLS1: Tricky-courses-only scale (same parse; 0 = off, <0 = refused).
inline float trickyScale() noexcept
{
    static const float s = parseScale(std::getenv("PS2X_SSX3_TRICKY_LOD_SCALE"));
    return s;
}

// Effective scale for one projection-set call. 0 = leave $f14 stock.
inline float selectScale(float global, float tricky, bool isTricky) noexcept
{
    if (isTricky && tricky != 0.0f)
        return tricky;
    return global;
}
} // namespace ps2_ssx3_lod
