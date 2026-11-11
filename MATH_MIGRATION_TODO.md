# Math migration follow-ups

These are remaining candidates, not a mandate to mechanically replace physics
math or change serialized, GPU, or network layouts. Preserve coordinate
conventions, units, precision and wire/ABI compatibility when addressing them.

- **Angle units at API boundaries:** `include/Zahlen/Math/Quat.hpp` still takes
  raw radians in `FromAxisAngle`/`FromEuler` and returns them in
  `ToAxisAngle`/`EulerAngles`; the Euler-vector degree helpers also convert
  component values internally. Consider typed angle APIs without introducing
  implicit math conversions, and check call sites and rotation order.
- **Raw scalar degree/radian conversions:** examples remain in
  `extensions/Animation/ProceduralAnimation.cpp` (camera yaw/pitch),
  `extensions/VFX/Explosion.cppm`, `gameplay/Interaction/BSPInteraction.cpp`,
  `gameplay/Interaction/InteractionSystem.cpp`,
  `gameplay/ProjectLight/ProjectLightClient/SceneMirror.cpp` (sun direction),
  and `plugins/BSP/BSPScene.cpp`, `BSPInteraction.cpp`, `BSPLighting.cpp`,
  and `BSPGeometry.cpp`. Replace with explicit `DegF`/`RadF` conversions where
  useful, preserving Source and ProjectLight coordinate/unit conventions.
  `extensions/editor/GUIEditor.cpp` and `include/Zahlen/Math/Math3D.hpp` also
  have Jolt-based degree conversions; review those in their actual boundary
  contexts rather than sweeping through physics.
- **Rotation representation:** `include/Zahlen/Math/Transform.hpp` retains
  older `FVec4` quaternion helpers alongside `Quat`, including
  `ComposeTransform` and the Euler/product/interpolation helpers. Audit their
  consumers and migrate rotation data to `Quat` before removing compatibility
  functions; four-element brace arguments can be ambiguous between overloads.
  `include/Zahlen/Math/Math3D.hpp` still has Jolt-based transform helpers and
  `TransformTRS`; determine which are real physics boundaries before changing
  them.
- **Physics boundary mismatch:** `PhysicsContext::SetKinematicPose` and body
  creation APIs in `include/Zahlen/Physics/Physics.hpp` accept quaternion
  lanes as `FVec4`, so native `Quat` callers such as
  `src/engine/PrefabFactory.cpp` explicitly use `AsVec4()`. Consider a deliberate
  interface migration to `Quat`, with conversion to Jolt inside physics; do not
  bypass `PhysicsContext` shape factories or introduce implicit conversions.
- **Imported animation data:** `extensions/Animation/AnimationContext.cpp`
  constructs quaternions from four scalar samples. Those arrays are an asset
  format boundary; review whether their representation can be typed without
  changing interpolation, glTF channel layout or serialization.
- **Transform precision and inversion:** native prefab spawn currently narrows
  the double-precision origin to a float matrix. Also, native matrix inversion
  returns identity near singularity, which can differ from Jolt at decal/IK
  call sites. Address deliberately with precision and degenerate-case tests.
- **Generated GPU headers:** `utils/zshader/GpuTypes.cpp` emits the native math
  includes in `GeneratedGpuTypes.hpp`. Rebuild `zshader` and regenerate shader
  outputs after include changes; stale generated headers may still include the
  deleted `ValueTypes.hpp`.
