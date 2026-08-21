# VK_EXT_descriptor_heap Migration

Project Zahlen's scene binding model has moved from Vulkan descriptor sets to
**descriptor heaps** (`VK_EXT_descriptor_heap`, ratified 2025). This document
describes the new model, what was ported, and what remains on the legacy path.

---

## 1. The Model

| Legacy (before)                          | Heap model (now)                                   |
| ---------------------------------------- | -------------------------------------------------- |
| `VkDescriptorSetLayout` per pass         | `VkShaderDescriptorSetAndBindingMappingEXT` per pipeline stage |
| `VkDescriptorPool` + `vkAllocateDescriptorSets` | Two heap buffers (resource + sampler) created once |
| `vkUpdateDescriptorSets` per frame       | `vkWriteResourceDescriptorsEXT` / `vkWriteSamplerDescriptorsEXT` into mapped heap memory (+ flush) |
| `vkCmdBindDescriptorSets` per draw/pass  | `vkCmdBindResourceHeapEXT` + `vkCmdBindSamplerHeapEXT` once per heap segment |
| `vkCmdPushConstants`                     | `vkCmdPushDataEXT` |

Heaps are not Vulkan objects: they are ranges of a device-addressable buffer
(`VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT`) that descriptors are *written into*.
Shaders keep their legacy `set`/`binding` declarations — the mapping API
remaps them onto heap offsets at pipeline creation, so **no shader changes were
needed** (this is the spec-sanctioned "binding interface" migration path).

### Device enablement

* Device extension: `VK_EXT_descriptor_heap` (required now) and
  `VK_KHR_maintenance5` (required; supplies `VkPipelineCreateFlags2CreateInfoKHR`
  for the mandatory `VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT` on 1.3 devices).
* Feature: `VkPhysicalDeviceDescriptorHeapFeaturesEXT::descriptorHeap = VK_TRUE`.
* Entry points are resolved once in `ZHLN_CreateDevice` and stored on
  `ZHLN_Device`; `ZHLN::Vk::Context` forwards to them.

### Pipeline layouts are NULL for heap pipelines

`VkPipelineCreateFlags2CreateInfoKHR::flags` with
`VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT` requires `layout` to be
`VK_NULL_HANDLE` (not an empty layout). `RenderCore.c` normalizes this at the
C layer; the builders accept a null layout only in heap mode.

### State-model caveat

Within a command buffer, heap/push-data commands and legacy
descriptor-set/push-constant commands **invalidate each other**. Every ported
pass therefore calls `RenderContext::Impl::BindHeapsAndPushFrame(cmd)` at the
start of its segment (bind both heaps + push the per-frame device-address
block). Legacy passes are ordered so their invalidations are harmless.

---

## 2. Heap Layout

`Vk::HeapManager` (src/render/DescriptorHeap.hpp) owns both heaps:

```
resource heap buffer:
[ 0 .. kSceneStaticResourceSlots )                       static slots (IBL/LUT/trans-lighting/decal-depth)
[ kSceneStaticResourceSlots .. +kGlobalTextureSlots )    globalTextures[] bindless array
[ +dynamic slots ]                                        per-frame dynamic region (double buffered)
[ reserved ]                                              minResourceHeapReservedRange (driver-owned)

sampler heap buffer: same partitioning for samplers
```

* One unified resource stride =
  `AlignUp(max(bufferDescriptorSize, imageDescriptorSize), max(bufferDescriptorAlignment, imageDescriptorAlignment))`
  so every slot fits every resource type and all spec alignment VUIDs hold.
* The heap base address is aligned to `resourceHeapAlignment` /
  `samplerHeapAlignment` (VMA `minAlignment` + runtime check).
* `VkBindHeapInfoEXT::reservedRangeOffset/size` point at the reserved tail;
  the application never touches it while bound.
* `HeapManager::Init` refuses to run if `maxPushDataSize` is too small for the
  push-data layout (below).

---

## 3. Scene Registry Bindings (GlobalSceneRegistry, common.slang)

Set-0 bindings map as follows (baked by `BuildSceneHeapMappings`):

