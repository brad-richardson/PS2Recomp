#pragma once

// GPU-aware Android defaults (device-gates-1, from odin-hacks-1 H2/H3/H5/H6).
//
// Odin/Adreno assumptions used to apply to every Android GPU: a blanket
// GE1_VK_TURNIP=1 with no fallback (H3, boot exit(78) on Mali), a blanket
// GE1_ADRENO_BLEND_MIX=1 compiled default (H2), an unconditional
// OverrideTextureBarriers/DisableFramebufferFetch force (H6), and a
// Turnip-only TU_AUTOTUNE_ALGO shipped to every device (H5).
//
// Policy here is pure (no platform calls) so the host suite checks it; the
// Android backend (ps2_gs_external_backend.cpp) gathers the inputs (system
// vendorID, Turnip HAL probe) and applies the answers with setenv before
// GE1 opens. Resolution rules, everywhere:
//
//   - null, empty or "auto" env  -> resolve from the GPU (AUTO semantics,
//     the GE1_ADRENO_DSTREAD precedent);
//   - any other explicit value    -> wins untouched (even on the "wrong" GPU).
//
// Under PS2X_PROFILE=reference the backend skips resolution, so the exact
// knob-off path is preserved. On the Odin (Qualcomm 0x5143, bundled Turnip
// serving an Adreno 830) every answer equals the previous blanket behavior.

#include <cstdint>
#include <cstring>

namespace ps2x::gpu_resolve
{
constexpr std::uint32_t kQualcommVendor = 0x5143;

// O1 precedent (GSDeviceVK AUTO): Qualcomm vendorID OR an Adreno device name.
inline bool isAdreno(std::uint32_t vendorId, const char *deviceName)
{
    if (vendorId == kQualcommVendor)
        return true;
    if (deviceName && std::strstr(deviceName, "Adreno") != nullptr)
        return true;
    return false;
}

// AUTO inputs (unset, empty, "auto") resolve from the GPU; explicit wins.
inline bool isAutoInput(const char *value)
{
    return !value || !*value || std::strcmp(value, "auto") == 0;
}

// H2/H6: Adreno keeps the current behavior ("1"); other GPUs get the
// upstream default ("0"). Returns the value the backend should setenv.
inline const char *resolveWorkaround(const char *env, bool adreno)
{
    if (!isAutoInput(env))
        return env;
    return adreno ? "1" : "0";
}

// H5: first integer run in a device name ("Adreno (TM) 830" -> 830),
// -1 when the name carries no number.
inline int parseDeviceModel(const char *deviceName)
{
    if (!deviceName)
        return -1;
    for (const char *p = deviceName; *p; ++p)
    {
        if (*p >= '0' && *p <= '9')
        {
            int model = 0, digits = 0;
            while (*p >= '0' && *p <= '9' && digits < 9)
            {
                model = model * 10 + (*p - '0');
                ++p;
                ++digits;
            }
            return model;
        }
    }
    return -1;
}

// H5: the TUR1-tuned part is the Adreno 8xx family (Odin 8 Elite's 830).
// Unparseable Adreno names are NOT 8xx: prefer_sysmem stays off rather than
// shipping A830 tuning to an unknown part.
inline bool isAdreno8xx(std::uint32_t vendorId, const char *deviceName)
{
    if (!isAdreno(vendorId, deviceName))
        return false;
    const int model = parseDeviceModel(deviceName);
    return model >= 800 && model < 900;
}

// H5: set prefer_sysmem only when the bundled Turnip serves an Adreno 8xx.
// Returns nullptr when the backend should leave the environment alone
// (non-Turnip drivers ignore the variable, so unsetting is a no-op there).
inline const char *resolveAutotune(const char *env, bool turnipServesAdreno8xx)
{
    if (!isAutoInput(env))
        return env;
    return turnipServesAdreno8xx ? "prefer_sysmem" : nullptr;
}
} // namespace ps2x::gpu_resolve
