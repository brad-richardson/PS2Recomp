#pragma once

#include <cstdint>
#include <cstdlib>

namespace ps2x
{
    struct FallbackRgba
    {
        uint8_t r, g, b, a;
    };

    // I26: colour presented while no guest frame exists yet (the first ~1 s
    // after launch). Black by default; PS2X_FALLBACK_MAGENTA=1 (dev only)
    // restores the old magenta so "no frame" stays visible when debugging.
    inline FallbackRgba fallbackFrameColor(const char *magentaEnv)
    {
        const bool magenta = magentaEnv && magentaEnv[0] != '\0' && magentaEnv[0] != '0';
        return magenta ? FallbackRgba{255u, 0u, 255u, 255u} : FallbackRgba{0u, 0u, 0u, 255u};
    }

    inline FallbackRgba fallbackFrameColorFromEnv()
    {
        return fallbackFrameColor(std::getenv("PS2X_FALLBACK_MAGENTA"));
    }
}
