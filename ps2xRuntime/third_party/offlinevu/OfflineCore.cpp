// OM1 Part 4: offline microVU core for the fork runtime (Mac dylib first).
//
// Provenance: the dispatch/install/resolve/trap-mirror code below is copied
// VERBATIM from the Part 3 bench (branch om1 @ e22c8dc,
// ps2xRuntime/tools/vu1bench/om1_engines.cpp), which served 548,966 jobs with
// 0 mismatch vs the JIT oracle. Bench-verbatim code keeps its om1_ names and
// comments so a diff stays mechanical. Everything NEW for the runtime is
// marked `// OM1 P4 (new vs bench):` and uses the olc_ prefix.
//
// Deltas vs the bench (complete list):
//  1. No jitdump oracle, no WX scan, no timing, no Om1Job I/O. Seed/export/
//     GIF-arming move to the bridge (OfflineBridge.cpp, MV2's shape).
//  2. olc_execute() = the bench Execute mirror's core (VPU_STAT early-out,
//     TPC <<=3, resume disarm, lookup+resolve, trap-arm, resume-stub call,
//     >>=3, flags clear). TPC arrives as an INDEX (bridge/seed set it, as
//     the bench seed did); the budget is the runtime's.
//  3. No hwIntcIrq: MV2's EMBED mode compiles the inline irq OUT of
//     Execute (armsx2.patch @ recMicroVU1::Execute) and the runtime takes
//     completion via the exported VPU_STAT D/T bits; the flags clear stays.
//  4. olc_reset_live_vu() runs at end of init AND on every MISS return: it
//     zeroes the VURegs pipeline subset the bench seed zeroed (fmac/ialu
//     pipes+positions, fdiv, efu, ebit/branch/pcs, flags, xgkick state) and
//     restores the microVU.prog lookup seizure (quick/lpState/cur/isSame/
//     cleared/curFrame/x86 sentinels). Rationale: the bench's miss path ran
//     the oracle (full re-seed + coherent prog evolution) before the next
//     offline job; the runtime's miss path runs the STATIC engine, which
//     touches neither. Restoring makes every post-miss job start from the
//     proven fresh-init state (bench job #1 ran from exactly this state).
//     Installed programs/managers/blocks/jumpCaches are NOT touched (owned,
//     process-lifetime); stale jumpCache entries self-invalidate because
//     their key (quick[pc/8].prog) was cleared.
//  5. Counters kept (served/fallback/miss/parks/jumpMisses/fallMisses) and
//     exposed via olc_stats() for the det-gate miss/restart report.
// Uppercase markers: OM1P4_CORE.
//
// ARMSX2 pin: 247fa6f09499f69192c52c0542305ed16119518b (+MV2 hunks, OM1 tree).

#include "OfflineCore.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <setjmp.h>
#include <string>
#include <utility>
#include <vector>

#include <dlfcn.h>

// ---- ARMSX2 (bench's include set minus bench-only test-env/jit/oracle) ----
#include "Config.h"
#include "Gif_Unit.h"
#include "VU.h"
#include "VUmicro.h"
#include "VU1Trace.h"
#include "common/FPControl.h"
#include "common/AlignedMalloc.h"
#include "common/Darwin/DarwinMisc.h"
#include "Memory.h"
#include "arm64/microVU-arm64.h"
#include "arm64/microVU_Persist-arm64.h"

// The recorder TU (microVU-arm64.cpp, linked here) strongly defines
// om1::om1_no_codegen_flag=false. The offline engine sets it true at init,
// arming the abort guards at the three codegen choke points (mVUreset,
// mVUopenCodeCache, compile entry). Plain extern: this TU always links the
// provider, so the header's weak attribute is unnecessary here.
namespace om1
{
extern bool om1_no_codegen_flag;
}

