#pragma once
// ACH2 (+QSR1): tiny reusable toast (one line of text, ~3 s, non-blocking).
//
// ps2x::ui::toast(text, seconds) from any thread; the render thread draws the
// live toast in the overlay pass (ps2_runtime.cpp, next to the DS1 status
// line: host-drawn over the presented frame, never into the guest frame).
// Latest toast wins; at most one is ever shown. Bursty callers queue on
// their side (ps2_ach's ToastQueue paces unlocks one at a time, ACH4).
// Output-only: no guest state is read or written, so det hashes are
// unaffected.
//
// Android mirrors to the Java Toast as well (Ux1Toast, like DS2/DS1's UX1
// mirror): the play config skips all GL (skipGl), so the overlay draw is
// invisible there. Where GL runs (iOS always, Mac, Android dev/underlay)
// the overlay draw shows it.

#include <chrono>
#include <mutex>
#include <string>

#include "ps2_android_toast.h" // no-op unless __ANDROID__

namespace ps2x::ui
{

namespace detail
{
struct Slot
{
    std::mutex mutex;
    std::string text;
    std::chrono::steady_clock::time_point expiry{};
    bool live = false;
};
inline Slot &slot()
{
    static Slot s;
    return s;
}
} // namespace detail

// Show a toast for `seconds` (any thread; latest wins; empty text clears).
inline void toast(const std::string &text, float seconds = 3.0f)
{
    detail::Slot &s = detail::slot();
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (text.empty() || !(seconds > 0.0f))
        {
            s.live = false;
            s.text.clear();
        }
        else
        {
            s.text = text;
            s.expiry = std::chrono::steady_clock::now() +
                       std::chrono::milliseconds(static_cast<long long>(seconds * 1000.0f));
            s.live = true;
        }
    }
#if defined(__ANDROID__)
    // UX1 mirror (DS1 pattern): the play config skips GL, so the overlay
    // draw below is invisible there; the Java Toast carries it instead.
    if (!text.empty() && seconds > 0.0f)
        ps2x::postQuickStatusToast(text);
#else
    (void)0;
#endif
}

// Render thread: the live toast text, or false when none (expired toasts
// clear). No-op cost when idle (one mutex + one flag).
inline bool pollToast(std::string &textOut)
{
    detail::Slot &s = detail::slot();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.live)
        return false;
    if (std::chrono::steady_clock::now() >= s.expiry)
    {
        s.live = false;
        s.text.clear();
        return false;
    }
    textOut = s.text;
    return true;
}

inline void clearToastForTest()
{
    detail::Slot &s = detail::slot();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.live = false;
    s.text.clear();
}

} // namespace ps2x::ui
