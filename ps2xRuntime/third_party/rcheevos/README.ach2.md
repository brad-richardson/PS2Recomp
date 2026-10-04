# Vendored rcheevos (ACH2)

- Upstream: https://github.com/RetroAchievements/rcheevos
- Pin: `f87c0de911bb6f15dc8c1f6bf8a250d4b83e44db` ("Rebranding: softcore ->
  casual (#547)"; the ACH1 pin).
- License: MIT (`LICENSE`, upstream verbatim).
- Scope: the `rc_runtime` condition engine (`src/rcheevos/*`,
  `src/rc_compat.c`, `src/rc_util.c`, `src/rc_version.c`) and the game hash
  (`src/rhash/*`, used once at init to log the PS2 disc identity).
  `rc_client` (login, server calls, offline queue) is deliberately NOT
  vendored: ACH2 is local-only, no RA server contact. No sockets, no threads.
- Unmodified upstream files. Refresh: shallow-clone upstream, checkout the
  pin, re-copy the same file list (see `CMakeLists.txt`).
