# COM world-editor consumer

`zahlen-editor` now owns its custom loop, HostUI/context/atlas, document policy,
selection/modal state and primitive recovery. It calls engine capabilities and
stages frames through IFramePipeline; there is no Engine, native renderer,
PrefabFactory service, native GUI callback or registry bridge in this host.

`zahlen_editor` / installed `Zahlen::Editor` exports the actual hierarchy,
reflection-driven inspector, component CRUD table, camera-ray G/R/S modal
transforms, axis constraints, confirm/cancel and selection deletion. Public
API: `editor/GUIEditor.hpp`. Input is copied `COM::HostInputInfo`; rotations use
the engine-independent Quat vocabulary rather than Jolt types.

`utils/editor/EditorSession.{hpp,cpp}` is the in-tree reusable host session;
it is intentionally separate from the independently installable panel library.
The executable links this session, application/process support, the COM scene
and FreeCam consumers, and document parsing. No native editor/scene/freecam/UI
target is linked into it; the transitional native GUI editor adapter has been removed.

## Frame and document behavior

- Simulation starts paused. Input, picking, inspector, camera motion, font
  reupload and rendering continue. AdvanceFrame receives zero simulation time
  when paused; the host drives its camera before FinishFrame.
- HostUI submits the hierarchy/toolbar/inspector directly. No UI callback is
  installed in Core. ProcessScope and FramePacer remain above COM.
- The actual panel rectangles determine the centre viewport. Picking and modal
  unprojection use that same rectangle and Vulkan Y-down NDC convention.
- `COM::ViewportInfo {viewport}` is a new reflected ApplySettings chain node.
  It extends no vtable; the whole settings/viewport chain validates before any
  settings or viewport is committed. The existing Viewport() reads it back.
- The five Add Shape requests use IAssetLibrary's procedural capabilities,
  honouring the host's buildBLAS policy. Empty objects also receive source
  provenance so they survive document extraction.
- Sphere/Cylinder/Cone values are appended to ShapeKind. Existing values and
  component field layouts are preserved. In scene documents, `extent` is the
  plane extent or round-primitive radius; cylinder/cone `halfExtents.y` is their
  half-height. All five primitives now survive TOML save/read/instantiate rather
  than becoming boxes or disappearing.
- Primitive material source values are retained in a generation-keyed host
  document table. Cache clear rebinds live owned meshes; device loss recreates
  primitive mesh/material data through graphics capabilities, publishes only
  after ownership replacement, and preserves entity IDs/physics/selection.
  Imported prefabs still follow their format host's explicit prepare/rebuild
  contract; this does not install a hidden importer or add automatic source I/O.
- Ctrl+S writes `scene.toml`; the reusable Save API accepts another path. Scene
  parsing/validation/creation and session failures preserve their typed errors.
- A session retains its engine, rejects use before Start or with another engine,
  restores the previous viewport at Stop/destruction, and cancels any unfinished
  staged frame. Edited world objects remain owned by the world, not the UI.

Example smoke run:

```
zahlen-editor --headless --frames 3 --raster-only --width 800 --height 480 --validation-mode Off
zahlen-editor --scene scene.toml --headless --frames 2 --raster-only --validation-mode Off
```

Headless operation requires bounded --frames. Ordinary windowed editing has no
frame limit. --raster-only avoids BLAS creation on software Vulkan drivers.

## Validation

Existing GNU 16 Release VM, Vulkan llvmpipe:
- Extras_EditorHost: 6 passed (CRUD/hierarchy cleanup, modal manipulation and UI
  capture, all-five-shape save/reload, offset-viewport real-physics picking,
  transactional viewport chain, actual rendered paused/simulating/recreated UI
  plus all five primitive GPU assets, lifecycle and typed save errors).
- Extras_SceneAssets: 5 passed.
- Extras_CameraHosts/frame pipeline: 14 passed.
- Extras_Application/screenshot: 20 passed.
- Extras_TerrainHost: 5 passed; Lightning and Explosion: 4 passed each.
- Actual editor CLI: bounded fallback and Sphere scene-document runs passed;
  unbounded headless use was correctly rejected.
- Installed Engine + Editor components into a separate prefix; an external
  C++26 `find_package(ZahlenEditor)` consumer included the public headers,
  linked only Zahlen::Editor, enumerated the real component/shape tables and
  ran successfully. The native adapter header is not installed by this package.
- Governance and patch apply/reverse checks are reported with the deliverable.

This slice migrates the world-editor consumer, not the remaining Console,
Benchmark, native gameplay/sample callers, or Animator/CharacterMotor runtime.
