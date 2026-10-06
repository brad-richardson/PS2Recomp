# Build SSX 3 from your own disc

Supported build host: **macOS on Apple silicon**. Play targets are **Android
arm64 and iOS**. **Windows/x86 is unsupported.** This source preview does not
provide game downloads or game-bearing app binaries. Start with the
[disc identity contract](../../docs/ssx3-inputs.md).

This entry provides a local EE generator, Mac runner build and native Metal
GS/microVU library build. The baseline uses interpreted VU0 and no PGO;
it is not the optimized device play build. Android dependency/library and APK construction is exposed
below but is unvalidated here. iOS offline-VU1 preparation remains incomplete. No Android or iOS acceptance is
claimed by a Mac title boot.

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
`fc1f3effa3f2a79f4482ac0b7952191fa5f3a51e` (HL3); the current game TOML/map
includes the Equip Gear extra entry. `prepare` checks ISO and extracted ELF
SHA256, clones the historical generator from the public fork, builds it, writes
absolute local TOML paths, and checks registration SHA256
`e982523ca1aa271500c5a40d9562fa27e2926ca87393f5d88cb40eb8157af789`.
Use a fresh work directory for regeneration; existing output is refused.

`mac-libs` pins the public ARMSX2 fork to
`46093cbc36c59e11992163a2054fdc4795bb8108` (UPR2 on ARMSX2 2.8.1+, including the texture-directory
API required by this GE1 adapter), builds plutovg v1.1.0 and plutosvg v0.0.7
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
```

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
compilation and on-device playback have not been validated by this lane;
record errors locally rather than substituting another user's binary pins.

After building, follow [Android setup/save protection](../../android/README.md)
for local ISO/ELF/resource staging and the 60/120 launcher layers. Use your own
signing key and preserve it for upgrades. Do not distribute generated code or
apps containing it.

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
