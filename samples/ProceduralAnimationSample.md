# Procedural Animation Reference Rig

`resources/assets/ProceduralAnimationBaseRig.glb` is the known-good reference
asset for `ProceduralAnimationSample`. It is deliberately simple: a colored box
humanoid, visible 18-strand hair, one skin, and no authored animation clips. The
procedural evaluator is therefore the only source of motion.

The GLB is generated entirely with Python and can be inspected or imported into
Blender:

```bash
python3 tools/generate_procedural_base_rig.py
```

## Running the reference and a project rig

The committed reference rig is loaded by default:

```bash
cmake --build build --target ProceduralAnimationSample
./build/samples/ProceduralAnimationSample
```

The subsystem is an optional extras module. Consumers opt in explicitly after
initializing the core scene:

```cpp
import ZHLN.ProceduralAnimation;

engine.InitializeDefaultScene();
ZHLN::ProceduralAnimation::Register(engine);
```

`Register` installs the extras-owned ECS types and inserts its evaluator before
the generic core `ArticulationSystem` phase. Core knows only about
`KinematicPoseOverrideComponent`, not about gait, hair, IK, or `RigBoneMap`.
When a procedural entity gains `RagdollComponent`, the extras evaluator creates
a missing `KinematicPoseOverrideComponent` automatically before publishing its
motor target, so articulation cannot silently fall back to bind pose because of
a spawn omission. Every distinct skin/joint offset on the character receives its
own procedural palette; this is required when body, feet, clothing, and hair are
exported as separate skins. The evaluator logs both the distinct palette count
and the number of non-skinned attachments synchronized each frame topology
changes, making skin-vs-attachment failures visible immediately.

Run the same sample with another packed GLB without editing C++:

```bash
ZHLN_PROCEDURAL_RIG=UziProc.glb ./build/samples/ProceduralAnimationSample
```

The sample loads the GLB before creating its `CharacterVirtual`. It transforms
every mesh-part AABB through the imported node hierarchy, unions the results,
and fits the lifter sphere and bumper capsule to that estimated visual envelope
with small horizontal/vertical safety margins. The estimate is independent of
asset names and works across nested transform containers. Consumers can use the
same generic helpers:

```cpp
const auto bounds = ZHLN::Locomotion::EstimateCharacterBounds(prefab);
const auto hull   = ZHLN::Locomotion::FitDualShapeToBounds(bounds);
```

Invalid or empty bounds retain the standard `DualShapeConfig` fallback.

The reference should report:

```text
Rig 'ProceduralAnimationBaseRig.glb': complete; mapped 21/21 core bones and 18/18 hair strands (108 deform hair bones).
```

A variable-length production hairstyle is also valid. For example, 18 strands
with 96 total deform bones should report `complete` as long as every strand has
at least one mapped link.

## Coordinate and scale contract

- Right-handed coordinates
- `+Y` is up
- `+Z` is character forward and the knee pole direction
- `+X` is character left
- One model unit is one metre
- Mesh vertices and inverse-bind matrices are in character model space

## Required semantic bones

The mapper is case- and separator-insensitive and accepts the `DEF-` prefix.
The reference uses:

```text
Root
DEF-Hips
DEF-Spine
DEF-Sup_Spine
DEF-Chest
DEF-Neck
DEF-Head
DEF-Upper_arm.L
DEF-Forearm.L
DEF-Hand.L
DEF-Upper_arm.R
DEF-Forearm.R
DEF-Hand.R
DEF-Thigh.L
DEF-Shin.L
DEF-Foot.L
DEF-Toe.L
DEF-Thigh.R
DEF-Shin.R
DEF-Foot.R
DEF-Toe.R
```

Hair bones use `DEF-Hair_Sxx_yy`, where `xx` is strand `01..18` and `yy` is
link `01..06`. Individual strands may stop at link 04 or 05; missing tail links
remain intentionally unmapped rather than shifting the following strand.

At initialization, each mapped hair bone's position and rotation are captured
relative to the head directly from the GLB bind pose. XPBD distance, bend, and
compliant shape constraints return the simulated particles to that authored
silhouette. Collision constraints preserve any intentional bind-pose overlap
with the head or torso instead of forcibly straightening the hairstyle.

## Keyframes and procedural layers

The reference GLB contains three authored tracks:

