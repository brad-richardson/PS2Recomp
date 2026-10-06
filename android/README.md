# Android SSX 3 local setup

Read the [input contract](../docs/ssx3-inputs.md) and
[private dependency gaps](../docs/ssx3-build.md) first. These commands assume
already prepared, matching EE/VU0 code, JNI libraries and GS resources. They
are not a verified disc-to-app recipe; no game-bearing APK is provided.

## Build a locally prepared preview

Use JDK 17, Gradle 8.7+ (AGP 8.6.1), SDK 34, **NDK 28.2.13676358** and SDK
CMake 3.22.1. The app requires Android API 29+ and builds only `arm64-v8a`;
default native flags target `armv8.2-a+fp16+dotprod`, so arm64 alone does not
establish device compatibility. See [AGP](build.gradle#L2) and
[toolchain/ABI](app/build.gradle#L33). Arbitrary Android devices are not validated.

From `android/`, using your local Gradle installation (no wrapper is committed):

```sh
gradle assembleRelease   -Pps2xBootElf=/storage/emulated/0/Android/data/com.ps2x.runner/files/SLUS_207.72   -Pps2xGameCodegenDir=/absolute/local/generated-ee   -Pps2xVu0RecompDir=/absolute/local/generated-vu0   -Pps2xJniLibsDir=/absolute/local/jniLibs
```

The JNI directory contains an `arm64-v8a/` subdirectory with matching GE1 and
microVU libraries (plus their runtime dependencies and Turnip if enabled).
**Do not include `libhardware.so`** there: it is built from source and Gradle
rejects a duplicate. PGO is off unless supplied; omit `ps2xPgoData` for a no-PGO
build ([properties](app/build.gradle#L6), [JNI guard](app/build.gradle#L93)).
Output: `app/build/outputs/apk/release/app-release.apk`.

## Install and place your inputs

Stop the app and back up existing saves before an upgrade. Install over the
existing app with the same signing key; do not uninstall or clear app storage:

```sh
adb shell am force-stop com.ps2x.runner
adb install -r app/build/outputs/apk/release/app-release.apk
adb shell mkdir -p /storage/emulated/0/Android/data/com.ps2x.runner/files
adb push /absolute/local/SLUS_207.72 /storage/emulated/0/Android/data/com.ps2x.runner/files/SLUS_207.72
adb push "/absolute/local/SSX 3 (USA).iso" "/storage/emulated/0/Android/data/com.ps2x.runner/files/SSX 3 (USA).iso"
```

Create a local `ps2x.env` with these paths, then push it beside the ELF:

```ini
PS2X_CD_IMAGE=/storage/emulated/0/Android/data/com.ps2x.runner/files/SSX 3 (USA).iso
PS2X_MC_ROOT=/storage/emulated/0/Android/data/com.ps2x.runner/files/mc0
```

`adb push /absolute/local/ps2x.env /storage/emulated/0/Android/data/com.ps2x.runner/files/ps2x.env`

The compiled boot path determines the env directory; `PS2X_CD_IMAGE` selects
the ISO and `PS2X_MC_ROOT` the card directory
([env paths](../ps2xRuntime/include/ps2_android_env.h#L93),
[runner paths](../ps2xRuntime/src/main.cpp#L309)). This minimal env is not the
complete play preset: stage matching GS shader resources under `files/ge1-resources`
and writable data under `files/ge1-data`. Defaults select external GE1, microVU,
VU0 recompilation and Turnip; supply those libraries or a separately validated
configuration ([defaults](../ps2xRuntime/include/ps2_knobs.h#L98)).

## Controls and 60/120

Pair a controller in Android settings before launch. The Android pad path uses
raylib gamepad index 0: bottom/right/left/top face buttons map to
Cross/Circle/Square/Triangle; shoulders map to L1/R1/L2/R2, and Menu/View to
Start/Select ([mapping](../ps2xRuntime/src/lib/ps2_pad.cpp#L50)). Check steering,
confirm/back and pause with a disposable new save before using an existing card.
The virtual-controls overlay defaults on only for iOS; other platforms require
an explicit opt-in. Use a controller for this Android setup; a touch-only Android
play path has not been validated here
([overlay enablement](../ps2xRuntime/src/lib/ps2_runtime.cpp#L2201)).

The 60 launcher reads only `ps2x.env`; the 120 launcher overlays `full120.env`
in the same directory. Fully close the app before switching; env is read once
per process ([launcher](app/src/main/java/com/ps2x/runner/LaunchActivity.java#L16),
[loader](../ps2xRuntime/src/lib/ps2_android_runtime.cpp#L61)). The base default
`PS2X_SSX3_SIM_MODE=split120_render60_v1` gives 120 physics with 60 drawing.
The full-120 layer uses `PS2X_SSX3_SIM_MODE=stock` and
`PS2X_SSX3_FULL120=events`, with the validated fix groups and display/pacing
settings for that build. Full 120 is the intended play default; 60 drawing is
the user's option. The separate layer must be staged: selecting the launcher
alone does not generate it ([layer semantics](../ps2xRuntime/ios/full120.env#L5)).

## Protect saves and upgrade keys

Cards are directories: default `<ELF directory>/mc0`, or the directory named
by `PS2X_MC_ROOT`. When that directory is named `mc0`, port 1 uses its sibling
`mc1`; with another name, port 1 uses `<name>_slot1`
([card paths](../ps2xRuntime/src/lib/Kernel/Stubs/MemoryCard.cpp#L124)). With the
app stopped, copy the entire files directory to a **new** local backup directory:

```sh
adb pull /storage/emulated/0/Android/data/com.ps2x.runner/files /absolute/local/new-backup-directory
```

Verify the copy contains your active card directory and settings. If cards live
elsewhere via `PS2X_MC_ROOT`, back up that directory and its port-1 directory too.
Never seed or overwrite an existing card when staging a new build. Keep backups
private. Restore only deliberately, with the app stopped.

Release builds currently use the debug signing key, `versionCode=1` and
`versionName=0.1.0` ([configuration](app/build.gradle#L41),
[signing](app/build.gradle#L119)). Another machine's debug key may prevent an
in-place upgrade. Preserve your key; a signing error is not a reason to
uninstall a save-bearing app. See [troubleshooting](../docs/ssx3-troubleshooting.md).
