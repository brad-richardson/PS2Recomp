// VX1: the VU0 micro-mode engine (see ps2_vu0.h). Everything else is the
// shared engine, VuCore<VU0Interpreter> (ps2_vu_core.cpp and friends).

#include "runtime/ps2_vu0.h"

VU0Interpreter::VU0Interpreter()
{
    reset();
}

void VU0Interpreter::resetForVu0Start()
{
#if PS2X_ENABLE_DET_HASH_TAP
    m_programStartCount = 0;
#endif
    m_cycle = 0;
}

void VU0Interpreter::execute(uint8_t *vuCode, uint32_t codeSize,
                             uint8_t *vuData, uint32_t dataSize,
                             GS &gs, PS2Memory *memory,
                             uint32_t startPC, uint32_t top, uint32_t itop,
                             uint32_t maxCycles)
{
    beginProgram(startPC, top, itop);
    run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
}

void VU0Interpreter::resume(uint8_t *vuCode, uint32_t codeSize,
                            uint8_t *vuData, uint32_t dataSize,
                            GS &gs, PS2Memory *memory,
                            uint32_t top, uint32_t itop, uint32_t maxCycles)
{
    beginResume(top, itop);
    run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
}
