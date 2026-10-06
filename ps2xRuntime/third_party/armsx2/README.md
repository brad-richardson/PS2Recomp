# ARMSX2 carry: adapters + platform builds (AX4b)

New local preparation/build entry: [tools/ssx3](../../../tools/ssx3/README.md). Supported build host: macOS Apple silicon; play targets: Android arm64 / iOS. Windows/x86 is unsupported.

The ARMSX2-side carry (GS + microVU on ARMSX2 master) lives on the PCSX2
fork branch `armsx2-ssx3-one` (12 in-tree fix groups SG01–SG12; class table
and group list: `SSX3-CARRY.md` at that branch's root). Everything additive
moved here, so adapter edits no longer need a fork commit + pin bump.

GPL-3.0-or-later, like PCSX2/ARMSX2. No game data, generated code, captures
or private OM1 inputs live here (OM1 inputs arrive via `-D` paths at build
time and fail fast when unset).

## External-user build status

The new tools/ssx3 entry supplies a public Mac dependency/library build and
an unvalidated Android source dependency/library command. The older `build.sh`
is the maintainer's host recipe: Mac requires private pinned GE1 dependencies
and OM1RT inputs; Android delegates to a private remote build helper; iOS
requires a pre-staged dependency bundle. The script has no `--help`/dry-run
mode and does not pin or fetch the supplied ARMSX2 checkout
([parser and checks](build.sh#L29), [Mac inputs](build.sh#L55),
[Android delegation](build.sh#L95), [iOS bundle](build.sh#L124)).

Building GE1 does not generate SSX 3 EE/VU0 code or iOS offline VU1 tables.
Those stay in external, user-owned input directories. See the
[platform dependency inventory](../../../docs/ssx3-build.md),
[input contract](../../../docs/ssx3-inputs.md) and
[troubleshooting](../../../docs/ssx3-troubleshooting.md).

## Layout

| Dir | From (at `ax4-one 8d603d5632`) | What |
| --- | --- | --- |
| `ge1/` | `ssx3-ge1/adapter/` | GE1 C ABI (`ge1_gs_*`), host, replay, exports, capture stub |
| `mv2/` | `mv2-adapter/` | microVU C bridge (C API, completed-packet forwarder, MP1 shared VU1 data, `PS2X_MICROVU_FLAG_HACK`) |
| `mv1/` | `mv1-adapter/` | microVU bench (`mv1_replay`, census print) |
| `om1/om1_hooks.h` | `om1/om1_hooks.h` | recorder header the in-tree OM1 sites include |
| `om1/record/` | `om1-adapter/` | OM1 route-a capture tool (`om1_record`) |
| `om1/rt/` | `om1rt-adapter/` | OM1 runtime lib (offline core + `mv2_microvu` EMBED reference) |
| `platform/mac/` | new (GE1S + `microvu_libs.sh` mac) | Mac top-level project |
| `platform/android/` | `ssx3-ge1/android-platform/` | Android top-level project (+ mv2/om1 guards) |
| `platform/ios/` | `ssx3-ge1/ios-platform/` + `om1-ios-platform/` | iOS top-level project + stubs (OM1 archive bits under `om1/`) |
| `build.sh` | new (unifies prior private host recipes) | one script: `mac\|android\|ios\|ios-sim --armsx2 <dir>` |

Dropped, deduplicated onto the vendor `cmake/` tree + the Android bundle:
`ssx3/mv1-android-platform/`, `ssx3/mv2-android-platform/` (3,225 lines of
copied CMake modules). The stale `third_party/microvu/` (247fa6f-era) is
deleted; the om1rt recipe reads its bridge from the pinned OM1RT inputs.

All moved files are byte-identical to their `ax4-one` paths except the
`platform/*/CMakeLists.txt` files (rewritten for this layout;
`receipts/moved-sha.txt` in `local/research/AX4b`).

## Products

- **Mac** `libge1_gs.dylib` + `ge1_replay` (Metal + MoltenVK), and
  `libmv2_microvu.dylib` from the om1rt recipe (the Mac det lib).
- **Android** `libge1_gs.so` via a private remote Docker recipe
  ([delegation](build.sh#L120)); the staged tree also carries mv2 sources.
- **iOS** static `libge1_gs.a` (+ `libPCSX2.a`, `libcommon.a`) via the IG1
  recipe (device + sim). mv2 is not wired on iOS (needs `shaderc_combined`;
  the app runs VU1 from the OM1 offline stage).

## Fix placement (AX4 §3, short)

Our code (adapters/knobs here) first; hooks over logic; driver bugs go to
our Mesa series; policy goes in ARMSX2's own policy tables; real bugs get
their own upstream-style commit on the branch; diagnostics compile out.
Carry budget: `git diff armsx2/master...armsx2-ssx3-one -- pcsx2 common`
(excl. `microVU_OM1-arm64.inl`) stays under ~1,900 changed lines.
