# Explicit host frame staging

`IFramePipeline` is a separately queryable COM capability on the engine object.
It adds no slot to IEngine's existing vtable, installs no callback/module, and
exposes no graph, Registry, renderer or mutable clock. Hosts still own their
loop and input/UI composition.

```cpp
#include <Zahlen/COM/IFramePipeline.hpp>
auto pipeline = engine.cast<ZHLN::COM::IFramePipeline>();
if (!pipeline) { /* engine does not support staged frames */ }
engine->ProcessEvents();
// host gameplay, animation authoring and explicit SubmitUI
if (auto advanced = pipeline->AdvanceFrame(dt); !advanced.HasValue()) {
    // report advanced.error; no pending frame remains on a step failure
} else {
    // Read current interpolated WorldTransform components, set camera poses.
    ZHLN::CameraRig::Step(*engine.Get(), dt);
    const auto finished = pipeline->FinishFrame();
    // report finished.error if needed
}
```

All operations are synchronous on the single host/frame-owner thread. Do not
pump events, run another simulation step or concurrently mutate the world
between AdvanceFrame and FinishFrame. This is a camera/tool presentation seam,
not an invitation to re-run engine physics or transform propagation.

## Order and state

AdvanceFrame freezes the current native compatibility plan, advances haptics
and the fixed-step clock once, and executes through core SceneCleanup:

1. Input evaluation and reload work.
2. Fixed physics and the core simulation/interpolation/transform graph.
3. Main command playback and pending scene cleanup.

It leaves the engine ready for host-owned camera policy. FinishFrame executes
the remaining frozen plan: compatibility camera steps (if explicitly installed),
core camera projection, visibility/render/presentation and transform history.
Hosts must not install a native camera writer that competes with a host camera
writer on the same entity. New native plan entries are not added to a pending
frame's step snapshot.

The delta and fixed-step batch cannot change between halves. The old Tick(dt)
remains supported and advances and finishes through the same code.
A second AdvanceFrame or Tick while a frame is active returns
FramePipelineError::FrameInProgress, without consuming additional clock time.
A finish/cancel without a ready frame returns NoPendingFrame. Negative,
infinite and NaN deltas are refused with InvalidDeltaTime. Zero is supported.
CurrentFrame advances exactly once when a consumed frame completes or is
cancelled, and once on a consumed frame-step failure, preserving the previous
Tick failure-counting convention. Invalid protocol calls do not increment it.
It is a consumed-frame index, not a count of GPU presentations.

CancelFrame does not roll back simulation. It discards the entire suffix and
counts the consumed frame, without committing camera or transform presentation
history. That history still describes the previous presented image, rather
than an intermediate pose that was never drawn. No native extension suffix
steps are replayed. Destroying an engine with a pending frame is safe: the
synchronous prefix owns no in-flight graph work, and normal teardown releases
its world without submitting the suffix.

ClearScene between halves automatically cancels the pending render before
replacing the scene. A subsequent FinishFrame is therefore refused. Explicit
RecreateGraphicsDevice is refused while a frame is pending; cancel or finish
first. Device-loss recovery encountered during normal presentation retains the
existing renderer recovery path.

WorldTransform is committed **before** the host seam. Writing a local Transform
or adding/removing entities during BeforeRender does not secretly re-run
simulation, cleanup or hierarchy propagation. Do those authoring operations in
Tick/the gameplay update, before AdvanceFrame. Use BeforeRender to read the
resulting output and finalize camera poses. Likewise, direct Apply calls before
AdvanceFrame still observe the previous committed world snapshot.

## Application layers

A layer may implement `BeforeRender(COM::IEngine&, float dt)` as void or
`std::expected<void, ErrorCode>`. Application executes it in registration order
after a successful AdvanceFrame and before FinishFrame. A typed failure or
RequestClose at this seam cancels the pending presentation before reverse-order
Stop hooks, while preserving the original typed failure.

`FreeCam::Layer` and `CameraRig::Layer` implement `BeforeRender`. Bespoke hosts
can call their `Step`/`Apply` entry points explicitly.

## Copied graphics diagnostics

`GetSettings` accepts `GraphicsDeviceInfo` alongside `GraphicsSettings`.
Renderer and GPU names are copied into fixed-capacity `String64` and `String256`
values. The `Info` convenience method returns the copied node. Copies remain
valid after renderer recreation.

Before graph resources have an extent, `GetViewport` uses the framebuffer
extent and clamps the requested subregion.

## Diagnostic scene host and fallback

`zahlen_scene_host` is an Application with COM document loading, host-owned
HostUI and an instance-owned DefaultPreset. Its optional FileSystemWatcher and
PresenceClient belong to the host layer and are retired in Stop while
the process/task scope is alive. It neither installs engine services nor links
the native Scene/document/HostUI or Discord engine-integration adapters.

DefaultPreset's scene, orbiting light, emblem animation and popup remain. All
mutable preset state is per-instance, including popup/animation settings and
entity handles; a retained COM reference prevents stale handles from silently
binding to a different engine allocation. ClearFallback releases that reference
and resets host state; geometry remains engine scene-owned. The preset does not
despawn geometry as a side effect of dropping UI state. The host explicitly
clears/replaces scene contents when changing documents.

The CLI retains --scene and adds --frames (0 unlimited)
and --raster-only (explicit no-BLAS scene upload); graphics settings come from
the scene document, not a Lua profile. BLAS remains enabled by
default; there is no environment/device detection workaround. The fallback's
RTR setting is disabled for its explicit raster-only build. Source prefabs must
be prepared by the host before a document references them.
