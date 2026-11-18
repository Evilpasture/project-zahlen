# Optional animation runtime and engine character motor

Animation is not an Engine subsystem. `zahlen_engine` does not compile or link
Context, skeletal evaluation, playback synchronization or TwoBoneIK.
Plain Engine/COM engine creation installs no animation modules, owns no animation
state and publishes no Animator or IK components.

## Explicit composition

Native composition roots link `zahlen_animation_controls` and call
`Animation::Install(engine)` and, when required, `IK::Install(engine)`. Both are
idempotent. IK installs its animation prerequisite. The optional runtime owns
its context, graph systems, prefab-root seeding, component publication and scene
cleanup through generic extension services and lifecycle hooks.

COM consumers link `Zahlen::AnimationControls`, include `<Animation/Context.hpp>`
and explicitly use `Animation::CreateEngineRef(configChain)`. This extension
factory installs the optional runtime before returning the ordinary COM engine;
it does not add an animation interface or hidden native accessor to IEngine.
`COM::CreateEngineRef` remains a plain, animation-free engine factory.

`<Animation/Animator.hpp>` provides capability-only helpers for attach/read,
play/restart, seek, pause/speed and validated IK input. Runtime types are owned
by `<Animation/Components.hpp>` (`Animation::AnimatorComponent` and
`Animation::AnimatorControlComponent`), not by core's `Components` container.
IK controls and AnimationID are likewise extension headers. The erroneous
`Zahlen/Animation/Native*` headers have been removed, not retained as core-to-
extension forwarding dependencies.

The engine supplies generic native module composition, optional component
publication and prefab notification mechanisms. It has no animation-specific
branch in its factory, graph wiring, resource release or prefab construction.
Renderer skin/palette upload and imported model data remain graphics/asset
facilities; they neither instantiate nor execute an animation evaluator.

## Ownership

The extension reclaims runtime handles for pending entities and scene clear.
Its managed-handle sweep also reclaims handles after generic component removal
or replacement, without teaching Engine's ECS bridge about animation resources.
Explicit raw Context clients still own their non-managed handles.
Animator::Detach removes host controls/IK, not the prefab or Animator state.
Cancelled frames and scene clear discard unpublished graphics palette uploads.

## Character motor

The fixed-step character motor remains a physics-facing engine facility. Hosts
supply POD desired velocity, acceleration/deceleration, gravity, jump and facing
controls. Input routing, camera-relative movement and gait decisions are host
policy. See [COMCharacterHost](COMCharacterHost.md).

## Unresolved and remaining work

The full Uzi rendered workload previously faulted/timed out on the second
rendered tick. Its cause remains unknown. Simulation-only success is not
presentation proof, and this extraction does not claim to fix that failure.
ProceduralAnimation, Locomotion and Rig remain native extension workloads,
not completed capability-only COM ports. The VM's Clang 22.1.8 rejects
`-freflection`; no Clang integration success is claimed.
