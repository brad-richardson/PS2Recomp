#ifndef PS2_RUNTIME_GAME_MODULE_H
#define PS2_RUNTIME_GAME_MODULE_H

// module-seam-1: the game-module plug-in point (repo-split design §2, step 4).
//
// A game module is a static library that owns one game's policy. The runtime
// knows nothing about it except this table. Each final target (runner, test
// binary) picks at most one module at link time with the CMake helper
// ps2x_attach_game_module(<target> <module-target>), which compiles a small
// glue TU into the target that defines linkedModule() and names the module's
// entry. Without glue the runtime's own definition (game_module_none.cpp, an
// archive member nothing else references) is linked and returns nullptr.
//
// No static constructors are involved: the glue is an object file of the
// final target, so the linker always keeps it, and it references the module
// entry, whose table references every registration-bearing function. Dead
// stripping cannot drop a module that is attached, and link order does not
// decide whether it registers.
//
// Precedence: applyMatching() applies the runtime's built-in descriptors
// first, then the module's overrides in table order. A module override of an
// address a built-in already replaced therefore wraps the built-in binding.
//
// Every hook is optional: a null field (or no module) is a no-op. New hooks
// are appended as fields with null defaults and bump kAbiVersion; the runtime
// refuses a module built against another version. This is a source-level
// contract compiled against the exact runtime headers, not a stable binary ABI.

#include "game_overrides.h"

#include <cstddef>
#include <cstdint>

// Dispatch cold-side shapes (full120 slice 3; defined in
// ps2_fh1_full120.h, which includes this header — forward-declared here
// so the Module table can name them).
namespace ps2_fh1
{
struct HookConfig;
struct HookInterest;
} // namespace ps2_fh1

struct R5900Context;
class PS2Runtime;
// A guest invocation the runtime must queue (defined in
// runtime/ee_scheduler.h); hooks report one through an out-parameter and
// return whether an invocation was produced.
struct GuestInvocation;

namespace ps2_savestate
{
class Writer;
class Reader;
} // namespace ps2_savestate

namespace ps2_game_module
{
    // 2 (module-seam-2): the kernel syscall hooks below.
    // 3 (sif-handshake-1): the SIF SendCmd hook below.
    // 4 (snd-seam-1 family A): the SND progress hooks below.
    // 5 (snd-seam-1 family #4): the Tricky song mix hooks below.
    // 6 (snd-seam-1 family #8): the TK34 patch-grow field providers below.
    // 7 (snd-seam-1 Part 5 slice 4): the Tricky-layer VBlank hook below.
    // 8 (full120 slice 1): the FIX-policy content below.
    // 9 (full120 slice 2): the clock policy below.
    // 10 (full120 slice 3): the dispatch cold side below.
    // 11 (full120 slice 4): the dispatch hot side below.
    // 12 (full120 slice 5): the post-call consume side below.
    // 13 (full120 slice 6): the tap-target lists below.
    // 14 (split-input-1 family #5): the input-marker pad-read hook below.
    // 15 (split-input-1 family #7): the widescreen presentation hooks below.
    // 16 (split-input-1 family #3): the VU0 visibility-test provider below.
    inline constexpr uint32_t kAbiVersion = 16u;

    // Tap-target lists (full120 slice 6). The tap bodies (srcTap,
    // fh26TapCall, fh9OnBranch, fh10HistBranch, in4NoteRider) stay generic
    // env-driven diagnostics; the game branch targets/sites they compare
    // live here, module-owned. Null (or no module) reads as zeros: no tap
    // ever fires (generic: no game hooks).
    struct Fh1TapTargets
    {
        uint32_t stickTracker = 0u;  // fh26 stick-angle tracker call
        uint32_t fh9RngTarget = 0u;  // fh9 stream-1 roll sites target
        uint32_t fh9GateTarget = 0u; // fh9 race-tick gate target
        uint32_t fh9GateSite[4] = {};
        uint32_t fh9RngSite[8] = {};
        uint32_t fh9WrapTarget = 0u; // fh9 trick-wrap recorder
        uint32_t fh9StateTarget = 0u; // fh9 control-state site
        uint32_t fh9WrapSite = 0u;   // fh9 wrap caller
        uint32_t fh9SubTarget = 0u;  // fh9 sub-state-7 gate call
        uint32_t fh9SubSite = 0u;    // fh9 sub-state-7 gate caller
        uint32_t fh9CrossTarget = 0u; // fh9 event-bit crossing setter
        uint32_t fh9ExactTarget = 0u; // fh9 event-bit exact setter
        uint32_t fh9EndTarget = 0u;   // fh9 event-bit end setter
        uint32_t fh9AdvTarget = 0u;   // fh9 per-channel advance
    };