// The header's `_mVUt extern ...` declarations are NOT extern-template
// declarations (wrong syntax position), so including the .inl bodies makes
// this TU implicitly instantiate _mVUt templates it odr-uses. Suppress that:
// the definitions live in microVU-arm64.cpp.
extern template void* mVUcompileJIT<0>(u32 startPC, uptr ptr);
extern template void* mVUcompileJIT<1>(u32 startPC, uptr ptr);
extern template void* mVUsearchProg<0>(u32 startPC, uptr pState);
extern template void* mVUsearchProg<1>(u32 startPC, uptr pState);

#ifndef PCSX2_RECOMPILER_TESTS
#error "offline core requires ENABLE_RECOMPILER_TEST_HOOKS=ON (gif hooks)"
#endif
// NOTE: no PS2X_MICROVU_EMBED requirement. The dylib links the PCSX2 archive
// as-is (bench-identical, thread-opening Reserve included); the offline path
// never calls Reset/Execute, so EMBED is behavior-neutral here.

// Behavior contracts the mirrors below depend on (all constexpr in
// pcsx2/arm64/microVU_Misc-arm64.h). If upstream flips one, this TU must not
// silently diverge.
static_assert(!doWholeProgCompare, "cmpProg mirror assumes range compare");
static_assert(doJumpCaching && !doJumpAsSameProgram, "jump_resolve mirror shape");
static_assert(sizeof(microRegInfo) == 96, "tables carry 96B pStates");
static_assert(!IsDevBuild, "mVUwaitMTVU replica omits the dev-only DevCon line");

// ---- symbols the recorded TEXT references ----------------------------------
// The .s files reference three TU-local (static) ARMSX2 symbols by their
// record-TU mangled names. They have no external linkage, so the consumer
// provides globals under those exact (asm-label) names:
//
//  _ZL7mVUglob      the clamp/const table (static constexpr: values identical
//                  in every TU, so a copy is exact)
//  _ZL7mVUEBitv     one fetch_or (body copied from microVU_Misc-arm64.inl:317)
//  _ZL11mVUwaitMTVUv WaitVU (body copied from microVU_Misc-arm64.inl:303;
//                  dev-only DevCon line omitted; static_assert above guards)

// Non-const: namespace-scope const implies internal linkage in C++, which would
// hide this from the recorded chunks (and clang confusingly re-underscores
// it). The chunks only read it.
alignas(32) mVU_Globals om1_mVUglob_storage asm("_ZL7mVUglob") = mVUglob;

void om1_mVUEBit_impl() asm("_ZL7mVUEBitv");
void om1_mVUEBit_impl()
{
    vu1Thread.mtvuInterrupts.fetch_or(VU_Thread::InterruptFlagVUEBit, std::memory_order_release);
}

void om1_mVUwaitMTVU_impl() asm("_ZL11mVUwaitMTVUv");
void om1_mVUwaitMTVU_impl()
{
    vu1Thread.WaitVU();
}

// ---- offline engine state --------------------------------------------------
// OM1 P4 (new vs bench): flat table headers from the private staging dir
// (-I$(PS2X_OM1_TABLES_DIR)); generated inputs, never committed.
#include "sp1_tables.h"
#include "mid_tables.h"
#include "full_tables.h"
#include "menus_tables.h"
#include "late_tables.h"
#include "sv1_tables.h"

