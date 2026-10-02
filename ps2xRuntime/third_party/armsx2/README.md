# ARMSX2 carry: adapters + platform builds (AX4b)

The ARMSX2-side carry (GS + microVU on ARMSX2 master) lives on the PCSX2
fork branch `armsx2-ssx3-one` (12 in-tree fix groups SG01–SG12; class table
and group list: `SSX3-CARRY.md` at that branch's root). Everything additive
moved here, so adapter edits no longer need a fork commit + pin bump.

GPL-3.0-or-later, like PCSX2/ARMSX2. No game data, generated code, captures
or private OM1 inputs live here (OM1 inputs arrive via `-D` paths at build
time and fail fast when unset).

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
| `build.sh` | new (unifies `microvu_libs.sh`, `bradflix_ge1_lib.sh`, GE1S, IG1) | one script: `mac\|android\|ios\|ios-sim --armsx2 <dir>` |

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
- **Android** `libge1_gs.so` via the `bradflix_ge1_lib.sh` recipe
  (Docker `ssx3-android`, shared ccache).
- **iOS** static `libge1_gs.a` (+ `libPCSX2.a`, `libcommon.a`) via the IG1
  recipe (device + sim). mv2 is not wired on iOS (needs `shaderc_combined`;
  the app runs VU1 from the OM1 offline stage).

## Fix placement (AX4 §3, short)

Our code (adapters/knobs here) first; hooks over logic; driver bugs go to
our Mesa series; policy goes in ARMSX2's own policy tables; real bugs get
their own upstream-style commit on the branch; diagnostics compile out.
Carry budget: `git diff armsx2/master...armsx2-ssx3-one -- pcsx2 common`
(excl. `microVU_OM1-arm64.inl`) stays under ~1,900 changed lines.
