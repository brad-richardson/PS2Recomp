#pragma once

// T18 runtime EE-syscall trace channel (+ T22 pc= field).
//
// Emits one line per EE SYSCALL dispatch in PCSX2 EE.Bios byte shape:
//   [<wall-s, %8.4f>] Bios    : Bios call: <NAME> (<hex>)
//
// T22: when PS2X_TRACE_SYSCALLS_PC is set (non-empty), each line gains the
// dispatch pc in the sibling-diag shape (cf. Dispatcher.cpp dropArgs):
//   [<wall-s, %8.4f>] Bios    : Bios call: <NAME> (<hex>) pc=0x<hex>
// Unset/empty (default) = T18 byte shape unchanged.
//
// Gated on PS2X_TRACE_SYSCALLS=<path> (unset/empty = off; callers pay one
// cached-bool check). Additive only: no guest-state writes, no control-flow
// changes. Emission point is the top of dispatchNumericSyscall, matching
// PCSX2 (log before the switch).

#include <cstdint>

namespace ps2_syscalls
{
    void traceChannelEmit(uint32_t syscallId, uint32_t callerPc);
}