- `IdlePose`: one static authored pose for hips, `Sup_Spine`, and chest.
- `Walk_Reach_Poses`: two opposing reach keys for legs, arms, and chest.
- `Run_Reach_Poses`: two larger opposing reach keys for the same controls.

`ProceduralLocomotionTracksComponent` selects idle/walk/run from movement state.
For walk and run, the stride wheel ping-pongs between the two authored reach keys;
pass poses occur halfway between them and are produced by the selected bicubic or
spring-damper interpolator. This keeps authored motion synchronized to actual
travel distance instead of clip playback speed. Gait, COM tilt, IK, look-at, and
hair are layered after that synchronized base pose.

`ProceduralAnimationConfigComponent::poseInterpolation` selects the authored-pose
interpolator. `Bicubic` uses four neighboring keys with configurable cardinal
`bicubicTension`. `SpringDamper` follows the eased key target through a unit-mass
spring configured by `springStiffness` and `springDampingFactor`. A damping factor
of `1` is critical damping, values below `1` overshoot, and values above `1` are
overdamped.

```cpp
registry.Add(character, ZHLN::ProceduralAnimationConfigComponent {
    .poseInterpolation = ZHLN::PoseInterpolationMode::Bicubic,
    .bicubicTension = 0.0f,
});

registry.Add(character, ZHLN::ProceduralAnimationConfigComponent {
    .poseInterpolation = ZHLN::PoseInterpolationMode::SpringDamper,
    .springStiffness = 2500.0f,
    .springDampingFactor = 0.90f,
});
```

The sample exposes the same selection without recompiling:

```bash
ZHLN_POSE_INTERPOLATION=bicubic ZHLN_BICUBIC_TENSION=0.0 \
    ./build/samples/ProceduralAnimationSample

ZHLN_POSE_INTERPOLATION=spring ZHLN_SPRING_STIFFNESS=2500 \
    ZHLN_SPRING_DAMPING_FACTOR=0.9 ./build/samples/ProceduralAnimationSample
```

Configure synchronized locomotion tracks for another GLB with:

```cpp
registry.Add(character, ZHLN::ProceduralLocomotionTracksComponent {
    .idleTrack = ZHLN::FindAnimationTrack(*prefab, "idle"),
    .walkTrack = ZHLN::FindAnimationTrack(*prefab, "walk"),
    .runTrack = ZHLN::FindAnimationTrack(*prefab, "run"),
    .runSpeedThreshold = 3.2f,
    .synchronizeToStrideWheel = true,
});
```

Acceleration pitch and roll use their own springs and rotate the whole hips
subtree around the estimated center of mass before foot IK re-establishes ground
contact.

The active clip is also inspected for authored upper-body coverage. If it keys
arms/hands, procedural arm counter-swing is suppressed. If it keys spine, chest,
neck, or head, procedural look-at is suppressed. This prevents the extras module
from adding an idle sway on top of an authored GLB track. Explicitly opt back into
both layers with:

```cpp
config.layerUpperBodyOverAuthoredChannels = true;
```

The debug overlay draws a sagittal stride wheel beside the COM. Cyan indicates
pass-pose landmarks, orange indicates reach-pose landmarks, and the moving spoke
shows the current distance-driven stride phase.

## Smooth gravity bounce

Pelvis bounce uses a continuous cosine arch for each support interval:

```text
y(phase) = 0.5 * amplitude * (1 - cos(2π * phase))
amplitude = gravity * supportInterval² / (2π²)
```

Position and velocity are continuous at every contact, so there is no takeoff or
landing snap. The amplitude formula makes acceleration at the apex equal
`-bounceGravity`. Faster steps shorten the support interval and automatically
produce a flatter curve without changing gravity. Very slow motion is limited by
`maxBounceHeight` (4.5 cm by default) to avoid exaggerated vertical travel.

Tune `bounceGravity` and `maxBounceHeight` on `ProceduralLocomotionComponent`, or
disable only this layer with
`ProceduralAnimationConfigComponent::enableGravityBounce`. Hold left Shift in
the sample to compare the normal-speed arc against the flatter sprint arc.

The final pelvis height also includes terrain reach correction. Previously,
`pelvisDrop` switched abruptly as feet entered the planted state, which could
mask the cosine wave as vertical popping. Pelvis drop now uses continuous plant
weights and a critically damped spring. To inspect each contribution separately:

