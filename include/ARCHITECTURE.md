# Zahlen Engine Architecture

This document provides a technical overview of Project Zahlen's architecture, mathematical conventions, frame loop execution order, deferred render graph topology, scripting IPC protocol, asset pipeline, and Three.js porting guidelines.

---

## 1. Core Principles

* **C++26 Static Reflection (`std::meta`)**: Eliminates manual binding glue code. ECS components, reflection metadata, JSON serialization, and scripting bindings are reflected automatically at compile-time.
* **Data-Oriented & Lock-Free**: Custom, page-aligned, lock-free/atomic data structures (`ZHLN::Array`, `HashMap`, `SkipList`, `MemoryPool`) eliminate runtime heap allocations.
* **PIMPL Encapsulation**: Public APIs (`RenderContext`, `PhysicsContext`, `Window`) hide internal Vulkan and Jolt headers behind opaque implementation pointers.
* **Fiber Task Scheduler**: Cooperative, multi-threaded stackful fibers (`ZHLN::TaskSystem`) drive parallel system updates and worker thread GPU command recording.

---

## 1.1 Strict ECS Mandate & Architectural Constraints

To preserve the engine's data-oriented design (DOD), cache locality, zero-allocation memory guarantees, and C++26 hot-reloadability, **ALL state in Zahlen MUST reside in ECS Components, and ALL logic MUST reside in ECS Systems.**

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

#### 4. Automated Component Resource Cleanup
* Component GPU allocations (VBOs, IBOs, Textures) MUST be released via the `static void OnDestroy(Component* c)` hook declared on the Component struct.
* Systems MUST NOT manually manage raw heap pointers or manage class destructors.

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

    BufferHandle vboPos  = BufferHandle::Invalid;
    BufferHandle vboAttr = BufferHandle::Invalid;

    // Automated RAII cleanup on entity destruction
    static void OnDestroy(LightningComponent* c) noexcept {
        if (auto* engine = GetEngineContext()) {
            engine->GetRenderContext().DestroyBuffer(c->vboPos);
            engine->GetRenderContext().DestroyBuffer(c->vboAttr);
        }
    }
};

// GOOD: Stateless System Function
namespace ZHLN::Lightning {
Entity Spawn(Engine& engine, JPH::RVec3Arg cloudPos, JPH::RVec3Arg groundPos, const LightningConfig& cfg);
void   Update(Engine& engine, float dt);
} // namespace ZHLN::Lightning
```

---

### Architectural Rationale

| Requirement | Why OOP Fails | Why ECS Succeeds |
| :--- | :--- | :--- |
| **Hot-Reloading** | Reloading `.so`/`.dll` modules invalidates class vtables and member offsets, crashing active class instances. | Components reside in C++ host memory. Hot-reloaded code modules simply re-attach to existing Component arrays seamlessly. |
| **Cache Locality** | Heap-allocated objects (`new MyClass()`) scatter data across RAM pages, causing CPU L1/L2 cache misses. | `SparseSet` arrays store components contiguously in RAM, allowing SIMD vectorization and prefetching. |
| **Parallel Execution** | Mutable class methods introduce thread races when accessed concurrently by multiple workers. | `SystemGraph` inspects Component Read/Write access patterns (`Read<T>()`, `Write<T>()`) to execute systems in parallel on fibers safely. |
| **State Save/Load** | Private class members cannot be serialized without custom, error-prone boilerplate. | Reflection (`std::meta`) automatically serializes all Component POD structs to disk or network instantly. |


---

## 1.2 The Core / Extras Dependency Boundary

`src/`, `include/` and `modules/` are the **Core Engine**. `extras/` is the
optional feature layer built on top of it.

> **Rule: the dependency is one-way.** Core must never include, import or link
> anything from `extras/`. `extras/` may consume Core freely.

`tools/check_core_extras_boundary.py` runs at CMake configure time and fails the
build on a violation, so the rule is enforced rather than documented. It catches
both `import ZHLN.<extras module>;` and any `#include` that resolves to a file
under `extras/` — including the short forms, because `extras/` is itself an
include root for consumers of `zahlen_extras`, so `#include <json/JSON.hpp>`
compiles happily from a core file and has to be rejected by path resolution, not
by spelling. What the script cannot see is linking: keep `zahlen_extras` out of
every target defined outside `extras/` and `tests/`.

### What the boundary buys

Anything behind it is genuinely optional — its third-party dependencies
included. The concrete case that motivated the rule:

