// SPDX-License-Identifier: GPL-3.0-or-later
// OM1 Part 4: private offline-microVU core, exported through
// ps2_microvu_api.h only (same four symbols as MV2's library).
//
// This file clones MV2's third_party/microvu/bridge/MicrovuBridge.cpp flow
// (seed/sync/execute/export/Path1) with three deliberate deltas, all marked
// below: (1) init runs the no-JIT data-only bring-up instead of the test-env
// Initialize (which would compile dispatchers); (2) seed() runs on EVERY run
// instead of once (a static restart between runs leaves microVU-live state
// stale; re-seeding is a no-op exactly when live is already correct);
// (3) Execute is replaced by the offline dispatch, whose MISS the caller
// converts to a static restart (PS2X_MICROVU_MISS).
#include "ps2_microvu_api.h"

#include "OfflineCore.h"

#include "Config.h"
#include "Gif_Unit.h"
#include "VU.h"
#include "VUmicro.h"
#include "common/FPControl.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <vector>

namespace {
bool s_ready = false;
uint64_t s_code_generation = std::numeric_limits<uint64_t>::max();
// Path1 capture (bench's shape, NOT MV2's complete-callback: the recorded
// chunks' XGKICKs land in the sink without firing complete — smoke evidence:
// 33 real served jobs, 0 complete calls. packet_sizes splits the stream back
// into per-packet submissions, MV2's cadence).
std::vector<uint8_t> s_path1;
std::vector<uint32_t> s_path1_sizes;

// MV2's seed verbatim (regs only; Micro/Mem are synced below, like MV2).
void seed(const ps2x_microvu_state& state)
{
    for (unsigned r = 0; r < 32; ++r)
        for (unsigned lane = 0; lane < 4; ++lane)
            VU1.VF[r].UL[lane] = state.vf[r][lane];
    for (unsigned r = 0; r < 16; ++r)
        VU1.VI[r].UL = state.vi[r];
    for (unsigned lane = 0; lane < 4; ++lane)
        VU1.ACC.UL[lane] = state.acc[lane];
    VU1.q.UL = VU1.VI[REG_Q].UL = state.q;
    VU1.p.UL = VU1.VI[REG_P].UL = state.p;
    VU1.VI[REG_I].UL = state.i;
    VU1.VI[REG_R].UL = state.r;
    VU1.VI[REG_MAC_FLAG].UL = state.mac;
    VU1.VI[REG_CLIP_FLAG].UL = state.clip;
    VU1.VI[REG_STATUS_FLAG].UL = state.status;
    for (unsigned k = 0; k < 4; ++k) {
        VU1.micro_macflags[k] = state.mac;
        VU1.micro_clipflags[k] = state.clip;
        VU1.micro_statusflags[k] = state.status;
    }
    VU1.macflag = state.mac;
    VU1.clipflag = state.clip;
    VU1.statusflag = state.status;
    VU1.pending_q = state.q;
    VU1.pending_p = state.p;
    VU1.VI[REG_TPC].UL = state.pc >> 3;
    VU1.cycle = state.cycles;
}

// MV2's exportState verbatim.
void exportState(ps2x_microvu_state& state, uint32_t top, uint32_t itop,
                 uint64_t entry_cycle, uint32_t budget)
{
    for (unsigned r = 0; r < 32; ++r)
        for (unsigned lane = 0; lane < 4; ++lane)
            state.vf[r][lane] = VU1.VF[r].UL[lane];
    for (unsigned r = 0; r < 16; ++r)
        state.vi[r] = VU1.VI[r].UL;
    for (unsigned lane = 0; lane < 4; ++lane)
        state.acc[lane] = VU1.ACC.UL[lane];
    state.q = VU1.VI[REG_Q].UL;
    state.p = VU1.VI[REG_P].UL;
    state.i = VU1.VI[REG_I].UL;
    state.r = VU1.VI[REG_R].UL;
    state.pc = VU1.VI[REG_TPC].UL << 3;
    state.mac = VU1.VI[REG_MAC_FLAG].UL;
    state.clip = VU1.VI[REG_CLIP_FLAG].UL;
    state.status = VU1.VI[REG_STATUS_FLAG].UL;
    state.cycles = VU1.cycle;
    state.top = top;
    state.itop = itop;
    const uint32_t stat = VU0.VI[REG_VPU_STAT].UL;
    state.stopped_d = (stat & 0x200u) != 0u;
    state.stopped_t = (stat & 0x400u) != 0u;
    state.budget_exhausted = (VU1.cycle - entry_cycle >= budget) && (stat & 0x100u);
}
} // namespace

extern "C" PS2X_MV2_EXPORT uint32_t ps2x_microvu_abi() { return PS2X_MICROVU_ABI; }

extern "C" PS2X_MV2_EXPORT int ps2x_microvu_get_stats(ps2x_microvu_stats* out)
{
    if (!out) return 0;
    olc_stats(out->served, out->fallback, out->miss, out->parks, out->jump_misses,
              out->fall_misses);
    return 1;
}

