#include "MiniTest.h"
#include "Ssx3LodScale.h"
#include "Ssx3StockMeter.h"
#include "Ssx3TrickyMenu.h"
#include "Ssx3TrickySong.h"
#include "Ssx3TrickyState.h"
#include "ps2_snd_spike.h"
#include "ps2_ssx3_anim_pick_guard.h"
#include "ps2_ssx3_lod.h"
#include "ps2_ssx3_tricky_menu.h"
#include "ps2_ssx3_module.h"
#include "runtime/ps2_savestate.h"

#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

void reset_ps2_test_function_table();

// module-seam-1: the SSX 3 module is the binary's linked module, and the
// runtime's override pass installs its bindings (product suite only).
namespace
{
    bool g_pickReached = false;
    bool g_playReached = false;
    void sentinelPick(uint8_t *, R5900Context *, PS2Runtime *) { g_pickReached = true; }
    void sentinelPlay(uint8_t *, R5900Context *, PS2Runtime *) { g_playReached = true; }

    void setReg(R5900Context &ctx, int reg, uint32_t v)
    {
        ctx.r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(v)));
    }

    struct Bound
    {
        PS2Runtime::RecompiledFunction pick;
        PS2Runtime::RecompiledFunction play;
    };

    // Sentinels at both guarded entries, then the runtime's override pass.
    Bound bindFor(PS2Runtime &runtime, const char *elf)
    {
        reset_ps2_test_function_table();
        runtime.replaceFunction(ps2_ssx3_anim_pick::kPicker, &sentinelPick);
        runtime.replaceFunction(ps2_ssx3_anim_pick::kPlay, &sentinelPlay);
        ps2_game_overrides::applyMatching(runtime, elf, 0x00100008u, 0u, false);
        return {runtime.lookupFunction(ps2_ssx3_anim_pick::kPicker),
                runtime.lookupFunction(ps2_ssx3_anim_pick::kPlay)};
    }
}

