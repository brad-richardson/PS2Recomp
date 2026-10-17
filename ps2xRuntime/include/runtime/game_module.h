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

struct R5900Context;
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
    inline constexpr uint32_t kAbiVersion = 8u;

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
    };

    // The module linked into this binary, or nullptr (see above).
    const Module *linkedModule() noexcept;

    // linkedModule() after the ABI check; aborts on a version mismatch.
    const Module *installed();
}

#endif // PS2_RUNTIME_GAME_MODULE_H
