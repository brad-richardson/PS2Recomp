#ifndef PS2_VU0_H
#define PS2_VU0_H

// VX1: the VU0 micro-mode engine (VCALLMS/VCALLMSR). VU0 is a subset of the
// shared engine (VuCore): 4 KiB micro memory (pc mask 0xFFF), MFP, XGKICK,
// the EFU ops and WAITP decode as reserved, no PATH1, no dev traces and no
// stage-4 blocks. Generated VU0 images (VR3, PS2X_VU0_RECOMP_DIR,
// vu0e_<hash>.cpp) run through issuePair like VU1's.

#include <cstdint>

#include "runtime/ps2_vu_core.h"

class VU0Interpreter final : public VuCore<VU0Interpreter>
{
    friend class VuCore<VU0Interpreter>;
    friend struct VuSavestate;
    template <uint64_t>
    friend struct VU0RecompImage;

public:
    static constexpr VuUnit kUnit = VuUnit::VU0;

    VU0Interpreter();

    // VR3: the part of reset() a VU0 start still needs (m_cycle, the program
    // count). The runtime's VU0 start rewrites all of m_state itself and
    // execute() resets the scheduler, so reset()'s m_state memset and
    // resetScheduler() were dead there.
    void resetForVu0Start();

    void execute(uint8_t *vuCode, uint32_t codeSize,
                 uint8_t *vuData, uint32_t dataSize,
                 GS &gs, PS2Memory *memory = nullptr,
                 uint32_t startPC = 0, uint32_t top = 0, uint32_t itop = 0,
                 uint32_t maxCycles = 65536);

    // Continues after a budget cut (tests; the runtime always restarts VU0
    // with execute()).
    void resume(uint8_t *vuCode, uint32_t codeSize,
                uint8_t *vuData, uint32_t dataSize,
                GS &gs, PS2Memory *memory = nullptr,
                uint32_t top = 0, uint32_t itop = 0, uint32_t maxCycles = 65536);
};

#endif
