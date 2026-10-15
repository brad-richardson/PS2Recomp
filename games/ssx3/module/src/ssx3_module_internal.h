#pragma once

// module-seam-1: apply functions the SSX 3 module table (ssx3_module.cpp)
// registers. Each one used to be a PS2_REGISTER_GAME_OVERRIDE in a runtime TU.

class PS2Runtime;

namespace ps2_ssx3_anim_pick
{
void applyAnimPickGuard(PS2Runtime &runtime); // "ssx3-anim-pick-guard" (HNG2/HNG3)
}

namespace ps2_ssx3_hle_iop
{
void applySsx3HleIop(PS2Runtime &runtime); // "ssx3-hle-iop" (UPR1)
}

namespace ps2_syscalls
{
// "ssx3-copied-payload" (K1); its syscall hooks are in Ssx3CopiedPayload.h.
void applySsx3CopiedPayload(PS2Runtime &runtime);
}
