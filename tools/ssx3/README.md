# Build SSX 3 from your own disc

Supported build host: **macOS on Apple silicon**. Play targets are **Android
arm64 and iOS**. **Windows/x86 is unsupported.** This source preview does not
provide game downloads or game-bearing app binaries. Start with the
[disc identity contract](../../docs/ssx3-inputs.md).

This entry provides a local EE generator, Mac runner build and native Metal
GS/microVU library build. The baseline uses interpreted VU0 and no PGO;
it is not the optimized device play build. A fresh clone built a signed
Android APK on macOS Apple silicon in PUB1 (2026-10-12). That build is not an
on-device acceptance result. iOS offline-VU1 preparation remains incomplete.

The runner-only path and source-built Metal/microVU libraries were validated
on macOS Apple silicon from an isolated fresh clone on 2026-10-06, using only
the caller's supported disc image. One boot reached the "Press START button"
title screen. The optional suite, Android and iOS were not validated by that
acceptance run.

## Mac

Install Xcode with its command-line and Metal toolchains, Python 3, Git,
CMake, Ninja, Homebrew LLVM and the public renderer dependencies. The build
reads Homebrew's prefix dynamically; it does not install tools for you:

```sh
brew install cmake ninja llvm python sdl3 libpng jpeg-turbo zstd lz4 webp freetype
```

Keep all outputs in an external directory; never commit generated sources.
Run from this checkout (replace the paths):

```sh
python3 tools/ssx3/build.py prepare --iso '/path/to/your/SSX 3 (USA).iso' --work /path/to/private-build
python3 tools/ssx3/build.py mac-libs --work /path/to/private-build
python3 tools/ssx3/build.py mac --work /path/to/private-build
python3 tools/ssx3/build.py run --work /path/to/private-build
```

The default Mac target is the runner. The development suite is optional and
requires analyzer/recompiler fixture targets; it is separate from the external
runner/title acceptance:

```sh
python3 tools/ssx3/build.py mac --work /path/to/private-build --suite
/path/to/private-build/runner-build/ps2xTest/ps2x_tests
```

Run that suite **from the checkout root**. The generator pin is
`9a6e02b26466f737e0ef419cc3da8d5d8ecdaf27` (public `ssx3` tip; its
`ps2xRecomp`/`ps2xAnalyzer` tree is identical to the SPR1 regen source, and
the game TOML/map — including the Equip Gear extra entry — is unchanged
since). `prepare` checks ISO and extracted ELF SHA256, clones the pinned
generator from the public fork, builds it, writes absolute local TOML
paths, and records the generated registration SHA256. Brad's optional local
play names sidecar reproduces SHA256
`ad96edb68ce78994684c736b745d79f02d88367eb4d5bfeb2d7b05d126b0867c`
(the SPR1 play pin, ABI v2). Use a fresh work directory for regeneration;
existing output is refused.

Names are optional. The public tree contains no game symbol names and
`prepare` uses the generator's default `sub_…` identifiers. For local
development, supply your own untracked sidecar with
`prepare --symbol-names /path/to/local/ssx3-symbols.tsv`; the file remains
outside the checkout. The sidecar changes identifiers and comments, so
its generated registration SHA can be compared with the play pin above.

`mac-libs` pins the public ARMSX2 fork to
`ebdd3fd45f74a77f7dc52b53c6666591c05c5b4d` (`armsx2-ssx3-four`, the play vendor), builds plutovg v1.1.0 and plutosvg v0.0.7
locally, builds GE1 and the JIT bridge, and stages public renderer resources
and metallibs. It requires no OM1 recordings, offline stage or private header
checkout. Libraries stay in the build directory and use installed system
dependencies. Keep the directory in place while running.

`run` uses the original disc path, extracted ELF and a **new card root in this
work directory**; it bypasses startup movies. It does not use another user's
cards, texture packs or play settings. See [knobs.tsv](knobs.tsv) for a
source-registry snapshot; supported defaults remain defined by
[ps2_knobs.h](../../ps2xRuntime/include/ps2_knobs.h). Registry classes describe
the source census, not a promise that every diagnostic knob is supported.

## Android APK

Use JDK 17, Android SDK 34, NDK 28.2.13676358 and CMake 3.22.1. Set
`ANDROID_HOME` and `JAVA_HOME` to your installs. EE output from `prepare` is
shared with Android; no remote build host or PGO profile is needed:

```sh
python3 tools/ssx3/build.py android-libs --work /path/to/private-build --ndk "$ANDROID_HOME/ndk/28.2.13676358" --jobs 4
python3 tools/ssx3/build.py android --work /path/to/private-build --jnilibs /path/to/private-build/jniLibs --jobs 4
python3 tools/ssx3/build.py install --work /path/to/private-build
```

The signed public build defaults to `com.ssx3.recomp`, with its own
Android `files/` directory and card. Use `--app-id your.dotted.package` on
**both** `android` and `install` to choose another id; install checks that it
matches the built APK and rewrites the staged env paths. The Java and JNI
helper classes retain the `com.ps2x.runner` source namespace. The separate
package id allows side-by-side installation with an existing play build.