void register_ps2_ssx3_module_tests()
{
    MiniTest::Case("Ps2Ssx3GameModule", [](TestCase &tc)
                   {
        tc.Run("the SSX 3 module is linked and passes the ABI check", [](TestCase &t)
               {
            const ps2_game_module::Module *m = ps2_game_module::linkedModule();
            t.IsTrue(m != nullptr, "module linked");
            t.IsTrue(m == &ps2x_ssx3_game_module(), "linked module is the SSX 3 entry");
            t.IsTrue(ps2_game_module::installed() == m, "installed() returns it");
            t.Equals(std::string(m->name), std::string("ssx3"), "name");
            t.Equals(m->abiVersion, ps2_game_module::kAbiVersion, "abi");
        });
        tc.Run("override table: the moved families, in order, with their old match", [](TestCase &t)
               {
            const ps2_game_module::Module &m = ps2x_ssx3_game_module();
            const char *kExpected[] = {"ssx3-anim-pick-guard", "ssx3-hle-iop", "ssx3-copied-payload",
                                       "ssx3-sif-handshake", "ssx3-tricky-menu",
                                       "ssx3-tricky-stock-meter", "ssx3-lod-scale"};
            t.Equals(m.overrideCount, std::size(kExpected), "override count");
            for (size_t i = 0; i < m.overrideCount && i < std::size(kExpected); ++i)
            {
                const ps2_game_overrides::Descriptor &d = m.overrides[i];
                t.Equals(std::string(d.name), std::string(kExpected[i]), "name");
                t.Equals(std::string(d.elfName), std::string("SLUS_207.72"), "elf");
                t.Equals(d.entry, 0x00100008u, "entry");
                t.Equals(d.crc32, 0u, "crc");
                t.IsTrue(d.apply != nullptr, "apply");
            }
            t.IsTrue(m.overrides[4].apply == &ps2_tricky_menu::applyTrickyMenu, "tricky-menu apply");
            t.IsTrue(m.overrides[5].apply == &ps2_stock_meter::applyTrickyStockMeter, "stock-meter apply");
            t.IsTrue(m.overrides[6].apply == &ps2_lod_scale::applyLodScale, "lod-scale apply");
        });
        tc.Run("lod-scale apply wraps nothing with the knobs off", [](TestCase &t)
               {
            // snd-seam-1 Part 5 slice 3: both scales default 0 (det keys)
            // installs no bindings — the keys cannot move.
            unsetenv("PS2X_SSX3_LOD_SCALE");
            unsetenv("PS2X_SSX3_TRICKY_LOD_SCALE");
            PS2Runtime runtime;
            reset_ps2_test_function_table();
            runtime.replaceFunction(ps2_ssx3_lod::kSetPerspective, &sentinelPick);
            ps2_game_overrides::applyMatching(runtime, "SLUS_207.72", 0x00100008u, 0u, false);
            t.IsTrue(runtime.lookupFunction(ps2_ssx3_lod::kSetPerspective) == &sentinelPick,
                     "perspective setter unwrapped");
            reset_ps2_test_function_table();
        });
        tc.Run("stock-meter apply wraps nothing with the knob off", [](TestCase &t)
               {
            // snd-seam-1 Part 5 slice 2: knob default off (det keys)
            // installs no bindings — the keys cannot move.
            unsetenv("PS2X_SSX3_TRICKY_HUD");
            PS2Runtime runtime;
            reset_ps2_test_function_table();
            runtime.replaceFunction(0x21d1a0u, &sentinelPick);
            ps2_game_overrides::applyMatching(runtime, "SLUS_207.72", 0x00100008u, 0u, false);
            t.IsTrue(runtime.lookupFunction(0x21d1a0u) == &sentinelPick, "meter helper unwrapped");
            reset_ps2_test_function_table();
        });
        tc.Run("tricky-menu apply wraps nothing with the knob off", [](TestCase &t)
               {
            // snd-seam-1 Part 5: knob default off (det keys) installs no
            // bindings — the keys cannot move. Sentinels first (a bare
            // lookup materializes a function, so absence is unwrappable).
            unsetenv("PS2X_SSX3_TRICKY_MENU");
            PS2Runtime runtime;
            reset_ps2_test_function_table();
            runtime.replaceFunction(ps2_ssx3_tricky::kMainMenuItemState, &sentinelPick);
            runtime.replaceFunction(ps2_ssx3_tricky::kMapFill, &sentinelPlay);
            ps2_game_overrides::applyMatching(runtime, "SLUS_207.72", 0x00100008u, 0u, false);
            t.IsTrue(runtime.lookupFunction(ps2_ssx3_tricky::kMainMenuItemState) == &sentinelPick,
                     "menu build unwrapped");
            t.IsTrue(runtime.lookupFunction(ps2_ssx3_tricky::kMapFill) == &sentinelPlay, "map fill unwrapped");
            reset_ps2_test_function_table();
        });
        tc.Run("the SND progress hooks point at the moved HLE (ABI 5)", [](TestCase &t)
               {
            const ps2_game_module::Module &m = ps2x_ssx3_game_module();
            t.Equals(m.abiVersion, 6u, "abi");
            t.IsTrue(m.noteSndAddCmdHandler == &ps2_snd_spike::noteAddCmdHandler, "addcmdhandler");
            t.IsTrue(m.noteSndRpc == &ps2_snd_spike::noteRpc, "rpc snoop");
            t.IsTrue(m.onSndSendCmd == &ps2_snd_spike::onSendCmd, "sendcmd");
            t.IsTrue(m.onSndSetDma == &ps2_snd_spike::onSetDma, "setdma");
            t.IsTrue(m.onSndSoundTick == &ps2_snd_spike::onSoundTick, "soundtick");
            t.Equals(m.sndTickCycles, ps2_snd_spike::kTickCycles, "tick period");
            t.Equals(m.sndTickCycles, 3145728ull, "one tick is exactly 3,145,728 EE cycles");
            t.IsTrue(m.sndSaveState == &ps2_snd_spike::saveSndState, "save provider");
            t.IsTrue(m.sndLoadState == &ps2_snd_spike::loadSndState, "load provider");
            const auto &sections = ps2_savestate::registeredSections();
            const auto snd = sections.find("snd");
            t.IsTrue(snd != sections.end(), "snd section registered by the module");
            t.Equals(snd->second.version, 1u, "snd version frozen for old saves");
            t.IsTrue(snd->second.save == &ps2_snd_spike::saveSndState, "snd save hook");
            t.IsTrue(snd->second.load == &ps2_snd_spike::loadSndState, "snd load hook");
        });
        tc.Run("the Tricky song mix hooks point at the provider (ABI 5)", [](TestCase &t)
               {
            const ps2_game_module::Module &m = ps2x_ssx3_game_module();
            t.Equals(m.abiVersion, 6u, "abi");
            t.IsTrue(m.sndTrickySongInit == &ps2_snd_tricky_song::initFromEnvHook, "init");
            t.IsTrue(m.sndTrickySongMix == &ps2_snd_tricky_song::mixIntoHook, "mix");
            t.IsTrue(m.sndTrickySongProbe == &ps2_snd_tricky_song::probeSnapshot, "probe");
        });
        tc.Run("the TK34 patch-grow providers point at the module (ABI 6)", [](TestCase &t)
               {
            const ps2_game_module::Module &m = ps2x_ssx3_game_module();
            t.Equals(m.abiVersion, 6u, "abi");
            t.IsTrue(m.patchGrowSave == &ps2_tricky_state::patchGrowSave, "save");
            t.IsTrue(m.patchGrowValidate == &ps2_tricky_state::patchGrowValidate, "validate");
            t.IsTrue(m.patchGrowLoad == &ps2_tricky_state::patchGrowLoad, "load");
            const auto &sections = ps2_savestate::registeredSections();
            const auto gems = sections.find("trickygems");
            t.IsTrue(gems != sections.end(), "trickygems section registered by the module");
            t.Equals(gems->second.version, 1u, "trickygems version frozen for old saves");
            t.IsTrue(gems->second.save == &ps2_tricky_state::trickyGemsSave, "gems save hook");
            t.IsTrue(gems->second.load == &ps2_tricky_state::trickyGemsLoad, "gems load hook");
            t.IsTrue(gems->second.validate == &ps2_tricky_state::trickyGemsLoad, "gems validate hook");
        });
        tc.Run("the override pass wraps both anim entries and chains to the originals", [](TestCase &t)
               {
            PS2Runtime runtime;
            const Bound b = bindFor(runtime, "SLUS_207.72");
            t.IsTrue(b.pick != &sentinelPick, "picker wrapped");
            t.IsTrue(b.play != &sentinelPlay, "play wrapped");

            // A single-variant pick and an in-range play event run the originals.
            std::vector<uint8_t> ram(PS2_RAM_SIZE, 0u);
            constexpr uint32_t kTable = 0x00b00000u;
            const uint32_t single = 7u | (1u << 16);
            std::memcpy(ram.data() + kTable + 5u * 4u, &single, 4u);
            R5900Context pick{};
            setReg(pick, 4, kTable);
            setReg(pick, 5, 5u);
            pick.pc = ps2_ssx3_anim_pick::kPicker;
            g_pickReached = false;
            b.pick(ram.data(), &pick, &runtime);
            t.IsTrue(g_pickReached, "picker original reached");

            R5900Context play{};
            setReg(play, 5, 3u);
            play.pc = ps2_ssx3_anim_pick::kPlay;
            g_playReached = false;
            b.play(ram.data(), &play, &runtime);
            t.IsTrue(g_playReached, "play original reached");
            reset_ps2_test_function_table();
        });
        tc.Run("another ELF gets none of the module's bindings", [](TestCase &t)
               {
            PS2Runtime runtime;
            const Bound b = bindFor(runtime, "SLUS_999.99");
            t.IsTrue(b.pick == &sentinelPick, "picker untouched");
            t.IsTrue(b.play == &sentinelPlay, "play untouched");
            t.IsTrue(!runtime.hleIopMode(), "IOP mode untouched");
            reset_ps2_test_function_table();
        });
        // module-seam-2: ssx3-hle-iop (UPR1) selects the HLE IOP for SSX 3.
        tc.Run("the override pass selects HLE IOP mode for SSX 3", [](TestCase &t)
               {
            PS2Runtime runtime;
            t.IsTrue(!runtime.hleIopMode(), "emulator mode before the pass");
            bindFor(runtime, "SLUS_207.72");
            t.IsTrue(runtime.hleIopMode(), "HLE IOP mode after the pass");
            reset_ps2_test_function_table();
        }); });
}