    // FIX-policy content (full120 slice 1). The module owns the group
    // name/bit tables, the "all" composition and the mask-compat predicates;
    // env parsing stays generic. Null (or no module) is empty content: no
    // group parses, "all" is empty, every compat predicate is false.
    struct Fh1FixPolicy
    {
        uint64_t allMain = 0u;
        uint32_t all12 = 0u;
        uint64_t (*itemMain)(const char *item) = nullptr; // 0 = not a main-mask name
        uint32_t (*item12)(const char *item) = nullptr;   // 0 = not an fh12 name
        const char *(*bitNameMain)(unsigned bit) = nullptr; // nullptr = unnamed
        const char *(*bitName12)(unsigned bit) = nullptr;   // nullptr = unnamed
        bool (*asymkOk)(uint64_t mask) = nullptr;
        bool (*normcapRequested)(uint64_t mask) = nullptr;
        bool (*normcapOk)(uint64_t mask) = nullptr;
        bool (*normcapSuppressed)(uint64_t mask) = nullptr;
    };

    // Song-side snapshot for the TK47 probe line (plain scalars: the song
    // player type stays with the game). `path` points at a static label
    // ("direct"/"stretch"/"?").
    struct TrickySongProbe
    {
        uint64_t burstsStarted = 0;
        uint64_t burstsDone = 0;
        uint64_t consumed = 0;
        int32_t songPeak = 0;
        int32_t busBefore = 0;
        int32_t busAfter = 0;
        uint64_t sat = 0;
        const char *path = "?";
    };

