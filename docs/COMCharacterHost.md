# Character and Animator hosts

## Ownership and composition

`zahlen_character_controller` / installed `Zahlen::CharacterController` is
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
attaches its CharacterBodyComponent/TransformComponent to an entity, and calls
`Character::Attach(entities, entity)`. Character attachment does not replace or
retire the body. `Character::Detach` releases the host's Movement, Input, HostCharacter and
motor/state components. Scene/entity cleanup retires the CharacterBodyComponent's
physics resource.

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
versions of the arithmetic from drifting. The host owns UI/game capture policy.
Disable ControllerComponent or supply a frozen explicit snapshot when the
application owns that decision.

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

Include `<Animation/Animator.hpp>` and link `zahlen_animation_controls` /
`Zahlen::AnimationControls`. The playback evaluator belongs to the optional AnimationControls runtime.
Compose it explicitly via `<Animation/Context.hpp>` and
`Animation::CreateEngineRef`, or native `Animation::Install`/`IK::Install`;
the helpers author published POD controls:

- `Animator::Attach` starts from the root's existing playback settings.
- `Play` requests a track and restart at time zero, including repeat calls for
  the same track; negative one selects no clip, smaller values are refused.
- `Seek` rejects negative/non-finite time; `Speed(0)` pauses playback.
- `SetIK` validates fixed-capacity chains and attaches published typed storage.
- `Read` copies playback progress without exposing/copying a runtime handle.
- `Detach` removes controls/IK, not the Animator or cached prefab resources.

Clip/gait selection stays in the host. Animator controls use published POD
state; native animation containers stay in their owning runtime.

## Composition constraints

Use one movement driver per character. The COM dynamic-component path rejects
a native typed Movement family.

Animator components are `Animation::AnimatorComponent` and
`Animation::AnimatorControlComponent` from `<Animation/Components.hpp>`.
See [COMRuntimeControls](COMRuntimeControls.md) for runtime composition and
ownership.

## Known limitations

The procedural-animation, locomotion, Rig and gameplay demos use native APIs.
The Uzi rendered animation workload has an unresolved llvmpipe fault/timeout on
the second rendered tick.
