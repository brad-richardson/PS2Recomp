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

namespace ps2_game_module
{
    // 2 (module-seam-2): the kernel syscall hooks below.
    inline constexpr uint32_t kAbiVersion = 2u;

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
    };

    // The module linked into this binary, or nullptr (see above).
    const Module *linkedModule() noexcept;

    // linkedModule() after the ABI check; aborts on a version mismatch.
    const Module *installed();
}

#endif // PS2_RUNTIME_GAME_MODULE_H
