# SoundTouch (vendored for AT1)

Pitch-preserving time-stretch, as PCSX2 uses for its default audio output.
Compiled statically into the runtime.

- Version: **2.4.1** (upstream release tag `2.4.1`)
- Source URL: https://www.surina.net/soundtouch/soundtouch-2.4.1.tar.gz
- Tarball SHA256: `e07abf20ce8f95850c280132e1f61ad400fc1f4011b7fac698a503de6aab6733`
- License: **LGPL-2.1** (`COPYING.TXT`, unmodified from the tarball).
  Per LGPL-2.1 section 3 the fork (GPL-3.0) applies the GPL to this code;
  static linking was approved by the orchestrator (AT1 brief, 2026-09-26).
  All upstream copyright notices are kept in the vendored files.

Vendored subset (byte-identical to the tarball; only the audio-processing
library, not the SoundStretch app, tests, or build scripts):

- `soundtouch/`: `BPMDetect.h`, `FIFOSampleBuffer.h`, `FIFOSamplePipe.h`,
  `SoundTouch.h`, `STTypes.h`
- `soundtouch/soundtouch_config.h`: our own empty stub (see the file)
- `source/SoundTouch/`: `AAFilter`, `BPMDetect`, `cpu_detect_x86`,
  `FIFOSampleBuffer`, `FIRFilter`, `InterpolateCubic`, `InterpolateLinear`,
  `InterpolateShannon`, `mmx_optimized`, `PeakFinder`, `RateTransposer`,
  `SoundTouch`, `sse_optimized`, `TDStretch` (`.cpp` + `.h` each)

Build: `CMakeLists.txt` in this dir defines the static `ps2x_soundtouch`
target (float samples, no exception handling, C++17), mirroring PCSX2's
`3rdparty/soundtouch/CMakeLists.txt`. Sample rate is set at runtime
(our source is 36 kHz); stretch parameters follow PCSX2's defaults
(sequence 30 ms, seek-window 20 ms, overlap 10 ms, quickseek off,
anti-alias filter off).
