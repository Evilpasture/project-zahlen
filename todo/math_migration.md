# Math migration follow-ups

These are remaining candidates, not a mandate to mechanically replace physics
math or change serialized, GPU, or network layouts. Preserve coordinate
conventions, units, precision and wire/ABI compatibility when addressing them.

- **Remaining angle representation:** quaternion axis-angle and Euler APIs,
  matrix rotations, and perspective now accept typed angles. The older Euler
  helpers in `include/Zahlen/Math/Transform.hpp` also carry typed angles but
  still expose quaternion lanes as `FVec4`; audit their remaining consumers
  before retiring them. Scene descriptions retain
  their existing serialized degree vectors at the conversion boundary. Camera
  yaw, pitch and FOV now use `DegF`; scene descriptions and network messages
  retain float degrees, with explicit conversions. The target-camera rig and
  procedural-animation tuning still store float degrees; the rig is an
  FFI-visible component, so audit generated bindings and consumers before
  changing its representation. The procedural IK, ankle, wrist and finger
  limit APIs carry radians as `RadF`; their authored tuning fields still cross
  a float-degrees boundary.
- **Legacy math boundaries:** `include/Zahlen/Math/Math3D.hpp` still has
  Jolt-based degree conversions; review them in their actual boundary contexts
  rather than sweeping through physics.
- **Rotation representation:** `include/Zahlen/Math/Transform.hpp` retains
  older `FVec4` quaternion helpers alongside `Quat`, including
  `ComposeTransform` and the Euler/product/interpolation helpers. Audit their
  consumers and migrate rotation data to `Quat` before removing compatibility
  functions; four-element brace arguments can be ambiguous between overloads.
  `include/Zahlen/Math/Math3D.hpp` still has Jolt-based transform helpers and
  `TransformTRS`; determine which are real physics boundaries before changing
  them.
- **Physics boundary mismatch:** body creation migrated already: the role
  structs (`StaticBody`, `KinematicBody`, `DynamicBody` in
  `include/Zahlen/Physics/PhysicsValues.hpp`) carry native `Quat`, with the
  conversion to Jolt inside `PhysicsContext`. The one remaining `FVec4` lane
  is `PhysicsContext::SetPose(KinematicBodyID, ...)`; the ISimulation facade
  takes `Quat` and the engine facades forward with `AsVec4()`. Finish the
  deliberate migration by moving that lane to `Quat` too; do not bypass
  `PhysicsContext` shape factories or introduce implicit conversions.
- **Imported animation data:** `extensions/Animation/Context.cpp`
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
