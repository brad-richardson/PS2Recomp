// module-seam-1: the SSX 3 game module. Families move here from the runtime
// one at a time (docs: games/ssx3/module/README.md); each keeps its override
// name, ELF match and apply function. Table order is application order.
#include "ps2_ssx3_module.h"
#include "Ssx3CopiedPayload.h"
#include "Ssx3SifHandshake.h"
#include "ps2_snd_spike.h"
#include "ssx3_module_internal.h"

#include <iterator>

namespace
{
    constexpr uint32_t kSsx3Entry = 0x00100008u;

    const ps2_game_overrides::Descriptor kOverrides[] = {
        {"ssx3-anim-pick-guard", "SLUS_207.72", kSsx3Entry, 0u, &ps2_ssx3_anim_pick::applyAnimPickGuard},
        {"ssx3-hle-iop", "SLUS_207.72", kSsx3Entry, 0u, &ps2_ssx3_hle_iop::applySsx3HleIop},
        {"ssx3-copied-payload", "SLUS_207.72", kSsx3Entry, 0u, &ps2_syscalls::applySsx3CopiedPayload},
        {"ssx3-sif-handshake", "SLUS_207.72", kSsx3Entry, 0u, &ps2_syscalls::applySsx3SifHandshake},
    };

    // K1: the copied GetEntryAddress/TLB payload (Ssx3CopiedPayload.h). Both
    // hooks are no-ops until "ssx3-copied-payload" arms the quirk.
    // P1ac: the SIF ready-handshake (Ssx3SifHandshake.h). The hook is a
    // no-op until "ssx3-sif-handshake" arms the quirk.
    // snd-seam-1 family A: the SSX 3 SND progress hooks (ps2_snd_spike.h).
    // Always on for SSX 3 (the HLE runs on guest time with sound on or
    // off); the SND savestate providers follow in the same family.
    const ps2_game_module::Module kModule{ps2_game_module::kAbiVersion,
                                          "ssx3",
                                          kOverrides,
                                          std::size(kOverrides),
                                          &ps2_syscalls::noteSsx3CopiedPayloadInstall,
                                          &ps2_syscalls::tryDispatchSsx3CopiedPayload,
                                          &ps2_syscalls::onSsx3SifSendCmd,
                                          &ps2_snd_spike::noteAddCmdHandler,
                                          &ps2_snd_spike::noteRpc,
                                          &ps2_snd_spike::onSendCmd,
                                          &ps2_snd_spike::onSetDma,
                                          &ps2_snd_spike::onSoundTick,
                                          ps2_snd_spike::kTickCycles,
                                          &ps2_snd_spike::saveSndState,
                                          &ps2_snd_spike::loadSndState};
}

const ps2_game_module::Module &ps2x_ssx3_game_module() noexcept
{
    return kModule;
}
