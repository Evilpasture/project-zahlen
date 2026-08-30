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
They are uploaded through `RenderContext::CreateStorageBuffer()`
(`VK_BUFFER_USAGE_STORAGE_BUFFER_BIT`), not `CreateVertexBuffer()`: nothing ever
binds them to the input assembler. Note that the flag that actually makes the
BDA reads legal is `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`, which
`CreateGPUBuffer` sets on every buffer -- accessing memory through a
`PhysicalStorageBuffer` pointer has no usage-flag requirement of its own, since
usage flags govern descriptor binding. `STORAGE_BUFFER_BIT` is for correct
intent and for binding these as descriptors later.
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

### Per-pass interface variants

`SceneGeometryOutput` is compiled once per pass. The three fragment shaders
consume different subsets of it, and a geometry-stage output that no fragment
stage reads is both wasted interpolation and a validation warning on every
pipeline build, so one shared interface cannot serve all three:

| Variant | Define | Varyings | Fragment stage |
| --- | --- | --- | --- |
| G-buffer | *(none)* | 11 | `PSMain` |
| Shadow | `ZHLN_PASS_SHADOW` | 6 | `PSShadow` |
| Forward | `FORWARD_PASS` | 6 | `PSForward` |

The shadow and forward passes drop motion vectors and the normal/tangent frame
(and one of `alphaMode` / `emissiveFactor`), halving their varying traffic.

`VSMain`/`MeshMain` stay `#ifdef`-free: they fill a plain `SceneGeometryVertex`
and call `PackGeometryOutput()`, which narrows it to the varyings the pass
actually rasterises. Dropped stores never happen, so nothing is paid for values
the fragment stage would discard.

Mixing variants across a pipeline mismatches varying locations, so the pairing
is centralised in `Resource::GetSceneShaders(SceneShaderVariant)` -- geometry
and fragment stages always come out of one lookup.
`tools/check_shader_interfaces.py` compiles all three variants and reflects the
SPIR-V to prove every interface is exact; it needs slangc but no GPU.

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

Procedural geometry (`CreateBoxMesh`, `CreatePlaneMesh`, `CreateTetrahedronMesh`,
both terrain builders) is meshletized in `MeshBuilder.cpp` via `AttachMeshlets()`.
Without it, a scene assembled from `CreateBox()` would carry `meshletCount == 0`
and quietly stay on the vertex pipeline forever — the feature would look
"working" while never actually running. `TestMeshShaders` guards this.

| Case | Path | Reason |
| --- | --- | --- |
| GPU-skinned draws | vertex | meshlet vertex indices address the pre-skinning pool |
| Debug lines | vertex | `LINE_LIST`; mesh pipelines declare their own topology |
| CSG stencil draws | vertex | they bind an explicit pipeline override |
| Particles / decals / post | vertex | not meshletized |
| Hi-Z occlusion culling | disabled while mesh shading | see the `firstInstance` note above |

---

## 7. Tests

`tests/render/TestMeshShaders.cpp`, in the `GPU_Pipeline` group binary (ctest: `GPU_Pipeline`, or `ctest -R GPU_Pipeline`):

| Test | What it proves |
| --- | --- |
| `meshlet_partitioning_invariants` | CPU only. Degenerate input falls back; one triangle → one meshlet; on a 48×48 grid no primitive is lost or duplicated, every micro index resolves inside its cluster, every referenced vertex lies inside the baked bounding sphere (cluster culling is only sound if it does), every `triangleOffset` is 4-byte aligned. |
| `procedural_meshes_carry_meshlet_streams` | Box/plane/tetrahedron all upload the three meshlet streams **and** keep their raw vertex pool intact (BLAS + vertex fallback). Regression guard for the "feature silently never runs" failure mode. |
| `mesh_shading_runtime_toggle` | `SetMeshShadingEnabled()` actually flips the active path; unsupported devices never report the path as active. |
| `mesh_and_vertex_paths_render_identically` | Renders the same scene twice in one process — once through task/mesh, once through the vertex pipeline — and compares the framebuffers. |

The parity test controls for engine nondeterminism explicitly:

* **TAA must be disabled at its source.** The camera's `AASettingsComponent` is
  authoritative — `RenderSystem` re-pushes it into the `RenderContext` every
  frame (so `RenderContext::SetAAState` alone is overwritten after one tick),
  and while it says TAA, `CameraSystem` jitters the projection by a different
  sub-pixel offset every frame. The first revision of this test missed that and
  blamed the mesh path for jitter: identical coverage (0.015 %) and silhouette
  (0.083 %), but 1.14 % of pixels differing with a max delta of 196 — the exact
  fingerprint of a half-pixel shift on high-contrast edges.