namespace
{
struct Om1TableSet
{
    const char* tag = nullptr;
    // Normalized row views (pointers into the tag's const tables; no copy).
    struct BlockRow
    {
        uint64_t lo, hi;
        uint32_t startPC;
        uint8_t exact;
        const uint8_t* pState;
        const uint8_t* pStateEnd;
        uint8_t hasJC;
        void* code;
        uint32_t objIdx;
    };
    struct ImageRow
    {
        uint64_t lo, hi;
        const uint8_t* code;
        const uint16_t* ranges;
        uint32_t nranges;
    };
    struct SlotRow
    {
        const char* name;
        uint32_t block;
        uint32_t field;
    };
    std::vector<BlockRow> blocks;
    std::vector<ImageRow> images;
    std::vector<SlotRow> slots;
    void* resumeAddr = nullptr;
    void* exitAddr = nullptr;
    // Per-row installed block copies (parallel to blocks; filled at init).
    std::vector<microBlock*> installed;
};

std::vector<Om1TableSet> g_tags;
std::map<std::pair<uint64_t, uint64_t>, microProgram*> g_programs;
std::map<std::pair<microProgram*, uint32_t>, microBlockManager*> g_managers;
bool g_off_inited = false;
void* g_resumeAddr = nullptr;

// Counters (no locks: the runtime calls from one MTVU worker at a time).
uint64_t g_served = 0, g_fallback = 0, g_miss = 0, g_parks = 0;
uint64_t g_jumpMisses = 0, g_fallMisses = 0;

// Trap landing for fallthrough/jump misses. Armed around each resume() call.
sigjmp_buf g_trapBuf;
volatile int g_trapArmed = 0;
volatile int g_trapKind = 0; // 1 = fallthrough, 2 = jump

// mVUcmpProg replica (microVU-arm64.cpp:1559, doWholeProgCompare=false path):
// range-compare of the owned program against live micro-mem; on match sets
// cleared/cur/isSame exactly like the original.
bool om1_cmpProg(microVU& mVU, microProgram& prog)
{
    for (const microRange& range : *prog.ranges)
    {
        if (std::memcmp(reinterpret_cast<const u8*>(prog.data) + range.start,
                        reinterpret_cast<const u8*>(mVU.regs().Micro) + range.start,
                        static_cast<size_t>(range.end - range.start)) != 0)
            return false;
    }
    mVU.prog.cleared = 0;
    mVU.prog.cur = &prog;
    mVU.prog.isSame = -1;
    return true;
}

// mVUsearchProg resolve-half mirror (no deques/contentMap/hydrate/compile):
// creation-order range-compare, quick fill, block search. Returns the host
// entry or nullptr (caller falls back). `pc` arrives in the caller's units:
// masked (dispatch, like mVUexecute) or raw (jump, like mVUcompileJIT).
void* om1_slowResolve(uint32_t pc, microRegInfo* key)
{
    microVU& mVU = microVU1;
    microProgramQuick& quick = mVU.prog.quick[mVU.regs().start_pc / 8];
    for (const auto& kv : g_programs)
    {
        microProgram* prog = kv.second;
        if (!om1_cmpProg(mVU, *prog))
            continue;
        microBlockManager* block = prog->block[pc / 8];
        quick.block = block;
        quick.prog = prog;
        if (!block)
            return nullptr;
        microBlock* pBlock = block->search(mVU, key);
        if (!pBlock)
            return nullptr;
        return pBlock->hostEntry;
    }
    return nullptr;
}

#define OM1_CODE_SIZE 0x4000u

bool om1_build_tag(Om1TableSet& t, std::string& err)
{
    microVU& mVU = microVU1;
    // Pass 1: merge images by content hash (stable order: table order).
    std::vector<microProgram*> imgProg;
    imgProg.reserve(t.images.size());
    for (const auto& im : t.images)
    {
        auto key = std::make_pair(im.lo, im.hi);
        auto it = g_programs.find(key);
        if (it != g_programs.end())
        {
            imgProg.push_back(it->second);
            continue;
        }
        microProgram* prog =
            static_cast<microProgram*>(_aligned_malloc(sizeof(microProgram), 64));
        if (!prog)
        {
            err = "aligned_malloc failed for microProgram";
            return false;
        }
        std::memset(prog, 0, sizeof(microProgram));
        prog->idx = mVU.prog.total++;
        prog->ranges = new std::deque<microRange>();
        for (uint32_t r = 0; r < im.nranges; ++r)
        {
            const s32 rs = static_cast<s32>(im.ranges[r * 2]);
            const s32 re = static_cast<s32>(im.ranges[r * 2 + 1]);
            if (rs < 0 || re < rs || re > static_cast<s32>(OM1_CODE_SIZE))
            {
                err = std::string("bad range in tag ") + t.tag;
                return false;
            }
            prog->ranges->emplace_back(microRange{rs, re});
        }
        prog->startPC = 0; // never read by the resolve/dispatch paths we run
        prog->contentHash.low64 = im.lo;
        prog->contentHash.high64 = im.hi;
        prog->contentHashValid = true;
        prog->writeGenAtAnchor = mVU.microMemWriteGen;
        prog->refcount = 0;
        prog->observed.clear();
        prog->persist = nullptr;
        std::memcpy(prog->data, im.code, OM1_CODE_SIZE);
        g_programs[key] = prog;
        imgProg.push_back(prog);
    }
    // Pass 2: install blocks via the REAL microBlockManager::add (dedups
    // first-wins exactly like the JIT: repeat add()s return the first copy).
    t.installed.assign(t.blocks.size(), nullptr);
    const uint32_t jcCount = mVU.progSize / 2;
    for (size_t i = 0; i < t.blocks.size(); ++i)
    {
        const auto& b = t.blocks[i];
        if (b.objIdx >= imgProg.size())
        {
            err = std::string("block objIdx out of range in tag ") + t.tag;
            return false;
        }
        microProgram* prog = imgProg[b.objIdx];
        auto mkey = std::make_pair(prog, b.startPC);
        microBlockManager*& mgr = g_managers[mkey];
        if (!mgr)
            mgr = new microBlockManager();
        const uint32_t slot = b.startPC / 8;
        if (slot >= static_cast<uint32_t>(mProgSize / 2))
        {
            err = std::string("block startPC out of range in tag ") + t.tag;
            return false;
        }
        if (!prog->block[slot])
            prog->block[slot] = mgr;
        else if (prog->block[slot] != mgr)
        {
            err = std::string("manager collision in tag ") + t.tag;
            return false;
        }
        microBlock tmpl;
        std::memset(&tmpl, 0, sizeof(tmpl));
        std::memcpy(&tmpl.pState, b.pState, sizeof(microRegInfo));
        std::memcpy(&tmpl.pStateEnd, b.pStateEnd, sizeof(microRegInfo));
        tmpl.x86ptrStart = static_cast<u8*>(b.code);
        tmpl.hostEntry = b.code;
        tmpl.jumpCache = b.hasJC ? new microJumpCache[jcCount] : nullptr;
        microBlock* added = mgr->add(mVU, &tmpl);
        if (!added)
        {
            err = "manager add returned null";
            return false;
        }
        t.installed[i] = added;
    }
    // Pass 3: fill init slots (consumer-owned microBlock field pointers).
    for (const auto& s : t.slots)
    {
        if (s.block >= t.installed.size() || !t.installed[s.block])
        {
            err = std::string("initslot block out of range in tag ") + t.tag;
            return false;
        }
        if (s.field + sizeof(void*) > sizeof(microBlock))
        {
            err = std::string("initslot field out of range in tag ") + t.tag;
            return false;
        }
        // macOS dlsym takes the name WITHOUT the Mach-O underscore.
        const char* lookup = s.name;
        if (lookup[0] == '_')
            ++lookup;
        void* addr = dlsym(RTLD_DEFAULT, lookup);
        if (!addr)
        {
            err = std::string("initslot symbol not found: ") + s.name + " (" +
                  (dlerror() ? dlerror() : "?") + ")";
            return false;
        }
        *static_cast<void**>(addr) =
            reinterpret_cast<u8*>(t.installed[s.block]) + s.field;
    }
    return true;
}

// No-JIT init (P3.1): the data-only subset of
// RecompilerTestEnvironment::Initialize that the offline engine needs.
// Skipped deliberately (each verified unused by the offline run path):
//   - executable code buffers (iPSX2_FORCE_EE_INTERP=1 => s_code_memory=null)
//   - EE/IOP Reserve+Reset (psxRec/recCpu/intCpu/psxInt) and the VU0 rec
//   - every Reset() (dispatcher emission); microVU1 dispatch fields are
//     seized by the caller exactly as before
//   - parking lots, IOP/EE counters, psxCpu/Cpu/CpuVU wiring (no EE/IOP
//     execution; hwIntcIrq short-circuits on the zeroed INTC_MASK)
//   - cpuinfo (emitter ISA checks only; nothing emits here)
// Kept: FPU default, data mappings + zeroing, manual persist recording,
// CpuMicroVU1.Reserve (mVUinit state + the parked MTVU thread), XgKickHack
// env mirror. All compiled code (startFunct/Resume, exitFunct, model and
// waitMTVU stubs) comes from the RECORDED chunks.
bool om1_nojit_init(std::string& err)
{
    FPControlRegister::SetCurrent(FPControlRegister::GetDefault());
    DarwinMisc::iPSX2_FORCE_EE_INTERP = 1;
    if (!SysMemory::Allocate())
    {
        err = "SysMemory::Allocate failed (data mappings)";
        return false;
    }
    mVUPersist::SetTestManualRecording(true);
    CpuMicroVU1.Reserve();
    SysMemory::Reset();
    if (const char* xgkick = std::getenv("PCSX2_VU_XGKICKHACK"))
        EmuConfig.Gamefixes.XgKickHack = std::atoi(xgkick) != 0;
    return true;
}

// ---- config: verbatim copy of the adapter's MV2 setup ----------------------
void om1_config()
{
    auto fpcr = FPControlRegister::GetDefault().DisableExceptions().SetDenormalsAreZero(true).SetFlushToZero(true);
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
}

// Normalize one tag's const tables into row views (no copy of the bytes).
template <typename B, typename I, typename S>
void om1_normalize_tag(Om1TableSet& t, const char* tag, const B* blocks, uint32_t nblocks,
                       const I* images, uint32_t nimages, const S* slots, uint32_t nslots,
                       void* (*resumeFn)(), void* (*exitFn)())
{
    t.tag = tag;
    t.blocks.reserve(nblocks);
    for (uint32_t i = 0; i < nblocks; ++i)
    {
        Om1TableSet::BlockRow r;
        r.lo = blocks[i].lo;
        r.hi = blocks[i].hi;
        r.startPC = blocks[i].startPC;
        r.exact = blocks[i].exact;
        r.pState = blocks[i].pState;
        r.pStateEnd = blocks[i].pStateEnd;
        r.hasJC = blocks[i].hasJC;
        r.code = blocks[i].code;
        r.objIdx = blocks[i].objIdx;
        t.blocks.push_back(r);
    }
    t.images.reserve(nimages);
    for (uint32_t i = 0; i < nimages; ++i)
    {
        Om1TableSet::ImageRow r;
        r.lo = images[i].lo;
        r.hi = images[i].hi;
        r.code = images[i].code;
        r.ranges = images[i].ranges;
        r.nranges = images[i].nranges;
        t.images.push_back(r);
    }
    t.slots.reserve(nslots);
    for (uint32_t i = 0; i < nslots; ++i)
    {
        Om1TableSet::SlotRow r;
        r.name = slots[i].name;
        r.block = slots[i].block;
        r.field = slots[i].field;
        t.slots.push_back(r);
    }
    t.resumeAddr = resumeFn();
    t.exitAddr = exitFn();
}

void om1_add_all_tags()
{
    {
        Om1TableSet t;
        om1_normalize_tag(t, "sp1", om1SP1_blocks, om1SP1_nblocks, om1SP1_images, om1SP1_nimages,
                          om1SP1_initslots, om1SP1_ninitslots, om1SP1_stub_resume, om1SP1_stub_exit);
        g_tags.push_back(std::move(t));
    }
    {
        Om1TableSet t;
        om1_normalize_tag(t, "mid", om1MID_blocks, om1MID_nblocks, om1MID_images, om1MID_nimages,
                          om1MID_initslots, om1MID_ninitslots, om1MID_stub_resume, om1MID_stub_exit);
        g_tags.push_back(std::move(t));
    }
    {
        Om1TableSet t;
        om1_normalize_tag(t, "full", om1FULL_blocks, om1FULL_nblocks, om1FULL_images,
                          om1FULL_nimages, om1FULL_initslots, om1FULL_ninitslots, om1FULL_stub_resume,
                          om1FULL_stub_exit);
        g_tags.push_back(std::move(t));
    }
    {
        Om1TableSet t;
        om1_normalize_tag(t, "menus", om1MENUS_blocks, om1MENUS_nblocks, om1MENUS_images,
                          om1MENUS_nimages, om1MENUS_initslots, om1MENUS_ninitslots,
                          om1MENUS_stub_resume, om1MENUS_stub_exit);
        g_tags.push_back(std::move(t));
    }
    {
        Om1TableSet t;
        om1_normalize_tag(t, "late", om1LATE_blocks, om1LATE_nblocks, om1LATE_images,
                          om1LATE_nimages, om1LATE_initslots, om1LATE_ninitslots, om1LATE_stub_resume,
                          om1LATE_stub_exit);
        g_tags.push_back(std::move(t));
    }
    {
        Om1TableSet t;
        om1_normalize_tag(t, "sv1", om1SV1_blocks, om1SV1_nblocks, om1SV1_images,
                          om1SV1_nimages, om1SV1_initslots, om1SV1_ninitslots, om1SV1_stub_resume,
                          om1SV1_stub_exit);
        g_tags.push_back(std::move(t));
    }
}

// OM1 P4 (new vs bench): miss/periodic notes. ssx3_boot.py SIGTERMs the
// runner at the target tick, so shutdown() never runs; misses print exactly
// (kind + TPC + cumulative counters) and served prints every 1024 runs, so
// the boot log always holds the counts (served within 1024 of final).
void olc_note_miss(const char* kind, uint32_t tpc)
{
    std::fprintf(stderr,
                 "[om1] miss kind=%s tpc=%04x served=%llu fallback=%llu miss=%llu parks=%llu "
                 "jump=%llu fall=%llu\n",
                 kind, tpc, (unsigned long long)g_served, (unsigned long long)g_fallback,
                 (unsigned long long)g_miss, (unsigned long long)g_parks,
                 (unsigned long long)g_jumpMisses, (unsigned long long)g_fallMisses);
}

// OM1 P4 (new vs bench): post-miss / first-run canonical reset. Zeroes the
// VURegs pipeline subset the bench seed zeroed every job, and restores the
// microVU.prog lookup seizure to its init values (delta 4 in the header).
// Does NOT touch Micro/Mem/regs/TPC/cycle (the bridge seeds those per run),
// start_pc (resume continuity), or the installed programs (owned).
void olc_reset_live_vu()
{
    VURegs& vu = vuRegs[1];
    vu.ebit = vu.branch = vu.branchpc = vu.delaybranchpc = 0;
    vu.takedelaybranch = false;
    vu.flags = 0;
    vu.fmacreadpos = vu.fmacwritepos = vu.fmaccount = 0;
    vu.ialureadpos = vu.ialuwritepos = vu.ialucount = 0;
    std::memset(&vu.fdiv, 0, sizeof(vu.fdiv));
    std::memset(&vu.efu, 0, sizeof(vu.efu));
    std::memset(&vu.fmac, 0, sizeof(vu.fmac));
    std::memset(&vu.ialu, 0, sizeof(vu.ialu));
    vu.xgkickaddr = vu.xgkickdiff = vu.xgkicksizeremaining = 0;
    vu.xgkicklastcycle = 0;
    vu.xgkickcyclecount = vu.xgkickenable = vu.xgkickendpacket = 0;
    microVU& mVU = microVU1;
    std::memset(&mVU.prog.quick, 0, sizeof(mVU.prog.quick));
    std::memset(&mVU.prog.lpState, 0, sizeof(mVU.prog.lpState));
    mVU.prog.cur = nullptr;
    // total untouched: installed programs keep their idx assignments.
    mVU.prog.isSame = 0;
    mVU.prog.cleared = 0;
    mVU.prog.curFrame = 0;
    mVU.prog.x86start = mVU.prog.x86ptr = reinterpret_cast<u8*>(1);
    mVU.prog.x86end = reinterpret_cast<u8*>(static_cast<uptr>(-1));
}
} // namespace

