#pragma once

#include <cstddef>
#include <cstring>

// ctrl-harden-1: physical-controller -> pad-port assignment (pure; unit-tested
// in ps2_controller_map_tests.cpp). The render thread rebuilds availability
// from raylib every host frame and the pad backend samples per port, so the
// mapping is live-evaluated with no relaunch:
//
// - pad 1 (guest port 0) is driven by the FIRST available raylib slot;
// - pad 2 (guest port 1) is driven by the SECOND available raylib slot;
// - with one controller, pad 2 gets no input (Brad 10-17: one controller
//   drove both riders because port 2 peeked port 1's live latch);
// - on disconnect the survivor promotes to pad 1 and pad 2 goes silent;
//   a reconnected or new controller takes pad 1 again immediately;
// - touch overlay, keyboard and pad-script behaviour per port is decided by
//   the callers (pad 1 owns touch/keyboard; see PSPadBackend::readState).
//
// Slot order is raylib's (SDL joystick index on iOS/Mac, the single merged
// slot 0 on Android, where a second controller cannot be separated — pad 2
// stays silent there; see the ANDROID note in ps2_pad.cpp).
namespace ps2x::ctrlmap
{
// raylib gamepad slots (MAX_GAMEPADS). Slots >= kSlots never exist; the
// callers size their availability array with this.
constexpr int kSlots = 4;

// Index of the slot driving pad 1, or -1 when no controller is connected.
inline int pad1Slot(const bool *avail, int n)
{
    if (!avail)
        return -1;
    for (int i = 0; i < n; ++i)
    {
        if (avail[i])
            return i;
    }
    return -1;
}

// Index of the slot driving pad 2, or -1 with fewer than two controllers.
inline int pad2Slot(const bool *avail, int n)
{
    if (!avail)
        return -1;
    bool skipped = false;
    for (int i = 0; i < n; ++i)
    {
        if (!avail[i])
            continue;
        if (!skipped)
        {
            skipped = true;
            continue;
        }
        return i;
    }
    return -1;
}

// iOS overlay rule (pure part): a connected controller hides the virtual pad
// (Touch = Auto) only when it looks real. The iOS Simulator reports an
// always-connected fake device named "Gamepad" with nothing attached, so
// that name (or a missing/empty one) never hides the overlay on its own; a
// first real press still hides via gamepadInUse, as before.
inline bool hidesOverlay(const char *name)
{
    if (!name || name[0] == '\0')
        return false;
    return std::strcmp(name, "Gamepad") != 0;
}
} // namespace ps2x::ctrlmap
