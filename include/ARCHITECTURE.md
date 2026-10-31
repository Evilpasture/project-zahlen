# Zahlen Engine Architecture

This document provides a technical overview of Project Zahlen's architecture, mathematical conventions, frame loop execution order, deferred render graph topology, scripting IPC protocol, asset pipeline, and Three.js porting guidelines.

---

## 1. Core Principles

* **C++26 Static Reflection (`std::meta`)**: Eliminates manual binding glue code. ECS components, reflection metadata, JSON serialization, and scripting bindings are reflected automatically at compile-time.
* **Data-Oriented & Lock-Free**: Custom, page-aligned, lock-free/atomic data structures (`ZHLN::Array`, `HashMap`, `SkipList`, `MemoryPool`) eliminate runtime heap allocations.
* **PIMPL Encapsulation**: Public APIs (`RenderContext`, `PhysicsContext`, `Window`) hide internal Vulkan and Jolt headers behind opaque implementation pointers — and never hand those pointers out. There is no `GetImpl()` anywhere in the tree: a class's implementation is not part of its API, and a caller that genuinely needs the contents (`src/render` reading a window's native descriptor) is served by a single named friend instead. The presentation seam follows the same rule: `Window` and `PlatformHost` no longer hand out a `PresentationTarget`, the kernel — which owns the session and every window in it — resolves a frame's destination, and a caller asks `Kernel::AcquireTarget()`/`Engine::AcquireTarget()` for an attachment. The engine's reach stops at the session's façade: it asks `PlatformHost` for the session's target and for any window's, and it never touches a `Window`'s state. `Window` therefore grants exactly one friendship — to `PlatformHost`, in its own subsystem, for the windowed case of the session's target — and `PlatformHost` grants exactly one, to the kernel that resolves frames. `configure/check_pimpl_encapsulation.py` runs at CMake configure time and fails the build if an accessor, a conversion operator, a `GetImpl`-style name, a public `PresentationTarget`, or any other class friendship on those two headers comes back.
* **Fiber Task Scheduler**: Cooperative, multi-threaded stackful fibers (`ZHLN::TaskSystem`) drive parallel system updates and worker thread GPU command recording.

---

## 1.1 Strict ECS Mandate & Architectural Constraints

To preserve the engine's data-oriented design (DOD), cache locality, zero-allocation memory guarantees, and C++26 reflection-based tooling, **ALL state in Zahlen MUST reside in ECS Components, and ALL logic MUST reside in ECS Systems.**

### The Core Law
> **There are no "Manager" or "Simulation" classes for gameplay or visual effects in Zahlen.**
> Every entity, bolt, particle, projectile, sound, and light source is represented as a plain-data `struct` inside the `ZHLN::Components` namespace.

---

### Strict Development Rules

#### 1. Zero Class-Based State
* **NO `class` instances may hold simulation, timing, or visual state.**
* Features MUST NOT wrap state inside private member variables (`m_phase`, `m_time`, `m_luminance`).
* All state MUST be stored in `ZHLN::Components` as POD (Plain Old Data) structs and accessed via `reg.Get<Component>()` or `reg.GetRawArray<Component>()`.

#### 2. The $N$-Concurrent Rule
* **Every feature MUST support $N$ simultaneous instances out of the box.**
* Hardcoding single-instance state assumes a feature will only happen once. By making state an ECS Component attached to an `Entity`, the engine automatically supports 1, 100, or 10,000 instances in contiguous memory without state collisions.

#### 3. Pure System Functions
* Logic MUST be written as pure, stateless system functions (`void SystemName(Engine& engine, float dt)`).
* Systems MUST NOT store internal state across frames. If a calculation needs memory across frames, that memory belongs in a Component attached to an Entity or a Global Settings Entity.

#### 4. External Resource Lifecycle
* Components store plain generational handles. Physics, audio, articulation and rendering do **not** keep entity-owner ledgers or poll ECS liveness. The registry has no removal observers.
* In an Engine scene, `DespawnEntity(engine, entity)` marks the hierarchy with `Components::PendingDestroy`; the Engine's main ECB also marks instead of destroying. The `SceneCleanup` scheduler step runs after `MainECBPlayback` and before camera/rendering. It queries intact components, collects physics handles for one `PhysicsContext::DestroyBodies(span)` call under one shadow lock, releases other owned handles (including registered VFX cleanup passes), then calls `Registry::Destroy` on the marked entities. Bodies finish retiring on the next physics step.
* Raw `Registry::Destroy` and `Registry::Clear` remain **immediate, data-only** primitives. Generic ECBs (including standalone `World` ECBs) likewise destroy immediately; only the Engine opts its ECB into deferred destruction. Never use raw destruction on an Engine scene with external-resource components: use `DespawnEntity` and `Engine::ClearScene` instead. `Engine::ProcessPendingDestroy` allows an explicit synchronous cleanup before the next frame. Standalone registries must explicitly release their owned resources before raw removal/clear (e.g. `PrefabFactory::ReleaseOwnedMeshes` and `TerrainSystem::ReleaseTerrainData`).
* Use the typed `SceneResources::Attach/Detach` helpers from `<Zahlen/Engine/SceneResources.hpp>` for direct replacement/removal of core resource-owning components; optional layers supply their own explicit helpers. A raw `Registry::Add` replacement or `Remove` erases the old handle without releasing its external resource. Systems MUST NOT manually manage raw heap pointers or manage class destructors.

#### 5. Environment & Global State Isolation
* Systems modifying global engine state (e.g., Post-Processing, Exposure, Sky Gradients) MUST NOT overwrite global base values.
* Global state changes MUST be applied as non-destructive deltas or read from an un-flashed baseline cached on the Global Settings entity.

---

### Anti-Pattern Reference Guide

#### ❌ FORBIDDEN: Classic OOP Class Bypass
```cpp
// BAD: Stateful class holding simulation variables, non-DOD memory, single-instance lock
class LightningSimulation {
  private:
    float  m_phaseTime    = 0.0f;
    float  m_baseExposure = 4.5f; // Clobbers global exposure!
    Entity m_flashLight;

  public:
    void TriggerStrike(Engine& engine, ...);
    void Update(Engine& engine, float dt);
};
```

#### ✅ MANDATED: Idiomatic Data-Oriented ECS
```cpp
// GOOD: Pure POD Component
struct LightningComponent {
    LightningConfig config {};
    LightningPhase  phase               = LightningPhase::Idle;
    float           phaseTime           = 0.0f;
    float           baseAmbientExposure = 4.5f;

    BufferHandle vboPos     = BufferHandle::Invalid;
    BufferHandle vboFrame   = BufferHandle::Invalid;
    BufferHandle vboSurface = BufferHandle::Invalid;

    // Component state only. The renderer sees non-owning mesh registrations;
    // Engine scene cleanup frees these buffers before destroying the component.
    // Direct replacement/removal must release them explicitly first.
};

// GOOD: Stateless System Function
// Parameters that cross a subsystem boundary are the engine's own value types --
// ZHLN::Float3, ZHLN::Float4, ZHLN::Double3 -- never Vec3Arg/QuatArg/RVec3Arg,
// never `const JPH::Vec3&`, and never a conversion helper at the call site. A
// caller writes `{1.0f, 2.0f, 3.0f}`, or passes the Vec3 it was already holding;
// the crossing is a conversion owned by the type (Math/ValueTypes.hpp), so Jolt's
// SIMD classes appear only where the maths happens. Quat has no value type yet
// and stays JPH::Quat.
namespace ZHLN::Lightning {
Entity Spawn(Engine& engine, ZHLN::Double3 cloudPos, ZHLN::Double3 groundPos, const LightningConfig& cfg);
void   Update(Engine& engine, float dt);
} // namespace ZHLN::Lightning
```