extern "C"
{
// Fallthrough-trap landing: a recorded chunk fell off its end (no next
// chunk). Longjmps back to the run loop, which reports a miss.
void om1_fallthrough_miss()
{
    if (!g_trapArmed)
    {
        std::fprintf(stderr, "[om1] fatal: fallthrough trap with no armed run\n");
        std::abort();
    }
    g_trapKind = 1;
    siglongjmp(g_trapBuf, 1);
}

// mVUcompileJIT substitute (same (startPC, ptr) convention; the recorder
// rewrote only the BL target). Mirrors the doJumpAsSameProgram=false +
// doJumpCaching=true path of microVU_Branch-arm64.inl:560, with
// om1_slowResolve in place of mVUsearchProg and a trap in place of the
// compile on miss.
void* om1_jump_resolve(uint32_t startPC, uptr ptr)
{
    microVU& mVU = microVU1;
    mVU.regs().start_pc = startPC;
    microBlock* pBlock = reinterpret_cast<microBlock*>(ptr);
    microJumpCache& jc = pBlock->jumpCache[startPC / 8];
    if (jc.prog && jc.prog == mVU.prog.quick[startPC / 8].prog)
        return jc.hostEntry;
    void* v = om1_slowResolve(startPC, &pBlock->pStateEnd);
    if (!v)
    {
        if (!g_trapArmed)
        {
            std::fprintf(stderr, "[om1] fatal: jump miss with no armed run\n");
            std::abort();
        }
        g_trapKind = 2;
        siglongjmp(g_trapBuf, 1);
    }
    jc.prog = mVU.prog.quick[startPC / 8].prog;
    jc.x86ptrStart = v;
    jc.hostEntry = v;
    return v;
}
} // extern "C"

