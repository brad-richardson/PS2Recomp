# vu1dump — regenerate VU1 sources from a capture (GV2)

A `vu1cap.bin` embeds every code image its window ran. `vu1dump` feeds those
bytes back through `emitRecompSource`, so an emitter change can be validated
(and its images regenerated) without a runner build + dump boot. Full story:
`local/research/GV2/REPORT.md` in the ssx3 repo.

```sh
# Mac (needs SSE2NEON=v1.9.1 for the sse2neon shim):
ps2xRuntime/tools/vu1dump/build-mac.sh <fork-worktree> <out>
<out> <vu1cap.bin> <outdir>   # writes <outdir>/vu1_<xxh64>.cpp per image
```

With an unmodified emitter this reproduces the checked-in images byte-identically
(`cmp` clean); with a modified emitter the diff is the emission change. Only the
capture's images are covered — anything else keeps its existing file or needs a
`PS2X_VU1_RECOMP_DUMP` boot.
