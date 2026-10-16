// device-gates-1: host coverage for ps2_gpu_resolve.h (H2/H3/H5/H6 policy).
// The Android backend applies these answers; the policy itself is pure.

#include "ps2_gpu_resolve.h"

#include <cstdlib>
#include <cstring>
#include <string>

#include "MiniTest.h"

void register_ps2_gpu_resolve_tests()
{
    MiniTest::Case("Ps2GpuResolve", [](TestCase &tc)
                   {
        tc.Run("isAdreno: vendorID or name (O1 precedent)", [](TestCase &t)
               {
            t.IsTrue(ps2x::gpu_resolve::isAdreno(0x5143, "Adreno (TM) 830"), "qualcomm+adreno");
            t.IsTrue(ps2x::gpu_resolve::isAdreno(0x5143, "weird-name"), "vendorID alone suffices");
            t.IsTrue(ps2x::gpu_resolve::isAdreno(0x0000, "Adreno 740"), "name alone suffices");
            t.IsTrue(ps2x::gpu_resolve::isAdreno(0x5143, nullptr), "null name with qualcomm vendor");
            t.IsFalse(ps2x::gpu_resolve::isAdreno(0x13B5, "Mali-G57 MC2"), "mali is not adreno");
            t.IsFalse(ps2x::gpu_resolve::isAdreno(0x0000, nullptr), "unknown is not adreno");
            t.IsFalse(ps2x::gpu_resolve::isAdreno(0x13B5, nullptr), "mali vendor alone is not adreno");
        });

        tc.Run("resolveWorkaround: auto resolves, explicit wins", [](TestCase &t)
               {
            using ps2x::gpu_resolve::resolveWorkaround;
            t.Equals(std::string(resolveWorkaround(nullptr, true)), std::string("1"), "unset adreno -> 1");
            t.Equals(std::string(resolveWorkaround(nullptr, false)), std::string("0"), "unset mali -> 0");
            t.Equals(std::string(resolveWorkaround("", true)), std::string("1"), "empty adreno -> 1");
            t.Equals(std::string(resolveWorkaround("auto", false)), std::string("0"), "auto mali -> 0");
            t.Equals(std::string(resolveWorkaround("1", false)), std::string("1"), "explicit 1 wins on mali");
            t.Equals(std::string(resolveWorkaround("0", true)), std::string("0"), "explicit 0 wins on adreno");
        });

        tc.Run("parseDeviceModel: first integer run", [](TestCase &t)
               {
            using ps2x::gpu_resolve::parseDeviceModel;
            t.Equals(parseDeviceModel("Adreno (TM) 830"), 830, "odin 830");
            t.Equals(parseDeviceModel("Adreno 740"), 740, "740");
            t.Equals(parseDeviceModel("Mali-G57 MC2"), 57, "mali g57");
            t.Equals(parseDeviceModel("no-digits"), -1, "no number");
            t.Equals(parseDeviceModel(nullptr), -1, "null");
        });

        tc.Run("isAdreno8xx: only the 800s family", [](TestCase &t)
               {
            using ps2x::gpu_resolve::isAdreno8xx;
            t.IsTrue(isAdreno8xx(0x5143, "Adreno (TM) 830"), "odin 830 is 8xx");
            t.IsFalse(isAdreno8xx(0x5143, "Adreno 740"), "740 is not 8xx");
            t.IsFalse(isAdreno8xx(0x5143, "Adreno (TM) X Elite"), "unparseable adreno is not 8xx");
            t.IsFalse(isAdreno8xx(0x13B5, "Mali-G57 MC2"), "mali is not 8xx");
            t.IsTrue(isAdreno8xx(0x0000, "Adreno 830"), "name-only 830 still 8xx");
        });

        tc.Run("resolveAutotune: prefer_sysmem only from turnip-on-8xx", [](TestCase &t)
               {
            using ps2x::gpu_resolve::resolveAutotune;
            const char *on = resolveAutotune(nullptr, true);
            t.IsTrue(on && std::string(on) == "prefer_sysmem", "unset + 8xx -> prefer_sysmem");
            t.IsTrue(resolveAutotune(nullptr, false) == nullptr, "unset + other -> leave alone");
            t.IsTrue(resolveAutotune("", true) && std::string(resolveAutotune("", true)) == "prefer_sysmem",
                     "empty + 8xx -> prefer_sysmem");
            t.Equals(std::string(resolveAutotune("bandwidth", true)), std::string("bandwidth"),
                     "explicit wins even on 8xx");
        });
    });
}