// OM1 P4 (new vs bench): runtime entry points.
bool olc_init(std::string& err)
{
    if (g_off_inited)
    {
        err = "offline already initialized";
        return false;
    }
    // No-JIT init: arm the codegen-abort guards FIRST, then bring up
    // the data-only subset. Any compile attempt aborts loud from here on.
    om1::om1_no_codegen_flag = true;
    if (!om1_nojit_init(err))
        return false;
    om1_config();
    if (vu1_trace::g_enabled.load(std::memory_order_relaxed))
    {
        err = "vu1 tracing enabled; offline mirror omits trace calls";
        return false;
    }
    // Seize dispatch state. Everything else in microVU1 (index, sizes,
    // flags) is whatever the no-JIT Reserve() left; there are no live
    // dispatchers (all entry stubs come from the recorded chunks).
    // prog.prog per-PC deques stay null (never consulted: mVUclear and the
    // lookup fast path touch only quick[]/lpState).
    microVU& mVU = microVU1;
    std::memset(&mVU.prog.quick, 0, sizeof(mVU.prog.quick));
    std::memset(&mVU.prog.lpState, 0, sizeof(mVU.prog.lpState));
    mVU.prog.cur = nullptr;
    mVU.prog.total = 0;
    mVU.prog.isSame = 0; // always set by resolve before any consumer reads it
    mVU.prog.cleared = 0;
    mVU.prog.curFrame = 0;
    // In-range triple so mVUcleanUp's reset check never fires. mVUexecute's
    // bounds check never runs (we never call mVUexecute).
    mVU.prog.x86start = mVU.prog.x86ptr = reinterpret_cast<u8*>(1);
    mVU.prog.x86end = reinterpret_cast<u8*>(static_cast<uptr>(-1));
    om1_add_all_tags();
    for (auto& t : g_tags)
    {
        if (!om1_build_tag(t, err))
            return false;
        std::fprintf(stderr, "[om1] tag %s: %u blocks %u images %u slots resume=%p exit=%p\n",
                     t.tag, (unsigned)t.blocks.size(), (unsigned)t.images.size(),
                     (unsigned)t.slots.size(), t.resumeAddr, t.exitAddr);
    }
    g_resumeAddr = g_tags[0].resumeAddr;
    if (!g_resumeAddr)
    {
        err = "first tag has no resume stub";
        return false;
    }
    // First-run canonical pipelines (delta 4): the bridge seeds regs-only,
    // so start the pipelines where the bench seed started every job.
    olc_reset_live_vu();
    std::fprintf(stderr, "[om1] offline ready: %u programs %u managers\n",
                 (unsigned)g_programs.size(), (unsigned)g_managers.size());
    g_off_inited = true;
    return true;
}

