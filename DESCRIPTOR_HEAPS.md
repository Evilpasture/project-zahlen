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

* Device extension: `VK_EXT_descriptor_heap` (required now),
  `VK_KHR_maintenance5` (required; supplies `VkPipelineCreateFlags2CreateInfoKHR`
  for the mandatory `VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT` on 1.3 devices),
  and `VK_EXT_extended_dynamic_state3` (required; its
  `dynamicRenderingUnusedAttachments` feature lets pipelines that declare a
  stencil format draw inside stencil-less passes/secondaries — otherwise
  VUID-vkCmdExecuteCommands-pStencilAttachment-06775 /
  VUID-vkCmdDraw-dynamicRenderingUnusedAttachments-08917 fire for the
  parallel-recorded MainPass1 secondaries). The pipeline names that stencil
  format itself: `ZHLN_CreateGraphicsPipeline` sets
  `VkPipelineRenderingCreateInfo::stencilAttachmentFormat` from any depth format
  that carries a stencil aspect (`zhln_format_has_stencil`) and refuses a stencil
  state handed over with a format that has none.
* Features: `VkPhysicalDeviceDescriptorHeapFeaturesEXT::descriptorHeap = VK_TRUE`,
  `VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT::dynamicRenderingUnusedAttachments = VK_TRUE`.
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

`Vk::HeapManager` (src/vulkan/pipeline/DescriptorHeap.hpp) owns both heaps:

```
resource heap buffer:
[ 0 .. kSceneStaticResourceSlots )                       static slots (IBL/LUT/trans-lighting/decal-depth)
[ +kGlobalTextureSlots )                                  globalTextures[] bindless array (offset-addressed)
[ +doubleBuffer * kFrameTransientResourceSlots )          frame partitions, one per frame parity
[ +kImmediateTransientResourceSlots )                     out-of-frame (bake) partition
[ reserved ]                                              minResourceHeapReservedRange (driver-owned)

sampler heap buffer: static only (scene registry + one slot per pass sampler binding)
```

* One unified resource stride =
  `AlignUp(max(bufferDescriptorSize, imageDescriptorSize), max(bufferDescriptorAlignment, imageDescriptorAlignment))`
  so every slot fits every resource type and all spec alignment VUIDs hold.
* The three dynamic groups are one bump allocator per lifecycle
  (`HeapLifecycle::Frame` / `Immediate`), rewound by `HeapManager::BeginFrame`
  and `BeginImmediate`. A write allocates its pass's whole block from the
  partition of the frame being recorded, which is why a pass no longer declares
  how many blocks per frame it needs: `BeginImmediate` is honest because the
  out-of-frame bakes run through `ExecuteImmediate`, which waits on the fence
  before the partition is reused.
* `globalTextures[]` is one offset-addressed region: the reservation returns its
  base, the set-0 mapping points at that base, and a static allocation added
  before the reservation moves the array instead of landing inside it. The
  hand-counted `SkipStatic*` cursors this replaced are gone.
* `globalTextures[]` slots are recycled rather than only ever appended to:
  `ReleaseBindlessTexture` parks a released slot (its descriptor keeps pointing
  at the parked image while frames that may still read it are in flight),
  `RenderContext::BeginFrame` points the slot at the white fallback and returns
  its index to the free list, and `AdoptBindlessTexture` pops that list before
  advancing `nextTextureIndex`. `RenderContext::UnloadTexture` and
  procedural-texture replacement are the release paths, so
  `ResourceSlotsExhausted` now means 32768 textures are genuinely live at once.
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
| 1       | frame           | `PUSH_ADDRESS` → reflected `frameAddress` offset |
| 2       | lights          | `PUSH_ADDRESS` → reflected `lightsAddress` offset |
| 3       | g_instances     | `PUSH_ADDRESS` → reflected `instancesAddress` offset |
| 4       | g_joints        | `PUSH_ADDRESS` → reflected `jointsAddress` offset |
| 5       | g_prevJoints    | `PUSH_ADDRESS` → reflected `previousJointsAddress` offset |
| 6       | g_morphDeltas   | `PUSH_ADDRESS` → reflected `morphDeltasAddress` offset |
| 7       | prefilteredMap  | `HEAP_WITH_CONSTANT_OFFSET` → static image slot |
| 8       | brdfLUT         | `HEAP_WITH_CONSTANT_OFFSET` → static image slot |
| 9       | clampSampler    | `HEAP_WITH_CONSTANT_OFFSET` → static sampler slot |
| 10      | texTransLighting| `HEAP_WITH_CONSTANT_OFFSET` → static image slot |
| 11      | globalTextures[]| `HEAP_WITH_CONSTANT_OFFSET`, `heapArrayStride = resource stride` |

