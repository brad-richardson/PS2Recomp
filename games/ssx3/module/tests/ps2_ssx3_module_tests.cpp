#include "MiniTest.h"
#include "ps2_ssx3_anim_pick_guard.h"
#include "ps2_ssx3_module.h"

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
            const char *kExpected[] = {"ssx3-anim-pick-guard", "ssx3-hle-iop", "ssx3-copied-payload"};
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