`install` uses adb to install the APK and SHA-check the ISO, ELF, envs and
GE1 resources in `/sdcard/Android/data/com.ssx3.recomp/files/`. A repeat
install skips files already matching by SHA and preserves existing cards and
saves. The ISO stays outside the APK. On Android 11+, the installed app uses
its own external files directory; adb has shell access to push those files.
Use `--serial DEVICE` if adb sees more than one device. `stage` remains a
local layout preview and prints manual adb commands. The shipped
Android presets are [presets/ps2x.env](presets/ps2x.env) and
[presets/full120.env](presets/full120.env), derived from the Odin play env
with every private path removed and the SET2 settings overlay on
(`PS2X_SETTINGS_OVERLAY=1`).

JNI input must contain your locally source-built `arm64-v8a/libge1_gs.so`
and `libmv2_microvu.so`; never include `libhardware.so`, which this fork builds
from [HAL source](../../android/hwcompat/hwcompat.c). Optional custom Vulkan
comes from [Mesa sources](https://github.com/brad-richardson/mesa/tree/ssx3).
The APK stays under `android/app/build/outputs/apk/release` locally.

`android-libs` invokes the pinned ARMSX2 public source dependency recipe
`.github/workflows/scripts/android/build-dependencies.sh` with your NDK,
ABI arm64-v8a and API 29, then builds the public GE1/mv2 adapters against
that local prefix. The dependency recipe has its own parallelism setting
(based on host CPU count). It stages JNI libraries and public renderer
resources into your work directory. This source-only route avoids the old
private remote helper and bundled dependency directory. Android library/APK
compilation and signing were validated on macOS Apple silicon by PUB1.
Installation, title, a finished race, controls, sound, and save/reload
remain for device acceptance. Linux hosts, the remaster pack and Tricky
preparation have not been validated by this route.

After building, follow [Android setup/save protection](../../android/README.md)
for the 60/120 launcher layers. `build.py
android` creates a per-user signing keystore once in your work directory
(`ssx3-signing.keystore`) and reuses it on every later build, with a
monotonic `versionCode` from `version-code.txt` in the same directory.
**Back up your keystore; a different key forces an uninstall, which wipes
saves.** Do not distribute generated code or apps containing it.

## Optional local optimization and iOS

VU0 images: run your own baseline runner with `PS2X_VU0_RECOMP_DUMP`
pointing to an existing private directory. The runtime emitter directly writes
`vu0e_<hash>.cpp` when it encounters the program; no pre-existing image is
needed. Supply that directory through CMake `PS2X_VU0_RECOMP_DIR` or Gradle
`ps2xVu0RecompDir` and rebuild, then use `PS2X_VU0_RECOMP=1`. If you already
have your own raw 4 KiB microprogram image, the public `vu1_fixture_gen
--image <image.bin> <output-dir>` can re-emit it without another boot. These
outputs are game-derived and stay private. The baseline command above
builds with interpreted VU0; it does not take another user's VU images.

Android PGO: use Gradle `-Pps2xPgoGenerate=ON
-Pps2xPgoHelperDir=/path/to/this/tools/ssx3/pgo`, play with your own disc and
save, pull your own raw profile, and merge with **the matching NDK's**
`llvm-profdata merge -o profile.profdata <raw files>`. Rebuild without the GEN
flag, passing `-Pps2xPgoData=/absolute/path/profile.profdata`. Profiles include
translated guest-function identities and counts; keep them private and
regenerate after guest-code changes. The helper only flushes every 30 seconds.
PGO is optional; no profile is bundled or fetched.

[iOS base preset](presets/ios.env) and [full120 layer](presets/full120-ios.env)
export settings without a personal host/card path or automatic pad route.
They require your own game inputs and offline VU1 stage. The handwritten
[offline host machinery](offline-core/README.md) is included as source,
separate from the external recorded assembly/tables. iOS has no JIT VU1;
its OM1 assembly/tables are local game-derived outputs. The complete portable
recording/generation and app-signing recipe remains incomplete; consult the
[build inventory](../../docs/ssx3-build.md). Shader prewarm lists are optional 24-byte host pipeline selector tuples
(PS/VS/extras, no texture, asset identifier, instruction or guest memory).
[tfx-selectors.json](tfx-selectors.json) contains 247 shader configurations;
write them to your private build directory with:

```sh
python3 tools/ssx3/write_prewarm.py /path/to/private-build/tfx-prewarm.bin
```

Set `GE1_TFX_PREWARM` to that writable file if desired. The renderer appends
new host selector tuples while running. The format is defined by
`PipelineSelectorMTL` in the pinned public ARMSX2 Metal renderer; this list
is configuration, separate from the forbidden offline VU recordings.

No ISO, ELF, BIOS, generated EE/VU source, offline recording, texture/font pack,
converted course/audio, memory card, capture, PGO profile or compiled app is
included here. Source pins, identity hashes, configuration and build recipes
can be shared; game-derived outputs and personal keys/settings stay local.
Preserve dependency licences from their source checkouts.
