#ifndef PS2_VU_RECOMP_GEN_COMMON_H
#define PS2_VU_RECOMP_GEN_COMMON_H

// VX1: what every generated VU image (VU0 and VU1) needs: the always-inline
// pair step and the upper/lower executors of the shared engine (VuCore).
// BT1: GS/memory full types stay out on purpose (forward decls in
// ps2_vu_core.h suffice; nothing here calls them), so GS churn doesn't
// invalidate the generated images in ccache.

#include "ps2_vu_step_impl.h"
#include "ps2_vu_upper_impl.h"
#include "ps2_vu_lower_impl.h"

// A generated pair hands off to the next one with a guaranteed tail call, so
// a chain of pairs never grows the stack. These headers are included only by
// generated images, so a compiler without musttail must fail here rather
// than silently emit nesting chains (Odin S1 stack overflow).
#if defined(__clang__)
#define PS2X_VU_MUSTTAIL [[clang::musttail]]
// VR2 2D: a block's body stays out of line so its entry trampoline (the guard)
// needs no stack frame when the guard fails.
#define PS2X_VU_NOINLINE [[clang::noinline]]
#else
#error "Generated VU images require [[clang::musttail]]; plain tail calls nest one frame per pair"
#endif

#endif
