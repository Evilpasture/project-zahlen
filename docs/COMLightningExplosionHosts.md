# Lightning and Explosion capability hosts

The `zahlen_vfx` / installed `Zahlen::VFX` target provides three ordinary
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

`BoltState` is a POD diagnostic component. CPU geometry and GPU mesh handles
are retained by the host keyed on device generation. Cache rebinding does
not reupload, and device recovery reuploads from retained geometry without
resetting phase.

Unlike the native adapter, this host does **not** overwrite global ambient
exposure. Owned flash/impact light entities provide flashes without one host
restoring settings over another host's effect. The host uses ground and cloud
positions as supplied, including cloud X/Z.

## Explosion

`Start`, `Spawn(SpawnInfo)` and `BeforeRender` return typed errors.
`SpawnInfo::kind` selects the authored artillery or standard-fireball policy.
The host preserves the real particle distributions, drag/gravity, fire/smoke
billboard batches, four air fronts and ground ring, fading flash light,
48-particle mesh-debris emitter, procedural DSP audio, and autonomous crater
projection/fade.

> [!WARNING]
> Explosion effects omit blast impulses; physics bodies receive no impulse.

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

## Scene host

`zahlen-scene-host --terrain-demo --raster-only` combines the snow
heightfield/falling-body host with Lightning and Explosion as well as CombatFX.
HostUI owns the panel and pause/quit controls. Pausing freezes effect clocks;
resource rebinding and rendering continue.

For an installed consumer, find `ZahlenVFX` and link `Zahlen::VFX`.
