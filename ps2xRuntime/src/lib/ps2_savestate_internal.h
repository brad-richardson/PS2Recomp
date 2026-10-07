#pragma once

// SS1: per-owner save-state serializers. Each is a friend of the class it
// reads, so owners only gain one friend line. Section versions live here:
// bump one when its layout changes and old files refuse cleanly.

#include "runtime/ps2_savestate.h"
#include "runtime/ps2_vu1.h"

#include <string>

class EeScheduler;
class PS2Runtime;
class PS2Memory;
class VU0Interpreter;
class GS;

namespace ps2_savestate
{
    inline constexpr uint32_t kHeaderVersion = 1u;
    inline constexpr uint32_t kMemoryVersion = 1u;
    inline constexpr uint32_t kRuntimeVersion = 2u; // HMS2: TK34 grow map and layout
    inline constexpr uint32_t kSchedulerVersion = 1u;
    inline constexpr uint32_t kVuVersion = 1u;
    inline constexpr uint32_t kGsVersion = 3u; // v3: CLUT tail + palette indices (S2), vertex queue (S3), footer
    inline constexpr uint32_t kSndVersion = 1u;

    // Resume hand-off: a loaded run skips the first processPendingEvents()
    // (the save was taken right after one).
    void setResumeSkip();
    bool takeResumeSkip();
} // namespace ps2_savestate

struct EeSchedulerSavestate
{
    static std::string ready(const EeScheduler &s);
    static void save(const EeScheduler &s, ps2_savestate::Writer &w);
    static bool load(EeScheduler &s, ps2_savestate::Reader &r, PS2Runtime &runtime);
};

struct PS2RuntimeSavestate
{
    static std::string ready(const PS2Runtime &rt);
    static void saveMemory(const PS2Memory &m, ps2_savestate::Writer &w);
    static bool loadMemory(PS2Memory &m, ps2_savestate::Reader &r);
    static void saveKernel(const PS2Runtime &rt, ps2_savestate::Writer &w);
    static bool validateKernel(const PS2Runtime &rt, ps2_savestate::Reader &r);
    static bool loadKernel(PS2Runtime &rt, ps2_savestate::Reader &r);
};

// VX1: one serializer for both engines (VuCore friend). The vu0 and vu1
// sections keep the pre-VX1 byte layout: VU0 writes zeros where VU1 writes
// its XGKICK pipeline (VU0 never had a live one).
struct VuSavestate
{
    static void save(const VU0Interpreter &vu, ps2_savestate::Writer &w);
    static void save(const VU1Interpreter &vu, ps2_savestate::Writer &w);
    static bool load(VU0Interpreter &vu, ps2_savestate::Reader &r);
    static bool load(VU1Interpreter &vu, ps2_savestate::Reader &r);

private:
    static void saveXgkick(const VU1Interpreter::XgkickPipeline &x, ps2_savestate::Writer &w);
    template <class D>
    static void saveCore(const D &vu, ps2_savestate::Writer &w);
    template <class D>
    static bool loadCore(D &vu, ps2_savestate::Reader &r);
};

struct GSSavestate
{
    // Worker thread (or inline without a worker), after a drain.
    static std::string ready(const GS &gs);
    // SQ1: settle host-side-only pending work (render tail, palette
    // uploads) so a requested save can land; true when it flushed
    // anything. State still awaiting the guest keeps reporting busy.
    static bool quiesce(GS &gs);
    static void save(GS &gs, ps2_savestate::Writer &w);
    static bool load(GS &gs, ps2_savestate::Reader &r);
};
