#ifndef PS2_PAD_H
#define PS2_PAD_H

#include <cstddef>
#include <cstdint>

class PSPadBackend
{
public:
    PSPadBackend() = default;
    ~PSPadBackend() = default;

    bool readState(int port, int slot, uint8_t *data, size_t size);
};

// IN2: raylib gamepad/keyboard button selection + sampling, shared by the
// render-thread latch publisher (PS2Runtime::run) and the PS2X_PAD_LATCH=0
// readState path. `pressed` is active-high PS2 button bits (1 = pressed).
struct PSRaylibPadSample
{
    uint16_t pressed = 0u;
    int gamepad = 0;
    bool useGamepad = false;
};
PSRaylibPadSample ps2xSampleRaylibPad();

#endif
