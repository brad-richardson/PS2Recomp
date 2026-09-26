#pragma once

// SS1: per-owner save-state serializers. Each is a friend of the class it
// reads, so owners only gain one friend line. Section versions live here:
// bump one when its layout changes and old files refuse cleanly.

#include "runtime/ps2_savestate.h"

#include <string>

class EeScheduler;
class PS2Runtime;
class PS2Memory;
class VU1Interpreter;
class GS;

namespace ps2_savestate
{
    inline constexpr uint32_t kHeaderVersion = 1u;
    inline constexpr uint32_t kMemoryVersion = 1u;
    inline constexpr uint32_t kRuntimeVersion = 1u;
    inline constexpr uint32_t kSchedulerVersion = 1u;
    inline constexpr uint32_t kVuVersion = 1u;
    inline constexpr uint32_t kGsVersion = 2u; // v2: paraLLEl blob carries an optional CLUT tail
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
    static bool loadKernel(PS2Runtime &rt, ps2_savestate::Reader &r);
};

struct VU1InterpreterSavestate
{
    static void save(const VU1Interpreter &vu, ps2_savestate::Writer &w);
    static bool load(VU1Interpreter &vu, ps2_savestate::Reader &r);
};

struct GSSavestate
{
    // Worker thread (or inline without a worker), after a drain.
    static std::string ready(const GS &gs);
    static void save(GS &gs, ps2_savestate::Writer &w);
    static bool load(GS &gs, ps2_savestate::Reader &r);
};
