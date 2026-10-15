#include "MiniTest.h"
#include "game_overrides.h"
#include "ps2_runtime.h"
#include "runtime/game_module.h"

void reset_ps2_test_function_table();

// module-seam-1: a binary with no game module attached links the runtime's
// no-module default, and the override pass applies nothing from a module.
namespace
{
    void sentinel(uint8_t *, R5900Context *, PS2Runtime *) {}
    // Guest PCs the SSX 3 module wraps today (its anim pick guard family).
    constexpr uint32_t kModuleOwnedPcs[] = {0x311710u, 0x3128e8u};
}

void register_game_module_none_tests()
{
    MiniTest::Case("GameModuleNone", [](TestCase &tc)
                   {
        tc.Run("no module is linked", [](TestCase &t)
               {
            t.IsTrue(ps2_game_module::linkedModule() == nullptr, "linkedModule() is null");
            t.IsTrue(ps2_game_module::installed() == nullptr, "installed() is null");
        });
        tc.Run("the override pass leaves module-owned PCs alone", [](TestCase &t)
               {
            PS2Runtime runtime;
            reset_ps2_test_function_table();
            for (uint32_t pc : kModuleOwnedPcs)
                runtime.replaceFunction(pc, &sentinel);
            ps2_game_overrides::applyMatching(runtime, "SLUS_207.72", 0x00100008u, 0u, false);
            for (uint32_t pc : kModuleOwnedPcs)
                t.IsTrue(runtime.lookupFunction(pc) == &sentinel, "untouched");
            reset_ps2_test_function_table();
        });
        // module-seam-2: the SSX 3 module, not the runtime, picks the HLE IOP.
        tc.Run("the override pass leaves the IOP mode to the module", [](TestCase &t)
               {
            PS2Runtime runtime;
            reset_ps2_test_function_table();
            ps2_game_overrides::applyMatching(runtime, "SLUS_207.72", 0x00100008u, 0u, false);
            t.IsTrue(!runtime.hleIopMode(), "IOP emulator mode stays");
            reset_ps2_test_function_table();
        }); });
}
