// SPDX-License-Identifier: GPL-3.0-or-later
// MV2: Android-only loader for the isolated ARMSX2 microVU shared library.
#include "ps2_microvu.h"
#include "ps2_microvu_api.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_vu1.h"

#include <cstdlib>
#include <cstring>
#include <stdexcept>

#if defined(__ANDROID__)
#include <dlfcn.h>
#endif

namespace ps2_microvu {
namespace {
#if defined(__ANDROID__)
struct Api {
    void* handle = nullptr;
    decltype(&ps2x_microvu_init) init = nullptr;
    decltype(&ps2x_microvu_shutdown) close = nullptr;
    decltype(&ps2x_microvu_run) run = nullptr;
} s_api;
bool s_selected = false;

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

void importState(ps2x_microvu_state& out, const VU1State& in)
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
void exportState(VU1State& out, const ps2x_microvu_state& in)
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
#if defined(__ANDROID__)
    const char* name = std::getenv("PS2X_VU1_ENGINE");
    if (!name || !*name || std::strcmp(name, "static") == 0)
        return true;
    if (std::strcmp(name, "microvu") != 0) {
        error = "PS2X_VU1_ENGINE must be static or microvu";
        return false;
    }
    if (!mtvu_threaded) {
        error = "microvu requires PS2X_MTVU=1 and no VU1 trace that disables MTVU";
        return false;
    }
    if (std::getenv("PS2X_VU1_WORKERS")) {
        error = "microvu requires PS2X_VU1_WORKERS unset";
        return false;
    }
    s_api.handle = dlopen("libmv2_microvu.so", RTLD_NOW | RTLD_LOCAL);
    if (!s_api.handle) {
        error = std::string("microvu dlopen failed: ") + dlerror();
        return false;
    }
    auto abi = reinterpret_cast<decltype(&ps2x_microvu_abi)>(dlsym(s_api.handle, "ps2x_microvu_abi"));
    s_api.init = reinterpret_cast<decltype(s_api.init)>(dlsym(s_api.handle, "ps2x_microvu_init"));
    s_api.close = reinterpret_cast<decltype(s_api.close)>(dlsym(s_api.handle, "ps2x_microvu_shutdown"));
    s_api.run = reinterpret_cast<decltype(s_api.run)>(dlsym(s_api.handle, "ps2x_microvu_run"));
    if (!abi || abi() != PS2X_MICROVU_ABI || !s_api.init || !s_api.close || !s_api.run) {
        error = "microvu ABI/symbol mismatch";
        shutdown();
        return false;
    }
    const char* why = nullptr;
    if (!s_api.init(&why)) {
        error = why ? why : "microvu init failed";
        shutdown();
        return false;
    }
    s_selected = true;
#else
    (void)mtvu_threaded;
    (void)error;
#endif
    return true;
}

bool selected()
{
#if defined(__ANDROID__)
    return s_selected;
#else
    return false;
#endif
}

void shutdown()
{
#if defined(__ANDROID__)
    if (s_api.close && s_selected)
        s_api.close();
    s_selected = false;
    if (s_api.handle)
        dlclose(s_api.handle);
    s_api = {};
#endif
}

void run(PS2Memory& memory, uint8_t* data, VU1State& state,
         uint32_t start_pc, bool resume, uint32_t top, uint32_t itop,
         uint32_t fbrst, uint32_t budget)
{
#if defined(__ANDROID__)
    if (!s_selected)
        throw std::runtime_error("microvu run called without selection");
    ps2x_microvu_state shadow{};
    importState(shadow, state);
    const char* why = nullptr;
    if (!s_api.run(memory.getVU1Code(), PS2_VU1_CODE_SIZE, memory.getVU1CodeGeneration(),
                   data, PS2_VU1_DATA_SIZE, start_pc, resume ? 1u : 0u,
                   top, itop, fbrst, budget, &shadow, path1, &memory, &why))
        throw std::runtime_error(why ? why : "microvu execution failed");
    exportState(state, shadow);
#else
    (void)memory; (void)data; (void)state; (void)start_pc; (void)resume;
    (void)top; (void)itop; (void)fbrst; (void)budget;
    throw std::runtime_error("microvu is Android-only");
#endif
}
} // namespace ps2_microvu
