# SSX 3 game module

SSX 3 policy that used to be compiled into the generic runtime lives here,
behind the plug-in point in `ps2xRuntime/include/runtime/game_module.h`
(repo-split design step 4). The runtime never includes these headers.

- `ssx3_game_module` is a static library. The root `CMakeLists.txt` attaches
  it to `ps2EntryRunner`, and `ps2xTest` attaches it to `ps2x_tests`, with
  `ps2x_attach_game_module(<target> ssx3_game_module)`.
- `-DPS2X_GAME_MODULE=none` configures the generic runtime with no game
  module. `ps2x_tests_generic` is always built without one.
- The module table (`src/ssx3_module.cpp`) lists load-time overrides. The
  runtime applies its built-in descriptors first, then these, in table
  order.

## Moved families

| Override | Knob | Source | Tests |
| --- | --- | --- | --- |
| `ssx3-anim-pick-guard` (HNG2/HNG3) | `PS2X_SSX3_ANIM_PICK_GUARD` (default on) | `src/anim_pick_guard.cpp`, `include/ps2_ssx3_anim_pick_guard.h` | `tests/ps2_ssx3_anim_pick_guard_tests.cpp`, `tests/ps2_ssx3_module_tests.cpp` |
| `ssx3-hle-iop` (UPR1) | none (always on for SSX 3) | `src/hle_iop.cpp` | `tests/ps2_ssx3_module_tests.cpp`; generic: `GameModuleNone` |

## Moving the next family

A family is self-contained when it is a `PS2_REGISTER_GAME_OVERRIDE` whose
apply function and wrappers touch only `PS2Runtime`'s public API and their
own header. The dispatch/FIX, SND and Tricky families are not of this kind
and need new hooks first (design steps 5a–5c).

1. `git mv` the family's header into `include/` and its test file into
   `tests/`, so the blobs stay unchanged.
2. Cut the override block (comment, anonymous namespace, apply function)
   out of the runtime TU into `src/<family>.cpp`. Leave the wrappers and
   their state byte-for-byte in an anonymous namespace. Only the apply
   function gains external linkage (declare it in `src/ssx3_module_internal.h`).
   Drop the `PS2_REGISTER_GAME_OVERRIDE` line and the runtime TU's include.
3. Add a row to `kOverrides` with the same name, ELF name, entry and CRC.
   Order: a family that wraps an address another override also wraps must
   keep its old relative order (built-ins apply first, then the table).
4. Add the source to `CMakeLists.txt`, and the test file to
   `PS2X_GAME_MODULE_TESTS`. Move its `register_*` call in
   `ps2xTest/src/main.cpp` under `#if PS2X_TEST_GAME_MODULE_SSX3`.
5. Extend `kExpected` in `tests/ps2_ssx3_module_tests.cpp`, and add the
   family's guest PCs to `kModuleOwnedPcs` in
   `ps2xTest/src/game_module_none_tests.cpp`.
6. Gate: both suites (`ps2x_tests`, `ps2x_tests_generic`), the four det keys
   IDENTICAL, `nm` on the runner shows the moved symbols, and the boot log's
   `[game-module] ssx3 (abi 1): N override(s)` line counts the new row.
   Android and iOS compile.

If a family needs something other than a load-time override, add an
optional field to `ps2_game_module::Module` (null = no-op), call it from
the runtime where the policy is hard-coded today, and bump `kAbiVersion`.
