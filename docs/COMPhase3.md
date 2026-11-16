# Phase 3 — consumer migration, first batch

This batch starts migration of shipped consumers. It does **not** claim the
entire application/sample/plugin tree has been migrated.

## Migrated executable

`app/cube.cpp` now uses `ZHLN::COM::App`, `IAssetLibrary` and
`IGraphicsDevice`. It does not include the concrete `Engine`, capability
facades, `PrefabFactory`, `Application`, or `HostRunner`. Its CMake target links
only `zahlen_engine` plus the host-owned CLI and screenshot libraries (and
those libraries' normal dependencies). Reflection is enabled explicitly for
this consumer rather than inherited through Application.

The no-argument behavior is still a window containing a generated cube. CLI
controls were added for reproducible host/capture testing:

```sh
zahlen_cube --help
zahlen_cube --headless --frames 10
zahlen_cube --headless --frames 2 --capture cube.png
zahlen_cube --headless --empty-scene --frames 2 --width 64 --height 48 --capture empty.png
```

`--empty-scene` deliberately skips procedural mesh/BLAS creation. It is not a
renderer fallback or a workaround applied to production rendering. It allows
the consumer lifecycle/capture path to be exercised on the known VM driver.

`--frames` requests exit after that many completed frames (zero means
interactive). Capture with no explicit frame limit defaults to one completed
frame. Capture happens in the pre-Tick callback, never before the first
rendered frame. `RequestClose` is consumed by the next event pass, so a final
Tick may occur after the request. Missing captures, spawn errors, tick errors,
and file-write errors give a failing process exit code. Help/version and
invalid CLI values are handled before engine creation.

## COM capture consumer

`Screenshot::CaptureBuffer` is host-owned reusable CPU pixel storage. It calls
`IGraphicsDevice::CaptureFrame` with a conservative extent-times-eight byte
buffer, avoiding the normal query-then-copy double readback. It retains vector
capacity between calls. If the reported required size is larger, it resizes
and retries **once**; a second increase fails rather than looping indefinitely.
Each call still requests fresh GPU readback. There is no cache or new vtable.

`Read` returns a borrowed pointer to its owned `CapturedFrame`, valid until the
next `Read` or buffer destruction. The caller serializes use of the buffer and
device. Zero/overflowing extent estimates and malformed capture metadata are
rejected. PNG encoding remains the existing `Screenshot::WritePNG` function;
the renderer knows nothing about file paths or image encoding.

## Verification

Using the existing VM `build/menuconfig` without clearing or resetting it:

- `zahlen_cube` and `Extras_Application` built successfully.
- Application/screenshot worker: **17 passed, 0 failed**, including positive COM
  GPU readback, PNG writing, buffer reuse and undersized-estimate retry.
- Migrated executable: help succeeded, width-zero CLI rejected, and headless
  empty-scene capture succeeded. PNG signature/IHDR verified as 64 by 48.
- Link command contains no Application, HostRunner or HostUI static library.
  `nm -uC` shows `COM::CreateEngine` as the only unresolved `ZHLN::` symbol.
- Governance and whitespace checks passed.

Tests select the installed lavapipe ICD only in their environment. Rendering a
procedural cube through the known VM BLAS path is **not** claimed as passing.
The ordinary cube path compiles/links and remains available for native GPU
validation; it is not silently replaced by the empty scene.

## Remaining consumer batches

- Default sandbox entrypoint: preserve reflected CLI configuration, help,
  graph export, camera/physics interactions, and capture exit semantics while
  replacing its typed facade calls. Do not silently drop optional host policy.
- HostUI/UIEditor: physical text/key consumption and per-window input reads
  still require a suitable COM-facing host input seam. Do not expose internal
  input routers or injection/evaluation methods merely to move these files.
- Import-heavy samples/plugins: glTF/BSP/procedural animation currently author
  `ModelPrefab` and install native systems. Move authoring through flat COM
  registration descriptors and address module composition deliberately, rather
  than passing concrete engine pointers through a nominal COM wrapper.
- Device-loss sample: its intentional fault injection/recovery controls are
  internal diagnostic operations not exposed by COM; notification support
  alone is insufficient to port the sample's original behavior.

No Core/COM files, Engine public methods or capability vtables change in this
batch. Phase 2 (including its naming follow-up) is a prerequisite.
