#pragma once

// PF1: a checkpoint that suspends the running guest thread inside nested
// generated code unwinds the host stack by returning false from every
// dispatchGuestBranch up to the EE scheduler. The post-call check can't tell
// that unwind apart from a normal return by ctx->pc alone: a callee that is
// suspended at a recursive call to its own entry leaves ctx->pc == its entry,
// which the check reads as "returned without setting pc". This flag marks the
// unwind so the check propagates it; the scheduler clears it before each
// top-level dispatch. One flag per host executor thread.
namespace ps2_guest_unwind
{
    inline bool &pendingFlag() noexcept
    {
        thread_local bool pending = false;
        return pending;
    }

    inline void mark() noexcept { pendingFlag() = true; }
    inline void clear() noexcept { pendingFlag() = false; }
    inline bool pending() noexcept { return pendingFlag(); }
}
