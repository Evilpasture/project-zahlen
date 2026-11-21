# Terrain and CombatFX capability hosts

`zahlen_terrain` (`Terrain/Terrain.hpp`) owns independent CPU terrain
 documents and publishes meshes, material bindings, entities and real physics
heightfields through engine capabilities. `TerrainGeometry` contains the shared
CPU algorithm.
The host retains its engine reference, supports independent owners and scene
clear, and republishes assets after device recreation. Replacement mesh
ownership is attached before registration, since replacing an ownership
component unregisters the preceding asset ID.

`zahlen_vfx` (`VFX/CombatFX.hpp`) is an ordinary COM host policy object. It owns
impact decals, procedural fallback textures, particles, rings and tracers,
uses audio capabilities, and isolates per-instance presets and state.
`Lightning` and `Explosion` are likewise pure COM host services inside
`zahlen_vfx` (`Zahlen::VFX`).

Run `zahlen-scene-host --terrain-demo --raster-only --frames 180` for the actual
snow-terrain/heightfield/falling-body host with periodic impacts, decals,
sparks, tracers, rings and audio events. HostUI owns pause/quit controls.
No native Engine, renderer context or asset-manager service is exposed.

Application layers may return void or `std::expected<void, ErrorCode>` from
Start/Tick (as with UI/BeforeRender). Errors retain their typed value. Start
failure stops entered layers, including the partially started failing layer,
in reverse order; unvisited layers are not stopped. Tick failure skips later
callbacks and rendering. Cleanup runs before engine shutdown.

Capture tests reuse a persistent CaptureBuffer. The query/copy capture API is
not a hidden frame cache; video/screenshot hosts should retain their buffers.
