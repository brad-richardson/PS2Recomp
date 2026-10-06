#pragma once

// DSP1: EE branch dispatch and checkpoint fast paths, PS2X_EE_DISPATCH_FAST
// (default on; 0 = today's exact out-of-line paths; exact refactor, same charges, same Count/IRQ service points,
// same hook calls and order, same binding reads, same resume behaviour).
//
// - eeCheckpointDue: knob on, the generated backward edge runs the
//   scheduler's inline checkpoint directly (no eeCheckpointDue ->
//   EeScheduler::checkpointDue call pair). split120 half mode keeps the
//   out-of-line path (its restart suppressor runs there).
// - dispatchGuestBranch: knob on, a lean front runs the common case (no
//   dispatch-side hook can act, the target has a generated function) and
//   hands everything else to the unchanged path (dispatchGuestBranchFull)
//   before any mutation. See ps2_runtime.cpp (DSP1).
//
// Included at the end of runtime/ee_scheduler.h (itself included at the end
// of ps2_runtime.h), so PS2Runtime and EeScheduler are complete here and
// every TU that calls eeCheckpointDue (the generated sources included) sees
// the inline body. Do not include it directly.

#include "runtime/ee_guest_unwind.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Call-site force-inline (clang statement attribute); a plain call where
// the compiler lacks it (same behaviour, one more call).
#if defined(__clang__) && __clang_major__ >= 18
#define PS2X_DSP1_INLINE_CALL [[clang::always_inline]]
#else
#define PS2X_DSP1_INLINE_CALL
#endif
#if defined(__clang__) || defined(__GNUC__)
#define PS2X_DSP1_ALWAYS_INLINE __attribute__((always_inline))
#else
#define PS2X_DSP1_ALWAYS_INLINE
#endif

namespace ps2_dsp1
{
// Set once at runtime construction from PS2X_EE_DISPATCH_FAST (unset/1 = on;
// 0 = the exact out-of-line paths). A plain load on every checkpoint.
inline bool g_fast = false;
// g_fast minus split120 (PS2X_SSX3_SIM_MODE=split120_render60_v1, the only
// mode where ps2_ts2_split60::halfMode() can be true): that mode keeps the
// out-of-line checkpoint and its restart suppressor.
inline bool g_fastCheckpoint = false;

inline bool knobFromEnv() noexcept
{
    const char *v = std::getenv("PS2X_EE_DISPATCH_FAST");
    if (!v || !*v || std::strcmp(v, "1") == 0)
        return true;
    if (std::strcmp(v, "0") == 0)
        return false;
    std::fprintf(stderr, "dsp1-refused PS2X_EE_DISPATCH_FAST=%s (want 0|1)\n", v);
    std::abort();
}

} // namespace ps2_dsp1

inline void PS2Runtime::markGuestUnwind() noexcept
{
    ps2_guest_unwind::mark();
    m_guestUnwindMarked = true;
}

inline void PS2Runtime::clearGuestUnwind() noexcept
{
    ps2_guest_unwind::clear();
    m_guestUnwindMarked = false;
}

inline bool PS2Runtime::guestUnwindPending() const noexcept
{
    // m_guestUnwindMarked is set at every mark() site of this runtime and
    // cleared only with the flag itself (one executor thread per runtime),
    // so it is a superset of pending(): false skips the TLS read exactly.
    return m_guestUnwindMarked && ps2_guest_unwind::pending();
}

// Forced inline: the generated functions are far above the inliner's size
// threshold, so a plain inline still leaves one call per backward edge.
PS2X_DSP1_ALWAYS_INLINE inline bool PS2Runtime::eeCheckpointDue(uint32_t cycles) noexcept
{
    if (m_fh32Preview)
    {
        if (m_fh32PreviewBudget) { --m_fh32PreviewBudget; return false; }
        m_fh32PreviewFailed = true;
        return true; // bounded private evaluation, no live guest unwind
    }
    if (!ps2_dsp1::g_fastCheckpoint)
        return eeCheckpointDueSlow(cycles);
    // Same sequence as eeCheckpointDueSlow with half mode off; the
    // scheduler's fast path is forced inline here only (the generated
    // backward edge otherwise keeps an out-of-line call).
    bool due;
    PS2X_DSP1_INLINE_CALL due = m_eeScheduler->checkpointDue(cycles);
    if (!due)
        return false;
    markGuestUnwind();
    return true;
}