* **Two-phase GPU culling is disabled process-wide** (`ZHLN_NO_GPU_CULLING`, set
  before any device exists). The mesh path bypasses indirect culling by design,
  so leaving it on would compare two *culling* strategies, and Hi-Z culling is
  temporal (it tests against the previous frame's depth pyramid).
* **A control measurement brackets the comparison.** The capture order is
  vertex → mesh → vertex; the two vertex captures establish the engine's own
  noise floor, and the mesh path is allowed to differ by at most twice that
  (or the absolute floor, whichever is larger). Without a control there is no
  way to distinguish a path divergence from engine noise.
* **Validation errors fail the test.** `RenderContext::ValidationErrorCount()`
  is snapshotted around the rendered frames; correct pixels produced through
  invalid API usage is not a pass.

The comparator was itself validated against synthetic divergences (identical frames, a dropped cluster, a
1-pixel and a 3-pixel geometry shift, a ±2 ULP jitter, two blank frames, and a
uniform shading change). Findings that shaped it:

* A "1 % of pixels over tolerance" budget **passed** a 3-pixel geometry shift —
  far too lax. The decisive metric is therefore the **silhouette mismatch rate**
  (pixels that are geometry in one image and background in the other, over the
  union), which flags even a 1-pixel shift at 1.3 % against a 0.5 % budget.
* Two blank frames match perfectly, so the test independently requires both
  frames to contain >500 shaded pixels before comparing them.
* TAA is switched off and `fullBright` enabled for the comparison, otherwise
  jitter history and lighting noise dominate the diff.

Every GPU test **skips** (does not fail) when the device lacks mesh shading, so
the binary stays green on lavapipe and pre-Turing/pre-RDNA2 hardware.

## 8. Verification status

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
- [x] The image comparator used by the parity test detects dropped clusters,
      1-pixel geometry shifts and shading changes, and rejects blank frames.
- [x] **On hardware (RTX 3050, NVIDIA, 320x240 headless):** the mesh path and
      the vertex path are **bit-identical** -- 67 346 shaded pixels each,
      coverage delta 0, silhouette mismatch 0, max channel delta 0, with the
      vertex-vs-vertex control also at 0.

Verified on hardware (RTX 3050, NVIDIA, headless):

- [x] `TestMeshShaders` suites green, 4/4 (now in the `GPU_Pipeline` group).
- [x] Zero validation errors across the whole suite (after the two pre-existing
      fixes below).

Still open:

- [ ] RenderDoc capture shows `AMP -> MSH` replacing `IA -> VS`.
- [ ] Cone-culling effectiveness: >50 % of meshlets rejected when viewing a
      closed mesh from one side (pipeline statistics query on mesh invocations).
- [ ] Shadow cascades unchanged, BLAS/ray-traced reflections unaffected.
- [ ] Performance comparison against the vertex path on a real scene. Note that
      mesh shading currently disables Hi-Z occlusion culling (section 5), so an
      occlusion-heavy scene may shade more fragments; compare against
      `ZHLN_NO_MESH_SHADING=1` rather than assuming a win.

### Pre-existing bugs surfaced by these tests

Neither is related to mesh shading. Both were found because the parity test is
the first thing in the engine that asks "is this frame VUID-clean?", and both
were reproduced with `ZHLN_NO_MESH_SHADING=1` and `ZHLN_NO_GPU_CULLING=1`.

1. **Headless frames submitted an unended command buffer.** In
   `RenderFrame.cpp`, `Vk::CommandBufferGuard recordGuard(cmd)` lived in the
   *same* block as the `vkQueueSubmit2` call, so `vkEndCommandBuffer` ran after
   the submit -- `VUID-vkQueueSubmit2-commandBuffer-03874`, once per frame, in
   every headless run (i.e. in every GPU test). A comment even claimed
   "recordGuard destructor ends the command buffer here"; the scope that would
   have made that true did not exist. Fixed by introducing the scope.
2. **The debug messenger was never created.** A
   `VkDebugUtilsMessengerCreateInfoEXT` chained into `VkInstanceCreateInfo` only
   covers `vkCreateInstance`/`vkDestroyInstance`;
   `vkCreateDebugUtilsMessengerEXT` was never called, so no runtime message ever
   reached `ZHLN_Internal_DebugCallback`. That silently disabled the new
   validation-error counter *and* the GPU-AV out-of-bounds `abort()` hook -- the
   engine had not been trapping shader OOB at runtime at all. `Context` now owns
   a persistent messenger.

   It subscribes to errors **and** warnings. The first warning it surfaced was a
   real (if benign) interface mismatch, fixed in turn -- see below.