### Push-data layout

`resources/shaders/descriptor_heap_layout.slang` is the layout authority. Its
`DescriptorHeapPushData` type places the largest ordinary per-pass block first,
then the six frame addresses and the descriptor index. slangc compiles that
type into the `gpu_abi` SPIR-V blob, and `src/render/GpuAbi.hpp` reads the field
offsets out of that bytecode at compile time (`Vk::SpirvTypes`, see
`src/vulkan/pipeline/SpirvLayout.hpp`), asserting them against
`Vk::kHeapPushDataLayout` before anything can build. Both mapping creation and
`vkCmdPushDataEXT` then use that constant, including any padding selected by
slangc's SPIR-V layout rules. `src/vulkan` never sees the `.slang` source.

Per-frame buffers keep their double-buffered allocations; their *stable* device
addresses are pushed per frame instead of re-writing descriptors per frame.
Static descriptors (samplers, IBL, trans-lighting, decal depth) are written
once at init / target-recreation (`WriteSceneStaticImageDescriptors`,
`WriteTransLightingToHeap`, `RecreateTargets`).

Textures register via `WriteTextureSlotToHeap(index, ...)`; the instance-data
`texIndices0/1` packing is unchanged — the heap array is indexed identically to
the old bindless set array.

---

## 4. Ported (heap + push data) — the entire frame

* Scene registry pipelines: materials, shadow (cascade + punctual), lines,
  CSG stencil passes, particle render, mesh-particle render + shadow, UI
  batches, decals (set 0 + scene set 1 merged into one mapping chain).
* Compute: particle update, mesh-particle update, HiZ generation (one block per
  mip, written while the mip is recorded), occlusion culling (one block per
  pass), cluster bounds/culling, all five volumetric passes, procedural bake
  (immediate partition).
* Post-processing: ambient, lighting, reflection (their RT/NoRT pipeline
  variants share one block per frame), translucent reflection, bloom (one block
  per chain step), TAA/FXAA/MLAA/SMAA, blit.
* ImGui: there is no renderer backend or ImGui-owned GPU state. Dear ImGui is
  treated as a CPU-side producer; `BlitPass` consumes `ImDrawData`, expands it
  into the current frame `uiVbos`, and draws it with the normal `uiPipeline` and
  bindless texture indices.
* `PipelineBuilder::HeapMappings` / `ComputePipelineBuilder::HeapMappings` set
  `descriptor_heap` on the pipeline desc; `RenderCore.c` chains the mapping
  structs into each `VkPipelineShaderStageCreateInfo` and adds
  `VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT` via
  `VkPipelineCreateFlags2CreateInfoKHR`.
* All heap pipelines are created with `layout = VK_NULL_HANDLE`
  (`VUID-VkGraphicsPipelineCreateInfo-flags-11311`; `Impl::emptyPipelineLayout`
  is the named null alias used at the call sites).
* `HeapBindings.hpp` bakes per-pass mapping tables from the reflected set. A
  pass's non-sampler bindings are one contiguous resource-heap block, and the
  mapping is slot-independent: binding ordinal `i` resolves at
  `i * resource stride` plus the block's base slot, which travels in the
  reflected push-data index word (`HeapBlockBase`), so no absolute heap slot is
  baked into a pipeline. Sampler bindings get one static slot each.