extern "C" PS2X_MV2_EXPORT int ps2x_microvu_init(const char** error)
{
    if (error) *error = nullptr;
    if (s_ready) return 1;
    // DELTA 1 vs MV2: data-only no-JIT bring-up (never compiles dispatchers).
    std::string why;
    if (!olc_init(why)) {
        static std::string held;
        held = why;
        if (error) *error = held.c_str();
        return 0;
    }
    // MV2's config verbatim (PCSX2-float math, no MTVU second thread).
    auto fpcr = FPControlRegister::GetDefault().DisableExceptions()
        .SetDenormalsAreZero(true).SetFlushToZero(true);
    fpcr.SetRoundMode(FPRoundMode::ChopZero);
    EmuConfig.Cpu.VU1FPCR = fpcr;
    FPControlRegister::SetCurrent(fpcr);
    auto& rc = EmuConfig.Cpu.Recompiler;
    rc.vu1Overflow = true;
    rc.vu1ExtraOverflow = false;
    rc.vu1SignOverflow = false;
    rc.vu1ExactMode = false;
    EmuConfig.Speedhacks.vuThread = false;
    EmuConfig.Speedhacks.vu1Instant = false;
    EmuConfig.Speedhacks.vuFlagHack = false;
    EmuConfig.Gamefixes.XgKickHack = false;
    s_ready = true;
    return 1;
}

extern "C" PS2X_MV2_EXPORT void ps2x_microvu_shutdown()
{
    if (!s_ready) return;
    gif_test_hooks::g_path1_sink = nullptr;
    gif_test_hooks::g_path1_packet_sizes = nullptr;
    gif_test_hooks::g_path1_discard = false;
    s_path1.clear();
    s_path1_sizes.clear();
    olc_shutdown();
    s_ready = false;
    s_code_generation = std::numeric_limits<uint64_t>::max();
}

extern "C" PS2X_MV2_EXPORT int ps2x_microvu_run(
    const uint8_t* code, uint32_t code_size, uint64_t generation,
    uint8_t* data, uint32_t data_size, uint32_t start_pc,
    uint32_t resume, uint32_t top, uint32_t itop, uint32_t fbrst,
    uint32_t budget, ps2x_microvu_state* state,
    ps2x_microvu_path1_fn path1, void* path1_opaque, const char** error)
{
    if (error) *error = nullptr;
    if (!s_ready || !code || !data || !state || !path1 ||
        code_size != 0x4000u || data_size != 0x4000u || !budget) {
        if (error) *error = "bad microVU run arguments or library not initialized";
        return 0;
    }
    // DELTA 2 vs MV2: seed every run (a static restart between runs leaves
    // microVU-live state stale; re-seeding is a no-op when live is correct).
    seed(*state);
    if (generation != s_code_generation) {
        uint32_t first = code_size;
        uint32_t last = 0;
        for (uint32_t i = 0; i < code_size; ++i) {
            if (VU1.Micro[i] != code[i]) {
                first = std::min(first, i);
                last = i + 1;
            }
        }
        std::memcpy(VU1.Micro, code, code_size);
        if (first == code_size) {
            first = 0;
            last = code_size;
        }
        first &= ~7u;
        last = (last + 7u) & ~7u;
        CpuMicroVU1.Clear(first, last - first);
        s_code_generation = generation;
    }
    std::memcpy(VU1.Mem, data, data_size);
    vif1Regs.top = top;
    vif1Regs.itop = itop;
    VU0.VI[REG_FBRST].UL = fbrst;
    VU0.VI[REG_VPU_STAT].UL = (VU0.VI[REG_VPU_STAT].UL & ~0x700u) | 0x100u;
    VU1.VI[REG_VPU_STAT].UL = VU0.VI[REG_VPU_STAT].UL;
    if (!resume) {
        VU1.VI[REG_TPC].UL = (start_pc & 0x3ff8u) >> 3;
        CpuMicroVU1.SetStartPC(start_pc & 0x3ff8u);
    }
    // Resume (MSVCNT): the live TPC stands (seed restored it from the
    // exported state, which the previous run left current).
    const uint64_t entry_cycle = VU1.cycle;
    auto fpcr = EmuConfig.Cpu.VU1FPCR;
    FPControlRegister::SetCurrent(fpcr);
    // Bench-shaped capture: sink + packet_sizes. A resume run continues the
    // interrupted run's stream (same job); a fresh run starts a new one.
    if (!resume) {
        s_path1.clear();
        s_path1_sizes.clear();
    }
    gif_test_hooks::g_path1_sink = &s_path1;
    gif_test_hooks::g_path1_packet_sizes = &s_path1_sizes;
    gif_test_hooks::g_path1_discard = false;
    gifUnit.gifPath[GIF_PATH_1].Reset();
    // DELTA 3 vs MV2: offline dispatch instead of Execute. TPC is already an
    // index (set above or restored by seed); the core shifts it like Execute.
    OlcDisposition const disp = olc_execute(budget);
    gif_test_hooks::g_path1_sink = nullptr;
    gif_test_hooks::g_path1_packet_sizes = nullptr;
    gif_test_hooks::g_path1_discard = false;
    if (disp != OLC_SERVED) {
        // Live VU is canonicalized by the core; the caller re-runs the
        // static engine from this job's inputs (data/state untouched above).
        // Drop the interrupted run's partial stream: the static restart
        // re-submits the whole job's packets.
        s_path1.clear();
        s_path1_sizes.clear();
        return PS2X_MICROVU_MISS;
    }
    // Drain per packet (MV2's cadence): sizes partition the sink stream.
    {
        size_t off = 0;
        for (uint32_t n : s_path1_sizes) {
            if (off + n > s_path1.size()) break; // never: sizes came with the bytes
            if (n) path1(path1_opaque, s_path1.data() + off, n);
            off += n;
        }
    }
    s_path1.clear();
    s_path1_sizes.clear();
    std::memcpy(data, VU1.Mem, data_size);
    exportState(*state, top, itop, entry_cycle, budget);
    return 1;
}
