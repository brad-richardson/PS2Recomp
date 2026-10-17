#pragma once

// full120 slice 3: dispatch cold side (hook-interest table content) moved
// from ps2_fh1_full120.h to games/ssx3/module/src/fh1_dispatch.cpp. The
// runtime reads it through Module.fh1BuildHookInterest; this header
// declares the module-side entry point. Config gathering, the table cache
// and the lean-front filter stay runtime-side; the k* PC constants stay
// runtime-side until their last consumers move (slice 4+).

namespace ps2_fh1
{
struct HookConfig;
struct HookInterest;
} // namespace ps2_fh1

namespace ps2_ssx3_fh1
{
void buildHookInterest(const ps2_fh1::HookConfig *cfg, ps2_fh1::HookInterest *out);
}
