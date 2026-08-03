# Zahlen Engine Architecture

This document provides a technical overview of Project Zahlen's architecture, mathematical conventions, frame loop execution order, deferred render graph topology, scripting IPC protocol, asset pipeline, and Three.js porting guidelines.

---

## 1. Core Principles

* **C++26 Static Reflection (`std::meta`)**: Eliminates manual binding glue code. ECS components, reflection metadata, JSON serialization, and scripting bindings are reflected automatically at compile-time.
* **Data-Oriented & Lock-Free**: Custom, page-aligned, lock-free/atomic data structures (`ZHLN::Array`, `HashMap`, `SkipList`, `MemoryPool`) eliminate runtime heap allocations.
* **PIMPL Encapsulation**: Public APIs (`RenderContext`, `PhysicsContext`, `Window`) hide internal Vulkan and Jolt headers behind opaque implementation pointers.
* **Fiber Task Scheduler**: Cooperative, multi-threaded stackful fibers (`ZHLN::TaskSystem`) drive parallel system updates and worker thread GPU command recording.

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
* **Storage Layout**: **Column-Major** in both C++ (`JPH::Mat44`) and HLSL (`#pragma pack_matrix(column_major)`).
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
| **Matrix Storage Layout** | Column-Major (`Matrix4`) | Column-Major (`JPH::Mat44` / HLSL) | **Direct 1:1 Mapping** |
| **Matrix Vector Multiplication** | $M \cdot v$ (`v.applyMatrix4(m)`) | $M \cdot v$ (`m * v` / `mul(m, v)`) | **Direct 1:1 Mapping** |
| **Winding Order** | Counter-Clockwise (CCW) | Counter-Clockwise (CCW) | **Direct 1:1 Mapping** |
| **Box Geometry Sizes** | Full-Extents $(W, H, D)$ | **Half-Extents** $(X, Y, Z)$ | ⚠️ **Divide dimensions by 2** |
| **Clip Depth Range** | $[-1, 1]$ (WebGL) | $[0, 1]$ (Vulkan) | ⚠️ **Use `Math::CreatePerspective`** |
| **Euler Rotation Order** | Default: 'XYZ' | Default: 'YXZ' (Yaw, Pitch, Roll) | Use `MathUtils::EulerYXZ` or `EulerXYZ` |
