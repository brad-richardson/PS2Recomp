# PS2Recomp — SSX 3 arm64 source preview

The `ssx3` branch is an SSX 3 PS2 static-recompilation fork of
[ran-j/PS2Recomp](https://github.com/ran-j/PS2Recomp). The active targets are
arm64 macOS, Android and iOS. GameCube and x86 are not supported play targets.
This is a source preview: an independent build from a disc alone is **not yet
reproducible with public tooling**. Existing play builds use private generated
inputs and dependency staging. No game-bearing downloads are provided.

Start with the [supported input contract](docs/ssx3-inputs.md), then the
[build dependency map](docs/ssx3-build.md). For a locally prepared Android
build, read [setup and save protection](android/README.md).
[Troubleshooting](docs/ssx3-troubleshooting.md) covers bounded diagnostics.

The intended play default is full 120 Hz simulation and drawing during events;
boot, menus, loading and pause retain the stock rate. The user's 60-draw option
retains 120 Hz physics. A 120 Hz display does not establish 120 Hz simulation.
The Android base configuration is the 60-draw path; its separate 120 launcher
loads a user-staged layer ([loader](ps2xRuntime/src/lib/ps2_android_runtime.cpp#L61),
[base defaults](ps2xRuntime/include/ps2_knobs.h#L113),
[iOS 120 layer](ps2xRuntime/ios/full120.env#L5)). See Android setup before choosing
an entry; checkout alone does not stage these presets or their dependencies.

## Source modules

- `ps2xAnalyzer`: ELF/function analysis and configuration; [Ghidra workflow](ps2xAnalyzer/Readme.md).
- `ps2xRecomp`: ELF + TOML to generated C++; its generic workflow is not a pinned SSX 3 preparation recipe.
- `ps2xRuntime`: EE, VU, graphics, audio and platform runtime.
- `ps2xIOP`: [IOP module execution and services](ps2xIOP/README.md).

Generated guest sources must remain outside this checkout, supplied through
`PS2X_GAME_CODEGEN_DIR` ([CMake](ps2xRuntime/CMakeLists.txt#L790)). Never commit
or link disc images, BIOS, ELFs, generated guest code, extracted textures,
replacement packs, converted courses or game audio. Users supply their own disc.

## Credits and licences

PS2Recomp upstream and this fork use [GPL v3](LICENSE). The renderer and
microVU integrations build on PCSX2/ARMSX2, with GPL-3.0-or-later adapters
([adapter documentation](ps2xRuntime/third_party/armsx2/README.md)). Optional
Turnip builds use Mesa; its components have individual licences, mostly MIT
([Mesa licence inventory](https://github.com/brad-richardson/mesa/blob/ssx3/docs/license.rst)).
ELFIO, toml11, fmt and other dependencies retain their notices. Consult the
[build map](docs/ssx3-build.md#licences-and-source-records) before selecting
sources for distribution; this short list is not a complete compliance audit.
