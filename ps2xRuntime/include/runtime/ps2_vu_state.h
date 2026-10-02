#ifndef PS2_VU_STATE_H
#define PS2_VU_STATE_H

#include <cstdint>

// VX1: the VU register file, shared by every VU engine and by the code that
// treats it as canonical VU state: the static interpreters (VU0 and VU1),
// microVU/OM1 (ps2_microvu import/export), savestates (written as raw bytes:
// the layout below is part of the savestate format, keep it byte-identical)
// and the VU0 macro-mode copies in ps2_runtime.cpp. Was VU1State in
// ps2_vu1.h; moved verbatim.
struct VuState
{
    float vf[32][4];
    int32_t vi[16];
    float acc[4];
    float q;
    float p;
    float i;
    uint32_t r;
    uint32_t pc;
    uint32_t mac;
    uint32_t clip;
    uint32_t status;
    uint64_t cycles;
    bool ebit;
    bool haltAfterDelaySlot;
    bool dBitEnabled;
    bool tBitEnabled;
    bool stoppedByD;
    bool stoppedByT;
    uint32_t top;  // VIF TOP visible to XTOP
    uint32_t itop; // VIF ITOP visible to XITOP

    bool branchPending;
    uint32_t branchTarget;
    uint32_t branchDelay;
};

// Savestate format guard: the "vu0"/"vu1" sections write VuState as raw
// bytes. A layout change is a savestate format change (bump kVuVersion).
static_assert(sizeof(VuState) == 664, "VuState layout is part of the savestate format");

// VX1: old name, kept for branches in flight. New code uses VuState.
using VU1State = VuState;

#endif
