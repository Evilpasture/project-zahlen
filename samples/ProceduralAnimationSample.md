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

Run the same sample with another packed GLB without editing C++:

```bash
ZHLN_PROCEDURAL_RIG=UziProc.glb ./build/samples/ProceduralAnimationSample
```

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
