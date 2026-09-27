# vu1bench — VU1 run capture + replay benchmark (VRB1)

Capture a race window's VU1 work once during a det boot, then replay it through
the runtime's VU1 core + compiled-in generated images in seconds — on the Mac or
as an adb-shell binary on the Odin — with bit-exact output checking. Full story:
`local/research/VRB1/REPORT.md` in the ssx3 repo.

## Capture (game boot, default off)

```sh
PS2X_VU1_CAPTURE=<dir>        # writes <dir>/vu1cap.bin (must exist)
PS2X_VU1_CAP_FROM=2400        # first vsync tick recorded
PS2X_VU1_CAP_TO=2500          # last tick recorded, inclusive
```

Records every VU1 MSCAL run at the `execute` boundary: input registers + data,
code-image identity, output registers + data, guest cycles, XGKICK bytes.
~7 KB/run uncompressed (gzip after the boot). MSCNT resumes are counted, never
recorded (format v1 is MSCAL-only). Capture reads guest state only: a det boot
with capture on stays det-identical (gated in VRB1 Part 1).

Canonical windows: `t2400–2500` (early race, 88,745 jobs) and `t6000–6100`
(mid-race, 129,904 jobs), both I26-FAST, `PS2X_MTVU=1 PS2X_VU1_BLOCKS=1`.

## Build the bench

```sh
# Mac (needs SSE2NEON=v1.9.1 for the sse2neon shim):
ps2xRuntime/tools/vu1bench/build-mac.sh <fork-worktree> <vu1gen-dir> <out>
# Android arm64 adb-shell binary (run where the NDK lives; default
# /opt/android-sdk/ndk/28.2.13676358, as in the ssx3-android image):
ps2xRuntime/tools/vu1bench/build-android.sh <fork-worktree> <vu1gen-dir> <out>
```

Links the VU1 core/upper/lower/recomp TUs + the vu1gen set with the runner's
Release ARM recipe (`-O3 -DNDEBUG -DPS2X_VU1_FMAC_SIMD=1 -DUSE_SSE2NEON
-ffp-contract=off`; Android adds `-march=armv8-a+fp+simd+crypto+crc`). The bench
refuses images with no compiled-in match, so replay always runs the same
generated code as the game.

## Run

```sh
PS2X_VU1_BLOCKS=1 ./vu1bench <vu1cap.bin> [--cpu N] [--repeat N] [--skip-verify]
```

Verifies every run's data, registers, cycles and XGKICK bytes bit-exact (exit 1
on any mismatch, first 5 printed), and reports per-image/per-entry timing, total
ms per guest-frame equivalent, and an FNV checksum over replayed outputs (equal
across hosts for the same capture + repeat count). `--cpu` pins on Linux/Android
(Mac: unsupported, same as the runtime). Env is inherited: capture and replay
with the same `PS2X_VU1_BLOCKS` / `PS2X_VU_FLOAT` mode. Timing wraps `execute()`
only (RV11 M1's common boundary); discard the first pass per invocation (cold).
