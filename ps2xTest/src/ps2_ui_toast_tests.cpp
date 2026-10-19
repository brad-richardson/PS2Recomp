#include "MiniTest.h"
#include "ps2_ui_toast.h"

#include <cstdlib>
#include <string>

// toast-audit-1: PS2X_DEV_TOASTS=1 gates diagnostic toasts; the default build
// (knob unset) shows only the player-facing keep set. Output-only: the polled
// strings are host-drawn, guest state is untouched.
void register_ps2_ui_toast_tests()
{
    MiniTest::Case("Ps2UiToast", [](TestCase &tc)
                   {
        tc.Run("dev toasts default off", [](TestCase &t)
               {
            ::unsetenv("PS2X_DEV_TOASTS");
            t.IsTrue(!ps2x::ui::devToastsEnabled(), "unset is off");
            ::setenv("PS2X_DEV_TOASTS", "0", 1);
            t.IsTrue(!ps2x::ui::devToastsEnabled(), "0 is off");
            ::setenv("PS2X_DEV_TOASTS", "", 1);
            t.IsTrue(!ps2x::ui::devToastsEnabled(), "empty is off");
            ::unsetenv("PS2X_DEV_TOASTS"); });

        tc.Run("dev toasts turn on with =1", [](TestCase &t)
               {
            ::setenv("PS2X_DEV_TOASTS", "1", 1);
            t.IsTrue(ps2x::ui::devToastsEnabled(), "=1 is on");
            ::unsetenv("PS2X_DEV_TOASTS");
            t.IsTrue(!ps2x::ui::devToastsEnabled(), "unset again is off"); });

        tc.Run("diagnostic toast hidden by default, shown with the knob", [](TestCase &t)
               {
            ::unsetenv("PS2X_DEV_TOASTS");
            ps2x::ui::clearToastForTest();
            ps2x::ui::toastDev("saving...", 60.0f);
            std::string msg;
            t.IsTrue(!ps2x::ui::pollToast(msg), "dev toast suppressed by default");
            ::setenv("PS2X_DEV_TOASTS", "1", 1);
            ps2x::ui::toastDev("saving...", 60.0f);
            t.IsTrue(ps2x::ui::pollToast(msg) && msg == "saving...", "dev toast shows with knob");
            ps2x::ui::clearToastForTest();
            ::unsetenv("PS2X_DEV_TOASTS"); });

        tc.Run("keep-set toast always shows", [](TestCase &t)
               {
            ::unsetenv("PS2X_DEV_TOASTS");
            ps2x::ui::clearToastForTest();
            ps2x::ui::toast("Quick save failed", 60.0f);
            std::string msg;
            t.IsTrue(ps2x::ui::pollToast(msg) && msg == "Quick save failed",
                     "keep toast shows by default");
            ::setenv("PS2X_DEV_TOASTS", "1", 1);
            ps2x::ui::clearToastForTest();
            ps2x::ui::toast("Quick save failed", 60.0f);
            t.IsTrue(ps2x::ui::pollToast(msg) && msg == "Quick save failed",
                     "keep toast shows with knob too");
            ps2x::ui::clearToastForTest();
            ::unsetenv("PS2X_DEV_TOASTS"); });

        tc.Run("notice card is not dev-gated", [](TestCase &t)
               {
            ::unsetenv("PS2X_DEV_TOASTS");
            ps2x::ui::clearNoticeForTest();
            ps2x::ui::notice("Quick Play result\n1st  You  2:31.40", 60.0f);
            std::string msg;
            t.IsTrue(ps2x::ui::pollNotice(msg) && msg == "Quick Play result\n1st  You  2:31.40",
                     "notice shows by default");
            ps2x::ui::clearNoticeForTest(); }); });
}
