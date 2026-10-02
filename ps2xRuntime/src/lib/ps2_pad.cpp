#include "runtime/ps2_pad.h"
#include "ps2_host_backend.h"
#include "ps2_pad_latch.h"
#include "ps2_virtual_pad.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    constexpr uint8_t kPadAnalogMarker = 0x73;
    constexpr uint8_t kPadStickCenter = 0x80;

    constexpr uint16_t PAD_LEFT = 0x0080u;
    constexpr uint16_t PAD_DOWN = 0x0040u;
    constexpr uint16_t PAD_RIGHT = 0x0020u;
    constexpr uint16_t PAD_UP = 0x0010u;
    constexpr uint16_t PAD_START = 0x0008u;
    constexpr uint16_t PAD_R3 = 0x0004u;
    constexpr uint16_t PAD_L3 = 0x0002u;
    constexpr uint16_t PAD_SELECT = 0x0001u;
    constexpr uint16_t PAD_SQUARE = 0x8000u;
    constexpr uint16_t PAD_CROSS = 0x4000u;
    constexpr uint16_t PAD_CIRCLE = 0x2000u;
    constexpr uint16_t PAD_TRIANGLE = 0x1000u;
    constexpr uint16_t PAD_R1 = 0x0800u;
    constexpr uint16_t PAD_L1 = 0x0400u;
    constexpr uint16_t PAD_R2 = 0x0200u;
    constexpr uint16_t PAD_L2 = 0x0100u;
}

PSRaylibPadSample ps2xSampleRaylibPad()
{
    PSRaylibPadSample out;
#if defined(PS2X_IOS)
    // I25: a Bluetooth pad (MFi/Xbox/PS) need not land at index 0, so take
    // the first ready one; and union keyboard + gamepad (N6's Android
    // behaviour), so a hardware keyboard works alongside a controller.
    int kGamepad = 0;
    for (int i = 0; i < 4; ++i)
    {
        if (IsGamepadAvailable(i))
        {
            kGamepad = i;
            break;
        }
    }
    const bool useGamepad = IsGamepadAvailable(kGamepad);
    const bool useKeyboard = true;
#else
    constexpr int kGamepad = 0;
    const bool useGamepad = IsGamepadAvailable(kGamepad);
    const bool useKeyboard = !useGamepad;
#endif
    out.gamepad = kGamepad;
    out.useGamepad = useGamepad;
    uint16_t pressed = 0u;
    auto setBit = [&pressed](uint16_t mask)
    { pressed |= mask; };

    if (useGamepad)
    {
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_UP))
            setBit(PAD_UP);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_DOWN))
            setBit(PAD_DOWN);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_LEFT))
            setBit(PAD_LEFT);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_RIGHT))
            setBit(PAD_RIGHT);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_DOWN))
            setBit(PAD_CROSS);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT))
            setBit(PAD_CIRCLE);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_LEFT))
            setBit(PAD_SQUARE);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_UP))
            setBit(PAD_TRIANGLE);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_TRIGGER_1))
            setBit(PAD_L1);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_TRIGGER_1))
            setBit(PAD_R1);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_TRIGGER_2))
            setBit(PAD_L2);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_TRIGGER_2))
            setBit(PAD_R2);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_MIDDLE_RIGHT))
            setBit(PAD_START);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_MIDDLE_LEFT))
            setBit(PAD_SELECT);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_THUMB))
            setBit(PAD_L3);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_THUMB))
            setBit(PAD_R3);
    }
    if (useKeyboard)
    {
        if (IsKeyDown(KEY_UP) || IsKeyDown(KEY_W))
            setBit(PAD_UP);
        if (IsKeyDown(KEY_DOWN) || IsKeyDown(KEY_S))
            setBit(PAD_DOWN);
        if (IsKeyDown(KEY_LEFT) || IsKeyDown(KEY_A))
            setBit(PAD_LEFT);
        if (IsKeyDown(KEY_RIGHT) || IsKeyDown(KEY_D))
            setBit(PAD_RIGHT);
        if (IsKeyDown(KEY_X) || IsKeyDown(KEY_SPACE))
            setBit(PAD_CROSS);
        if (IsKeyDown(KEY_C) || IsKeyDown(KEY_ESCAPE))
            setBit(PAD_CIRCLE);
        if (IsKeyDown(KEY_Z) || IsKeyDown(KEY_KP_0))
            setBit(PAD_SQUARE);
        if (IsKeyDown(KEY_V) || IsKeyDown(KEY_KP_1))
            setBit(PAD_TRIANGLE);
        if (IsKeyDown(KEY_Q))
            setBit(PAD_L1);
        if (IsKeyDown(KEY_E))
            setBit(PAD_R1);
        if (IsKeyDown(KEY_LEFT_SHIFT))
            setBit(PAD_L2);
        if (IsKeyDown(KEY_RIGHT_SHIFT))
            setBit(PAD_R2);
        if (IsKeyDown(KEY_ENTER))
            setBit(PAD_START);
        if (IsKeyDown(KEY_TAB))
            setBit(PAD_SELECT);
    }
    out.pressed = pressed;
    return out;
}

