# VK_EXT_mesh_shader Integration

Project Zahlen can rasterise scene geometry with **task + mesh shaders**
(`VK_EXT_mesh_shader`) instead of the fixed-function input assembler and vertex
stage. The feature is *additive*: every material still gets its classic vertex
pipeline, and the renderer picks a path per draw call, so unsupported hardware,
skinned meshes, debug line lists and CSG stencil draws keep working unchanged.

---

## 1. End-to-end data flow

```
 zcook / GLTFImporter          Types.hpp / InstanceData        GPU
 ─────────────────────         ────────────────────────        ───
 meshoptimizer                 meshletAddress ────────────► GPUMeshlet[]   (64B)
   buildMeshlets       ──►     meshletVertexAddress ──────► uint32_t[]     (unique verts)
   computeMeshletBounds        meshletTriAddress ─────────► uint8_t[]      (micro indices)
   optimizeMeshlet             meshletCount

 basic_task.slang  : 32 meshlets/workgroup → frustum + normal-cone cull → payload
 basic_mesh.slang  : 1 meshlet/workgroup   → 64 verts, ≤124 prims → PSMain/PSShadow
```

Meshlet streams are an *additional index view* over the existing vertex pool.
The raw position/attribute/index buffers are still uploaded and still used by
BLAS builds (`ZHLN_CmdBuildBlas`) and by the vertex pipeline.

### Partitioning parameters (`include/Zahlen/Meshlet.hpp`)

| Parameter | Value | Why |
| --- | --- | --- |
| `kMeshletMaxVertices` | 64 | one output vertex per mesh-shader thread |
| `kMeshletMaxTriangles` | 124 | multiple of 4 (meshoptimizer), 2 prims/thread |
| `kMeshletConeWeight` | 0.5 | balances cluster locality against cone tightness |
| `kMeshletsPerTaskGroup` | 32 | payload slots per task workgroup |

`BuildMeshlets()` is shared by the offline cooker and the runtime glTF importer
so both emit byte-identical streams. It also **re-aligns every meshlet's
micro-index run to 4 bytes**: meshoptimizer packs those runs back to back, but
the mesh shader loads them as 32-bit words (no 8-bit storage requirement).

### `.zmesh` format

Header bumped to **version 4**: three counts appended
(`meshletCount`, `meshletVertexCount`, `meshletTriByteCount`), and the three
streams are written after the index stream. `sizeof(CookedMeshHeader)` is now
56 bytes (was 44).

---

## 2. Device enablement

* Extension: `VK_EXT_mesh_shader` — requested **optionally**
  (`RenderInit.cpp: GetDeviceExtensions`), gated on `CheckMeshShaderSupport()`.
* Features: `taskShader`, `meshShader`, `multiviewMeshShader`
  (`VkPhysicalDeviceMeshShaderFeaturesEXT`, chained only when supported).
* Limits: `ZHLN_QueryMeshShaderLimits()` +
  `ZHLN_MeshShaderLimitsSufficient()` demand
  `maxMeshOutputVertices ≥ 64`, `maxMeshOutputPrimitives ≥ 124`,
  `maxTaskWorkGroupInvocations ≥ 32`, `maxMeshWorkGroupInvocations ≥ 64`
  — exactly the budget baked into the shaders.
* Entry points (`vkCmdDrawMeshTasksEXT`, `…IndirectEXT`, `…IndirectCountEXT`)
  are resolved once in `ZHLN_CreateDevice` and stored on `ZHLN_Device`;
  `ZHLN::Vk::Context` forwards to them, and `Context::MeshShadersSupported()`
  is the single source of truth.

`ZHLN_NO_MESH_SHADING=1` forces the vertex path at runtime (mirrors
`ZHLN_NO_GPU_CULLING`), which makes A/B comparison and driver-bug bisection a
one-liner.

### Two traps in the enablement path

1. **Truncated extension enumeration.** `IsDeviceExtensionSupported()` used to
   read the device extension list into a fixed `std::array<..., 128>` and clamp
   the count, hiding every extension the driver reported past index 127.
   Desktop drivers expose far more than that (NVIDIA: >200), and because the
   list is roughly `VK_KHR_*` before `VK_EXT_*`, the KHR ray-tracing trio
   survived the cut while `VK_EXT_mesh_shader` did not — mesh shading looked
   unsupported on hardware that fully supports it, while
   `VK_EXT_descriptor_heap` kept working because `ExtensionBuilder::ForDevice()`
   enumerates into a growable vector. Both probes now enumerate the full list
   (`EnumerateDeviceExtensions` / `EnumerateInstanceExtensions`, with a
   `VK_INCOMPLETE` retry loop), and the same clamp was removed from the C-side
   instance-extension filter.
2. **All-or-nothing optional features.** `FeatureChain::Optional<T>` drops the
   *entire* feature struct if any single requested `VkBool32` is unsupported.
   Requesting `multiviewMeshShader` unconditionally would therefore have
   silently disabled `taskShader`/`meshShader` on a device missing only the
   multiview bit — enabled extension, no mesh shading, and a validation error
   at pipeline creation instead of a clear message. It is now probed
   separately and requested only when present.

Failure diagnostics are per-gate: the log distinguishes "extension not
reported" (and prints how many extensions *were* reported, so a suspiciously
round number reveals a truncation regression) from "features not advertised",
"entry points did not resolve" and "limits below the meshlet budget".

---

## 3. Pipelines

