# GX occlusion query guard

A crash on the game's render thread (`GXRenderThread`, a guest fault at `eboot+0x2169edc` reading
address 0) comes from an occlusion query that never got its handle. When the device's allocator
fails, the query's creation leaves the handle null, and nothing checks for it. The render command
later begins and ends that query, and both read through the null handle.

The guard (`InstallGxQueryGuard` in `gpu/shim/bbport_guest_hooks.cpp`) detours three places in the
1.09 eboot:

| Offset | What | Guard |
| --- | --- | --- |
| `0xe2f134` | the query's begin, after its state check | a query without a handle whose creation failed is created again; one still without a handle is not begun (its state stays 0, so its result is 0: nothing visible) |
| `0xe2f164` | the query's end | a query without a handle is not ended |
| `0x21675b0` | the creation's common exit | notes each failed creation (result 8 or 9), and forgets the query once it is created |

A null handle without a failed creation (a query released while the render thread holds it) is
only skipped: creating it would write into a freed object. Failed creations are logged once a
minute.

- `BB_GX_QUERY_GUARD=0`: not installed.
- `BB_RE_GX_QUERY=1`: logs the first creations and guard events.
- `BB_GX_QUERY_FAIL=N` (diagnostics): every N-th creation fails as with a full heap, so the
  guard's paths run in any session. `N = 2` reaches a scene.

`guest-hooks-test` runs the guard against small functions it writes at these offsets.

## Credit

Ported from deadinside28's bbport 0.7: [deadinside28/bloodborne_pc](https://github.com/deadinside28/bloodborne_pc)
commit 0fc2921, `gpu/games/bloodborne/hooks_gx.cpp` (their pull request #343 by whp199).
GPL-2.0-or-later, Copyright 2026 bbport contributors. That project's NOTICE asks that works based
on its code say so, with a link to the repository: this code comes from bbport. Thanks to
deadinside28 and whp199.
