#pragma once

#include "ps2_stubs.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ps2_stubs
{
    void PadSyncCallback(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void scePadEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void scePadEnterPressMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void scePadExitPressMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void scePadGetButtonMask(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void scePadGetDmaStr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void scePadGetFrameCount(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void scePadGetModVersion(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void scePadGetPortMax(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void scePadGetReqState(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void scePadGetSlotMax(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void scePadGetState(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void scePadInfoAct(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void scePadInfoComb(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void scePadInfoMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void scePadInfoPressMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void scePadInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void scePadInit2(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void scePadPortClose(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void scePadPortOpen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void scePadRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void scePadReqIntToStr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void scePadSetActAlign(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void scePadSetActDirect(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void scePadSetButtonInfo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void scePadSetMainMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void scePadSetReqState(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void scePadSetVrefParam(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void scePadSetWarningLevel(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void scePadStateIntToStr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);

    static constexpr size_t kPadDebugPortCount = 2u;
    static constexpr size_t kPadDebugSlotCount = 1u;
    static constexpr size_t kPadDebugDataSize = 32u;

    struct PadDebugPortSnapshot
    {
        bool open = false;
        bool analogMode = false;
        bool pressureEnabled = false;
        bool lastUsedOverride = false;
        bool lastUsedBackend = false;
        bool lastReadOk = false;
        uint16_t buttonMask = 0xFFFFu;
        uint16_t lastButtons = 0xFFFFu;
        uint32_t dmaAddr = 0u;
        uint32_t reqState = 0u;
        uint32_t readCount = 0u;
        uint32_t lastReadDataAddr = 0u;
        uint8_t rx = 0x80u;
        uint8_t ry = 0x80u;
        uint8_t lx = 0x80u;
        uint8_t ly = 0x80u;
        uint8_t lastData[kPadDebugDataSize]{};
    };

    struct PadDebugSnapshot
    {
        bool overrideEnabled = false;
        uint16_t overrideButtons = 0xFFFFu;
        uint8_t overrideRx = 0x80u;
        uint8_t overrideRy = 0x80u;
        uint8_t overrideLx = 0x80u;
        uint8_t overrideLy = 0x80u;
        int readLogCount = 0;
        PadDebugPortSnapshot ports[kPadDebugPortCount][kPadDebugSlotCount]{};
    };

    PadDebugSnapshot getPadDebugSnapshot();
    void setPadOverrideState(uint16_t buttons, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry);
    void clearPadOverrideState();

    // E31 DEV-ONLY scripted pad input (PS2X_PAD_SCRIPT). One parsed entry:
    // press `pressMask` (active-low clear mask, 0 = no buttons) and/or drive
    // the flagged analog axes while atMs <= nowMs < atMs + holdMs, where
    // nowMs is milliseconds since the first pad call (wall clock), or guest
    // milliseconds (vsyncTick * 1000/59.94) when PS2X_PAD_SCRIPT_CLOCK=vsync.
    struct PadScriptEntry
    {
        uint64_t atMs = 0u;
        uint64_t holdMs = 0u;
        uint16_t pressMask = 0u;
        bool hasLx = false;
        bool hasLy = false;
        bool hasRx = false;
        bool hasRy = false;
        uint8_t lx = 0x80u;
        uint8_t ly = 0x80u;
        uint8_t rx = 0x80u;
        uint8_t ry = 0x80u;
    };

    // Parses "t_ms:spec:hold_ms,..." where spec is '+'-joined button names
    // (select/l3/r3/start/up/right/down/left/l2/r2/l1/r1/triangle/circle/
    // cross/square) and/or axis assignments (lx/ly/rx/ry = 0..255).
    // Returns false (entries untouched) on any malformed entry.
    bool parsePadScript(const char *spec, std::vector<PadScriptEntry> &entries);

    // Test hooks. Install a script without the env var, drive its clock
    // explicitly, and restore the default-off state. Production code paths
    // never call these.
    bool setPadScriptForTest(const char *spec);
    void setPadScriptNowMsForTest(uint64_t nowMs);
    // E33: select the guest-vsync clock (vsyncTick * 1000/59.94 ms) instead
    // of the wall clock, and drive the tick explicitly.
    void setPadScriptVsyncClockForTest(bool vsyncClock);
    void setPadScriptVsyncTickForTest(uint64_t tick);
    void clearPadScriptForTest();
}