`ZHLN_ShaderStages` grew `task` and `mesh` modules.
`ZHLN_PopulateShaderStageInfos` emits **task+mesh instead of vertex** when a
mesh module is present (a pipeline may not declare both), and chains the same
`VkShaderDescriptorSetAndBindingMappingInfoEXT` (`vs_mapping`) into their
`pNext`, because task/mesh consume the identical `scene` parameter block.
`ZHLN_CreateGraphicsPipeline` passes `pVertexInputState`/`pInputAssemblyState`
as `NULL` for mesh pipelines; the descriptor-heap flag path is untouched.

`SlangReflectedLayout::Build` now folds `VK_SHADER_STAGE_TASK_BIT_EXT` and
`VK_SHADER_STAGE_MESH_BIT_EXT` into the reflected bindless layout.

Every `NativeMaterial` may hold a second pipeline (`meshPipeline`) built from
`basic_task` + `basic_mesh` + the material's fragment shader; the shadow pass
gets its own `shadowMeshPipeline` (depth-only, `PSShadow`).

---

## 4. Shaders

* `common.slang` — `GPUMeshlet`, the `InstanceData` meshlet tail, BDA accessors
  (`fetchMeshlet`, `fetchMeshletVertex`, `fetchMeshletTriangle`),
  `SphereFrustumVisible`, `MatrixMaxScale`, `ConeBackfaceCulled`, and the shared
  geometry→fragment interface `SceneGeometryOutput` (aliased to `VSOutput` in
  `basic.slang`, so vertex and mesh paths cannot drift apart).
* `basic_task.slang` — 32 threads, one meshlet each: sphere-vs-frustum, then
  meshoptimizer normal-cone rejection, then compaction and `DispatchMesh`.
  Compaction uses a **groupshared atomic**, not `WavePrefixCountBits`: the
  subgroup width is 8/16/32/64 depending on the vendor, and wave-relative
  prefix sums would hand out overlapping payload slots on anything but wave32.
  Cluster culling is skipped for skinned/morphed instances, whose baked bounds
  no longer describe the deformed geometry.
* `basic_mesh.slang` — 64 threads: cooperative vertex fetch/transform (a
  verbatim port of `VSMain`, including skinning, morph targets, viewmodel and
  shadow-cascade matrices) plus micro-index assembly, 2 primitives per thread.

---

## 5. Draw submission

`CommandEncoder` gained `DrawMeshTasks`, `DrawMeshTasksIndirect` and
`DrawMeshTasksIndirectCount` (heap-mode push data at offset 0, exactly like the
vertex path). `SubmitDrawInstanced` in `RenderPasses.cpp` transparently routes a
draw to `DrawMeshTasks` when the material has a mesh pipeline and the instance
has meshlets, dispatching `ceil(meshletCount / 32)` task workgroups.

### Interaction with two-phase GPU culling

`VkDrawMeshTasksIndirectCommandEXT` has **no `firstInstance` field**, while the
whole indirect path encodes the instance id there (`kGpuCullingSentinel`). So
while mesh shading is active, `MainPass1`/`MainPass2` take the per-draw
recording policy and instance-level culling moves into the task shader
(per-cluster frustum + normal cone). The predicate is duplicated in
`RenderGraphBuilder.cpp` — it selects the command-buffer topology those passes
record into and **must stay identical**.

The shadow pass keeps its CPU-side cascade visibility list and replays it as
direct dispatches, re-reading the instance ids from the (host-visible) indirect
buffer it just filled.

---

## 6. What is *not* on the mesh path

| Case | Path | Reason |
| --- | --- | --- |
| GPU-skinned draws | vertex | meshlet vertex indices address the pre-skinning pool |
| Debug lines | vertex | `LINE_LIST`; mesh pipelines declare their own topology |
| CSG stencil draws | vertex | they bind an explicit pipeline override |
| Particles / decals / post | vertex | not meshletized |
| Hi-Z occlusion culling | disabled while mesh shading | see the `firstInstance` note above |

---

## 7. Verification status

Verified in this repository:

- [x] Meshlet cooking: single-triangle and 64×64 grid cases — no primitive is
      lost or duplicated, every micro-index resolves inside its meshlet, every
      resolved position lies inside the meshlet bounding sphere, every
      `triangleOffset` is 4-byte aligned, degenerate input returns empty.
- [x] ABI: `sizeof(GPUMeshlet) == 64`, `sizeof(InstanceData) == 304`
      (`meshletAddress` at 272, `meshletCount` at 296), `InstanceData` stays
      trivially copyable *and* trivially constructible (`DrawCommand` asserts it).
- [x] Shader compilation: every Slang entry point in the project still compiles,
      including the new `TaskMain`/`MeshMain`.
- [x] SPIR-V: `TaskMain` emits `ExecutionModel TaskEXT` with `LocalSize 32,1,1`;
      `MeshMain` emits `MeshEXT`, `OutputVertices 64`, `OutputPrimitivesEXT 124`,
      `OutputTrianglesEXT`, `LocalSize 64,1,1`, capability `MeshShadingEXT`.
- [x] Binding parity: `TaskMain`/`MeshMain` decorate the `scene` resources with
      the *same* set/binding pairs as `VSMain` (set 0, bindings 1/3/4/5/6), so
      the heap mapping table reflected from `basic.slang` applies verbatim.

Still requires a GPU (cannot be checked without a Vulkan device):

- [ ] Validation layers clean with `taskShader`/`meshShader` enabled.
- [ ] RenderDoc capture shows `AMP → MSH` replacing `IA → VS`.
- [ ] Cone-culling effectiveness: >50 % of meshlets rejected when viewing a
      closed mesh from one side (pipeline statistics query on mesh invocations).
- [ ] Pixel-identical output versus `ZHLN_NO_MESH_SHADING=1` (UV seams, normal
      smoothing, T-junctions).
- [ ] Shadow cascades unchanged, BLAS/ray-traced reflections unaffected.
