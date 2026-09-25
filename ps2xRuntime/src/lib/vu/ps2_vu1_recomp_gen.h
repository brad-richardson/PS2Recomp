#ifndef PS2_VU1_RECOMP_GEN_H
#define PS2_VU1_RECOMP_GEN_H

// VR1: everything a generated VU1 image (vu1_<hash>.cpp) needs: the
// always-inline pair step and the upper/lower executors.

#include "runtime/ps2_vu1.h"
#include "runtime/gs/ps2_gif_arbiter.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"
#include "ps2_vu1_step_impl.h"
#include "ps2_vu1_upper_impl.h"
#include "ps2_vu1_lower_impl.h"

// A generated pair hands off to the next one with a guaranteed tail call, so
// a chain of pairs never grows the stack. This header is included only by
// generated images, so a compiler without musttail must fail here rather
// than silently emit nesting chains (Odin S1 stack overflow).
#if defined(__clang__)
#define PS2X_VU1_MUSTTAIL [[clang::musttail]]
#else
#error "Generated VU1 images require [[clang::musttail]]; plain tail calls nest one frame per pair"
#endif

#endif
