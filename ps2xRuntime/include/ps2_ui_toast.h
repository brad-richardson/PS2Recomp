#pragma once
// Tiny reusable toast (one line of text, ~3 s, non-blocking).
//
// ps2x::ui::toast(text, seconds) from any thread; the render thread draws the
// live toast in the overlay pass (ps2_runtime.cpp, next to the DS1 status
// line: host-drawn over the presented frame, never into the guest frame).
// Latest toast wins; at most one is ever shown. Bursty callers queue on
// their side.
// Output-only: no guest state is read or written, so det hashes are
// unaffected.
//
// Android mirrors to the Java Toast as well (Ux1Toast, like DS2/DS1's UX1
// mirror): the play config skips all GL (skipGl), so the overlay draw is
// invisible there. Where GL runs (iOS always, Mac, Android dev/underlay)
// the overlay draw shows it.

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "ps2_android_toast.h" // no-op unless __ANDROID__

namespace ps2x::ui
{

// toast-audit-1: diagnostic toasts hide unless PS2X_DEV_TOASTS=1. Default 0,
// even in --dev builds (Brad runs a --dev build). Player-facing toasts use
// toast()/notice() directly and always show. Output-only: host display only.
inline bool devToastsEnabled()
{
    const char *v = std::getenv("PS2X_DEV_TOASTS");
    return v != nullptr && std::strcmp(v, "1") == 0;
}

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

// Diagnostic toast: shows only when PS2X_DEV_TOASTS=1, silent by default.
// Use for progress/jargon lines a player can't act on.
inline void toastDev(const std::string &text, float seconds = 3.0f)
{
    if (devToastsEnabled())
        toast(text, seconds);
}

namespace notice_detail
{
// A longer-lived multi-line card (online-default-1: first-run privacy
// notice). Same contract as the toast: any thread posts, the render
// thread draws, output-only, latest wins, at most one live.
struct Card
{
    std::mutex mutex;
    std::string text; // '\n'-separated lines, up to kNoticeMaxLines
    std::chrono::steady_clock::time_point expiry{};
    bool live = false;
};
inline Card &card()
{
    static Card c;
    return c;
}
} // namespace notice_detail

constexpr size_t kNoticeMaxLines = 4u;
constexpr float kNoticeSeconds = 12.0f;

// Post a multi-line notice card (empty text clears). Mirrors to the Java
// Toast on Android like toast() (the play config skips GL).
inline void notice(const std::string &text, float seconds = kNoticeSeconds)
{
    notice_detail::Card &c = notice_detail::card();
    {
        std::lock_guard<std::mutex> lock(c.mutex);
        if (text.empty() || !(seconds > 0.0f))
        {
            c.live = false;
            c.text.clear();
        }
        else
        {
            std::string clipped = text;
            size_t lines = 0u;
            for (size_t pos = 0; (pos = clipped.find('\n', pos)) != std::string::npos; ++pos)
            {
                if (++lines >= kNoticeMaxLines)
                {
                    clipped.resize(pos);
                    break;
                }
            }
            c.text = clipped;
            c.expiry = std::chrono::steady_clock::now() +
                       std::chrono::milliseconds(static_cast<long long>(seconds * 1000.0f));
            c.live = true;
        }
    }
#if defined(__ANDROID__)
    if (!text.empty() && seconds > 0.0f)
        ps2x::postQuickStatusToast(text);
#else
    (void)0;
#endif
}

// Render thread: the live notice text, or false when none.
inline bool pollNotice(std::string &textOut)
{
    notice_detail::Card &c = notice_detail::card();
    std::lock_guard<std::mutex> lock(c.mutex);
    if (!c.live)
        return false;
    if (std::chrono::steady_clock::now() >= c.expiry)
    {
        c.live = false;
        c.text.clear();
        return false;
    }
    textOut = c.text;
    return true;
}

inline void clearNoticeForTest()
{
    notice_detail::Card &c = notice_detail::card();
    std::lock_guard<std::mutex> lock(c.mutex);
    c.live = false;
    c.text.clear();
}

} // namespace ps2x::ui
