// module-seam-1: the no-module default. Keep this TU to this one definition:
// the linker pulls it from the ps2_runtime archive only when no attach glue
// (ps2x_attach_game_module) already defines linkedModule().
#include "runtime/game_module.h"

const ps2_game_module::Module *ps2_game_module::linkedModule() noexcept
{
    return nullptr;
}

