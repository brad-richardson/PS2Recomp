// SPDX-License-Identifier: GPL-3.0-or-later OR MIT
// full120 slice 5: zeroed copies of the module-owned post-call state for
// a no-module binary (the runtime sees the state through extern
// declarations). This TU must stay its OWN archive member (never merged
// into game_module_none.cpp or any other object): on some link orders
// (Android) the archive is scanned before the game module, and pulling a
// member that also defines linkedModule() would collide with attach glue.
//
// The definitions are weak so every order links: a module-attached binary
// takes the module's strong definitions (no duplicate error even when
// this member is pulled first); a no-module binary keeps these. Clang and
// GCC (ELF + Mach-O) both honor weak definitions; there is no MSVC target.
#include "ps2_fh1_full120.h"

namespace ps2_fh1
{
__attribute__((weak)) PostCall g_post;
__attribute__((weak)) bool g_postArmed = false;
__attribute__((weak)) PubRecord g_pub;
__attribute__((weak)) bool g_pubArmed = false;
__attribute__((weak)) NormcapPost g_normcap;
__attribute__((weak)) bool g_normcapArmed = false;
__attribute__((weak)) NormcapSlot g_normcapSlots[kNormcapSlots];
__attribute__((weak)) NormcapOwner g_normcapOwners[kNormcapSlots];
} // namespace ps2_fh1
