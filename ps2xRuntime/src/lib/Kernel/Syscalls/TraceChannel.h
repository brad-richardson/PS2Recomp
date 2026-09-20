#pragma once

// T18 runtime EE-syscall trace channel.
//
// Emits one line per EE SYSCALL dispatch in PCSX2 EE.Bios byte shape:
//   [<wall-s, %8.4f>] Bios    : Bios call: <NAME> (<hex>)
//
// Gated on PS2X_TRACE_SYSCALLS=<path> (unset/empty = off; callers pay one
// cached-bool check). Additive only: no guest-state writes, no control-flow
// changes. Emission point is the top of dispatchNumericSyscall, matching
// PCSX2 (log before the switch).

#include <cstdint>

namespace ps2_syscalls
{
    void traceChannelEmit(uint32_t syscallId);
}
