# Lightning and Explosion capability hosts

The `zahlen_vfx` / installed `Zahlen::VFX` target now provides three ordinary
host-owned COM policies: CombatFX, `Lightning::System`, and
`Explosion::System`. New public entry points are `VFX/Lightning.hpp` and
`VFX/Explosion.hpp`. They retain their engine reference and expose no
native Engine, renderer context, registry, or service installation API.

## Lightning

Start the host, then call `Spawn(cloud, ground, camera, ConfigInfo)` with an
explicit camera position; there is no fabricated main-camera fallback.
`BeforeRender(engine, dt)` advances stepped leader, Heidler return-stroke and
exponential dissipation, updates emissive material/visible triangles, and
submits the authored thunder event. The fractal ribbon generator is shared
with the native compatibility adapter. Input validation bounds subdivisions,
width, current, time dilation, sound and emissive magnitude.

`BoltState` is a POD diagnostic component. CPU geometry is retained by the host;
GPU buffers are owned by the scene's OwnedMeshComponent. Cache rebinding does
not reupload, and device recovery reuploads from retained geometry without
resetting phase. Ownership replacement precedes asset publication.

Unlike the native adapter, this host does **not** overwrite global ambient
exposure. Owned flash/impact light entities provide flashes without one host
restoring settings over another host's effect. Ground/cloud positions are used
as supplied, rather than silently discarding cloud X/Z.

## Explosion

`Start`, `Spawn(SpawnInfo)` and `BeforeRender` return typed errors.
`SpawnInfo::kind` selects the authored artillery or standard-fireball policy.
The host preserves the real particle distributions, drag/gravity, fire/smoke
billboard batches, four air fronts and ground ring, fading flash light,
48-particle mesh-debris emitter, procedural DSP audio, and autonomous crater
projection/fade. No blast impulse is claimed: the original implementation did
not apply one.

The STL simulation vectors and procedural texture policy stay in private host
storage, not Core or an ECS component. Only POD `EmitterState` and `CraterState`
diagnostics cross the entities capability. `ExplosionPolicy.hpp` is a private
implementation header, not an installed public API. It preserves the authored
native equations/textures; the legacy native implementation remains separate
for unported callers.

Each host has independent RNG, texture handles, asset IDs, emitters, craters and
resource entity. Six procedural textures, debris mesh/material and crater
bindings are restored on device recreation. Craters outlive their emitter,
shrink over the last six seconds, then expire at 28 seconds. Clear/Stop,
move assignment, pending destruction and scene replacement affect only owned
entities/resources. Audio events are best effort (as in the native demo);
graphics and draw-submission failures propagate to Application cleanup.

## Real consumer and validation

`zahlen-scene-host --terrain-demo --raster-only` now combines the snow
heightfield/falling-body host with Lightning and Explosion as well as CombatFX.
HostUI owns the panel and pause/quit controls. Pausing freezes effect clocks;
resource rebinding and rendering continue.

Existing GNU 16 VM, Release, Vulkan llvmpipe:
- Extras_LightningHost: 4 passed.
- Extras_ExplosionHost: 4 passed.
- Extras_TerrainHost: 5 passed; rendered Application test now asserts live
  lightning, explosions and craters before/after graphics recreation and reads
  back lit frames with host UI.
- Extras_CombatHost: 4 passed.
- Extras_CameraHosts (including frame pipeline): 14 passed.
- Extras_Application (including screenshot): 20 passed.
- Actual terrain/VFX CLI: 60 headless raster frames, exit 0.
- Native compatibility VFX target builds; it is not linked into COM consumers.

The previous Terrain/Combat host report describes the preceding slice; its
statement that Lightning/Explosion hosts are unported is superseded here.
The **legacy callers/adapters** remain explicitly native. This is not completion
of editor, Console, Benchmark, or Animator/CharacterMotor runtime migration.

Installed-package check also passed: install Engine + VFX components into a
separate prefix, configure an external C++26 consumer with `find_package(ZahlenVFX)`,
include all three public host headers, link `Zahlen::VFX`, construct/destruct the
hosts and run the resulting executable. Set `CMAKE_CXX_STANDARD` to 26 before
`find_package`, so the package's compiler/reflection checks use C++26 too.
No private policy header or source-tree include path is required.
