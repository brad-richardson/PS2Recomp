#include "MiniTest.h"
#include "ps2_diag_gate.h"

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

// F4 (appsec-fix-1, lightweight): public builds ignore diag-class file knobs
// unless PS2X_DEV=1; one line lists the ignored keys. Lab envs set PS2X_DEV=1.
void register_ps2_diag_gate_tests()
{
    MiniTest::Case("Ps2DiagGate", [](TestCase &tc)
                   {
        tc.Run("known diag knobs gate, play knobs pass", [](TestCase &t)
               {
            t.IsTrue(ps2x::diaggate::knobIsDiagGated("PS2X_NET_CAPTURE"), "capture gated");
            t.IsTrue(ps2x::diaggate::knobIsDiagGated("PS2X_NP_LINK_ENABLE"), "link gated");
            t.IsTrue(ps2x::diaggate::knobIsDiagGated("PS2X_GS_CAPTURE"), "gs capture gated");
            t.IsTrue(!ps2x::diaggate::knobIsDiagGated("PS2X_SOUND"), "sound passes");
            t.IsTrue(!ps2x::diaggate::knobIsDiagGated("PS2X_NET"), "net passes");
            t.IsTrue(!ps2x::diaggate::knobIsDiagGated("PS2X_NET_SERVER"), "server passes");
            t.IsTrue(!ps2x::diaggate::knobIsDiagGated("PS2X_DEV"), "gate key itself passes"); });

        tc.Run("split keeps diag out unless dev is open", [](TestCase &t)
               {
            const std::vector<std::pair<std::string, std::string>> entries{
                {"PS2X_SOUND", "1"},
                {"PS2X_NET_CAPTURE", "/tmp/cap"},
                {"PS2X_NP_LINK_ENABLE", "1"},
            };
            std::vector<std::pair<std::string, std::string>> kept;
            std::vector<std::string> ignored;
            ps2x::diaggate::splitDiagEntries(entries, false, &kept, &ignored);
            t.Equals(kept.size(), static_cast<size_t>(1), "one kept");
            t.Equals(ignored.size(), static_cast<size_t>(2), "two ignored");
            t.Equals(ps2x::diaggate::joinIgnored(ignored),
                     std::string("PS2X_NET_CAPTURE,PS2X_NP_LINK_ENABLE"), "ignored keys listed");
            kept.clear();
            ignored.clear();
            ps2x::diaggate::splitDiagEntries(entries, true, &kept, &ignored);
            t.Equals(kept.size(), static_cast<size_t>(3), "dev open keeps all");
            t.IsTrue(ignored.empty(), "dev open ignores none"); });

        tc.Run("dev opens from process env or the same file", [](TestCase &t)
               {
            ::unsetenv("PS2X_DEV");
            t.IsTrue(!ps2x::diaggate::devModeOpen(), "unset closes the gate");
            const std::vector<std::pair<std::string, std::string>> withDev{
                {"PS2X_DEV", "1"}, {"PS2X_NET_CAPTURE", "x"}};
            t.IsTrue(ps2x::diaggate::devModeOpenIn(withDev), "file PS2X_DEV=1 opens");
            const std::vector<std::pair<std::string, std::string>> withoutDev{
                {"PS2X_SOUND", "1"}};
            t.IsTrue(!ps2x::diaggate::devModeOpenIn(withoutDev), "no dev stays closed");
            ::setenv("PS2X_DEV", "1", 1);
            t.IsTrue(ps2x::diaggate::devModeOpen(), "process PS2X_DEV=1 opens");
            t.IsTrue(ps2x::diaggate::devModeOpenIn(withoutDev), "process dev covers files");
            ::unsetenv("PS2X_DEV"); }); });}
