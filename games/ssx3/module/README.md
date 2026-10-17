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
| `ssx3-copied-payload` (K1) + syscall hooks (ABI 2) | none (always on for SSX 3) | `src/copied_payload.cpp`, `include/Ssx3CopiedPayload.h`; savestate section `syscalls:k1` | `tests/ps2_ssx3_copied_payload_tests.cpp`; generic: `GameModuleNone` |
| `ssx3-sif-handshake` (P1ac) + SendCmd hook (ABI 3) | none (always on for SSX 3) | `src/sif_handshake.cpp`, `include/Ssx3SifHandshake.h` | `tests/ps2_ssx3_sif_handshake_tests.cpp`; shared kernel SIF tests stay in `ps2_runtime_kernel_tests.cpp`; generic: `GameModuleNone` stub + owned PC |
| SND progress hooks + savestate providers (ABI 4, snd-seam-1 family A) | none (always on for SSX 3; AU10: guest time with sound on or off) | `include/ps2_snd_spike.h`, `include/ps2_snd_spu.h`, `include/ps2_snd_spu_gauss.h` (moved from `ps2xRuntime/include/`); generic PCM ring stays runtime-owned (`ps2xRuntime/include/runtime/ps2_pcm_ring.h`) | `tests/ps2_snd_tests.cpp` (moved from `ps2xTest/src/`); `tests/ps2_ssx3_module_tests.cpp` (hook + section wiring); generic: `GameModuleNone` (null hooks = no SND, no `snd` section) |
| SND section ownership (snd-seam-1 family B) | none | `src/ssx3_module.cpp` (`snd` section, version 1, K1 registration pattern) | `tests/ps2_ssx3_module_tests.cpp` (section present, version 1); generic: `GameModuleNone` (section absent) |
| Tricky song mix provider (ABI 5, snd-seam-1 family #4) | none (output-only; default silent) | `src/tricky_song.cpp`, `include/Ssx3TrickySong.h` (forwards to `ps2_ssx3_tricky_song.h`, which stays runtime-side until the Tricky families move) | `tests/ps2_ssx3_tricky_song_provider_tests.cpp` (hook-path exact bytes); `tests/ps2_ssx3_module_tests.cpp` (hook wiring); shared song tests keep asserting the mix math |
| Tricky savestate + TK34 patch-grow field (ABI 6, snd-seam-1 family #8) | none | `src/tricky_savestate.cpp`, `include/Ssx3TrickyState.h` (`trickygems` section, version 1, K1 registration pattern; kernel save/validate/load field providers; bodies verbatim, bytes unchanged) | `tests/ps2_ssx3_module_tests.cpp` (provider + section wiring); shared `ps2_ssx3_tricky_gems_tests.cpp` (round trip with module, absent without); shared `ps2_savestate_tests.cpp` HMS2 (round trip with module, fail-closed without); generic: `GameModuleNone` (section absent, kernel save fails closed) |
| SSX 3 movie completion stubs (snd-seam-1 family #9) | none (unwired: no hook address runtime-verified) | `src/movie.cpp`, `include/Ssx3Movie.h` (moved verbatim from `ps2xRuntime/src/lib/Kernel/Stubs/`; `Common.h` swapped for public runtime headers per K1) | `tests/ps2_ssx3_movie_stub_tests.cpp` (handler contract: one log line, `$v0` = 1, invocation count) |
| Tricky-menu frontend override (snd-seam-1 Part 5 slice 1) | `PS2X_SSX3_TRICKY_MENU` (default off: wraps nothing) | `src/tricky_menu.cpp`, `include/Ssx3TrickyMenu.h` (whole `ssx3-tricky-menu` override moved verbatim; policy headers stay runtime-side, `tricky_list.h` + `course_manifest.h` included read-only) | `tests/ps2_ssx3_module_tests.cpp` (5th table row + knob-off no-op); Tricky-on smoke boot vs pre-move build |
| Stock-meter suppression (snd-seam-1 Part 5 slice 2) | `PS2X_SSX3_TRICKY_HUD` (default off: wraps nothing) | `src/stock_meter.cpp`, `include/Ssx3StockMeter.h` (whole `ssx3-tricky-stock-meter` override moved verbatim; layer header read-only) | `tests/ps2_ssx3_module_tests.cpp` (6th table row + knob-off no-op); Tricky-on smoke boot vs pre-move build |

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
   `[game-module] ssx3 (abi N): K override(s)` line counts the new row.
   Android and iOS compile.

If a family needs something other than a load-time override, add an
optional field to `ps2_game_module::Module` (null = no-op), call it from
the runtime where the policy is hard-coded today, and bump `kAbiVersion`.
K1 is the worked example (ABI 2): `System.cpp` called two SSX 3 functions
directly; it now calls `noteSyscallInstall` and `serveUnmappedSyscall`
through `ps2_game_module::installed()`, and the module table points them
at the moved functions. Keep the family's own enable flag inside the hook
(the hook runs for every ELF; the override row still arms it).

More notes from module-seam-2:

- A family whose whole file is SSX 3 (`Ssx3CopiedPayload.cpp`) moves with
  `git mv` too, so history follows it; then edit the moved file (includes,
  apply function out of the anonymous namespace, registration line gone).
  Prefer public runtime headers (`ps2_runtime.h`, `runtime/ps2_memory.h`)
  over the kernel's private `Syscalls/Common.h`.
- Savestate sections (`ps2_savestate::registerSection`) may move with their
  family. The registry is a `std::map` keyed by name, so the file layout
  doesn't depend on which object registers first; the module object is
  always linked because the table references it. A save made with the
  module can't be loaded by a no-module runtime ("unknown section"), which
  is expected for the generic build.
- Tests that use another test file's helpers (TestEnv, callSyscall) move
  with a private copy of the few helpers they need. A shared generic test
  that lists module-owned state (`ps2_savestate_tests.cpp`'s section list)
  drops that entry, and the module test asserts it instead.
- Check the generic suite for tests that relied on the moved override
  through `applyMatching(..."SLUS_207.72"...)`; they must move, or assert
  the no-module behavior in `GameModuleNone`.
- Budget each family as one lane build (product + generic suites + runner)
  and one `mac_gate.sh` (pool build + four det boots). Don't commit while a
  gate runs: it reads `HEAD` again for the runner-dir check.