---

### Architectural Rationale

| Requirement | Why OOP Fails | Why ECS Succeeds |
| :--- | :--- | :--- |
| **Cache Locality** | Heap-allocated objects (`new MyClass()`) scatter data across RAM pages, causing CPU L1/L2 cache misses. | `SparseSet` arrays store components contiguously in RAM, allowing SIMD vectorization and prefetching. |
| **Parallel Execution** | Mutable class methods introduce thread races when accessed concurrently by multiple workers. | `SystemGraph` derives component Read/Write hazards from each system's `Query<const T, U&>` signature (or explicit `Registry&` for structural writers) and orders fiber tasks accordingly. |
| **State Save/Load** | Private class members cannot be serialized without custom, error-prone boilerplate. | Reflection (`std::meta`) automatically serializes all Component POD structs to disk or network instantly. |


---

## 1.2 Core and Optional-Layer Boundaries

`src/`, `include/`, and the existing top-level `modules/` directory
form the **Core Engine**. The public `app/include/Zahlen/{App,AppOutcome,ConfigCli,HostRunner,KernelApp}.hpp`
headers are owned and published by `zahlen_application` / `zahlen_host_runner`,
not `zahlen_engine`; their `#include <Zahlen/...>` spellings stay the same.
Only the host-facing `ConfigCli.hpp` and `App.hpp` consume CLI bindings. The `modules/` directory
is reserved for the engine's C++ module interfaces; it is not a catch-all for
additional subsystems. Optional code is organized by architectural role instead:

| Root | Owns | Examples |
| :--- | :--- | :--- |
| `plugins/` | Asset formats, serializers, importers, and codecs | `glTF/`, `json/`, `toml/`, `Fonts/`, `SVG/`, `AssetCooking/` |
| `extensions/` | Reusable engine subsystems and network/platform I/O | `Animation/`, `Camera/`, `Scripting/`, `UI/`, `VFX/`, `net/Network/`, `net/HTTP/`, `net/RemoteAsset/`, `net/GitHub/` |
| `gameplay/` | Domain-specific systems and project integrations | `ALife/`, `Interaction/`, `FallbackScene/`, `ProjectLight/` |

`gameplay/ProjectLight/` groups the canonical stable-ID `DataModel` and the
native ProjectLight client that adapts the existing server protocol. The
ProjectLight-specific MessagePack/zlib protocol types live with that client,
not in the reusable Zahlen wire-protocol extension.

> **Rule: dependencies point outward from Core.** Plugins, extensions, and
> gameplay and the application host may consume Core; code under `src/`,
> `include/`, or `modules/` must never include, import, or link an outer-layer
> target. Core may not include the app-owned public headers
> `<Zahlen/{App,AppOutcome,ConfigCli,HostRunner,KernelApp}.hpp>`.

`configure/check_core_layer_boundary.py` enforces the source/header half of this
rule during CMake configuration. It scans module imports and resolves headers
against the optional layers' and `app/include/`'s public include roots, so both
a spelled path and a short include such as `#include <Zahlen/App.hpp>` or
`#include <json/JSON.hpp>` are caught, with no application-facade exception.
Only `zahlen_application` and `zahlen_host_runner` publish `app/include/` to
their consumers; Core targets never receive that include root. Thus a Core
source including `<Zahlen/App.hpp>` also fails at compilation, independently of
the Python check. `ConfigCli.hpp` can consume the generic CLI contract because the application
and host-runner targets link `zahlen_command_line`; Core does not link app/. CMake
links remain explicit: the composition roots in `app/`, samples, tests, and
host tools may select the targets they use, while Core targets must not depend
on them.

`configure/check_layer_dag.py` enforces the edge *between* the optional layers
that has to point one way: nothing under `plugins/` or `extensions/` may include
a `gameplay/` header or **link** a `gameplay/` target. `gameplay/` is written
against the reusable layers, so the reverse edge is circular in effect -- the
layer below cannot be built or run without the domain layer above it. The check
exists because that edge really was crossed: `Scripting.cpp` called
`RagdollAuthoring::BuildHumanoidBipedRagdoll` and its target linked
`zahlen_ragdoll_authoring` and `zahlen_datamodel`. Both are gone; the mechanism
that replaced them is below.

The `plugins/` ↔ `extensions/` edges are deliberately *not* restricted: UITOML is
a plugin that names `GUI::UINode` from `extensions/UI`, and the network
extensions build on the plugin serializers. Those are design decisions, not
accidents, and a rule against them would fail the existing composition.

### Inverting an edge: how gameplay reaches a generic layer

A generic layer must be able to invoke domain-provided policy without knowing the
domain exists, and a gameplay or plugin module must be able to contribute one
without inverting the DAG. `Zahlen/GUI/FontLoader.hpp` (`BakedFontLoader`) and
`Zahlen/Engine/GameModule.hpp` (`Module`) are the canonical seams:

1. **Core declares the shape.** `Module` declares lifecycle hooks, `ModuleStep`
   placements, and `GraphSystemsFn` callbacks; `Engine::InstallModule` composes
   them. Core names no gameplay domain.
2. **The module provides the value.** Gameplay and extension modules implement
   stateless systems or data-driven loaders against Core's reflected ECS
   components.
3. **The host connects them.** `app/Application.cpp` supplies the reusable
   `zahlen_application` preset and outer loop for ordinary C++ games;
   `app/main.cpp` is a compiled `App().Run(...)` physics/lighting/audio sandbox.
   Host-specific choices live
   in dedicated executables: `utils/editor/main.cpp` owns the paused world
   editor, `utils/benchmark/main.cpp` owns a headless logic workload, and
   `utils/scene/main.cpp` owns document boot, fallback, Discord and the Lua
   graphics-file watcher. They install their modules before the first scene.
   Core exposes neutral Engine and module contracts and never names optional
   layers; the host-facing `<Zahlen/App.hpp>` belongs to `zahlen_application`
   and binds `EngineConfig` through reflection in the host layer. `EngineConfig` is the canonical engine, physics and renderer configuration;
   App's lifecycle callbacks live on `Application`, while the low-level
   `Engine::Run` accepts an explicit `EventPoller` for hosts that need one.

### Targets and dependency order

Each feature directory owns its sources and target. The directory hierarchy is
not a monolithic library boundary: consumers link the capability they need,
and optional third-party dependencies stay local to the owning target.

