# Character and Animator hosts

## Ownership and composition

`zahlen_character_controller` / installed `Zahlen::CharacterController` is now
a capability-only host library. Include
`<CharacterController/CharacterController.hpp>` and explicitly compose
`Character::Layer` into an Application, or call `Character::Step` before
`IFramePipeline::AdvanceFrame`. It does not install an Engine module, register
graph nodes/substep callbacks, acquire a Registry, or hold native services.

The engine executes every fixed motor substep. The host interprets physical
input and chooses camera-relative movement, sprint/recovery speed, jump
requests, yaw and interaction tuning. Grounded/velocity state comes from the
engine's `CharacterMotorStateComponent`; facing is latched and interpolated by
the engine before world transforms are propagated, not patched late through
renderer internals. The default motor does not drive facing unless requested.

A composition root creates a character body with `ISimulation::CreateCharacter`,
attaches its PhysicsComponent/TransformComponent to an entity, and calls
`Character::Attach(entities, entity)`. Character attachment does not replace or
retire the body. Explicit `Character::Detach` releases the host's Movement,
Input, HostCharacter and motor/state components, never the PhysicsComponent.
Scene/entity cleanup continues to retire the body's real physics resource.

```cpp
// after creating player and attaching its Physics/Transform components:
Character::Attach(*engine.Entities(), player);
// app composition (create the player in an earlier layer's Start):
app.AddLayer(Character::Layer {}).AddLayer(CameraRig::Layer {});
```

`Character::ReadInput` reads each player's routed seat and the first gamepad
assigned to it. The host preserves the existing WASD/gamepad normalization,
RMB look, jump/sprint, trigger threshold and wheel/D-pad zoom rules. The same
pure snapshot translator is shared by the native input adapter to prevent two
versions of the arithmetic from drifting. Physical reads do not decide UI/game
capture policy: disable HostCharacterComponent or supply a frozen explicit
snapshot when the application owns that decision.

`Character::Apply` accepts explicit intent for replay, AI and deterministic
input tests. `dt <= 0` or non-finite time changes nothing. A jump pulse survives
a host frame with no fixed substep; airborne requests are consumed by the
engine rather than buffered for a later landing. Free-camera mode suppresses
movement/jump, while ordinary deceleration still applies. Camera orientation is
the previous resolved main-camera yaw, as in the native controller.

Prop pushing remains host interaction policy. It queries real overlaps and
body state through ISimulation, ignores self/static bodies, and queues impulses
away from the character without a downward component. Its old 60 Hz impulse
strength is scaled by host elapsed time rather than applied once per variable
frame. This is not a second character integrator.

## Animator controls

Include `<Animation/Animator.hpp>` and link `zahlen_skeletal_animation` /
`Zahlen::SkeletalAnimation`. The real playback evaluator is already in Engine as core service/graph/cleanup
wiring, not an application-installed module;
this archive's new helpers only author published POD controls:

- `Animator::Attach` starts from the root's existing playback settings.
- `Play` requests a track and restart at time zero, including repeat calls for
  the same track; negative one selects no clip, smaller values are refused.
- `Seek` rejects negative/non-finite time; `Speed(0)` pauses playback.
- `SetIK` validates fixed-capacity chains and attaches published typed storage.
- `Read` copies playback progress without exposing/copying a runtime handle.
- `Detach` removes controls/IK, not the Animator or cached prefab resources.

Clip/gait selection stays in the host. These helpers do not acquire an
AnimationContext or export native animation containers across COM.

## Native compatibility remains explicit

Old native controller callers now include
`<CharacterController/NativeCharacterController.hpp>` and link
`zahlen_native_character_controller` / `Zahlen::NativeCharacterController`.
The sample, ProjectLight, Interaction and native-test composition roots were
updated accordingly. The new COM host never links this adapter; the native
adapter may link the host's shared input arithmetic. Do not install both
movement drivers in the same world. A native typed Movement family is refused
by the COM dynamic-component path instead of becoming duplicate unread state.

The existing procedural-animation, locomotion, Rig and gameplay demos remain
native compatibility workloads. Their algorithms were not replaced with a toy
COM demo, and their successful native compilation is not claimed as a complete
procedural COM port. A full procedural host needs explicit metadata/pose and
articulation output contracts and remains separate work.

## Verification

- Six COM character-host tests cover snapshot rules, attach/freeze/pause,
  real ground/jump/wall sliding, dynamic prop impulses, 30/144 Hz motion,
  Application composition and scene retirement.
- Three non-rendering runtime-control tests cover actual motor movement,
  published typed playback/IK storage and Animator helper validation.
- Seventeen existing native character/input tests pass, including ledges,
  slopes, acceleration, interpolation, jump/landing and dynamic-body pushing.
- An installed C++26 GNU consumer finds CharacterController/SkeletalAnimation,
  checks the host's exported dependencies exclude Native/Jolt adapters, drives
  an actual character through the staged engine pipeline, and validates
  Animator controls and cleanup without source-tree include paths.
- The VM's Clang 22.1.8 rejects `-freflection`; no Clang/libc++ integration pass
  is claimed.

The real Uzi rendered animation test is retained, not skipped in source or
changed to synthetic geometry. Its prior llvmpipe worker fault/timeout remains
an unresolved rendering gate. The focused non-rendering regression explicitly
filters that one test; those passing counts do not include it.

### Final verification and limits

The broad host/native-character regression passed 135 tests with zero failures.
A separate real-Uzi simulation-stage test passed 12 AdvanceFrame/CancelFrame
updates, paused seek, handle stability, scene clear and recreation with a new
handle. This deliberately does not call FinishFrame and is not rendered proof.
The native GPU_Application lifecycle suite passed 16 tests after removing
animation's implicit application-module installation. Core owns the evaluator,
IK nodes and cleanup; native Install calls cannot add a duplicate evaluator.
CancelFrame and scene clear discard unpublished palette uploads.

The installed GNU C++26 Engine/CharacterController/SkeletalAnimation consumer
was rebuilt and ran successfully. VM Clang 22.1.8 rejects `-freflection`; no
Clang integration success is claimed. An additional native procedural test
translation unit was stopped after over 15 minutes of compilation; no test-run
success is claimed for that optional suite. Its compatibility library built.
The original full rendered real-skinned second-tick failure remains unresolved.
