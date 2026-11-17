# Host-driven camera policy

`zahlen_freecam` and `zahlen_camera` are consumer libraries. Their implementations
use `IEntities`, `IActionContext` and POD camera/transform components; neither
installs a Module, invokes a graph, borrows Registry, or retains a native Engine.
Core continues to own camera matrix projection.

## Composition

```cpp
#include <Zahlen/App.hpp>
#include <FreeCam/FreeCam.hpp>
#include <Camera/TargetCamera.hpp>

// Explicitly opt in to fly-camera behavior. The ordinary game now does this
// when the optional target exists; Application itself has no optional defaults.
auto app = ZHLN::App();
app.AddLayer(ZHLN::FreeCam::Layer {});
// Or choose the orbit layer, and set TargetCameraComponent::target yourself:
// app.AddLayer(ZHLN::CameraRig::Layer {});
```

`Step(IEngine&, dt)` is the same ordinary host operation called by each layer.
Call it once after ProcessEvents and before Tick. It seeds only the main camera,
including after ClearScene replaces it. Attach/Detach operate through IEntities;
Detach records a consumer-owned opt-out so Step does not undo the choice.
Explicit Attach clears that opt-out and preserves the existing sensitivity,
sprint and enabled settings while changing speed/capture policy.

For custom scheduling, call `ReadInput(IActionContext&)`, retain the copied
InputSnapshot in the host, then `Apply(IEntities&, snapshot, dt)`. Apply does not
seed or install anything. Physical input is sampled for the primary seat; like
the native implementation it does not apply UI capture filtering. Hosts that
own UI capture should suppress or replace the corresponding snapshot fields.
This is consumer policy, not an engine device-injection API.

FreeCam::Drive(Camera&, settings, snapshot, dt) is also a standalone policy
function: normalized movement, sprint, RMB-gated look or captured-mouse look,
and pitch clamps. Step seeds on a zero delta but does not drive the pose.
Nonpositive/nonfinite fly deltas do not move or rotate the camera.

The orbit Apply visits every rig and writes the camera on that same entity,
instead of applying the first rig to an unrelated camera singleton. A fly tag
wins ownership. Missing/dead/pending-destroy targets or cameras are skipped.
WorldTransform is preferred, with local Transform fallback. Zoom limits, linear
distance/FOV convergence, exponential target smoothing and teleport snapping
are retained. A zero delta may initialize/snap authored orbit state but does
not consume mouse-look motion. Negative/nonfinite deltas are ignored.

## Scheduling limit — not yet phase-equivalent for physics follow

The host loop runs before the combined IEngine::Tick. Consequently an orbit
layer reads the **last committed world-transform snapshot**, not transforms
produced later inside that Tick's simulation/interpolation passes. A host that
updates a target's Transform but leaves an older WorldTransform attached must
publish its intended world pose or supply a separate pose-driving policy; the
library cannot infer whether a local edit invalidates a hierarchy result.

Do not call this phase-equivalent acceptance for following physics/animation
inside the same Tick. A deliberate simulation/render staging seam is still
needed for those consumers. They remain on the native adapter until that seam
and moving-target regressions are validated. There is no hidden engine callback
or Registry escape to conceal this ordering issue.

## Explicit native compatibility

The old module implementation remains in `NativeFreeCam.*` /
`NativeTargetCamera.*`, linked only through `zahlen_native_freecam` and
`zahlen_native_camera`. Existing native world-editor/sample/animation consumers
now name these headers and targets explicitly. Their old frame-phase placement
is retained. Component-only readers can include the primary headers and do not
need a native dependency. Neither COM library links back to a native adapter.

This is a host-camera consumer slice, **not completion of the native editor,
sample, animation, CharacterMotor, Terrain, VFX, Console or Benchmark ports**.
No bulk ECS mapping or new COM vtable method is introduced.

## Tests

`Extras_CameraHosts` covers standalone fly math/capture/pause, main-camera
seeding and opt-out, scene reset, sequential engine recreation, independent
multi-camera orbit state and fly ownership, world-transform selection,
zoom/pitch limits, smoothing/teleport, and explicit Application-layer composition
through two actual empty-world engine frames. Tests do not inject device input
and do not assert scene pixels. Native compatibility targets are separately
build-checked; a COM build or empty-world frame is not a physics-follow timing
acceptance test.