| Target | Directory | Role |
| :--- | :--- | :--- |
| `zahlen_serialization` | `plugins/json/` + `plugins/toml/` | Reflection-driven JSON/TOML documents; JSON owns simdjson |
| `zahlen_lua` | `plugins/lua/` | Sandboxed reflection-driven Lua state evaluation and transactional patching; owns LuaJIT |
| `zahlen_gltf` | `plugins/glTF/` | glTF/GLB importer; owns cgltf and uses the serializer |
| `zahlen_fonts` | `plugins/Fonts/` | Baked-font loader for fontbm pairs and cooked font containers |
| `zahlen_svg` | `plugins/SVG/` | Optional resvg-backed SVG rasterizer |
| `zahlen_asset_cooking` | `plugins/AssetCooking/` | Host-side image and cooked-asset codecs used by `zcook` |
| `zahlen_network` | `extensions/net/Network/` | `ZHLN.Wire` and replication over the engine ECS |
| `zahlen_http`, `zahlen_remote_asset`, `zahlen_github` | `extensions/net/{HTTP,RemoteAsset,GitHub}/` | Optional HTTP transfer, remote-asset cache, and GitHub tree adapter |
| `zahlen_cdn` | `extensions/net/CDN/` | Base-URL asset `Fetch` (cache, else download) and `Load` (cache, else raw bytes) over the remote-asset cache |
| `zahlen_animation`, `zahlen_camera`, `zahlen_character_controller`, `zahlen_terrain`, `zahlen_ui_schema`, `zahlen_vfx` | `extensions/` | Reusable animation, camera, controller, terrain, UI, and VFX capabilities |
| `zahlen_ragdoll_authoring` | `gameplay/RagdollAuthoring/` | Domain-specific humanoid ragdoll authoring |
| `zahlen_scripting`, `zahlen_console`, `zahlen_editor` | `extensions/` | Lua-independent reflection property inspector, developer console, and editor |
| `zahlen_project_light_lua` | `gameplay/ProjectLight/LuaBindings/` | DataModel ↔ LuaJIT adapter; the only target where the two layers meet |
| `zahlen_alife`, `zahlen_interaction`, `zahlen_fallback_scene` | `gameplay/` | Domain-specific simulation, interaction rules, and fallback game scene |
| `zahlen_datamodel`, `zahlen_project_light_client` | `gameplay/ProjectLight/` | ProjectLight's stable-ID object graph and optional native client |
| `zahlen_host_runner` | `app/` | Shared process/crash/task lifetime and frame pacing; `KernelApp` drives Kernel-only tools with no gameplay links; publishes `app/include/` |
| `zahlen_application` | `app/` | Game loop, reflected/registered CLI, layers and CMake-generated optional-module wiring; publishes `app/include/` and links the engine-independent `zahlen_command_line` for the `App.hpp` facade |
| `zahlen_project_light_app` | `app/ProjectLightClientMain.cpp` | Optional ProjectLight entrypoint: App lifecycle with only the client's selected gameplay modules and connection preflight |
| `zahlen_scene_documents` | `utils/scene/` | TOML/Lua scene import shared only by tools |
| `zahlen_world_editor`, `zahlen_benchmark_app`, `zahlen_scene_host` | `utils/{editor,benchmark,scene}/` | Separate world-editor, headless-benchmark and scene/file-watcher hosts |

`ZHLN_BUILD_EXTRAS` remains the existing CMake option for compatibility; it now
gates the optional `plugins/`, `extensions/`, and `gameplay/` build layers.
`zahlen_extras` also remains as a downstream compatibility `INTERFACE` target,
but in-tree code links individual targets so it does not pull unrelated
subsystems into an executable. `plugins/AssetCooking/` is the deliberate
exception to the option: `cmake/AssetPipeline.cmake` configures the host-side
codec target for `zcook` even when optional runtime layers are disabled.

The root CMake configuration establishes target order: plugins provide the
serializer, extensions provide reusable systems and I/O, gameplay creates the
ProjectLight DataModel, and the Lua binding target is configured after that
DataModel exists. `zahlen_serialization` publishes `zahlen_ui_schema` because
`UITOML.hpp` names the UI schema in its public API. Other dependencies stay
private where they are implementation details.

The ProjectLight Lua bindings live inside `gameplay/ProjectLight/LuaBindings/`:
the adapter links both `zahlen_datamodel` and `zahlen_lua`, while `zahlen_lua`
links no gameplay target at all, letting any host or tool take declarative Lua
state evaluation without ProjectLight.

### What the boundary buys

The core-only configuration (`-DZHLN_BUILD_EXTRAS=OFF`) does not build the
optional format/runtime/gameplay libraries or require simdjson, resvg, libcurl,
or LuaJIT for those layers. The offline `zcook` tool and its asset-cooking
codecs remain available. More optional dependencies can be disabled at their
own target, such as `-DZHLN_BUILD_SVG=OFF` or `-DZHLN_BUILD_HTTP=OFF`; an
unavailable library skips only the target that needs it.

A few examples illustrate the intended seam:

* **Scenes are Core data; text formats are plugins.** `Zahlen/Engine/Scene.hpp` and
  `Scene::Instantiate()` remain Core. `plugins/toml/SceneTOML.hpp` turns a
  scene into TOML and binds Jolt vectors; Core does not parse a document.
* **Model import is a plugin.** `plugins/glTF/GLTFImporter.cpp` reads a model
  using cgltf, stb_image, and `plugins/json`, then writes a plain
  `ZHLN::ModelPrefab` to the Core prefab cache. Core reads the cached structure
  and instantiates it; it never calls the importer or depends on glTF parsing.
* **Fonts follow the same pattern.** Core owns the `BakedFontLoader` seam and
  embedded fallback bake. `plugins/Fonts/` installs the production loader for
  fontbm atlases or cooked font containers; Core parses no outline font.
* **ProjectLight is application/gameplay policy.** Its `DataModel` owns stable
  Instance IDs and the object graph; bindings and the native client adapt that
  graph to existing engine APIs without making Core depend on LuaJIT, sockets,
  or the ProjectLight protocol.
* **Lua is a state-only plugin, never a per-frame scripting runtime.** Core
  knows nothing about Lua, FFI, or Fennel; all frame simulation runs in C++26
  systems. `plugins/lua/` (`ReflectLua`) evaluates bounded, sandboxed Lua
  chunks (`no ffi/io/os/package/debug`) into C++26 reflected structs and
  components at startup, scene load, hot-reload, or console `patch`. The
  Lua-independent property inspector lives in `extensions/Scripting/`.

The ordinary-game composition root lives in `app/Application.cpp`, above
`src/`. `app/CMakeLists.txt` generates `DefaultModules.cpp` from
`app/DefaultModules.cpp.in`: only existing optional targets contribute includes,
links and calls, in font → character → camera → interaction → terrain → IK →
FreeCam → ragdoll order. Application code contains no optional-module compiler
definitions or `#if` wall. `app/main.cpp` links the preset and contains only game
code. Tool-specific composition roots remain separate executables under
`utils/`; `zahlen_ui_editor` is a Kernel-only composition root under `app/`.
Core remains usable without optional targets and does not register or call
into an optional implementation unless a Core-owned seam is provided.

---

## 1.3 Type Ownership and Include Discipline

There is no engine-wide type header, and there is no plan to grow one. A shared
type lives with the subsystem that owns its meaning, and a file reaches every
type it spells through its own includes.

> **Rule: a header declares what its subsystem owns; a source names what it
> uses.** Reaching a type through a neighbour's includes is the defect the rule
> prevents. It is invisible in review, it survives every test, and it turns one
> struct edit into a rebuild of the engine.

