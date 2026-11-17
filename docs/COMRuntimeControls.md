# Engine-owned animation and character motor controls

**Status:** Engine controls and COM Character/Animator host slices have focused
GNU and installed-consumer verification (see [COMCharacterHost](COMCharacterHost.md)).
The full Uzi rendered workload remains an unresolved gate: it faults during the
second rendered tick on llvmpipe, while a validated non-mesh-shader raster probe
times out there. A diagnostic fault address was in generated code, not a resolved
engine symbol. This is not proof of either a driver defect or a migration defect;
that rendering test is not included in passing regression counts.

This change moves the existing skeletal evaluator and its ECS synchronization,
IK, morph, rigid-node and palette-upload passes into `zahlen_engine`. Engine
creation initializes them as core services and core graph/cleanup passes,
not optional application modules. Native Install calls are compatibility no-ops. COM hosts do not install an Animation module, retain an
`AnimationContext`, or acquire a native Engine. The old AnimationContext/AnimationSystem/IK headers remain native compatibility
forwarders. The `zahlen_skeletal_animation` archive also supplies the new
capability-only Animator host helpers; the actual evaluator is linked from
Engine exactly once.

## Playback and IK

Instantiate a prepared animated prefab through `IAssetLibrary::Instantiate`
with `SpawnParams::isAnimated = true`. On its root, attach the published POD
`Components::AnimatorControlComponent` using `IEntities::Attach`:

- `trackIndex`, `playbackSpeed`, `blendDuration` and `loop` are persistent controls;
- set `seekTime` and `seekRequested` for a consumed seek request;
- read playback progress from `Components::AnimatorComponent`. Its runtime ID
  is engine-owned: never copy it between roots or worlds.

The controller does not create a prefab or choose a clip for the application.
`IK::TwoBoneIKComponent` and its fixed-capacity chains are published typed
storage too. Their POD declarations live in `Zahlen/Animation/IKControls.hpp`,
without an ECS system, module, registry, solver service or native Engine API.
`AnimationID` similarly has a separate fixed-layout declaration.

The Console/Scripting reflection policy treats runtime Animator data and
CharacterMotorState as read-only; controls remain editable. This is not a new
const-enforced `IEntities` ABI: the existing typed capability returns mutable
component pointers. COM consumers must respect the documented ownership rule.
COM replacement/detachment of an Animator now retires its old runtime handle.
Pending destruction and scene reset use the real animation cleanup pass.

## Character motor

Attach `Components::CharacterMotorComponent` to the entity owning a non-static
character PhysicsComponent. Supply desired world-space X/Z velocity,
acceleration, deceleration, gravity, and a jump speed/request. Y in desired
velocity is intentionally ignored. Non-finite command inputs are sanitized.
The engine creates `CharacterMotorStateComponent` and executes velocity
integration and commit before **every fixed physics substep**, then reads
velocity and grounded state back immediately afterward. Airborne jump requests
are consumed rather than buffered. Pending destruction and active ragdolls do
not receive motor steering. Scene destruction still retires the physics body
through the existing owner cleanup.

Input routing, camera-relative intent, sprint/gait choice, facing, prop pushing,
and procedural animation policy are not motor substrate. They remain host
responsibilities. The new Character host implements input, sprint, facing and
prop-interaction policy through COM capabilities. Native Locomotion/Rig and
procedural demos remain separate compatibility workloads; they have **not**
been relabeled as COM ports. Do not install two motor drivers on the same character.

## Verification and scope

`Extras_RuntimeControls` links Engine, Application (process/task lifetime), and
the COM glTF asset host and the skeletal host helper archive; it does not link
the native character-controller adapter. It tests real character movement/retirement, published
playback/IK storage, and the repository's real Uzi skinned prefab through COM
Decode/Upload/Instantiate/Tick, including playback, pause/seek, skinning scratch,
bounds and scene recreation. Fetch the Git LFS Uzi asset before running this
suite; an LFS pointer is not a model and is not silently skipped.

Existing native animated-mesh and animation-lifecycle workloads no longer call
Animation::Install: they exercise the engine's automatic ownership. Installed Engine/CharacterController/SkeletalAnimation consumer verification
passes on GNU. Full procedural-host migration and a Clang/libc++ integration
run remain separate acceptance work; this document does not claim those are
complete.