bool PSPadBackend::readState(int port, int /*slot*/, uint8_t *data, size_t size)
{
    if (!data || size < 32)
        return false;

    std::memset(data, 0, 32);
    data[0] = 0x01;
    data[1] = kPadAnalogMarker;
    data[2] = 0xFF;
    data[3] = 0xFF;
    data[4] = data[5] = data[6] = data[7] = kPadStickCenter;

    uint16_t btns = 0xFFFFu;
    const PSRaylibPadSample ray = ps2xSampleRaylibPad();
    if (ray.useGamepad)
    {
        const int kGamepad = ray.gamepad;
        float lx = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_LEFT_X);
        float ly = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_LEFT_Y);
        float rx = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_RIGHT_X);
        float ry = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_RIGHT_Y);
        data[6] = static_cast<uint8_t>(128 + lx * 127);
        data[7] = static_cast<uint8_t>(128 + ly * 127);
        data[4] = static_cast<uint8_t>(128 + rx * 127);
        data[5] = static_cast<uint8_t>(128 + ry * 127);
    }

    // IN2: the latch path takes the render-thread-published mask (port 0
    // consumes one presented mask per guest read; other ports follow live
    // without advancing it). PS2X_PAD_LATCH=0 keeps the pre-IN2 direct
    // sampling (raylib buttons + liveMask) for A/B.
    if (ps2x::padlatch::latchEnabled())
    {
        const uint16_t latched = (port == 0) ? ps2x::padlatch::sharedLatch().consume()
                                             : ps2x::padlatch::sharedLatch().peekLive();
        btns &= static_cast<uint16_t>(~latched);
    }
    else
    {
        btns &= static_cast<uint16_t>(~ray.pressed);
        // I26: on-screen virtual controls (iOS overlay; always 0 elsewhere).
        btns &= static_cast<uint16_t>(~ps2x::vpad::liveMask().load(std::memory_order_relaxed));
    }

    // I32: the virtual analog stick overrides the left stick while the
    // overlay drives it (kStickNoOverride while the overlay is off or
    // hidden behind a physical controller).
    {
        const uint16_t vst = ps2x::vpad::liveStick().load(std::memory_order_relaxed);
        if (vst != ps2x::vpad::kStickNoOverride)
        {
            data[6] = static_cast<uint8_t>(vst & 0xFFu);
            data[7] = static_cast<uint8_t>(vst >> 8);
            // DEV-ONLY: with PS2X_VPAD_TEST_STICK set, log each distinct
            // injected value as it lands in the pad bytes (the Simulator
            // proof run; unset everywhere else, so production stays quiet).
            static const bool s_logStick = std::getenv("PS2X_VPAD_TEST_STICK") != nullptr;
            if (s_logStick)
            {
                static uint16_t s_last = ps2x::vpad::kStickNoOverride;
                if (vst != s_last)
                {
                    s_last = vst;
                    std::fprintf(stderr, "[vpad] stick pad bytes lx=0x%02x ly=0x%02x\n", data[6], data[7]);
                }
            }
        }
    }

    data[2] = static_cast<uint8_t>(btns & 0xFF);
    data[3] = static_cast<uint8_t>(btns >> 8);
    return true;
}
