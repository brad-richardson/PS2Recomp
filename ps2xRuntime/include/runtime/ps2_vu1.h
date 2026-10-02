#ifndef PS2_VU1_H
#define PS2_VU1_H

// VX1: the VU1 engine on the shared core (ps2_vu_core.h); this class adds
// what only VU1 has: XGKICK/PATH1 to the GIF. VX2: interpret-only (the
// generated VU1 images, VR2 blocks, VF1 flag elision and the E36/E37/VRB1
// dev traces are gone). Play runs VU1 on microVU; this engine is
// PS2X_VU1_ENGINE=static (interpreter, for debugging), the iOS OM1 MISS
// fallback and the canonical register file (state()) microVU and savestates
// read.

#include <array>
#include <cstddef>
#include <cstdint>

#include "runtime/ps2_vu_core.h"

class VU1Interpreter final : public VuCore<VU1Interpreter>
{
    friend class VuCore<VU1Interpreter>;
    friend struct VuSavestate;

public:
    static constexpr VuUnit kUnit = VuUnit::VU1;

    VU1Interpreter();

    void execute(uint8_t *vuCode, uint32_t codeSize,
                 uint8_t *vuData, uint32_t dataSize,
                 GS &gs, PS2Memory *memory = nullptr,
                 uint32_t startPC = 0, uint32_t top = 0, uint32_t itop = 0,
                 uint32_t maxCycles = 65536);

    void resume(uint8_t *vuCode, uint32_t codeSize,
                uint8_t *vuData, uint32_t dataSize,
                GS &gs, PS2Memory *memory = nullptr,
                uint32_t top = 0, uint32_t itop = 0, uint32_t maxCycles = 65536);

    // Savestate format: the vu0 section writes this many zero bytes where the
    // vu1 section writes m_xgkick (VuSavestate; layout unchanged by VX1).
    static constexpr size_t kXgkickSavestateBytes = 0x10000u + 40u;

private:
    struct XgkickPipeline
    {
        static constexpr uint32_t kBufferSize = 0x10000u;
        std::array<uint8_t, kBufferSize> packet{};
        uint32_t sourceAddress = 0;
        uint32_t totalBytes = 0;
        uint32_t copiedBytes = 0;
        uint32_t currentTagEnd = 0;
        uint32_t cycleCredit = 0;
        uint64_t issueCycle = 0;
        bool active = false;
        bool currentTagEop = false;

        // NP1: per-execute reset used to value-init the whole struct (a 64 KiB
        // memset on every VU1/VU0 program). The packet bytes need no clearing:
        // every read is of this-transfer data (the tag parse reads the qword
        // just written; submit sends [0, totalBytes), all written after the
        // reset), and no tap hashes the staging buffer. Scalars only.
        void reset()
        {
            sourceAddress = 0;
            totalBytes = 0;
            copiedBytes = 0;
            currentTagEnd = 0;
            cycleCredit = 0;
            issueCycle = 0;
            active = false;
            currentTagEop = false;
        }
    };
    static_assert(sizeof(XgkickPipeline) == kXgkickSavestateBytes, "savestate xgkick blob size");

    XgkickPipeline m_xgkick{};

    // XGKICK/PATH1 (called from the shared executors, VU1 only).
    void startXgkick(uint32_t qwordAddress);
    void progressXgkick();
    void finishXgkick();
};

#endif
