#pragma once

// Public Android preset policy. Inputs are gathered once before GE1 opens;
// this header is platform-neutral so the host suite can check the policy.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace ps2x::public_tier
{
constexpr std::uint64_t kCapableRam = 5ull * 1024ull * 1024ull * 1024ull;
constexpr std::uint32_t kQualcommVendor = 0x5143;

struct Input
{
    bool ios = false;
    std::uint64_t ramBytes = 0;
    std::uint32_t vendorId = 0;
    int panelW = 0;
    int panelH = 0;
    bool turnipAvailable = false;
    const char *driver = nullptr;
    const char *render = nullptr;
    const char *output = nullptr;
    const char *cas = nullptr;
    const char *savedResolution = nullptr;
};

struct Choice
{
    bool capable = false;
    std::string driver;
    std::string render;
    std::string output;
    std::string cas;
    // Empty leaves GE1_ANISO alone (capable keeps the compiled 16x);
    // "0" disables it (low tier, MAL2 +8.7% on Mali-G57).
    std::string aniso;
};

inline bool automatic(const char *value) { return !value || !*value || std::strcmp(value, "auto") == 0; }

inline Choice choose(const Input &in)
{
    Choice out;
    out.capable = in.ios || in.ramBytes >= kCapableRam;
    const bool is1080p = (in.panelW > 0 && in.panelH > 0 &&
                          (in.panelW < in.panelH ? in.panelW : in.panelH) == 1080);
    out.driver = automatic(in.driver) ? (!in.ios && in.vendorId == kQualcommVendor && in.turnipAvailable ? "1" : "0")
                                      : in.driver;
    out.render = out.capable ? (is1080p ? "native" : "2") : "1";
    if (!automatic(in.render))
        out.render = in.render;
    if (in.savedResolution && *in.savedResolution)
        out.render = in.savedResolution;
    if (out.capable)
        out.output = "display";
    else
    {
        float scale = 1.0f;
        if (out.render == "native" && in.panelW > 0 && in.panelH > 0)
            scale = static_cast<float>(in.panelW < in.panelH ? in.panelW : in.panelH) / 448.0f;
        else if (out.render != "native")
            scale = std::strtof(out.render.c_str(), nullptr);
        if (scale < 1.0f || scale > 8.0f)
            scale = 1.0f;
        char size[32];
        std::snprintf(size, sizeof(size), "%ux%u", static_cast<unsigned>(640.0f * scale + 0.5f),
                      static_cast<unsigned>(448.0f * scale + 0.5f));
        out.output = size;
    }
    if (!automatic(in.output))
        out.output = in.output;
    out.cas = automatic(in.cas) ? (out.capable ? "0.25" : "0") : in.cas;
    out.aniso = out.capable ? "" : "0";
    return out;
}
} // namespace ps2x::public_tier
