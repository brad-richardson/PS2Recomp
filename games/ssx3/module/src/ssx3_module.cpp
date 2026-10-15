// module-seam-1: the SSX 3 game module. Families move here from the runtime
// one at a time (docs: games/ssx3/module/README.md); each keeps its override
// name, ELF match and apply function. Table order is application order.
#include "ps2_ssx3_module.h"
#include "Ssx3CopiedPayload.h"
#include "ssx3_module_internal.h"

#include <iterator>

namespace
{
    constexpr uint32_t kSsx3Entry = 0x00100008u;

    const ps2_game_overrides::Descriptor kOverrides[] = {
        {"ssx3-anim-pick-guard", "SLUS_207.72", kSsx3Entry, 0u, &ps2_ssx3_anim_pick::applyAnimPickGuard},
        {"ssx3-hle-iop", "SLUS_207.72", kSsx3Entry, 0u, &ps2_ssx3_hle_iop::applySsx3HleIop},
        {"ssx3-copied-payload", "SLUS_207.72", kSsx3Entry, 0u, &ps2_syscalls::applySsx3CopiedPayload},
    };

    // K1: the copied GetEntryAddress/TLB payload (Ssx3CopiedPayload.h). Both
    // hooks are no-ops until "ssx3-copied-payload" arms the quirk.
    const ps2_game_module::Module kModule{ps2_game_module::kAbiVersion,
                                          "ssx3",
                                          kOverrides,
                                          std::size(kOverrides),
                                          &ps2_syscalls::noteSsx3CopiedPayloadInstall,
                                          &ps2_syscalls::tryDispatchSsx3CopiedPayload};
}

const ps2_game_module::Module &ps2x_ssx3_game_module() noexcept
{
    return kModule;
}
