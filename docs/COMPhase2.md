# COM capability expansion (Phase 2)

This change builds on `com-api` at `da21636b09f8c7de04876fca3719796d92f799b3`.
Phase 1 is already present at that revision; do not reapply its patch.
Rebuild the engine and every COM consumer together: existing capability
vtables acquire methods and their reflected interface identities change.
There are no changes to `Zahlen/Core/COM` and no new public `Engine` methods.

## Capabilities

- `IEngine`: debug capability, frame/FPS/device-generation reads, tokenized
  device-loss notifications, window creation/removal/focus/cursor/capture,
  clipboard, borrowed-view file drops, and copied host UI submissions.
- `IDebugDraw`: lines, billboards, debug triangles, sprite and mesh particle
  submissions, and clearing the current debug draw list. Fixed-layout draw
  values are separated from `SceneData` into `Render/DebugDrawTypes.hpp`.
- `IActionContext`: physical mouse/modifier/gamepad edge/stick/trigger reads,
  haptics, interactive binding capture, and caller-buffer binding persistence.
  Input injection and frame evaluation remain engine-owned and unexposed.
- `IGraphicsDevice`: joint allocation/update, morph allocation, cube/render
  textures, environment radiance submission, and caller-buffer frame capture.
- `IAssetLibrary`: VFS mounts/reads, cache clearing, collision/environment
  registration/readback, and flat prefab registration (nodes, parts, skeletons,
  animation channels/key arrays, lights and CSG descriptors).

## Descriptor naming

New COM chain descriptors and nested descriptor values use `*Info`, including
`EnvironmentImageCacheInfo`, `PrefabRegistrationInfo`, `PrefabNodeInfo` and
`PrefabCSGModifierInfo`. `FileDropView` marks the callback-only borrowed payload;
`AssetSlice` is the offset/count helper rather than a descriptor.

## Ownership and sequencing

Chain descriptors and nested input spans are borrowed for the duration of the
call only. Registration copies the authored values. No `World`, registry,
`SceneData`, backend pointer, vector, string or `ModelPrefab` is added to a
vtable signature.
All capability calls, registration changes and cache operations must be
serialized by the host; this is not a concurrent cache or callback API.

Prefab registration validates flat slices, node cycles, joint parents and
animation key arities/times before inserting anything. Duplicate prefab paths
are rejected. On success the cache owns the prefab's supplied GPU mesh
resources; do not destroy those resources separately. Clear the live scene
before clearing caches used by it. Collision/environment output spans are
borrows invalidated by replacement, cache clear or engine destruction. Missing
objects return errors rather than dangling spans. `GetEnvironmentImage` leaves
the output key empty rather than borrowing the caller's key storage.

UI geometry is copied synchronously and routed by `WindowID`; submission also
updates that window's UI input-capture flags. Compose and submit after
`ProcessEvents` and before `Tick`. Engine ticking does not build host UI.
File-drop views and their nested data live only during the callback.
The callback may replace/remove its subscription. Removing the primary window
is inert; use `CloseWindow` to request shutdown.

Device-loss callbacks return engine-local removal tokens. New registrations
wait until the next notification; removing a later registration suppresses it
within the active notification. No forced device loss or backend reset entry
point is exposed. Registered callbacks do not own the COM engine.

Joint allocation is monotonic within the engine's shared 8192-joint palette.
Exhaustion returns an error without wrapping over live allocations; prefab
instantiation skips a mesh if it cannot allocate its palette. Morph allocation
requires complete float4 entries and reports buffer exhaustion as an error.

Clipboard, bindings, VFS and capture reads use **exact bytes without a NUL**:
query with a null/zero buffer; a short buffer returns the required size and
remains untouched. An empty existing VFS file returns zero, while a missing
file returns an error. Each `CaptureFrame` call performs a fresh readback;
do not tick or resize between a size query and the copy when the same frame is
required. There is no hidden capture cache: querying byte count and then copying
performs two GPU readbacks. For repeated screenshots/video, reuse a buffer large
enough for the capture extent and pixel format (up to eight bytes per pixel for
the current formats), passing it in the initial call. Resize it after a required
size larger than its capacity is reported; that resize/retry performs another
readback. Capture metadata is fixed-layout and the caller owns pixel storage.

## Verification

`tests/core/TestCOMPhase2.cpp` is a separate public-header/vtable consumer test.
It does not invoke the historical boundary test's BLAS path and treats engine
initialization failure as a test failure. It covers capability queries, FPS and
callback tokens, rejected malformed/null inputs, copied binding persistence,
VFS short/full/empty/missing reads and mount lifetime, collision/environment
cache round-trips, prefab cycle rejection/registration/duplicates, and joint
palette exhaustion without wraparound.

Runtime GPU rendering, hardware haptics, native multiwindow/file-drop delivery,
and actual device-loss callback delivery require the corresponding hardware or
platform integration tests; the focused test does not claim those scenarios.