| Type | Home | Who names it |
| :--- | :--- | :--- |
| `EnumFlag`, `EnableEnumFlags<Enum>` | `Zahlen/Core/EnumFlags.hpp` | any header with a flags enum |
| `AssetID`, `MaterialID`, `HashAssetID`, `InvalidAssetID`, `InvalidMaterialID` | `Zahlen/Core/AssetID.hpp` | asset-facing headers and the components that hold a reference |
| `ScissorRect`, `ViewportRect` (with `Extent2D`, `Offset2D`) | `Zahlen/Math/Geometry2D.hpp` | GUI, windowing and renderer alike |
| `VertexPosition`, `VertexTangentFrame`, `VertexSurface`, `VertexSkin`, `PackedRGBA8`, `Packed1010102`, `PackedHalf2` | `Zahlen/Render/Vertex.hpp` | the cooker and both consumers of a vertex |
| `AudioHandle`, `AudioBus`, `AudioBusId`, `AudioAttenuationModel`, `AudioPlaybackMode`, `AudioReverbPreset`, `AudioAttenuationDesc`, `AudioOcclusionDesc`, `AudioReverbConfig`, `AudioFilterType`, `AudioWaveformType`, `AudioNoiseType` | `Zahlen/Audio/AudioTypes.hpp` | audio and its callers; no renderer is involved |
| `UIBatch`, `UIDrawData` | `Zahlen/GUI/UIData.hpp` | GUI produces it, the renderer's `RenderUI` consumes it |
| `GlyphMetric`, `FontAtlas` | `Zahlen/GUI/Font.hpp` | text layout and the atlas bake |
| `TextureHandle`, `RenderTextureHandle`, `BufferHandle`, `PipelineHandle`, `ResourceGroupHandle`, `SystemTextures`, `FrameTarget` | `Zahlen/Render/Handles.hpp` | the renderer and the components that hold a GPU resource — deliberately free of Jolt |
| `Mesh`, `MeshletStreams`, `Material`, `DrawFlags`, `CSGOperation`, `CSGModifier` | `Zahlen/Render/Types.hpp` | the renderer |
| `MeshletDesc`, `MeshletBuildResult`, the `kMeshlet*` limits | `Zahlen/Render/Meshlet.hpp` | the meshlet cooker and the renderer |

Two consequences are the point of the split. Editing a renderer struct
recompiles the renderer and its consumers instead of every translation unit that
wanted an `EnumFlag`; and a subsystem that names no Jolt type never compiles
`<Jolt/Jolt.h>` — `zahlen_window` carries neither Jolt's headers nor its `JPH_*`
ABI macros, because `<Zahlen/Window/Window.hpp>` reaches none of its types.

`configure/check_include_provenance.pl` runs at CMake configure time and fails the
build when a file spells a tracked first-party type, or any `JPH::` type, that no
include in its own closure provides — and when an include names a first-party
header that does not resolve. It is Perl rather than Python because the check
re-reads the whole tree at every configure: it caches per-file scan results in
the system temp directory keyed by path, mtime and size, so a rerun on an
unchanged tree revalidates instead of rescanning, and a changed or missing file
is always recomputed from scratch. As with the boundary rule above, this is
enforced rather than documented.

The generated GPU types are not part of this closure at all. `GeneratedGpuTypes.hpp`
(shader tool output) is named in exactly one header, `src/render/GpuLayout.hpp`,
which is renderer implementation: nothing under `include/` includes it, names a
`Slang::` type, or needs the shader tool to have run.

The structs the engine and the shaders share are *generated*, from the same
reflection that decides their layout: `utils/zshader` emits every GPU-visible
struct -- `Particle`, `ParticleEmitterParams`, `MeshParticleEmitterParams`,
`Light`, `FrameUniforms` and the renderer-only ones -- with the members at the
offsets Slang seated them, the `_padN` between them and the `alignas` each lane
carries, and asserts each struct against the reflection it came from. There is no
hand-written half of the ABI to keep in sync, and no second spelling per concept.
The engine authors its own terms (`Zahlen/Render/ParticleEmitterDesc.hpp`,
`Zahlen/Render/FrameData.hpp`) and the renderer packs them at the submit boundary
(`src/render/GpuPack.cpp`): a member the shader no longer declares breaks the
build in that pack function, where the shader's layout is the only thing being
spoken -- never in a public type.

---

## 1.4 Header Layout Mirrors the Module Layout

`include/Zahlen/` is not a flat bag. Every header lives in the directory named
after the subsystem that owns it, and that directory is the one its module
fragment and CMake library are named after:

| Header directory | Module unit | CMake library |
| :--- | :--- | :--- |
| `include/Zahlen/Core/` | `modules/zahlen-core.cppm` | header-only; its two sources build into `zahlen_engine` |
| `include/Zahlen/Math/` | `modules/zahlen-math.cppm` | header-only |
| `include/Zahlen/Threading/` | `modules/zahlen-threading.cppm` | `zahlen_threading` |
| `include/Zahlen/ECS/` | `modules/zahlen-ecs.cppm` | `zahlen_ecs` |
| `include/Zahlen/Physics/` | `modules/zahlen-physics.cppm` | `zahlen_physics` |
| `include/Zahlen/Render/` | `modules/zahlen-render.cppm` | `zahlen_render` |
| `include/Zahlen/Audio/` | `modules/zahlen-audio.cppm` | `zahlen_audio` |
| `include/Zahlen/Window/` | — | `zahlen_window` |
| `include/Zahlen/Input/` | — | header-only |
| `include/Zahlen/GUI/` | — | `zahlen_gui` |
| `include/Zahlen/FileSystem/` | — | `zahlen_filesystem` |
| `include/Zahlen/Engine/` | `modules/zahlen-engine.cppm` | `zahlen_engine` |

`include/Zahlen/Engine/` holds the composition layer only: lifecycle and kernel
hosting (`Engine.hpp`, `Kernel.hpp`), frame orchestration (`FramePlan.hpp`,
`Frame.hpp`, `Clock.hpp`), scene composition (`Scene.hpp`, `SceneResources.hpp`,
`PrefabFactory.hpp`, `ModelPrefab.hpp`), and the seams that bridge two
subsystems (`EngineServices.hpp`, `PoseUploads.hpp`, `SystemContext.hpp`,
`GameModule.hpp`, `AssetManager.hpp`, `World.hpp`).

Only pervasive definitions stay at the root of `include/Zahlen/`: `Common.h`
(the `ZHLN_API` ABI macros), `Config.hpp`, `Error.hpp`, `Log.hpp`, and
`Format.hpp` (whose formatter specializations span Entity, Jolt, and the
renderer, so it cannot live under any one of them without inverting an edge).

There are no forwarding headers. The relocation was staged with one-line shims
at the old root spellings and completed by re-pointing every consumer and
deleting them, so a header is reachable at exactly one path — the one its
subsystem owns. Three directories were also re-cased in place (`ecs/` → `ECS/`,
`gui/` → `GUI/`, `physics/` → `Physics/`); that rename never had shims, because
a shim beside a case-only-renamed directory collides with its real header on a
case-insensitive filesystem.

An application links `app/` and may therefore use `<Zahlen/Zahlen.hpp>`
(`app/include/Zahlen/Zahlen.hpp`), which pulls the engine surface an
application actually compiles against together with the host headers
(`App.hpp`, `HostRunner.hpp`, `KernelApp.hpp`, `ConfigCli.hpp`,
`AppOutcome.hpp`). It lives in the application layer on purpose: the engine
itself still has no engine-wide header, and the umbrella is not visible to
Core targets.

---

## 2. Mathematical & Geometric Conventions

