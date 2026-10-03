#include "MiniTest.h"
#include "ps2x/iop/ps2_path.h"
#include "../../ps2xIOP/tests/iop_compat_test_support.h"

void register_ps2_iop_tests()
{
    MiniTest::Case("PS2IopSubsystem", [](TestCase &tc)
    {
        tc.Run("PS2 path parsing is shared and normalizes ISO/module names", [](TestCase &t)
        {
            const ps2x::iop::ParsedPs2Path cd = ps2x::iop::parsePs2Path("CDROM0:\\MODULES\\LIBSD.IRX;1");
            t.Equals(cd.device, ps2x::iop::Ps2PathDevice::Cdrom,
                     "device names should be case-insensitive");
            t.Equals(cd.path, std::string("MODULES/LIBSD.IRX"),
                     "separators and ISO version suffixes should normalize once");
            t.Equals(ps2x::iop::ps2PathLeafKey(cd), std::string("libsd"),
                     "module lookup should use a normalized IRX leaf key");

            const ps2x::iop::ParsedPs2Path rom = ps2x::iop::parsePs2Path("rom0:ROMVER");
            t.Equals(rom.device, ps2x::iop::Ps2PathDevice::Rom0,
                     "ROM0 should remain a distinct virtual device");
            t.IsFalse(static_cast<bool>(ps2x::iop::parsePs2Path("unknown0:file.irx")),
                      "unsupported devices must not fall through to cdrom0");
        });

        tc.Run("HLE services activate only after a recognized module load", [](TestCase &t)
        {
            iop_test::Host host;
            ps2x::iop::IopSubsystem subsystem(host);
            t.IsFalse(subsystem.canBindRpc(0x80000701u),
                      "LIBSD RPC must not exist before LIBSD is loaded");

            const ps2x::iop::ModuleLoadResult unknown = subsystem.loadModule("rom0:NOT_A_REAL_MODULE");
            t.IsTrue(unknown.handled, "the module manager should return a real load result");
            t.IsTrue(unknown.moduleId < 0, "unknown ROM modules must fail instead of receiving fake IDs");

            const ps2x::iop::ModuleLoadResult loaded = subsystem.loadModule("rom0:LIBSD");
            t.IsTrue(loaded.moduleId > 0, "a registered no-BIOS HLE module should load");
            t.IsTrue(subsystem.canBindRpc(0x80000701u),
                     "loading LIBSD should activate its HLE RPC endpoint");

            ps2x::iop::RpcRequest request{};
            request.sid = 0x80000701u;
            request.function = 3u;
            t.IsTrue(subsystem.handleRpc(request).handled,
                     "the activated LIBSD service should handle its RPC");
            t.Equals(host.audioCalls, 1u, "the RPC should reach the HLE audio contract");

            int32_t stopResult = -1;
            t.IsTrue(subsystem.stopModule(loaded.moduleId, &stopResult),
                     "an HLE module should have a real stoppable lifecycle");
            t.Equals(stopResult, 0, "stopping an HLE module should report success");
            t.IsFalse(subsystem.canBindRpc(0x80000701u),
                      "stopping LIBSD should deactivate its RPC endpoint");
        });

        tc.Run("unknown SID remains unhandled without a loaded IRX", [](TestCase &t)
        {
            iop_test::Host host;
            ps2x::iop::IopSubsystem subsystem(host);

            ps2x::iop::RpcRequest request{};
            request.sid = 0xDEADC0DEu;
            request.function = 0x99u;
            const ps2x::iop::RpcResult result = subsystem.handleRpc(request);
            t.IsFalse(result.handled, "an unknown SID should not be claimed by the IOP subsystem");
            t.Equals(result.resultAddress, 0u, "an unknown SID should not return a guest result address");
            t.IsFalse(result.signalNowaitCompletion, "an unknown SID should not signal nowait completion");
            t.Equals(result.callbackPolicy, ps2x::iop::CallbackPolicy::RuntimeDefault,
                     "an unknown SID should preserve runtime callback handling");

        });

        tc.Run("RBF1: HLE mode routes the core services without module loads, across resets", [](TestCase &t)
        {
            // RRV1 #2: the pre-rebase policy registered MCSERV, dbcman and
            // libsd unconditionally. SSX 3's HLE-mode loads are tracked-only
            // and never enter the module manager, so without this the three
            // routes stay absent even after successful binds.
            iop_test::Host host;
            ps2x::iop::IopSubsystem subsystem(host);
            subsystem.setHleCoreServices(true);
            t.IsTrue(subsystem.hleCoreServices(), "HLE routing is on");

            constexpr uint32_t kLibSdSid = 0x80000701u;
            constexpr uint32_t kMcservSid = 0x80000400u;
            constexpr uint32_t kDbcmanSid = 0x80001300u;
            t.IsTrue(subsystem.canBindRpc(kLibSdSid), "libsd binds with no module loaded in HLE mode");
            t.IsTrue(subsystem.canBindRpc(kMcservSid), "MCSERV binds with no module loaded in HLE mode");
            t.IsTrue(subsystem.canBindRpc(kDbcmanSid), "dbcman binds with no module loaded in HLE mode");
            for (const auto &row : subsystem.debugSnapshot().services)
                t.IsTrue(row.active, "HLE service active without a load: " + row.name);

            auto callLibSd = [&]() {
                return subsystem.handleRpc(iop_test::request(kLibSdSid, 3u)).handled;
            };
            t.IsTrue(callLibSd(), "bind -> call reaches libsd in HLE mode");
            t.Equals(host.audioCalls, 1u, "the call reaches the HLE audio contract");

            subsystem.reset(); // models the SifInitRpc transport reset
            t.IsTrue(subsystem.hleCoreServices(), "HLE routing survives the reset");
            t.IsTrue(subsystem.canBindRpc(kLibSdSid), "libsd still binds after the reset");
            t.IsTrue(subsystem.canBindRpc(kMcservSid), "MCSERV still binds after the reset");
            t.IsTrue(subsystem.canBindRpc(kDbcmanSid), "dbcman still binds after the reset");
            t.IsTrue(callLibSd(), "reset -> call still reaches libsd in HLE mode");
            t.Equals(host.audioCalls, 2u, "the post-reset call reaches the HLE audio contract");

            subsystem.setHleCoreServices(false);
            t.IsFalse(subsystem.canBindRpc(kLibSdSid),
                      "emulator mode keeps the module gate once HLE routing is off");
            t.IsFalse(callLibSd(), "emulator mode without a load does not claim the SID");
        });

    });
}
