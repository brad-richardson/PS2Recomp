#include "MiniTest.h"
#include "game_overrides.h"
#include "ps2_fh1_fix.h"
#include "ps2_fh1_full120.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "ps2_syscalls.h"
#include "runtime/game_module.h"
#include "runtime/ps2_savestate.h"

#include <cstring>
#include <iterator>
#include <vector>

void reset_ps2_test_function_table();

// module-seam-1: a binary with no game module attached links the runtime's
// no-module default, and the override pass applies nothing from a module.
namespace
{
    void sentinel(uint8_t *, R5900Context *, PS2Runtime *) {}
    // Guest PCs the SSX 3 module wraps today (its anim pick guard entries
    // plus the SIF private SendCmd entry the handshake override binds).
    constexpr uint32_t kModuleOwnedPcs[] = {0x311710u, 0x3128e8u, 0x426078u};
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
        });
        // snd-seam-1: without a module, no SIF handshake can arm. A
        // SET_SREG(1,1) SendCmd still returns success but leaves sregs[1]
        // alone; the armed behavior lives in the module suite.
        tc.Run("SendCmd without the module leaves sregs[1] alone", [](TestCase &t)
               {
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0u);
            R5900Context ctx{};
            constexpr uint32_t kPacketAddr = 0x00009000u;
            constexpr uint32_t kSregs1Addr = 0x52BE04u;
            constexpr uint32_t kPacket[] = {0u, 0u, 0u, 0u, 1u, 1u};
            for (size_t i = 0; i < std::size(kPacket); ++i)
                std::memcpy(rdram.data() + kPacketAddr + i * sizeof(uint32_t), &kPacket[i],
                            sizeof(uint32_t));
            SET_GPR_U32(&ctx, 4, 0x80000001u);
            SET_GPR_U32(&ctx, 5, kPacketAddr);
            SET_GPR_U32(&ctx, 6, 0x18u);
            SET_GPR_U32(&ctx, 7, 0u);
            SET_GPR_U32(&ctx, 29, 0x00100000u);
            {
                PS2Runtime runtime;
                ps2_syscalls::sceSifSendCmd(rdram.data(), &ctx, &runtime);
            }
            t.Equals(static_cast<int32_t>(getRegU32(&ctx, 2)), 1, "SendCmd still returns 1");
            uint32_t sregs1 = 0u;
            std::memcpy(&sregs1, rdram.data() + kSregs1Addr, sizeof(sregs1));
            t.Equals(sregs1, 0u, "sregs[1] stays 0 with no module");
            reset_ps2_test_function_table();
        });
        // snd-seam-1 family B: the "snd" savestate section belongs to the
        // module now. Without one, no SND state is registered (a product
        // save's "snd" section fails closed as unknown, like "syscalls:k1").
        tc.Run("no SND savestate section is registered without the module", [](TestCase &t)
               {
            const auto &sections = ps2_savestate::registeredSections();
            t.IsTrue(sections.find("snd") == sections.end(), "no snd section with no module");
        });
        // snd-seam-1 family #8: "trickygems" and the TK34 patch-grow
        // kernel field belong to the module too. Without one, the section
        // is absent (a product save's "trickygems" section fails closed as
        // unknown) and the kernel save fails closed at the grow field.
        tc.Run("no Tricky savestate section is registered without the module", [](TestCase &t)
               {
            const auto &sections = ps2_savestate::registeredSections();
            t.IsTrue(sections.find("trickygems") == sections.end(), "no trickygems section with no module");
        });
        // full120 slice 1: the FIX-policy content belongs to the module.
        // Without one, no group parses ("all" is empty, names refuse) and
        // every compat predicate is false.
        tc.Run("no FIX-policy content without the module", [](TestCase &t)
               {
            const ps2_fh1::FixMasks all = ps2_fh1::parseFix("all");
            t.IsTrue(all.ok, "all is a known keyword");
            t.Equals(all.main, 0u, "all main is empty with no module");
            t.Equals(all.fh12, 0u, "all fh12 is empty with no module");
            t.IsFalse(ps2_fh1::parseFix("rider").ok, "names refuse with no module");
            t.IsFalse(ps2_fh1::asymkMaskOk(~0ull), "compat false with no module");
            t.IsFalse(ps2_fh1::normcapMaskOk(~0ull), "compat false with no module");
        });
        // full120 slice 2: the clock policy belongs to the module. Without
        // one, every reader sees stock behavior (divisors 1, shifts 0,
        // accumulators 0, commit and patching no-ops, words refused).
        tc.Run("no clock policy without the module", [](TestCase &t)
               {
            t.Equals(ps2_fh1::vblankDivisor(), 1u, "stock divisor");
            t.Equals(ps2_fh1::schedDivisor(), 1u, "stock sched divisor");
            t.Equals(ps2_fh1::eeClockShiftNow(), 0u, "stock shift");
            t.Equals(ps2_fh1::stockHalfTicks(), 0u, "stock half ticks");
            t.Equals(ps2_fh1::stockHalfTicksLegacy(), 0u, "stock legacy half ticks");
            t.Equals(ps2_fh1::stockHalfStepNext(), 2u, "stock half step");
        });
        // full120 slice 3: the dispatch cold side belongs to the module.
        // Without one, the interest table is empty (table mode then refuses
        // every branch: generic no-game-hooks behavior).
        tc.Run("no dispatch cold side without the module", [](TestCase &t)
               {
            ps2_fh1::HookConfig c;
            c.mode = ps2_fh1::Mode::Events;
            c.main = ~0ull;
            c.fix12 = ~0u;
            c.guestActive = true;
            const ps2_fh1::HookInterest hi = ps2_fh1::buildHookInterest(c);
            t.IsTrue(hi.src.empty(), "no src with no module");
            t.IsTrue(hi.tgt.empty(), "no tgt with no module");
            t.IsFalse(hi.always, "no always with no module");
        });
        // full120 slice 4: the dispatch hot side belongs to the module.
        // Without one, onBranch skips the chain (generic: no game hooks).
        tc.Run("no dispatch hot side without the module", [](TestCase &t)
               {
            std::vector<uint8_t> ram(PS2_RAM_SIZE, 0u);
            R5900Context ctx{};
            t.IsFalse(ps2_fh1::onBranch(ram.data(), &ctx, 0x11a3f0u, 0x117638u, nullptr),
                      "chain skipped with no module");
        });
        // full120 slice 5: the post-call consume side belongs to the
        // module. Without one, onReturn/postUnwindRestore are no-ops:
        // RAM is untouched and no record arms.
        tc.Run("no post-call consume side without the module", [](TestCase &t)
               {
            std::vector<uint8_t> ram(PS2_RAM_SIZE, 0xA5u);
            R5900Context ctx{};
            ps2_fh1::onReturn(ram.data(), &ctx, 0x117638u, true);
            ps2_fh1::postUnwindRestore(ram.data());
            t.IsFalse(ps2_fh1::g_postArmed, "nothing armed");
            t.IsFalse(ps2_fh1::g_pubArmed, "nothing published");
            t.IsFalse(ps2_fh1::g_normcapArmed, "nothing held");
            bool clean = true;
            for (uint8_t b : ram)
                clean = clean && b == 0xA5u;
            t.IsTrue(clean, "RAM untouched");
        });
        // module-seam-2: K1. Without a module, a syscall handler with no
        // recompiled function drops to KE_ERROR even after the SSX 3 pass
        // and with SSX 3's copied payload intact in RAM.
        tc.Run("an unmapped syscall handler drops to KE_ERROR", [](TestCase &t)
               {
            PS2Runtime runtime;
            std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0u);
            R5900Context ctx{};
            reset_ps2_test_function_table();
            ps2_game_overrides::applyMatching(runtime, "SLUS_207.72", 0x00100008u, 0u, false);
            constexpr uint32_t kSrc = 0x4561C0u, kDstPhys = 0x75000u, kSize = 0x330u;
            for (uint32_t i = 0; i < kSize; ++i)
                rdram[kSrc + i] = static_cast<uint8_t>((i * 31u + 7u) & 0xFFu);
            std::memcpy(rdram.data() + kDstPhys, rdram.data() + kSrc, kSize);
            runtime.setEeSyscallOverride(rdram.data(), 0x5Bu, 0x80075000u);
            SET_GPR_U32(&ctx, 4, 0x55u);
            t.IsTrue(ps2_syscalls::dispatchNumericSyscall(0x5Bu, rdram.data(), &ctx, &runtime), "dispatched");
            t.Equals(static_cast<int32_t>(getRegU32(&ctx, 2)), -1, "KE_ERROR");
            reset_ps2_test_function_table();
        }); });
}