| Layer | Contents | Dependencies it carries |
| :--- | :--- | :--- |
| `extras/json/` | `JSON.hpp` (reflection-driven reader + `Reflect::SerializeJSON`), `JSONSchema.hpp` (compile-time schema → C++ type) | simdjson |
| `extras/toml/` | `TOML.hpp` (reflection-driven documents), `SceneTOML.hpp` (binds a core `Scene::Scene` to the document format) | none |
| `extras/glTF/` | `GLTFImporter.*` (the glTF/GLB reader), `glTF.*` (the drop-a-file inspector, module `ZHLN.glTF`) | cgltf, stb_image, meshoptimizer, and `extras/json` for the custom node members |

Core has no JSON, TOML or model-file dependency at all, so a core-only build
(`-DZHLN_BUILD_EXTRAS=OFF`) needs none of those installed and links no parser.

### Consequences worth knowing

* **`Zahlen/Scene.hpp` is pure data.** The structs, their defaults and
  `Scene::Instantiate(engine, scene)` are Core. Turning a scene into text and
  back is `extras/toml/SceneTOML.hpp`, which also holds the
  `ReflectTOML::TOMLVector<JPH::Float3>` specialisations that make a Jolt vector
  read as `[x, y, z]`. Include *that* header — not `toml/TOML.hpp` alone — or a
  scene serialises its vectors as tables of members.
* **`DefaultPreset` does not parse anything.** The engine's fallback scene is the
  one scene that has to work when nothing else loaded, so it is a compiled-in
  `ZHLN::Scene::Scene` handed to `Scene::Instantiate()` rather than a baked-in
  document parsed at runtime. A mistake in it fails the build instead of
  surfacing on the day the game already failed to boot.

* **The glTF importer is an extra, and Core never calls it.** Reading a model
  file means a container parser, an image decoder, a mesh partitioner and a JSON
  reader for the custom node members — far more machinery than the engine needs
  to run, so `extras/glTF/GLTFImporter.cpp` sits behind the boundary and uses
  the real `extras/json` parser rather than a bespoke scanner. What it produces
  is a plain `ZHLN::ModelPrefab` — the same struct the ECS already describes —
  which it leaves in `CreativeWorksManager`'s prefab cache under
  `HashCreativeWorkPath(path)`. `CreativeWorksFactory::LoadModelPrefab(path)`,
  the entry point `Scene::ShapeKind::Prefab` and the scripting bindings use, is
  that cache lookup and nothing else:

  ```cpp
  // the extra: parse, upload, cache
  auto* prefab = ZHLN::GLTF::LoadGLBPrefab(ctx, cwMgr, "Crate.glb");

  // core only: read the struct back out of the cache and spawn it
  ZHLN::CreativeWorksFactory::InstantiatePrefab(engine, "Crate.glb", params);
  ```

  There is no function table and no registration step. The importer writes the
  cache, Core reads it, and nothing has to be installed first. In a core-only
  build nothing ever fills the cache, so the lookup returns null — a core-only
  build simply has no model files, the same way it has no JSON.
* **Device loss is the case where a callback is the right shape.** The GPU
  handles inside a `ModelPrefab` die with the `VkDevice`, and getting them back
  means reading the `.glb` again — an action only the importer can perform, and
  one Core cannot reach by inspecting state it already owns. So `Engine` keeps a
  list of `DeviceLostCallback`s and runs it, in registration order, once
  `CreativeWorksFactory::RebuildVulkanResources()` has rebuilt what Core owns:

  ```cpp
  ZHLN::GLTF::InstallDeviceLostHandler(*engine);   // once, next to the first import
  ```

  The list lives on `Engine` rather than on `RenderContext` because the context
  is destroyed and rebuilt inside `HandleDeviceLost()`; anything stored on it
  would die with the device it is meant to survive. `ZHLN::glTF::Initialize()`
  installs the handler for the inspector, and an application that imports models
  directly calls it once itself. `Engine::DeviceLostCallbackCount()` lets a host
  assert that the owners it expects actually subscribed.

That is the whole distinction, and it is worth stating precisely because the two
cases look alike. **When Core needs *data* an extra produces, the extra writes
ordinary Core state and Core reads it back** — a prefab cache, a `Scene::Scene`,
a `ModelPrefab` — with nothing to register and no way for the seam to be
forgotten. **When Core needs an extra to *act*, because the work requires
knowledge Core does not have, the extra subscribes to a notification.** The
first needs no callback; the second cannot work without one. Neither points the
dependency arrow the wrong way, and the build still works with the extra absent
— a callback that was never registered is simply never called.

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

Each frame executes in a strict, deterministic sequence:

```
[ ProcessEvents ] ──> [ Physics System (60Hz Jolt Step) ] ──> [ Physics State Write-Back ]
                                                                        │
                                                                        ▼
[ Render System ] <── [ ECS Update Graph ] <── [ Gameplay Update ] <── [ Visual Interpolation ]
```

