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
// Or choose the orbit layer, and set TargetComponent::target yourself:
// app.AddLayer(ZHLN::CameraRig::Layer {});
```

`Step(IEngine&, dt)` is the same ordinary host operation called by each layer.
Call it once between IFramePipeline::AdvanceFrame and FinishFrame. The
Application layer uses BeforeRender for that ordering. It seeds only the main camera,
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

## Same-frame target timing

Application's BeforeRender seam runs after physics/interpolation, transform
propagation and cleanup, and before camera projection/culling/presentation.
The supplied layers use that seam, so a target's world pose is current for the
frame being rendered. Bespoke hosts use the queryable IFramePipeline capability
explicitly. See COMFramePipeline.md for protocol, cancellation and failure rules.

Apply itself does not advance anything: a direct call before AdvanceFrame still
reads the previous committed WorldTransform. Local target edits belong before
AdvanceFrame, not between the halves, because propagation is not silently
replayed.

## Tests

`Extras_CameraHosts` covers standalone fly math/capture/pause, main-camera
seeding and opt-out, scene reset, sequential engine recreation, independent
multi-camera orbit state and fly ownership, world-transform selection,
zoom/pitch limits, smoothing/teleport, and explicit Application-layer composition
through two actual empty-world engine frames. Tests do not inject device input
and do not assert scene pixels. Native compatibility targets are separately
build-checked; a COM build or empty-world frame is not a physics-follow timing
acceptance test.