```bash
# Authored hips plus cosine bounce, without IK drop or acceleration tilt:
ZHLN_DISABLE_IK=1 ZHLN_DISABLE_ACCELERATION_TILT=1 \
    ./build/samples/ProceduralAnimationSample

# Keep leg IK, but remove only its pelvis compensation:
ZHLN_PELVIS_DROP_WEIGHT=0 ./build/samples/ProceduralAnimationSample
```

## Isolation switches

Leg IK is an additive grounding layer. Authored walk/run keys determine each
foot's horizontal placement and drive the complete swing phase. During stance,
the solver raycasts beneath that authored X/Z location and limits itself to a
small vertical/normal correction. Its knee pole is derived from the authored shin
pose instead of resetting to a fixed forward bend.

World-locking is **off by default** because freezing X/Z necessarily cancels part
of an authored plant animation. Enable it only when deliberate foot locking is
more important than preserving that motion. Tune each contribution independently:

```bash
ZHLN_LEG_IK_WEIGHT=0.4 ./build/samples/ProceduralAnimationSample
ZHLN_MAX_FOOT_HEIGHT_CORRECTION=0.10 ./build/samples/ProceduralAnimationSample
ZHLN_WORLD_LOCK_FEET=1 ./build/samples/ProceduralAnimationSample
ZHLN_DISABLE_IK=1 ./build/samples/ProceduralAnimationSample
```

`legIKWeight=0` preserves authored legs; `1` gives full terrain correction at the
center of the plant interval. Touchdown and toe-off use smooth nonlinear fades.

Evaluate only the authored clip and the selected bicubic or spring-damper pose interpolator:

```bash
ZHLN_AUTHORED_POSE_ONLY=1 ./build/samples/ProceduralAnimationSample
```

Games can configure the same behavior per entity:

```cpp
registry.Add(character, ZHLN::ProceduralAnimationConfigComponent {
    .enableLegIK = false,
});

// Or isolate the authored pose completely:
registry.Add(character, ZHLN::ProceduralAnimationConfigComponent {
    .authoredPoseOnly = true,
});
```

`authoredPoseOnly` does **not** select an animation. It only bypasses procedural
layers after evaluating `AnimatorComponent::currentTrackIdx`. If that index is
`-1`, the authored result is correctly the bind pose. Use `FindAnimationTrack`
to select by name:

```cpp
animator.currentTrackIdx = ZHLN::FindAnimationTrack(*animator.prefab, "idle");
```

The lookup is case-insensitive, prefers an exact name, then accepts a substring
such as `Combat_Idle_Loop`. If an imported GLB detaches `DEF-Hand.L/R` from its
matching forearm, the rig builder adds a jump-free child-of constraint using the
bind-relative transform. The constrained hand carries its finger subtree and
retains authored local wrist motion. Detached upper-body nodes are repaired as a
complete semantic chain:

```text
SupSpine -> Chest -> Neck -> Head
```

Each constraint preserves its bind-relative offset and authored local animation,
and each node carries its own descendants and attachments. Detached semantic
foot copies in secondary skins are attached to the primary `FootL`/`FootR`
controls. Foot IK carries its model-space correction through every imported
child transform, so mesh parts below a `DEF-Foot.*` transform stay coherent.

For detached footwear, compact mesh bounds are aggregated at their highest
non-rig transform ancestor. That owning transform is attached after IK and moves
all of its child parts together. Discovery uses hierarchy, geometry, and
rig-relative dimensions—not asset paths or node names. A skinned child part may
participate when it is under a detached transform container, while an ordinary
standalone skinned mesh remains palette-driven.

Disable only intentionally different relationships with `enforceHandChildOf`,
`enforceChestChildOf`, `enforceNeckChildOf`, `enforceHeadChildOf`, or
`enforceFootAttachments`. Large meshes are never spatially auto-attached.

Startup logs print the selected index, clip name,
duration, total channel count, and usable transform-channel count. A T-pose with
`usable transforms=0` is an import/export or node-capacity issue; a T-pose with
`no valid authored track selected` is a track-selection issue.

Other switches allow gait, COM tilt, upper-body procedural motion, and secondary
motion to be toggled independently.
