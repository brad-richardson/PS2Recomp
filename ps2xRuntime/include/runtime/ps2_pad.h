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
// render-thread latch publisher (PS2Runtime::run) and readState. `pressed` is active-high PS2 button bits (1 = pressed).
struct PSRaylibPadSample
{
    uint16_t pressed = 0u;
    int gamepad = 0;
    bool useGamepad = false;
};
PSRaylibPadSample ps2xSampleRaylibPad();

// DS1: quick-save/load chord (SELECT+L3 save, SELECT+R3 load), edge-triggered
// on the thumb press while SELECT is held. Pure step function (unit-tested);
// the shell strips the chord bits from the guest mask while engaged.
struct PSChordState
{
    bool l3Was = false;
    bool r3Was = false;
};
inline void psChordStep(PSChordState &st, bool selectDown, bool l3Down, bool r3Down, bool &saveEdge,
                        bool &loadEdge)
{
    saveEdge = selectDown && l3Down && !st.l3Was;
    loadEdge = selectDown && r3Down && !st.r3Was;
    st.l3Was = l3Down;
    st.r3Was = r3Down;
}

#endif