Zahlen adheres strictly to standard Vulkan and Jolt Physics conventions across both CPU host code and GPU shaders:

### Coordinate System (World Space)
* **Handedness**: **Right-Handed**
* **Axes**: 
  * **$+X$**: Right
  * **$+Y$**: Up
  * **$-Z$**: Forward
* **Camera Orientation**: Default forward vector looks down **$-Z$** (at `yaw = -90.0f` and `pitch = 0.0f`).

### Matrix Layout & Multiplication Order
* **Storage Layout**: **Column-Major** in both C++ (`JPH::Mat44`) and Slang (compiled with `-matrix-layout-column-major`).
* **Multiplication Order**: **Column Vectors** ($M \cdot v$). Shaders and host code execute `mul(matrix, vector)` / `m * v`.

### Winding Order & Culling
* **Front-Face Winding**: **Counter-Clockwise (CCW)** (`VK_FRONT_FACE_COUNTER_CLOCKWISE`).
* **Cull Mode**: **Back-Face Culling** (`VK_CULL_MODE_BACK_BIT`).

### Extents & Bounding Volumes
* **Box Convention**: **Half-Extents** ($\frac{\text{Width}}{2}, \frac{\text{Height}}{2}, \frac{\text{Depth}}{2}$).
* **Usage**: `CreateBox(ctx, halfExtents)`, Jolt's `JPH::BoxShape`, and culling bounds all expect half-extents. Passing `(1.0, 1.0, 1.0)` creates a box of dimensions $2 \times 2 \times 2$.

### Projection & Clip Space
* **Depth Range**: **$[0, 1]$** (Vulkan Zero-to-One depth range).
* **Y-Axis Clip Space**: **Y-Down** in clip space, handled natively inside `Math::CreatePerspective` and `Math::CreateOrtho` via a flipped Y-column.

---

## 3. Frame Lifecycle & Execution Order

Each frame executes a **frame plan**: an ordered list of steps that core seeds
with its own and that modules contribute to by declaration. `Engine::Tick`
executes the plan and nothing else -- the engine holds no callbacks to replay and
no ordering of its own beyond the steps `BuildCoreFramePlan` places.

A module says what it wants to run and where, and never touches the schedule:

```cpp
// extensions/CharacterController/CharacterController.cpp
constexpr ModuleStep kFrameSteps[] = {
    {.phase = FramePhase::Input,        .name = "CharacterInput",       .run = &CharacterInputStep,
     .placement = StepPlacement::Before, .anchor = "HostUICallback"},
    {.phase = FramePhase::PlayerIntent, .name = "PlayerInputTranslate", .run = &PlayerIntentStep,
     .placement = StepPlacement::After,  .anchor = "StateAndShaderReload"},
};
```

The application installs the module (`engine.InstallModule(Character::Describe())`),
and `FramePlan::Compose` places the steps against the anchor names -- all or
nothing, reporting `FramePlanError::AnchorMissing` rather than appending a step
somewhere it was not asked to be. A step whose anchor is missing is a composition
error the host sees, not a schedule that is quietly wrong.

Two consequences are worth stating, because they are what the design buys:

* **The plan is composable, and its vocabulary is public.** Anchor names are the
  contract between a module and core. A rename in `BuildCoreFramePlan` is a
  breaking change, and it is reported rather than absorbed.
* **The plan is not rebuilt with the scene.** `InitializeDefaultScene` rebuilds
  the graphs because they are scene state; the timeline is composition state and
  is built once. Rebuilding it per scene was how a replayed module step could
  accumulate a second copy.

An ordinary game's C++ source is compiled into its executable. `Application`
runs its callback **before** the engine's frame plan. System-driven games may
omit the callback and install their own ECS systems instead:

```
App: ProcessEvents (resize included) → layers.Tick → Run(Engine&, dt) callback → Engine::Tick(dt)
Tick: UI (layers.UI) / state-file watches → PlayerIntent → Physics
      → ECS systems (including interpolation / audio) → Camera → Render / Present
Exit: layers.Stop (reverse registration order) → Engine destroyed
```

1. **Input & OS Events**: `Application` calls `Engine::ProcessEvents` before layers/gameplay. `Kernel::ProcessEvents` detects a changed primary framebuffer extent and notifies the renderer; Engine consumes the WindowInputReceiver's `InputStateComponent::needsResize` notification during the same pump (including headless/TTY synthetic sizes) and clears it. Neither `Application::RunConfigured` nor the low-level `Engine::Run` reads the singleton or skips a gameplay frame for resize.
2. **Compiled gameplay**: `App().Run(callback)` owns the per-frame game update; `App().Install(...)` composes systems and frame steps before the first scene. `App().AddLayer(...)` owns heterogeneously typed Start/Tick/UI/Stop layers (including UI-only, Stop-only and move-only ones) using a one-time state allocation and four plain function pointers, no virtual call per frame. Start runs after the scene, Tick runs before the optional `Run(callback)`, and UI runs inside Core's HostUICallback step during `Engine::Tick`; these phases follow registration order. Stop runs once in reverse registration order while Engine resources still exist, whether the loop closes normally or a frame fails; CLI early exits and failed initialization do not start/stop layers. `Engine::Run` remains a low-level Core entry for bespoke hosts with their own composition and event poller.
   `Application::Run(std::span<char* const> args, callback)` reflects its `EngineConfig` and combines it with optional `.WithConfig(gameLaunch)`, `.AddCliOption(...)` and `.WithCliOptions(...)` bindings; the `Run(argc, argv, callback)` adapter checks C arguments first. `ConfigCli` owns generated flag keys, aliases and enum choices across copies, while config targets and custom handler captures are borrowed through the synchronous parse. It walks nested aggregates with prefixed keys (e.g. `physics.maxBodies` -> `--physics-max-bodies`); the non-owning pipeline binary span is intentionally C++-only. Duplicate engine/game/custom keys are rejected, never shadowed or mirrored. The independent `zahlen_command_line` library only tokenizes, dispatches, and handles help/version. Host-owned `ParseEngineConfig` handles graph/debugger commands and environmental defaults without adding process state to EngineConfig.
   Before module installers, `OnStart` and graph construction, Application publishes a **copy of the actual `EngineConfig`** in `Engine::Services()`. Systems may resolve `ECS::Res<EngineConfig>` from `ExtensionServices`. This service exists on CLI and no-CLI runs; neither argv nor CLI handlers enter Engine services. `KernelApp` binds the same canonical value and returns the same typed early-exit reasons.
   `Application::Run` and `KernelApp::Run` return `AppRunResult = std::expected<AppExitReason, ErrorCode>` rather than an OS exit integer. CLI help/version/graph requests and normal host closure are distinct `AppExitReason` values; the parser's `CommandLineError` and the underlying Engine/Kernel/render/recovery errors are propagated unchanged. `AppError` represents invalid C argument vectors or scene/tick failures without a lower-layer error code. `app/main.cpp` and `app/UIEditor.cpp` map success/failure to process status **only inside main**; tests inspect typed outcomes. This does not make process-wide `ProcessScope` state safe for uncoordinated concurrent runners.