1. **Input & OS Events**: `ProcessEvents()` pumps OS/window events and updates raw mouse/keyboard states.
2. **Physics Simulation Step**: `PhysicsSystem::Update()` steps Jolt Physics at a semi-fixed 60 Hz timestep (`1/60s`).
3. **Physics State Write-Back**: `PhysicsStateSystem::WriteBack()` writes new Jolt rigid body poses into double-buffered `PhysicsStateComponent` history structures.
4. **Visual Interpolation**: `VisualInterpolationSystem::Update()` interpolates between previous and current physics transforms based on the remaining frame remainder (`alpha = accumulator / targetDt`).
5. **Gameplay Scripting Update**: The active gameplay driver (Fennel/Lua or Native C++ `.so`/`.dll`) executes script update ticks.
6. **ECS System Graph**: `SystemGraph::Execute()` runs parallel engine systems (Animation, Articulation, Transforms, Audio, Interaction).
7. **Render Graph Execution**:
   * `CullingSystem`: Performs frustum culling on main and shadow viewports.
   * `LightingSystem`: Gathers active light sources and updates light cluster volumes.
   * `RenderSystem`: Records multi-pass Vulkan commands and presents to the swapchain.

### 3.1 Graphics Settings Flow

Graphics configuration flows in **one direction** through a single canonical model
(`include/Zahlen/GraphicsSettings.hpp`):

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
RenderContext state (FrameUniforms & ScenePassPushConstants assembly,
  pipeline-variant selection, reactive GPU target resizes)
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
* **GPU ABI safety**: the per-pass push blob is mirrored by
  `GPUTypes::Heap::ScenePassPushConstants` (C++ alias of the renderer's
  `PPPushConstants`), size-checked against the compiled `gpu_abi` SPIR-V by
  `ValidateSlangTypeLayouts()` at startup together with every other GPU type.

---

## 4. Deferred Render Graph Topology

The renderer executes a multi-pass pipeline managed by a compile-time type-checked frame graph:

```
[ ShadowPass ] ──> [ MainPass (G-Buffer) ] ──> [ DecalPass ]
                                                      │
                                                      ▼
[ TranslucentPrePass ] <── [ AmbientPass (AO/GI) ] <──┘
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

* **ShadowPass**: Renders directional Cascaded Shadow Maps (CSM) and punctual light shadow atlases.
* **MainPass**: Writes primary G-Buffer channels (`SceneColor`, `Velocity`, `NormalRoughness`, `Depth`).
* **DecalPass**: Projects screen-space decals directly onto the G-Buffer before lighting.
* **AmbientPass**: Calculates SSAO/HBAO/GTAO or SSGI and spherical harmonic sky irradiance.
* **LightingPass**: Computes direct sun lighting, clustered point/spot/area (LTC) lights, and ray-traced shadows.
* **ReflectionPass**: Evaluates Screen-Space Reflections (SSR) or Hardware Ray-Traced Reflections (RTR).
* **TranslucentPrePass & TranslucentReflectionPass**: Evaluates scene reflections for glass and refractive surfaces.
* **ForwardPass**: Draws particle emitters, volumetric fog integration, and transparent quads.
* **BloomPass**: Dual-Kawase downsampling and upsampling blur pyramid.
* **Anti-Aliasing**: Applies TAA, SMAA, FXAA, or MLAA.
* **BlitPass**: ACES tonemapping, vignette, immediate-mode UI rendering, and presentation.

---

## 5. C++ <-> Scripting FFI & Zero-Copy Buffer Protocol

* **IPC Command Dispatch**: Scripting languages (Fennel/LuaJIT) communicate with the C++ core via `ZHLN_GetCommandID` and `ZHLN_DispatchCommand` using integer jump-table IDs.
* **Zero-Copy Memory Protocol**: Scripts query native memory layouts through `ZHLN_BufferView`. A `BufferSync` atomic counter (`shadowLock`) locks C++ vector reallocations while raw FFI pointers are held in Lua land.

---

## 6. Asset Cooking & Virtual File System (VFS)

1. **Source Models**: Blender `.blend` files in `./blender/` are scanned by `tools/export_metadata.py`.
2. **Intermediate Extraction**: Uncompressed binary metadata (`.bin`) and textures are emitted into `resources/intermediate/`.
3. **Ninja Parallel Compilation**: `zcook` compiles meshes (`.zmesh`), animations (`.zanim`), and textures (`.ztex`) in parallel.
4. **Archive Packing**: `zcook pak` packs all cooked targets into `data/base.pak` (Zstandard compressed archive).
5. **VFS Loading**: `CreativeWorksManager` mounts `.pak` files and streams assets via memory-mapped IO and fiber tasks.

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
