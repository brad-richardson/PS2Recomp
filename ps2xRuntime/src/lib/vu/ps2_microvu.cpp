// SPDX-License-Identifier: GPL-3.0-or-later
// MV2: Android-only loader for the isolated ARMSX2 microVU shared library.
// SS4: + Mac (dlopen of the OM1-built dylib) for savestate tests, and the
// savestate save-gate/load-reset below.
// OM1: + the offline engine (Mac first) with MISS->static restart.
// OM1 P5: + PS2X_MICROVU_STATIC (iOS app; the offline core is linked in,
// no dlopen; the ARMSX2-derived sources stay outside this repo).
#include "ps2_microvu.h"
#include "ps2_microvu_api.h"
#include "ps2_mtvu.h"
#include "ps2_telemetry.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_vu_state.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#if defined(PS2X_MICROVU_STATIC) || defined(__ANDROID__) || defined(__APPLE__)
#define PS2X_MICROVU_LOADABLE 1
#endif
#if defined(PS2X_MICROVU_LOADABLE) && !defined(PS2X_MICROVU_STATIC)
#include <dlfcn.h>
#endif

namespace ps2_microvu {
namespace {
#if defined(PS2X_MICROVU_LOADABLE)
struct Api {
#if !defined(PS2X_MICROVU_STATIC)
    void* handle = nullptr;
#endif
    decltype(&ps2x_microvu_init) init = nullptr;
    decltype(&ps2x_microvu_shutdown) close = nullptr;
    decltype(&ps2x_microvu_run) run = nullptr;
    decltype(&ps2x_microvu_get_stats) getStats = nullptr; // optional (offline only)
    decltype(&ps2x_microvu_vu1_data) vu1Data = nullptr;   // optional (MP1 L1)
} s_api;
// MP1 L1: the memory whose VU1 data lives in the library (adoptData).
PS2Memory* s_dataHost = nullptr;
bool s_selected = false;
std::string s_engine;
std::atomic<uint64_t> s_restarts{0};
// SS5: a cycle-budget break parks JIT-private resume state (lpState,
// resumeEntry) in the library. run() continues the job in place, so
// s_parked[unit] is set only when the continue cap hits and the job returns
// truncated; a set flag defers save states until the next library run
// supersedes the park (resume=0 discards it, resume=1 consumes it). Only VU1
// uses the library today ([1]); VU0's slot ([0]) stays clear.
std::atomic<bool> s_parked[2] = {false, false};
std::atomic<uint64_t> s_budgetBreaks{0};
// Max in-place resume iterations per run(): 64 x 65536 cycles of headroom for
// real jobs (refined H1: grouping variance pushes ordinary jobs over one
// budget), still a hang guard. Past it the job truncates (pre-SS4 behavior)
// with the park flag set.
constexpr uint32_t kMaxContinueIters = 64;

uint32_t bits(float value)
{
    uint32_t result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}
float unbits(uint32_t value)
{
    float result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}
void path1(void* opaque, const uint8_t* bytes, uint32_t size)
{
    static_cast<PS2Memory*>(opaque)->submitGifPacket(GifPathId::Path1, bytes, size);
}

void importState(ps2x_microvu_state& out, const VuState& in)
{
    for (unsigned r = 0; r < 32; ++r)
        for (unsigned lane = 0; lane < 4; ++lane)
            out.vf[r][lane] = bits(in.vf[r][lane]);
    for (unsigned r = 0; r < 16; ++r)
        out.vi[r] = static_cast<uint32_t>(in.vi[r]);
    for (unsigned lane = 0; lane < 4; ++lane)
        out.acc[lane] = bits(in.acc[lane]);
    out.q = bits(in.q);
    out.p = bits(in.p);
    out.i = bits(in.i);
    out.r = in.r;
    out.pc = in.pc;
    out.mac = in.mac;
    out.clip = in.clip;
    out.status = in.status;
    out.cycles = in.cycles;
    out.top = in.top;
    out.itop = in.itop;
}
void exportState(VuState& out, const ps2x_microvu_state& in)
{
    for (unsigned r = 0; r < 32; ++r)
        for (unsigned lane = 0; lane < 4; ++lane)
            out.vf[r][lane] = unbits(in.vf[r][lane]);
    for (unsigned r = 0; r < 16; ++r)
        out.vi[r] = static_cast<int32_t>(in.vi[r]);
    for (unsigned lane = 0; lane < 4; ++lane)
        out.acc[lane] = unbits(in.acc[lane]);
    out.q = unbits(in.q);
    out.p = unbits(in.p);
    out.i = unbits(in.i);
    out.r = in.r;
    out.pc = in.pc;
    out.mac = in.mac;
    out.clip = in.clip;
    out.status = in.status;
    out.cycles = in.cycles;
    out.top = in.top;
    out.itop = in.itop;
    out.stoppedByD = in.stopped_d != 0;
    out.stoppedByT = in.stopped_t != 0;
    out.ebit = false;
    out.haltAfterDelaySlot = false;
    out.branchPending = false;
    out.branchTarget = 0;
    out.branchDelay = 0;
}
#endif
} // namespace

bool configure(bool mtvu_threaded, std::string& error)
{
#if defined(PS2X_MICROVU_LOADABLE)
    const char* name = std::getenv("PS2X_VU1_ENGINE");
    if (!name || !*name || std::strcmp(name, "static") == 0)
        return true;
    const bool offline = std::strcmp(name, "offline") == 0;
    if (!offline && std::strcmp(name, "microvu") != 0) {
        error = "PS2X_VU1_ENGINE must be static, microvu or offline";
        return false;
    }
    if (!mtvu_threaded) {
        error = std::string(name) + " requires PS2X_MTVU=1 and no VU1 trace that disables MTVU";
        return false;
    }
#if defined(PS2X_MICROVU_STATIC)
    // Static (iOS app): the offline core is linked in; only it is available.
    // IB2: the engine summary below prints lib= in both branches.
    const char* lib = "static";
    if (!offline) {
        error = "this build links the offline core statically (microvu unavailable)";
        return false;
    }
    s_api.init = &ps2x_microvu_init;
    s_api.close = &ps2x_microvu_shutdown;
    s_api.run = &ps2x_microvu_run;
    s_api.getStats = &ps2x_microvu_get_stats;
    // IV1: the static offline core exports its VU1 data (non-null only under PS2X_OM1_LEAN=1/2).
    s_api.vu1Data = &ps2x_microvu_vu1_data;
    if (ps2x_microvu_abi() != PS2X_MICROVU_ABI) {
        error = "offline ABI mismatch (static)";
        shutdown();
        return false;
    }
#else
    const char* lib = std::getenv("PS2X_MICROVU_LIB");
    std::string fallback;
    if (!lib || !*lib) {
#if defined(__ANDROID__)
        fallback = offline ? "libom1_offline.so" : "libmv2_microvu.so";
#else
        fallback = offline ? "libom1_offline.dylib" : "libmv2_microvu.dylib";
#endif
        lib = fallback.c_str();
    }
    // RTLD_GLOBAL for offline: the recorded tables resolve their init slots
    // via dlsym(RTLD_DEFAULT) at init.
    const int mode = RTLD_NOW | (offline ? RTLD_GLOBAL : RTLD_LOCAL);
    s_api.handle = dlopen(lib, mode);
    if (!s_api.handle) {
        error = std::string(name) + " dlopen failed: " + dlerror();
        return false;
    }
    auto abi = reinterpret_cast<decltype(&ps2x_microvu_abi)>(dlsym(s_api.handle, "ps2x_microvu_abi"));
    s_api.init = reinterpret_cast<decltype(s_api.init)>(dlsym(s_api.handle, "ps2x_microvu_init"));
    s_api.close = reinterpret_cast<decltype(s_api.close)>(dlsym(s_api.handle, "ps2x_microvu_shutdown"));
    s_api.run = reinterpret_cast<decltype(s_api.run)>(dlsym(s_api.handle, "ps2x_microvu_run"));
    s_api.getStats =
        reinterpret_cast<decltype(s_api.getStats)>(dlsym(s_api.handle, "ps2x_microvu_get_stats"));
    s_api.vu1Data =
        reinterpret_cast<decltype(s_api.vu1Data)>(dlsym(s_api.handle, "ps2x_microvu_vu1_data"));
    if (!abi || abi() != PS2X_MICROVU_ABI || !s_api.init || !s_api.close || !s_api.run) {
        error = std::string(name) + " ABI/symbol mismatch";
        shutdown();
        return false;
    }
#endif
    const char* why = nullptr;
    if (!s_api.init(&why)) {
        error = why ? why : "microvu init failed";
        shutdown();
        return false;
    }
    s_selected = true;
    s_engine = name;
    std::fprintf(stderr, "[microvu] engine=%s lib=%s\n", name, lib);
    ps2x::telemetry::vuInit(); // TEL1: PS2X_VU_TELEMETRY (output-only)
#else
    (void)mtvu_threaded;
    (void)error;
#endif
    return true;
}

bool adoptData(PS2Memory& memory)
{
#if defined(PS2X_MICROVU_LOADABLE)
    // MP1 L1: the microvu engine's library exports its VU1 data memory;
    // keeping the runtime's VU1 data there removes run()'s two 16 KiB
    // staging copies (CU4 B3: the PS2X_MICROVU_BRIDGE_LEAN=0 copy-keeping
    // path is deleted). Same bytes at every point the runtime can observe
    // (EE access and det-hash ticks sync the unit first). The offline engine
    // keeps its own copies (it may MISS and restart statically).
    // IV1: the offline engine shares too when its library exports the data
    // (PS2X_OM1_LEAN=1/2; =1 keeps a per-job backup for the MISS->static restart).
    if (!s_selected || (s_engine != "microvu" && s_engine != "offline") || !s_api.vu1Data)
        return false;
    uint8_t* const lib = s_api.vu1Data();
    if (!lib)
        return false;
    memory.adoptExternalVU1Data(lib);
    s_dataHost = &memory;
    std::fprintf(stderr, "[microvu] vu1 data shared with the library (MP1 L1)\n");
    return true;
#else
    (void)memory;
    return false;
#endif
}

bool selected()
{
#if defined(PS2X_MICROVU_LOADABLE)
    return s_selected;
#else
    return false;
#endif
}

void shutdown()
{
#if defined(PS2X_MICROVU_LOADABLE)
    if (s_selected) {
        ps2x::telemetry::vuFlush(0); // TEL1: write the open window
        std::fprintf(stderr, "[microvu] engine=%s restarts=%llu breaks=%llu\n", s_engine.c_str(),
                     (unsigned long long)s_restarts.load(std::memory_order_relaxed),
                     (unsigned long long)s_budgetBreaks.load(std::memory_order_relaxed));
        if (s_api.getStats) {
            ps2x_microvu_stats st{};
            if (s_api.getStats(&st))
                std::fprintf(stderr,
                             "[microvu] lib served=%llu fallback=%llu miss=%llu parks=%llu "
                             "jump=%llu fall=%llu\n",
                             (unsigned long long)st.served, (unsigned long long)st.fallback,
                             (unsigned long long)st.miss, (unsigned long long)st.parks,
                             (unsigned long long)st.jump_misses,
                             (unsigned long long)st.fall_misses);
        }
    }
    // MP1 L1: move VU1 data back into the runtime before the library (and
    // its memory) goes away.
    if (s_dataHost) {
        s_dataHost->adoptExternalVU1Data(nullptr);
        s_dataHost = nullptr;
    }
    if (s_api.close && s_selected)
        s_api.close();
    s_selected = false;
    s_engine.clear();
    // A fresh library holds no park (the break counter stays cumulative).
    s_parked[0].store(false, std::memory_order_relaxed);
    s_parked[1].store(false, std::memory_order_relaxed);
#if !defined(PS2X_MICROVU_STATIC)
    if (s_api.handle)
        dlclose(s_api.handle);
#endif
    s_api = {};
#endif
}

std::string saveReady(const VuState& state)
{
    // SS4: at an E-bit job boundary the bridge holds no guest state outside
    // VuState + VU memories (compiled code recompiles deterministically), so
    // a save is exact. A D/T stop awaiting an MSCNT resume keeps JIT-private
    // state (rings, pending Q/P, TPC chain) that no re-seed can rebuild.
    if (state.stoppedByD || state.stoppedByT)
        return "microvu VU1 D/T stop awaiting MSCNT resume (not a job boundary)";
    // SS5: a budget break parked by the continue cap keeps the same class of
    // JIT-private resume state; the next library run supersedes the park, so
    // the save lands on a later tick like any other deferral.
#if defined(PS2X_MICROVU_LOADABLE)
    if (s_parked[1].load(std::memory_order_relaxed))
        return kBudgetParkedReason;
#endif
    return {};
}

uint64_t budgetBreaks()
{
#if defined(PS2X_MICROVU_LOADABLE)
    return s_budgetBreaks.load(std::memory_order_relaxed);
#else
    return 0;
#endif
}

bool resetForLoad(std::string& error)
{
#if defined(PS2X_MICROVU_LOADABLE)
    // SS4: drop all live JIT state (compiled code, VURegs, PATH1 sink, the
    // seed latch) so the next run re-seeds from the freshly loaded VuState
    // + VU memories. A shutdown/configure cycle, no ABI change.
    PS2Memory* const host = s_dataHost; // MP1 L1: re-share after the reload
    shutdown();
    if (!configure(ps2_mtvu::threaded(), error))
        return false;
    if (!selected()) {
        error = "microvu engine lost during load reset";
        return false;
    }
    if (host)
        adoptData(*host);
    std::fprintf(stderr, "[microvu] library reset for state load\n");
    return true;
#else
    (void)error;
    return false;
#endif
}

bool run(PS2Memory& memory, uint8_t* data, VuState& state,
         uint32_t start_pc, bool resume, uint32_t top, uint32_t itop,
         uint32_t fbrst, uint32_t budget)
{
#if defined(PS2X_MICROVU_LOADABLE)
    if (!s_selected)
        throw std::runtime_error("microvu run called without selection");
    // TEL1: program census + run wall (output-only; inert unless PS2X_VU_TELEMETRY).
    ps2x::telemetry::VuRun tel;
    if (ps2x::telemetry::vuOn())
        tel.begin(memory.getVU1Code(), PS2_VU1_CODE_SIZE, memory.getVU1CodeGeneration(), start_pc, resume,
                  memory.gs().vsyncTick.load(std::memory_order_relaxed));
    ps2x_microvu_state shadow{};
    importState(shadow, state);
    const char* why = nullptr;
    const int rc = s_api.run(memory.getVU1Code(), PS2_VU1_CODE_SIZE, memory.getVU1CodeGeneration(),
                             data, PS2_VU1_DATA_SIZE, start_pc, resume ? 1u : 0u,
                             top, itop, fbrst, budget, &shadow, path1, &memory, &why);
    if (rc == PS2X_MICROVU_MISS) {
        // The job was NOT run (data/state untouched): the caller restarts it
        // in the static engine. Only the offline engine may miss. The library
        // is untouched, so a previous park (if any) is preserved as-is.
        if (s_engine != "offline")
            throw std::runtime_error("unexpected MISS from the microvu engine");
        s_restarts.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // This run supersedes any previous park: resume=0 discards it in the
    // library (SetStartPC), resume=1 consumes it.
    s_parked[1].store(false, std::memory_order_relaxed);
    if (!rc)
        throw std::runtime_error(why ? why : "microvu execution failed");
    // SS5: a cycle-budget break continues in place (resume iterations until
    // the job ends at E-bit/D/T) so the guest result does not depend on the
    // budget. Past the continue cap the job returns truncated (pre-SS4
    // behavior) with the park flag set, and saves defer until the next run
    // supersedes the park.
    uint32_t continued = 0;
    while (shadow.budget_exhausted) {
        const uint64_t n = s_budgetBreaks.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 8 || (n % 1024) == 0)
            std::fprintf(stderr,
                         "[microvu] budget break #%llu unit=vu1 iter=%u start_pc=0x%x resume=%d gen=%llu "
                         "cycles=%llu budget=%u\n",
                         (unsigned long long)n, continued, start_pc, resume ? 1 : 0,
                         (unsigned long long)memory.getVU1CodeGeneration(),
                         (unsigned long long)shadow.cycles, budget);
        if (continued >= kMaxContinueIters) {
            s_parked[1].store(true, std::memory_order_relaxed);
            std::fprintf(stderr,
                         "[microvu] CONTINUE-CAP: unit=vu1 start_pc=0x%x resumes=%u breaks=%llu cycles=%llu; "
                         "job truncated, saves defer while parked\n",
                         start_pc, continued, (unsigned long long)n,
                         (unsigned long long)shadow.cycles);
            break;
        }
        ++continued;
        why = nullptr;
        const int rc2 = s_api.run(memory.getVU1Code(), PS2_VU1_CODE_SIZE, memory.getVU1CodeGeneration(),
                                  data, PS2_VU1_DATA_SIZE, start_pc, 1u,
                                  top, itop, fbrst, budget, &shadow, path1, &memory, &why);
        if (rc2 == PS2X_MICROVU_MISS)
            throw std::runtime_error("microvu MISS on a budget-break resume (cannot restart mid-program)");
        if (!rc2)
            throw std::runtime_error(why ? why : "microvu execution failed");
    }
    exportState(state, shadow);
    return true;
#else
    (void)memory; (void)data; (void)state; (void)start_pc; (void)resume;
    (void)top; (void)itop; (void)fbrst; (void)budget;
    throw std::runtime_error("microvu needs Android or Mac");
#endif
}
} // namespace ps2_microvu