void olc_shutdown()
{
    // Owned programs/managers/blocks leak by design (process-lifetime core).
    // Mirror of the no-JIT init: only what was Reserved gets Shut down.
    CpuMicroVU1.Shutdown();
    SysMemory::Release();
}

// Mirror of recMicroVU1::Execute (microVU-arm64.cpp:2213, EMBED shape): the
// VPU_STAT early-out, TPC shift and resume disarm are identical to the bench;
// the startFunct call is substituted (REAL fast-path lookup + resolve
// mirror, invoked through the RECORDED resume stub, which is Execute's own
// resume-path invocation shape); the flags/irq postamble keeps the flags
// clear but drops the inline irq (EMBED compiles it out; completion flows
// via the exported VPU_STAT D/T bits). TPC arrives as an INDEX.
OlcDisposition olc_execute(uint32_t budget)
{
    if (!THREAD_VU1)
    {
        if (!(VU0.VI[REG_VPU_STAT].UL & 0x100))
        {
            ++g_served;
            return OLC_SERVED;
        }
    }
    VU1.VI[REG_TPC].UL <<= 3;
    void* const resume = std::exchange(microVU1.resumeEntry, nullptr);
    if (resume && !mVUPersist::IsRecordingEnabled())
    {
        // A cycle-budget break parked a resume: the recorded resume stub
        // cannot re-enter it (needs the live stub's register state).
        ++g_parks;
        ++g_miss;
        VU1.VI[REG_TPC].UL >>= 3;
        olc_note_miss("park", VU1.VI[REG_TPC].UL << 3);
        olc_reset_live_vu();
        return OLC_MISS;
    }
    void* entry = mVUlookupProg_VU1(VU1.VI[REG_TPC].UL, budget);
    if (!entry)
        entry = om1_slowResolve(VU1.VI[REG_TPC].UL & 0x3ff8,
                                &microVU1.prog.lpState);
    if (!entry)
    {
        ++g_fallback;
        ++g_miss;
        VU1.VI[REG_TPC].UL >>= 3;
        olc_note_miss("fallback", VU1.VI[REG_TPC].UL << 3);
        olc_reset_live_vu();
        return OLC_MISS;
    }
    g_trapArmed = 1;
    g_trapKind = 0;
    if (sigsetjmp(g_trapBuf, 1) != 0)
    {
        g_trapArmed = 0;
        ++g_miss;
        if (g_trapKind == 1)
            ++g_fallMisses;
        else if (g_trapKind == 2)
            ++g_jumpMisses;
        VU1.VI[REG_TPC].UL >>= 3;
        olc_note_miss(g_trapKind == 1 ? "fall" : (g_trapKind == 2 ? "jump" : "trap?"),
                      VU1.VI[REG_TPC].UL << 3);
        olc_reset_live_vu();
        return OLC_MISS;
    }
    reinterpret_cast<mVUrecCallResume>(g_resumeAddr)(entry, budget);
    g_trapArmed = 0;
    VU1.VI[REG_TPC].UL >>= 3;
    if (microVU1.regs().flags & 0x4 && !THREAD_VU1)
    {
        microVU1.regs().flags &= ~0x4;
        // No hwIntcIrq: EMBED shape (delta 3).
    }
    ++g_served;
    if ((g_served & 1023u) == 0)
        std::fprintf(stderr, "[om1] runs served=%llu miss=%llu\n",
                     (unsigned long long)g_served, (unsigned long long)g_miss);
    return OLC_SERVED;
}

void olc_stats(uint64_t& served, uint64_t& fallback, uint64_t& miss, uint64_t& parks,
               uint64_t& jumpMisses, uint64_t& fallMisses)
{
    served = g_served;
    fallback = g_fallback;
    miss = g_miss;
    parks = g_parks;
    jumpMisses = g_jumpMisses;
    fallMisses = g_fallMisses;
}
