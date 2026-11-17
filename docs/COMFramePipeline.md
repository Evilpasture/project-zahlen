# Explicit host frame staging

`IFramePipeline` is a separately queryable COM capability on the engine object.
It adds no slot to IEngine's existing vtable, installs no callback/module, and
exposes no graph, Registry, renderer or mutable clock. Hosts still own their
loop and input/UI composition.

```cpp
#include <Zahlen/COM/IFramePipeline.hpp>
auto pipeline = engine.As<ZHLN::COM::IFramePipeline>();
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
remains supported and simply advances then finishes through the same code.
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

FreeCam::Layer and CameraRig::Layer now implement BeforeRender instead of Tick.
Their normal Step/Apply entry points remain explicit for bespoke hosts. This
closes the previous one-frame committed-transform gap for normal host-layer
orbit use. The tests cover current local target edits and a real falling
physics body, host pose selection before projection, the resulting view/proj
matrix and no duplicate physics step during FinishFrame.

## Copied graphics diagnostics

GetSettings's output chain now also accepts GraphicsDeviceInfo, which may share
a chain with GraphicsSettings. Names are copied into fixed-capacity String64
(renderer) and String256 (GPU), not borrowed std::string_views. The nonvirtual
Info convenience returns that copied node. Existing IGraphicsDevice vtable
slots are unchanged. The scene host reconstructs the previous RenderInfo Lua
argument entirely in consumer code so graphics-profile scripts retain their
field names and values. A recreation test verifies old diagnostic copies remain
valid after the renderer is replaced.

Before the first BeginFrame lazily allocates sceneColor, GetViewport now uses
the actual framebuffer extent and the requested subregion/clamping. Previously
the first camera pass saw zero dimensions and retained identity matrices. The
fallback is only used while graph resources have no extent; normal allocated
viewport behavior is unchanged.

## Diagnostic scene host and fallback

`zahlen_scene_host` is now an Application with COM document loading, host-owned
HostUI and an instance-owned DefaultPreset. Its optional FileSystemWatcher and
DiscordPresenceClient belong to the host layer and are retired in Stop while
the process/task scope is alive. It neither installs engine services nor links
the native Scene/document/HostUI or Discord engine-integration adapters.

DefaultPreset's scene, orbiting light, emblem animation and popup remain. All
mutable preset state is per-instance, including popup/animation settings and
entity handles; a retained COM reference prevents stale handles from silently
binding to a different engine allocation. ClearFallback releases that reference
and resets host state; geometry remains engine scene-owned. The preset does not
despawn geometry as a side effect of dropping UI state. The host explicitly
clears/replaces scene contents when changing documents.

The CLI retains --scene and --graphics-config and adds --frames (0 unlimited)
and --raster-only (explicit no-BLAS scene upload). BLAS remains enabled by
default; there is no environment/device detection workaround. The fallback's
RTR setting is disabled for its explicit raster-only build. Source prefabs must
still be prepared by the host before a document references them; this port adds
no implicit glTF/BSP importer hook.

## Not completion of the migration

Native world-editor, Project Light and large sample loops still use their
explicit compatibility adapters. FreeCam/Camera can now run with correct host
staging, but those loops have not magically become COM ports. Native animation
and character substep code, Terrain, VFX, Console/Benchmark UI and editor policy
remain to be migrated. The agreed engine-owned Animator/CharacterMotor data
runtime is still outstanding. No bulk ECS mapping or registry escape was added.
