// module-seam-1: the no-module default. Keep this TU to this one definition:
// the linker pulls it from the ps2_runtime archive only when no attach glue
// (ps2x_attach_game_module) already defines linkedModule().
#include "runtime/game_module.h"

const ps2_game_module::Module *ps2_game_module::linkedModule() noexcept
{
    return nullptr;
}

// full120 slice 5: the module-owned post-call state the runtime sees
// through extern declarations. A no-module binary gets zeroed copies
// here (same TU as linkedModule, so the archive pulls them together and
// a module-attached binary never sees a duplicate).
#include "ps2_fh1_full120.h"

namespace ps2_fh1
{
PostCall g_post;
bool g_postArmed = false;
PubRecord g_pub;
bool g_pubArmed = false;
NormcapPost g_normcap;
bool g_normcapArmed = false;
NormcapSlot g_normcapSlots[kNormcapSlots];
NormcapOwner g_normcapOwners[kNormcapSlots];
} // namespace ps2_fh1
