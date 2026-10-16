#include "MiniTest.h"
#include "Ssx3SifHandshake.h"
#include "ps2_ssx3_module.h"

// sif-handshake-1: the P1ac SIF handshake hook wiring. The handshake
// behavior itself stays covered by the shared kernel SIF tests (product
// suite, armed by the module table); this only pins the table wiring.
using namespace ps2_syscalls;

void register_ps2_ssx3_sif_handshake_tests()
{
    MiniTest::Case("Ps2Ssx3SifHandshake", [](TestCase &tc)
                   {
        tc.Run("the module wires the P1ac SendCmd hook", [](TestCase &t)
               {
            const ps2_game_module::Module &m = ps2x_ssx3_game_module();
            t.IsTrue(m.onSifSendCmd == &onSsx3SifSendCmd, "sendcmd hook");
            t.Equals(m.abiVersion, ps2_game_module::kAbiVersion, "abi");
        }); });
}