3. **Physics Simulation Step**: `PhysicsSystem::Update()` gathers character steering and `ImpulseCommand`s, then steps Jolt Physics at a semi-fixed 60 Hz timestep (`1/60s`). Character grounded flags are written back onto `MovementComponent` after the step.
4. **ECS System Graph**: `SystemGraph::Execute()` runs parallel engine systems (Animation, Articulation, Transforms, Audio — plus, when the matching modules are installed, Interaction and Terrain nodes contributed through the `Module` the application installed; `BuildSystemGraphs` walks `Engine::GetInstalledModules()` and asks each one for its systems). `VisualInterpolationSystem` reads PhysicsWorld pose history and writes interpolated transforms; static bodies are skipped.
5. **Render Graph Execution**: `CullingSystem` performs frustum culling, `LightingSystem` gathers lights, and `RenderSystem` records Vulkan commands and presents. `Engine::Tick` reports `FrameStatus::Error` if device-lost recovery fails.

### 3.1 Signature-driven ECS systems

Register a free function, static member, or stateless callable with one line:

```cpp
void MoveUp(ECS::Query<Components::TransformComponent&> transforms, FrameDt dt) {
    transforms.ForEach([&](Entity, Components::TransformComponent& transform) {
        transform.position += JPH::Vec3(0.0f, dt.value, 0.0f);
    });
}

updateGraph.AddSystem<&MoveUp>();
// updateGraph.AddSystemBefore<&MoveUp>("TransformSystem"); // ordered insertion
```

`SystemGraph` reflects the callable's name and parameter types, resolves each
parameter from `SystemContext`, builds a direct invocation thunk, and infers
component dependencies from the query: `const T`/`const T&` reads, `T`/`T&`
writes. `Query::ForEach` iterates the intersection; `Get`, `Entities`, `Raw`,
`Patch`, and `GetSingleton` support optional lookups/independent passes but
only for declared families. Queries can be projected to a read-only subset.
Declare `Registry&` explicitly when making structural ECS changes or calling
an existing callback that may do so; it conservatively conflicts with all
component accesses. `Res<T>` and `ResMut<T>` inject required services,
`Optional<T&>` / `Optional<const T&>` injects a nullable service, and `FrameDt`, `FrameAlpha`, and
`FrameIndex` avoid guessing between otherwise identical scalar types. The
legacy `AddSystem(SystemInfo)` API remains available to external callers.
`Reflect::CallableInspector` in `Core/Reflection/Callable.hpp` provides
reusable P2996/P3096 callable names, parameter types, and invocation without
an ECS dependency. `ECS::SystemSignature` in `ecs/SystemSignature.hpp` applies
ECS-specific query/registry access rules to those parameter types. Bloomberg
Clang needs `-freflection` plus P3096's `-fparameter-reflection` (or the
unified `-freflection-latest`) for this API; `zahlen_enable_reflection()` probes
and applies the supported flag.

### 3.2 Graphics Settings Flow

Graphics configuration flows in **one direction** through a single canonical model
(`include/Zahlen/Render/GraphicsSettings.hpp`):

```
UI (ImGui) / Lua scripts / quality presets
        │ write
        ▼
ECS settings components (the editing surface)
  PostProcessSettingsComponent · ShadowSettingsComponent · AASettingsComponent
        │ CollectGraphicsSettings() — once per frame, start of RenderSystem::RenderMain
        ▼
GraphicsSettings (canonical model: quality tier, post/GI, AA, shadows, RT config, environment)
        │ RenderContext::ApplySettings() — delta-detected
        ▼
RenderContext state (SetFrameData packs the engine's FrameData into the shader's
  own FrameUniforms; scene-pass push block, pipeline-variant selection, reactive
  GPU target resizes)
```

* **Single collector**: `system/GraphicsSettingsSync.cpp` folds the ECS
  components into `GraphicsSettings` and applies it once per frame. The
  renderer never queries the components directly, and the former
  `PostProcessSystem` ECS→`SetGISettings` bridge is gone.
* **Reactive deltas**: `ApplySettings` compares against the current state and
  reacts — e.g. a `shadowResolution` change (from ImGui, a script or a preset)
  resizes the cascade targets automatically. `SetGISettings` / `SetAAState` /
  `SetShadowResolution` remain only as legacy bridges for tools/tests.
* **Quality tiers**: `Low / Medium / High / Ultra / Custom` presets pin a
  signature of quality-relevant fields (`GraphicsSettings::ApplyPreset`);
  `DetectPreset()` reports the effective tier (Custom after manual tweaks).
  `RayTracingConfig` is the extension point for the planned RT shadow-mask
  pass, À-Trous denoiser and VNDF glossy reflections (SPP, denoiser
  iterations, roughness cutoff, bounce budget).
* **GPU ABI safety**: every generated GPU type (the buffers and uniform blocks
  the renderer uploads, generated from the compiled `gpu_abi.slang` by
  `utils/zshader`) stays inside `src/render/`; the types the engine also touches are
  its own domain structs (`ParticleEmitterDesc`, `LightDesc`, `FrameData`), which
  the renderer packs into the generated ones at the submit boundary. Each
  generated type is checked against that same module at compile time
  (`src/render/GpuAbi.hpp`, a renderer header beside the types
  it checks). Push blocks are the renderer's, not the engine's -- they live in
  `src/render/RenderInternal.hpp`, and each is held
  against the shader modules that read it at the point of use --
  `ExecuteHeap<Shaders::Modules::BlitPS>(...)`, `DispatchHeap<...>`,
  `DrawIndirect<...>` all name their module(s) and assert
  `Vk::PushConstantLayoutMatchesAll` inside -- so a struct that drifts from its
  `.slang` declaration cannot build, and no new pass can skip the check by
  forgetting to register it.

---

## 4. Deferred Render Graph Topology

The renderer executes a multi-pass pipeline managed by a compile-time type-checked frame graph:

```
[ ShadowPass ] ──> [ MainPass (G-Buffer) ] ──> [ DecalPass ]
                                                      │
                                                      ▼
[ TranslucentPrePass ] ──> [ GtaoPass (half-res AO) ] <──┘
         │
         ▼
[ LightingPass (Clustered/RTR) ] ──> [ ReflectionPass (SSR/RTR) ]
                                             │
                                             ▼
[ ForwardPass (Particles/Fog) ] <── [ TranslucentReflectionPass ]
         │
         ▼
[ BloomPass (Kawase Dual-Filter) ] ──> [ Anti-Aliasing (TAA/SMAA/FXAA/MLAA) ]
                                                       │
                                                       ▼
                                              [ BlitPass & ImGui / UI ] ──> [ Swapchain ]
```

* **ShadowPass**: Renders directional Cascaded Shadow Maps (CSM) in one multiview pass, then every shadow-casting point/spot light into tiles of a single 2D depth atlas (`features/ShadowAtlas.hpp`): one tile for a spot, six for a point's cube faces. Tile size falls off with distance and tiles whose light and casters did not move are reused, so a light that loses the ranking contest loses resolution rather than its shadows.
* **MainPass**: Writes primary G-Buffer channels (`SceneColor`, `Velocity`, `NormalRoughness`, `Depth`).
* **DecalPass**: Projects screen-space decals directly onto the G-Buffer before lighting.
* **GtaoPass**: Half-resolution GTAO horizon search for the AO-only GI modes (3/4), writing a single-channel R8 target that the lighting pass depth-weighted-upsamples. Sample AO / SSGI gather and spherical harmonic sky irradiance stay inline in the lighting pass (an earlier full-screen ambient pass was removed: it wrote an HDR intermediate that lighting immediately re-sampled). The occlusion factor modulates only indirect light: the lighting pass applies it to the SH/gather ambient term and passes it in the lighting target's alpha, where the reflection pass applies it to specular IBL; direct sun and point/spot terms carry their own shadow visibility instead.
* **LightingPass**: Computes direct sun lighting, clustered point/spot/area (LTC) lights, and ray-traced shadows.
* **ReflectionPass**: Evaluates Screen-Space Reflections (SSR) or Hardware Ray-Traced Reflections (RTR).
* **TranslucentPrePass & TranslucentReflectionPass**: Evaluates scene reflections for glass and refractive surfaces.
* **ForwardPass**: Draws particle emitters, volumetric fog integration, and transparent quads.
* **BloomPass**: Dual-Kawase downsampling and upsampling blur pyramid.
* **Anti-Aliasing**: Applies TAA, SMAA, FXAA, or MLAA.
* **BlitPass**: ACES tonemapping, vignette, immediate-mode UI rendering, and presentation.