    struct Module
    {
        uint32_t abiVersion = kAbiVersion;
        const char *name = "";
        // Load-time function overrides (matched by ELF name/entry/CRC like the
        // built-in registry), applied in table order after the built-ins.
        const ps2_game_overrides::Descriptor *overrides = nullptr;
        size_t overrideCount = 0u;
        // Kernel syscall hooks. SetSyscall reports each install after the
        // runtime records it. A syscall whose installed handler has no
        // recompiled function (code the guest copied at run time) is offered
        // to the module before the runtime's KE_ERROR drop; true means the
        // module served it and set the return registers.
        void (*noteSyscallInstall)(uint32_t syscallIndex, uint32_t handler) = nullptr;
        bool (*serveUnmappedSyscall)(uint32_t syscallNumber, uint32_t handler, uint8_t *rdram,
                                     R5900Context *ctx) = nullptr;
        // SIF hook. Offered every SendCmd after the runtime's SND/net
        // handling and extra copy, before the return register is set. The
        // module completes game-specific ready-handshakes host-side.
        void (*onSifSendCmd)(uint32_t cid, uint32_t packetAddr, uint32_t packetSize,
                             uint8_t *rdram) = nullptr;
        // SND progress hooks (family A). Each is called at the exact site
        // the SSX 3 SND HLE ran, in the same order (SND before net, the
        // extra copy, and `onSifSendCmd`). A null field (or no module)
        // skips that site, which is the generic no-SND behavior.
        //
        // `noteSndAddCmdHandler` records the cid-1 handler; true arms the
        // runtime's sound clock (the generic queue machinery stays with the
        // runtime; `sndTickCycles` below is the game timing policy).
        bool (*noteSndAddCmdHandler)(uint32_t cid, uint32_t handler, uint32_t data,
                                     uint32_t gp) = nullptr;
        // `noteSndRpc` snoops the SND init RPC (status-block address).
        void (*noteSndRpc)(const uint8_t *rdram, uint32_t sid, uint32_t fno, uint32_t send,
                           uint32_t size) = nullptr;
        // `onSndSendCmd` handles cid-0 SND commands (SPU uploads, done
        // callbacks); `onSndSetDma` captures SND-library SetDma transfers
        // (true consumes the transfer: the runtime skips the EE copy, as
        // today); `onSndSoundTick` delivers one guest-time tick. Each
        // reports its at-most-one queued invocation through `out`.
        bool (*onSndSendCmd)(uint8_t *rdram, uint64_t vsync, uint32_t ra, uint32_t cid,
                             uint32_t packet, uint32_t psize, GuestInvocation *out) = nullptr;
        bool (*onSndSetDma)(const uint8_t *rdram, uint64_t vsync, uint32_t ra, uint32_t src,
                            uint32_t dst, uint32_t size, uint32_t attr) = nullptr;
        bool (*onSndSoundTick)(uint8_t *rdram, uint64_t guestCycle, GuestInvocation *out) = nullptr;
        // EE cycles per sound tick (the old `kTickCycles`); 0 means no SND
        // clock. Read by the scheduler's SoundTick arming and chain.
        uint64_t sndTickCycles = 0u;
        // SND savestate providers (family A bodies; family B moves the
        // section registration). The runtime forwards its "snd" section to
        // these; with no module it writes/reads a defined empty payload.
        void (*sndSaveState)(ps2_savestate::Writer &w) = nullptr;
        bool (*sndLoadState)(ps2_savestate::Reader &r) = nullptr;
        // Tricky song mix hooks (snd-seam-1 family #4). The host audio
        // output calls these around its sink: init stages the song from
        // the environment, mix adds one burst over the final frames, and
        // probe snapshots the song side of the TK47 burst line. Null (or
        // no module) is silence: no staging, no mix, no probe lines —
        // the same as the knob-off player. Output-only: guest hashes
        // never cover PCM.
        void (*sndTrickySongInit)() = nullptr;
        void (*sndTrickySongMix)(int16_t *out, size_t frames, const char *path) = nullptr;
        void (*sndTrickySongProbe)(TrickySongProbe *out) = nullptr;
        // TK34 patch-grow providers (snd-seam-1 family #8). The kernel
        // save/validate/load call these at the exact field position, in
        // the same order and with the same bytes. Null (or no module)
        // fails closed: a no-module binary never round-trips game state.
        void (*patchGrowSave)(ps2_savestate::Writer &w) = nullptr;
        bool (*patchGrowValidate)(ps2_savestate::Reader &r) = nullptr;
        bool (*patchGrowLoad)(ps2_savestate::Reader &r) = nullptr;
        // Tricky-layer VBlank hook (snd-seam-1 Part 5 slice 4). The
        // scheduler calls this at the exact site the layer reducer ran,
        // with the same arguments and in the same order. Null (or no
        // module) runs the reducer inline (the generic behavior below);
        // the module forwards to the same reducer today and owns the
        // reduction once the layer moves. One cached-config check when off.
        void (*trickyLayerTick)(uint8_t *rdram, size_t ramSize, uint64_t tick) = nullptr;
        // FIX-policy content (full120 slice 1): see Fh1FixPolicy above.
        const Fh1FixPolicy *fh1FixPolicy = nullptr;
        // Clock policy (full120 slice 2): the VBlank/EE rate switch, the
        // stock-time accumulators, the VBlank commit, the manager word
        // table and its helpers. Bodies live in the module and operate on
        // the same inline guest-conversion state. Null (or no module) is
        // stock behavior: divisors 1, shifts 0, accumulators 0, commit and
        // patching no-ops, word conversion refused.
        uint32_t (*fh1SchedDivisor)() = nullptr;
        uint32_t (*fh1VblankDivisor)() = nullptr;
        uint32_t (*fh1EeClockShiftNow)() = nullptr;
        uint64_t (*fh1StockHalfTicks)() = nullptr;
        uint64_t (*fh1StockHalfTicksLegacy)() = nullptr;
        uint32_t (*fh1StockHalfStepNext)() = nullptr;
        void (*fh1OnVBlankCommit)(uint8_t *ram, uint64_t tick) = nullptr;
        bool (*fh1ApplyWords)(uint8_t *ram, uint32_t a, bool toActive, uint64_t mainMask, uint32_t fix12Mask,
                              bool playBuild) = nullptr;
        void (*fh1PatchAtManagerInit)(uint8_t *ram) = nullptr;
        void (*fh1InputChainPoolFlip)(uint8_t *ram, bool toActive) = nullptr;
        void (*fh1InputChainPoolStep)(uint8_t *ram, bool odd) = nullptr;
        bool (*fh1Fh28PoolOverride)(uint32_t address, uint64_t mainMask) = nullptr;
        // Dispatch cold side (full120 slice 3): the hook-interest table
        // content (which source/target PCs each FIX group lists). The
        // runtime owns the config gathering, the cache and the filter; the
        // module fills a caller-owned table. Null (or no module) is an
        // empty table: table mode then refuses every branch, which is the
        // generic no-game-hooks behavior.
        void (*fh1BuildHookInterest)(const ps2_fh1::HookConfig *cfg, ps2_fh1::HookInterest *out) = nullptr;
        // Dispatch hot side (full120 slice 4): the branch-chain body (the
        // old onBranchT). Runs at every dispatched guest branch the table
        // does not refuse; `fast` selects the cached-flags (FASTHOOKS=1)
        // or the original-call order. Null (or no module) skips the chain
        // (generic: no game hooks). The table check, the refused guard and
        // the call sites stay runtime-side.
        bool (*fh1OnBranch)(uint8_t *ram, R5900Context *ctx, uint32_t sourcePc, uint32_t targetPc,
                            PS2Runtime *runtime, bool fast) = nullptr;
        // Post-call consume side (full120 slice 5): the branch post-call
        // fixups (armed by chain pre-hooks) and the unwind restore. Null
        // (or no module) is a no-op (generic: no game hooks).
        void (*fh1OnReturn)(uint8_t *ram, R5900Context *ctx, uint32_t targetPc, bool returned) = nullptr;
        void (*fh1PostUnwind)(uint8_t *ram) = nullptr;
        // Tap-target lists (full120 slice 6): see Fh1TapTargets above.
        const Fh1TapTargets *fh1TapTargets = nullptr;
        // Input-marker hook (split-input-1 family #5). Offered every
        // pad read after the record tap; scalars only (the pad state
        // type stays runtime-side). True means the L3+R3 chord fired
        // and `markMsOut` (always valid) carries the trigger ms for
        // the runtime's padrec stamp. Null (or no module) skips the
        // marker: one null check per pad read.
        bool (*notePadRead)(uint16_t buttons, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry,
                           uint64_t guestVsyncTick, int port, int slot, const uint8_t *rdram,
                           uint64_t *markMsOut) = nullptr;
        // Widescreen-default presentation hooks (split-input-1 family
        // #7). Armed by the "ssx3-widescreen-default" row from
        // PS2X_WIDESCREEN (default anamorphic); null (or no module)
        // keeps the game default at both read sites.
        void (*noteWidescreenDispatch)(uint8_t *rdram, uint32_t sourcePc) = nullptr;
        bool (*widescreenAnamorphic)(void) = nullptr;
        // VU0 visibility-test provider (split-input-1 family #3). Called
        // at every VU0 start in the same order the runtime ran SSX 3's
        // 0xDB8/0x570 tests: pc match, knob mode, image match, then serve
        // (mode 1) or the check-mode pair around the engine run. mode()
        // reads PS2X_SSX3_VIS_NATIVE once (0 = engine, 1 = host, 2 =
        // both, compared). Null (or no module) runs the VU0 engine: the
        // exact/reference path.
        int (*visNativeMode)(void) = nullptr;
        bool (*visNativeMatchesPc)(uint32_t startPC) = nullptr;
        bool (*visNativeImageMatches)(const uint8_t *vu0Code, uint64_t generation) = nullptr;
        void (*visNativeRun)(R5900Context *ctx, const uint8_t *vu0Data, uint32_t startPC) = nullptr;
        void (*visNativeCheckBegin)(R5900Context *ctx, const uint8_t *vu0Data, uint32_t startPC) = nullptr;
        void (*visNativeCheckEnd)(R5900Context *ctx, uint32_t startPC) = nullptr;
    };

    // The module linked into this binary, or nullptr (see above).
    const Module *linkedModule() noexcept;

    // linkedModule() after the ABI check; aborts on a version mismatch.
    const Module *installed();
}

#endif // PS2_RUNTIME_GAME_MODULE_H
