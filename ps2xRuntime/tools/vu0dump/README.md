# vu0dump — regenerate a VU0 image source (VX2; was GV2's vu1dump)

`vu0dump` feeds one 4 KiB VU0 code image through `VU0Interpreter::emitRecompSource`,
so an emitter change can be validated (and the image regenerated) without a runner
build + `PS2X_VU0_RECOMP_DUMP` boot. VX2 removed vu1dump's VU1 capture mode together
with the generated VU1 path.

```sh
# Mac (needs SSE2NEON=v1.9.1 for the sse2neon shim):
ps2xRuntime/tools/vu0dump/build-mac.sh <fork-worktree> <out>
<out> <code.bin> <outdir>   # writes <outdir>/vu0e_<xxh64>.cpp
```

With an unmodified emitter this reproduces the image byte-identically (`cmp` clean).