---

## 5. Reflection-Driven Lua State Management (`plugins/lua/`)

* **State-Only Contract**: C++ ECS components (`std::is_trivially_copyable_v`) and reflected structs are the sole source of truth. Lua chunks run only at initialization, scene/asset recipe evaluation, file-watch hot-reload (`config/graphics.lua`), or explicit console `patch` commands — never inside a per-frame `TickUpdate`.
* **Hermetic Sandbox & Transactional Patching**: `ReflectLua` evaluates plain-text Lua in an isolated VM (never opening `ffi`/`io`/`os`/`package`/`debug`/`jit`, enforcing deterministic instruction and memory budgets), hydrating staging copies first so invalid fields or types roll back without mutating live engine state.

---

## 6. Asset Cooking & Virtual File System (VFS)

1. **Graph Generation**: `zcook ninja` scans the asset root and writes `assets.ninja` -- the graph of its own invocations. The cooker generates the plan it is about to execute, for the same reason it reads the manifest it cooks from: a rule and the subcommand it names cannot drift when one program owns both. The graph regenerates itself when a source file, an exported manifest, or zcook itself changes.
2. **Source Models**: Blender `.blend` files in `./blender/` are scanned, and `scripts/export_metadata.py` -- run inside Blender by `scripts/run_blender.py`, because only Blender's Python can open a `.blend` -- writes the level's manifest.
3. **Intermediate Extraction**: Uncompressed binary metadata (`.bin`) and textures are emitted into `resources/intermediate/<level>/`.
4. **Ninja Parallel Compilation**: `zcook` compiles meshes (`.zmesh`), animations (`.zanim`), and textures (`.ztex`) in parallel. The virtual-path to cooked-file map is `build_assets/manifest.txt`, written by the generator and read only by `zcook pak`.
5. **Archive Packing**: `zcook pak` packs all cooked targets into `data/base.pak` (Zstandard compressed archive).
6. **VFS Loading**: `CreativeWorksManager` mounts `.pak` files and streams assets via memory-mapped IO and fiber tasks.

---

## 7. Three.js / TypeScript Porting Reference Guide

When porting prototype gameplay or math logic from a **TypeScript + Three.js + React** codebase into Zahlen:

| Property | Three.js (TS / React) | Zahlen Engine (C++) | Porting Action |
| :--- | :--- | :--- | :--- |
| **World Coordinate System** | Right-Handed, $+Y$ Up | Right-Handed, $+Y$ Up | **Direct 1:1 Mapping** |
| **Forward Vector** | $-Z$ | $-Z$ | **Direct 1:1 Mapping** |
| **Matrix Storage Layout** | Column-Major (`Matrix4`) | Column-Major (`JPH::Mat44` / Slang) | **Direct 1:1 Mapping** |
| **Matrix Vector Multiplication** | $M \cdot v$ (`v.applyMatrix4(m)`) | $M \cdot v$ (`m * v` / `mul(m, v)`) | **Direct 1:1 Mapping** |
| **Winding Order** | Counter-Clockwise (CCW) | Counter-Clockwise (CCW) | **Direct 1:1 Mapping** |
| **Box Geometry Sizes** | Full-Extents $(W, H, D)$ | **Half-Extents** $(X, Y, Z)$ | ⚠️ **Divide dimensions by 2** |
| **Clip Depth Range** | $[-1, 1]$ (WebGL) | $[0, 1]$ (Vulkan) | ⚠️ **Use `Math::CreatePerspective`** |
| **Euler Rotation Order** | Default: 'XYZ' | Default: 'YXZ' (Yaw, Pitch, Roll) | Use `MathUtils::EulerYXZ` or `EulerXYZ` |

## 8. Immediate-mode GUI (`Zahlen/GUI/GUI.hpp`)

ImGui stays for debug overlays. In-engine UI is Clay immediate-mode: a
persistent `GUI::Context` owns its Clay layout arena and widget interaction
state directly (`std::unique_ptr<Impl>`), `BeginFrame(const UIInput&)` mints
an ephemeral move-only `GUI::Frame` capability token that gates boxes, text,
buttons, sliders and dropdowns, and `std::move(frame).End()` returns the
frame's `UIDrawData` — spans of `UIBatch` / `VertexPosition` /
`VertexSurface` the host hands back through `FrameScope::RenderUI(UIView, UIDrawData)`.
`RenderContext` is not a GUI interface and knows nothing about `GUI::Context`;
it forwards the payload to the renderer-private `UIRenderer`. The UI shader
does not import `common` and does not bind GlobalSceneRegistry. A host that
builds its UI in the UI phase (before the frame is open) drains per-frame input
with `Engine::ConsumeUIInput(dt)` and banks the `UIDrawData` view (batches,
vertices, and mouse/keyboard capture flags) by value with
`Engine::SubmitUI(std::move(ui).End())`, which `RenderSystem` composes over the
finished scene in the same frame.

```cpp
// Persistent GUI::Context owned by the host session:
gui.SetFontAtlas(engine.GetFontAtlas());
GUI::Frame ui = gui.BeginFrame(engine.ConsumeUIInput(dt));
ui.Box("Panel", cfg, [&]() {
    ui.Text("Hello", 16.0f);
    if (ui.Button("Reload")) { ... }
});
engine.SubmitUI(std::move(ui).End());   // drawn by RenderSystem
```

A host that owns the frame outright (the UI-tree editor) calls `RenderUI`
itself:

```cpp
auto& rc = kernel.GetRenderContext();
auto began = rc.BeginFrame();
FrameScope gpuFrame = std::move(**began);
const auto target = kernel.AcquireTarget(window); // the kernel owns the presentation seam
if (!target) { ... }                              // acquisition error
if (!*target) { ... }                             // no drawable image this frame
const auto ui = gpuFrame.RenderUI(UIView {.viewport = ..., .target = **target}, std::move(uiFrame).End());
if (!ui) { ... }                                  // hard failure (propagate it)
else if (ui->has_value()) { ... }                 // FrameSkipped: nowhere drawable this frame
std::move(gpuFrame).End();
```