| binding | member          | mapping source |
| ------- | --------------- | -------------- |
| 0       | defaultSampler  | `HEAP_WITH_CONSTANT_OFFSET` → static sampler slot |
| 1       | frame           | `PUSH_ADDRESS` → push data @ `kHeapFrameAddrPushOffset + 0` |
| 2       | lights          | `PUSH_ADDRESS` → + 8 |
| 3       | g_instances     | `PUSH_ADDRESS` → + 16 |
| 4       | g_joints        | `PUSH_ADDRESS` → + 24 |
| 5       | g_prevJoints    | `PUSH_ADDRESS` → + 32 |
| 6       | g_morphDeltas   | `PUSH_ADDRESS` → + 40 |
| 7       | prefilteredMap  | `HEAP_WITH_CONSTANT_OFFSET` → static image slot |
| 8       | brdfLUT         | `HEAP_WITH_CONSTANT_OFFSET` → static image slot |
| 9       | clampSampler    | `HEAP_WITH_CONSTANT_OFFSET` → static sampler slot |
| 10      | texTransLighting| `HEAP_WITH_CONSTANT_OFFSET` → static image slot |
| 11      | globalTextures[]| `HEAP_WITH_CONSTANT_OFFSET`, `heapArrayStride = resource stride` |

### Push-data layout

```
offset 0 .. sizeof(push struct)     per-draw data (legacy push_constant blocks read this)
offset kHeapFrameAddrPushOffset (=192) .. +48   six uint64 device addresses
                                 (frame, lights, instances, joints, jointsPrev, morphDeltas)
```

Per-frame buffers keep their double-buffered allocations; their *stable* device
addresses are pushed per frame instead of re-writing descriptors per frame.
Static descriptors (samplers, IBL, trans-lighting, decal depth) are written
once at init / target-recreation (`WriteSceneStaticImageDescriptors`,
`WriteTransLightingToHeap`, `RecreateTargets`).

Textures register via `WriteTextureSlotToHeap(index, ...)`; the instance-data
`texIndices0/1` packing is unchanged — the heap array is indexed identically to
the old bindless set array.

---

## 4. Ported (heap + push data)

* Scene registry pipelines: materials, shadow (cascade + punctual), lines,
  CSG stencil passes, particle render, mesh-particle render + shadow, UI
  batches, decals (set 0 + scene set 1 merged into one mapping chain).
* Compute: particle update, mesh-particle update (`ComputePass::BuildHeap` +
  `DispatchHeap`).
* `PipelineBuilder::HeapMappings` / `ComputePipelineBuilder::HeapMappings` set
  `descriptor_heap` on the pipeline desc; `RenderCore.c` chains the mapping
  structs into each `VkPipelineShaderStageCreateInfo` and adds
  `VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT` via
  `VkPipelineCreateFlags2CreateInfoKHR`.
* All heap pipelines are created with `layout = VK_NULL_HANDLE`
  (`VUID-VkGraphicsPipelineCreateInfo-flags-11311`; `Impl::emptyPipelineLayout`
  is the named null alias used at the call sites).
* Parallel/secondary recording: `ParallelDrawDispatch` supports heap-binding
  inheritance (`VkCommandBufferInheritanceDescriptorHeapInfoEXT`) plus an
  optional per-secondary push-data block; ported passes running in secondaries
  simply bind the heaps themselves (legal with a NULL inheritance chain).

## 5. Still Legacy (descriptor sets + push constants)

Ordered after/around heap segments so invalidations are harmless:

* HiZ generation, GPU occlusion culling, cluster culling/bounds,
  volumetric fog passes, ambient/lighting/reflection/translucent-reflection
  post passes, bloom, TAA/FXAA/MLAA/SMAA, blit, procedural bake compute,
  skinning (BDA + push constants only), and the ImGui backend (its own
  descriptor pool, rendered last).

These port by the same recipe: reflect the layout → allocate static heap slots
+ `VkDescriptorSetAndBindingMappingEXT` entries → `BuildHeap`/`HeapMappings` →
`BindHeapsAndPushFrame` at segment start → `DrawHeap`/`DispatchHeap`.

## 6. Follow-up Checklist

- [ ] Port HiZ / culling / cluster passes (image + buffer bindings).
- [ ] Port volumetric passes.
- [ ] Port post-processing (TAA/FXAA/MLAA/SMAA, bloom, blit).
- [ ] Port ambient/lighting/reflection passes.
- [ ] Port procedural bake compute.
- [ ] Consider `VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_PUSH_INDEX_EXT` for
      per-draw material descriptor selection instead of push data fields.
- [ ] Optional: direct descriptor access (`layout(descriptor_heap)`) for hot
      bindless paths once slangc with `-capability spvDescriptorHeapEXT` is
      the build requirement.
- [ ] Remove the now-unused `DescriptorLayout` DSL and `legacy/` pass helpers
      once no legacy pass references them.

## 7. Requirements Bumped

* Vulkan SDK ≥ 1.4.321 (headers/loader with `VK_EXT_descriptor_heap`).
* Driver with `VK_EXT_descriptor_heap` + `VK_KHR_maintenance5`
  (NVIDIA ≥ 610, recent RADV/ANV/AMD/Intel drivers).
* Slang shaders unchanged; no new slangc capability required.
