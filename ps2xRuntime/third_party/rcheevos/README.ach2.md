# Vendored rcheevos (ACH2)

- Upstream: https://github.com/RetroAchievements/rcheevos
- Pin: `f87c0de911bb6f15dc8c1f6bf8a250d4b83e44db` ("Rebranding: softcore ->
  casual (#547)"; the ACH1 pin).
- License: MIT (`LICENSE`, upstream verbatim).
- Scope: the full library at this pin: `rc_runtime` condition engine
  (`src/rcheevos/*`, `src/rc_compat.c`, `src/rc_util.c`, `src/rc_version.c`),
  the game hash (`src/rhash/*`, used once at init to log the PS2 disc
  identity), `rc_client` (`src/rc_client*.c`) and the api builders
  (`src/rapi/*`). `src/rc_libretro.*` is NOT vendored (libretro glue we
  never build).
- Why the full library: our engine only calls `rc_runtime`/`rc_hash` and
  never touches `rc_client` (ACH2 is local-only, no RA server contact; no
  sockets, no threads in our path). But the PCSX2 GE1 archives bundle
  their own newer rcheevos copy, whose `rc_*` symbols collide with ours at
  link time (earlier archive wins, silently swapping the engine under our
  version-locked headers). So the GE1 link excludes their copy
  (`ps2xRuntime/CMakeLists.txt`, `rcheevos/*.a` filter) and our copy backs
  their Achievements TU too. Single `rc_*` implementation, our pin,
  everywhere.
- One non-upstream file: `src/rc_client_compat_ach2.c`, a 1-symbol shim for
  `rc_client_set_unofficial_enabled()` (added upstream after f87c0de;
  called by PCSX2's Achievements TU). f87c0de's client never loads
  unofficial achievements, which is exactly the disabled behavior.
- Unmodified upstream files otherwise. Refresh: shallow-clone upstream,
  checkout the pin, re-copy the same file list (see `CMakeLists.txt`).
  If the refresh pin has `rc_client_set_unofficial_enabled`, delete the
  compat shim.