* `HeapManager::WriteHeapParameters` allocates that block from the caller's
  partition and returns its base, which is what the dispatch pushes; its
  arguments are `Vk::Slot<"name">(value)` values, each carrying the name of the
  shader binding it fills, resolved against the names SPIRV-Reflect reported for
  that pass's set. Argument order carries no meaning, and a name the module does
  not declare -- a binding a configuration compiled out, which Slang removes --
  is skipped rather than shifting every later descriptor by one slot. A binding
  that cannot supply the reflected descriptor type, a binding left unnamed, or an
  unwritten sampler slot fails an assertion in dev builds. Samplers are written
  once by `InitHeapPassSamplers` from `Vk::SamplerSlot<"name">` values, matched
  the same way.
* Parallel/secondary recording: `ParallelDrawDispatch` supports heap-binding
  inheritance (`VkCommandBufferInheritanceDescriptorHeapInfoEXT`) plus an
  optional per-secondary push-data block; ported passes running in secondaries
  simply bind the heaps themselves (legal with a NULL inheritance chain).

## 5. Still Legacy

* Skinning only: a BDA + push-constant compute pass recorded before the
  frame's heap segments (it binds no descriptor sets).
* Everything descriptor-set related was deleted from the render layer after
  the migration: the `DescriptorLayout` DSL, the descriptor-pool builders,
  the legacy dynamic-pass/framebuffer cache, `Texture.hpp`'s staged uploader,
  and all pool/set members on the pass structs. Only the write-POD field types
  survive (`DescriptorWrites.hpp`: `ImageWrite`, `BufferWrite`, `IsTypedImage`).

## 6. Test Coverage

* `tests/render/TestDescriptorHeaps.cpp` — 64 distinct procedural textures
  rendered through the `globalTextures[]` CONSTANT_OFFSET heap region
  (including indices past the static-slot boundary), plus a per-frame
  PUSH_ADDRESS-block check (camera pan must change the image).
* `tests/render/TestDescriptorHeapsParallel.cpp` — forces
  `ZHLN_NO_GPU_CULLING` so MainPass1 records 400 heap-based draws into
  parallel secondary command buffers that inherit the primary's heap
  bindings and re-push the per-frame device-address block.
* Both run with validation layers enabled (`ValidationMode::On`), so heap
  VUID violations fail the suite.

## 7. Follow-up Checklist

- [x] Port HiZ / culling / cluster passes (image + buffer bindings).
- [x] Port volumetric passes.
- [x] Port post-processing (TAA/FXAA/MLAA/SMAA, bloom, blit).
- [x] Port ambient/lighting/reflection passes.
- [x] Port procedural bake compute.
- [x] Remove the ImGui renderer backend; render `ImDrawData` directly in Blit/UI.
- [ ] Consider `VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_PUSH_INDEX_EXT` for
      per-draw material descriptor selection instead of push data fields, now
      that the frame partition can hand out a block per draw.
- [ ] Optional: direct descriptor access (`layout(descriptor_heap)`) for hot
      bindless paths once slangc with `-capability spvDescriptorHeapEXT` is
      the build requirement.
- [x] Remove the now-unused `DescriptorLayout` DSL, the legacy pass helpers,
      and the per-pass pool/set members.

## 8. ImGui Rendering

`third_party/imgui/backends/imgui_impl_vulkan_heap.{h,cpp}` was removed, and no
replacement renderer backend exists. Dear ImGui owns no Vulkan objects, upload
pools, descriptor slots, or geometry buffers. The engine uploads the font atlas
through its texture path, stores bindless texture indices in `ImTextureID`, and
consumes `ImDrawData` directly in the Blit/UI pass using the same frame UI VBOs
and `uiPipeline` as native UI batches.

## 9. Requirements Bumped

* Vulkan SDK ≥ 1.4.321 (headers/loader with `VK_EXT_descriptor_heap`).
* Driver with `VK_EXT_descriptor_heap` + `VK_KHR_maintenance5` +
  `VK_EXT_extended_dynamic_state3` (NVIDIA ≥ 610, recent RADV/ANV/AMD/Intel drivers).
* Slang shaders unchanged; no new slangc capability required.
