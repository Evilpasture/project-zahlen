# Terrain and CombatFX capability hosts

`zahlen_terrain` (`Terrain/Terrain.hpp`) owns independent CPU terrain
 documents and publishes meshes, material bindings, entities and real physics
heightfields through engine capabilities. `TerrainGeometry` contains the shared
CPU algorithm; the transitional global-slot terrain adapter has been removed.
The host retains its engine reference, supports independent owners and scene
clear, and republishes assets after device recreation. Replacement mesh
ownership is attached before registration, since replacing an ownership
component unregisters the preceding asset ID.

`zahlen_vfx` (`VFX/CombatFX.hpp`) is an ordinary COM host policy object. It owns
impact decals, procedural fallback textures, particles, rings and tracers,
uses audio capabilities, and isolates per-instance presets and state.
`Lightning` and `Explosion` are likewise pure COM host services inside
`zahlen_vfx` (`Zahlen::VFX`); all transitional native VFX adapters are gone.

Run `zahlen-scene-host --terrain-demo --raster-only --frames 180` for the actual
snow-terrain/heightfield/falling-body host with periodic impacts, decals,
sparks, tracers, rings and audio events. HostUI owns pause/quit controls.
No native Engine, renderer context or asset-manager service is exposed.

Application layers may return void or `std::expected<void, ErrorCode>` from
Start/Tick (as with UI/BeforeRender). Errors retain their typed value. Start
failure stops entered layers, including the partially started failing layer,
in reverse order; unvisited layers are not stopped. Tick failure skips later
callbacks and rendering. Cleanup runs before engine shutdown.

Validation on the existing GNU 16 VM / Vulkan llvmpipe build:
- Extras_TerrainHost: 5 passed (geometry, independent lifetime, GPU recovery,
  real heightfield raycast/cleanup, rendered Application/UI/capture/recreation).
- Extras_CombatHost: 4 passed (including custom presets/lifetimes/recreation).
- Extras_CameraHosts: 14 passed including frame-pipeline/error regressions.
- Extras_Application: 20 passed including screenshot regressions.
- Actual scene CLI: 3 headless raster frames, exit 0.
- Governance checks and patch whitespace checks passed.

Capture tests reuse a persistent CaptureBuffer. The query/copy capture API is
not a hidden frame cache; video/screenshot hosts should retain their buffers.

This is an incremental migration, not completion of editor, Console,
Benchmark, native Lightning/Explosion, or Animator/CharacterMotor runtime.
