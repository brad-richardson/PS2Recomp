# MV2 Android microVU library

This is an Android-only, default-off VU1 engine. `PS2X_VU1_ENGINE=microvu`
loads `libmv2_microvu.so`; `PS2X_MTVU=1` is required. The default static
engine does not load PCSX2 code. Save states are refused in microVU mode.

## Source pin and patches

- ARMSX2 `247fa6f09499f69192c52c0542305ed16119518b` (GPL-3.0+).
- `armsx2.patch` applies to that clean tree. It contains MV1's test-only
  ordered Path1 sink and wrap handling, then MV2's completed-packet callback,
  the no-secondary-VU-thread embed guard, and suppression of PCSX2's EE INTC
  dispatch. Our runtime owns the worker, VIF, GIF queue, and interrupt bits.
- `bridge/` is the private C boundary. Only four `ps2x_microvu_*` symbols are
  exported from the shared library; PCSX2 symbols stay hidden.
- `platform/` is the Android CMake wrapper and modules. Its transitive
  `3rdparty` bundle is the pinned GE1 Android dependency set described in
  `ssx3/local/research/GE1/REPORT.md` under “Android dependency”.

The build layout on bradflix is:

```
MV2/
  vendor/armsx2/                 # clean pin plus armsx2.patch and bridge/
  android-platform/              # platform/ from this directory
    3rdparty/                    # GE1 bundle, mounted read-only
  android-build/                 # CMake output
```

Configure with NDK 28.2.13676358, API 28, arm64-v8a, Release, Ninja in
`ssx3-android`. Build target `mv2_microvu`, then strip the library with the
matching NDK `llvm-strip --strip-unneeded`. Stage it alongside the canonical
Turnip/HAL `jniLibs/arm64-v8a` and pass that directory to
`local/tooling/build/bradflix_apk.sh <fork-sha> <name> --jnilibs <dir>`.
The GE1/MV1 reports carry the exact Docker invocation and dependency SHAs.

The ABI header in `bridge/ps2_microvu_api.h` must byte-match
`ps2xRuntime/include/ps2_microvu_api.h`. The app calls the bridge inside its
existing MTVU worker. It transfers 16 KiB VU data at each execution boundary;
on a changed micro-memory generation it copies the code and calls
`CpuMicroVU1.Clear` over the changed range before entering compiled code.
Completed XGKICK packets go synchronously into the app's ordered Path1 submit.
