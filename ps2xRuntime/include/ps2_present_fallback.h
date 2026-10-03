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
    // after launch). Always black (the DEV-ONLY PS2X_FALLBACK_MAGENTA=1
    // magenta override is deleted).
    inline FallbackRgba fallbackFrameColor()
    {
        return FallbackRgba{0u, 0u, 0u, 255u};
    }
}
