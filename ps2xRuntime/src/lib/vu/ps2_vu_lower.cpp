#include "runtime/ps2_vu0.h"
#include "runtime/ps2_vu1.h"
#include "ps2_vu_lower_impl.h"

template <class D>
void VuCore<D>::execLower(uint32_t instr, uint8_t *vuData, uint32_t dataSize, GS &gs, PS2Memory *memory, uint32_t upperInstr)
{
    execLowerImpl(instr, vuData, dataSize, gs, memory, upperInstr);
}

template void VuCore<VU0Interpreter>::execLower(uint32_t, uint8_t *, uint32_t, GS &, PS2Memory *, uint32_t);
template void VuCore<VU1Interpreter>::execLower(uint32_t, uint8_t *, uint32_t, GS &, PS2Memory *, uint32_t);