`Kernel::AcquireTarget` (delegated by `Engine`; no argument means the session's
own window) is the verb that takes the frame's image for a window and opens the
command stream that window's passes record into. `GetAcquiredTarget` is the
query beside it: it answers only for an open frame and never acquires an image.
The returned `FrameTarget` identifies the exact renderer, frame, window and
acquisition. It is a non-owning value: after `EndFrame`, a window release or
rebuild, or renderer destruction, rendering through it fails instead of
adopting whatever image has since reused the slot. To render into a persistent
render texture, create a `RenderTextureHandle` and use
`frameTarget.ForTexture(renderTexture)`: the output still belongs to the
explicitly acquired frame's command stream. Window presenters persist; their
acquired images, layout and written state live only with the frame. The two
are not stored together in a table of versioned texture handles.

`Engine` owns the device-scoped SDF `FontAtlas` (`GetFontAtlas` / `EnsureFontAtlas`),
rebuilding it automatically across GPU device loss and feeding it as a plain
asset value into `GUI::Context`. Immediate-mode widget emission is gated behind
the move-only `GUI::Frame` capability token returned by
`GUI::Context::BeginFrame(const UIInput&)`, so `zahlen_gui` has zero binary or
header coupling to `Engine`, `ECS::Registry`, or `PlatformHost`. Core never
walks a private UI parent link: `DespawnEntity` follows
`Components::HierarchyComponent` only.

A document cannot store a C++ callback or a `float&`, so a layout that will
later load from TOML is a `GUI::UINode` tree: `kind` / `label` /
`onClickAction` / `bindProperty`, no function pointers. `RenderUITree`
walks it into `Context` calls and looks actions up in a host-owned
`ActionRegistry` (`"editor.save_scene"` → the function that runs) and
bound values in a `PropertyStore`. Preview mode invokes; Design mode
records the clicked node id (including empty Box/Row/Column hits) instead
so a builder click cannot fire Save, and tints `selectedId`.
`FindNodeById` / `InsertChild` / `RemoveNodeById` turn that string into a
live node. The tree is format-free — `plugins/toml/UITOML.hpp` walks it
the same way `SceneTOML.hpp` walks `Scene::Scene`.

### Optional layer: the native editor

The native world editor (Hierarchy + Inspector) implementation is
`extensions/editor/` (`zahlen_editor`, a library). Its dedicated executable
is `zahlen_world_editor` (output `zahlen-editor`) at `utils/editor/main.cpp`;
it links FreeCam and scene serialization without imposing those tool concerns
on `app/main.cpp` or the game binary. The scene host (`zahlen-scene-host`)
and headless benchmark (`zahlen-benchmark`) are separate executables, too.
`zahlen --editor` and `zahlen --benchmark` are unknown game flags, not host
modes; run `zahlen-editor` or `zahlen-benchmark` directly. Tool-specific flags
(such as `ztest --benchmark`) remain independent.

The v0.1 UI-tree editor is a second composition-root binary, `zahlen_ui_editor`
(`app/UIEditor.cpp`), built only when optional layers are enabled (the document it edits,
`GUI::UINode`, is the `extensions/UI/` schema): left Hierarchy of `UINode` ids, centre canvas
`RenderUITree(..., TreeMode::Design)`, right Inspector on
`FindNodeById(tree, selectedId)`. It has no `Engine`, World or ECS: it configures
`KernelApp` with input/update/render callbacks and an atlas-rebuild callback.
`zahlen_host_runner` shares `ProcessScope` and `FramePacer` with games, while
`KernelApp` owns Kernel creation, event polling, frame acquisition/presentation,
device-loss recovery and teardown. The atlas rebuild runs only after Kernel has
created the replacement renderer and cleared its GPU caches. Preview is a second
OS window owned by the same Kernel (`Kernel::AddWindow`) and drawn by the editor:
`gpuFrame.RenderUI` into `kernel.AcquireTarget(previewWindow)`; the single
`FrameScope::End` presents every window touched in that frame. Closing the preview
leaves the editor running.

Engine-based hosts can also add a window; their 3D `BlitPrimary` passes mirror the
resolved output, and a `RenderScene` call targeting a second window re-executes
the graph for that window. The Kernel-only UI editor does neither.
G / S / R on the canvas grab, scale and rotate the selection with pixel /
15° snap; inspector sliders snap to whole pixels so layout is not float soup.

---

## 9. Runtime Directories & Distribution

The engine used to answer "where do I read/write this?" with a path relative to
the working directory: the pipeline cache at `build/cache/pipeline_cache.bin`,
the asset pack at `build/data/base.pak`, a vendor crash dump at
`gpu_crash_dump.bin`. That is correct for exactly one launch -- the one CMake
performs, since every target runs with `WORKING_DIRECTORY` set to the source
root -- and wrong for every other. Launched from Finder the working directory is
`/`, so the cache write fails and every run recompiles every pipeline; launched
from a folder the user picked, a stray `build/` tree appears there.

`src/engine/RuntimePaths.{hpp,cpp}` is now the one place that answers it, and
it separates two regimes:

| | Dev tree | Anywhere else |
| :--- | :--- | :--- |
| **Recognized by** | the working directory is the source root, or the executable lives under `<source root>/build` | a distributed or hand-launched copy |
| **Pipeline cache** | `<source root>/build/cache/pipeline_cache.bin` | macOS `~/Library/Caches/Zahlen/`, Linux `$XDG_CACHE_HOME/zahlen/` (else `~/.cache/zahlen/`), Windows `%LOCALAPPDATA%\Zahlen\Cache\` |
| **GPU crash dumps** | `<source root>/build/cache/gpu_crash_dump.bin` | the same per-user directory as the cache |
| **`data/base.pak`** | the working directory, then `<source root>/build/data/` | `$ZHLN_DATA_DIR`, then next to the executable (a bundle's `Contents/Resources` first), then the working directory and `build/` |

`ZHLN_CACHE_DIR` and `ZHLN_DATA_DIR` override the choice in either regime. Data
lookup is first-hit-wins, and the two `build/` probes are the last ones, so a dev
tree resolves exactly what it always did.

This is policy, so it stays private to the layer that owns the process: it has
no installed header, no umbrella entry and no `ZHLN_API`, and no other layer
includes it. Pipeline binaries cross into the renderer as data, never as a
path: the kernel reads `pipeline_cache.bin` itself and hands the bytes in via
`EngineConfig::initialPipelineBinaryData`, then extracts the updated set
with `RenderContext::GetPipelineBinaryData()` before teardown and writes it
back, so the RHI works unchanged on hosts with no file system at all. There
is no `VkPipelineCache` anywhere in the renderer: `VK_KHR_pipeline_binary`
supplies per-pipeline blobs keyed by the driver's own pipeline keys, held in
`Vk::PipelineBinaryStore` and replayed or captured inside the pipeline
builders; hosts without the extension compile from scratch. A caller
that supplies its own cache bytes keeps persistence for itself; only when it
supplies none does the kernel fill in the on-disk cache and take on writing it
back. `EngineConfig::crashDumpPath` is still a path, forwarded into
`Vk::DiagnosticConfig::crashDumpPath`; an empty path is what the engine fills
in, and a path set by the caller is used as given.

Caches are regenerable by definition: a missing or foreign cache costs compile
time, never correctness -- `Vk::MatchesDevice` discards a store recorded on
another driver or device (the global pipeline key check in `PipelineBinaries.cpp`), and an
unwritable directory costs one log line.

`ZHLN_PROJECT_ROOT` (`Config.hpp`) stays a *compile-time source path*. It is only
used to recognize the tree a developer is running from; a distributed binary
fails that test on the receiving machine, because the executable is not under the
builder's `build/` directory, so it never consults a build tree that is not
there.
