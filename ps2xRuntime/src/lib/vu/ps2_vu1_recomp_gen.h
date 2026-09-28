#ifndef PS2_VU1_RECOMP_GEN_H
#define PS2_VU1_RECOMP_GEN_H

// VR1: everything a generated VU1 image (vu1_<hash>.cpp) needs: the
// always-inline pair step and the upper/lower executors.
// BT1: GS/memory full types stay out on purpose (forward decls in
// ps2_vu1.h suffice; nothing here calls them), so GS churn doesn't
// invalidate the generated images in ccache.

#include "runtime/ps2_vu1.h"
#include "ps2_vu1_step_impl.h"
#include "ps2_vu1_upper_impl.h"
#include "ps2_vu1_lower_impl.h"

// A generated pair hands off to the next one with a guaranteed tail call, so
// a chain of pairs never grows the stack. This header is included only by
// generated images, so a compiler without musttail must fail here rather
// than silently emit nesting chains (Odin S1 stack overflow).
#if defined(__clang__)
#define PS2X_VU1_MUSTTAIL [[clang::musttail]]
// VR2 2D: a block's body stays out of line so its entry trampoline (the guard)
// needs no stack frame when the guard fails.
#define PS2X_VU1_NOINLINE [[clang::noinline]]
#else
#error "Generated VU1 images require [[clang::musttail]]; plain tail calls nest one frame per pair"
#endif

#endif
