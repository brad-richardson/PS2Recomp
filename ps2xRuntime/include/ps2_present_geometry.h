#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string_view>

// I26 (G46 §4): host presentation of the guest frame. A PS2 frame (e.g.
// 512x448) is shown on a 4:3 TV, so by default the picture is drawn at a 4:3
// display aspect, not at the framebuffer's pixel aspect (8:7 for 512x448).
// SSX 3's anamorphic option uses a 16:9 display aspect. PS2X_ASPECT overrides
// either default with native, 4:3, or 16:9.
// Filtering: raylib's default is POINT, which at a non-integer scale (x2.946
// on an iPhone 16 Pro Max) makes glyph stems alternate 2/3 px; so BILINEAR
// unless both axis scales are whole numbers. PS2X_PRESENT_FILTER=point|bilinear
// forces one.
namespace ps2x::present
{
    enum class Aspect
    {
        FourThree,
        SixteenNine,
        Native
    };
    enum class Filter
    {
        Auto,
        Point,
        Bilinear
    };

    struct Rect
    {
        float x, y, w, h;
    };

    inline uint32_t ssx3WidescreenModeFromEnv(const char *value)
    {
        return value && std::string_view(value) == "0" ? 0u : 2u;
    }

    inline Aspect aspectFromEnv(const char *value, bool anamorphic = false)
    {
        if (value)
        {
            const std::string_view override(value);
            if (override == "native") return Aspect::Native;
            if (override == "4:3") return Aspect::FourThree;
            if (override == "16:9") return Aspect::SixteenNine;
        }
        return anamorphic ? Aspect::SixteenNine : Aspect::FourThree;
    }

    inline Filter filterFromEnv(const char *value)
    {
        if (value && std::string_view(value) == "point")
            return Filter::Point;
        if (value && std::string_view(value) == "bilinear")
            return Filter::Bilinear;
        return Filter::Auto;
    }

    // Largest rect of the chosen aspect centred in the screen.
    inline Rect presentRect(float screenW, float screenH, float srcW, float srcH, Aspect aspect)
    {
        srcW = std::max(1.0f, srcW);
        srcH = std::max(1.0f, srcH);
        float w, h;
        if (aspect == Aspect::Native)
        {
            const float scale = std::min(screenW / srcW, screenH / srcH);
            w = srcW * scale;
            h = srcH * scale;
        }
        else
        {
            const float ratio = aspect == Aspect::SixteenNine ? 16.0f / 9.0f : 4.0f / 3.0f;
            h = std::min(screenH, screenW / ratio);
            w = h * ratio;
        }
        return Rect{(screenW - w) * 0.5f, (screenH - h) * 0.5f, w, h};
    }

    inline bool isWhole(float v)
    {
        return v >= 1.0f && std::fabs(v - std::round(v)) < 1e-3f;
    }

    // scaleX/scaleY = drawn size / source size, in drawable pixels.
    inline bool useBilinear(Filter filter, float scaleX, float scaleY)
    {
        if (filter != Filter::Auto)
            return filter == Filter::Bilinear;
        return !(isWhole(scaleX) && isWhole(scaleY));
    }
}
