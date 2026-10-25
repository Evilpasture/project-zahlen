# VK_EXT_descriptor_heap Migration

Project Zahlen's scene binding model has moved from Vulkan descriptor sets to
**descriptor heaps** (`VK_EXT_descriptor_heap`, ratified 2025). This document
describes the heap-only backend and how the migration was completed.

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
  format itself: `Vk::CreateGraphicsPipeline` sets
  `VkPipelineRenderingCreateInfo::stencilAttachmentFormat` from any depth format
  that carries a stencil aspect and rejects stencil state when the configured
  depth format has none.
* Features: `VkPhysicalDeviceDescriptorHeapFeaturesEXT::descriptorHeap = VK_TRUE`,
  `VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT::dynamicRenderingUnusedAttachments = VK_TRUE`.
* Device entry points are loaded through Volk once `Vk::Context` creates the
  logical device; the typed context exposes the queues and enabled capabilities.

### Pipeline creation is heap-only

`VkPipelineCreateFlags2CreateInfoKHR::flags` with
`VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT` requires `layout` to be
`VK_NULL_HANDLE` (not an empty layout). Every engine pipeline is created in
heap mode with a null layout. The pipeline builders do not accept or create
traditional pipeline layouts, and `src/vulkan` has no descriptor-set or classic
push-constant fallback path.

### State-model caveat

Within a command buffer, heap/push-data commands and legacy
descriptor-set/push-constant commands **invalidate each other**. The renderer
now records only heap/push-data commands.

### Heap state is command-stream state, bound once per stream

The heap bindings and the per-frame device-address block are **state of the
command stream**, so they are established once, where the stream begins
recording, and never from pass code:

| Stream                           | Begins at                                                                          |
| -------------------------------- | ---------------------------------------------------------------------------------- |
| a window's graphics stream       | `RenderFrameContext::BeginStream`, in `RenderContext::Impl::AcquireTarget`           |
| the async-compute stream         | `RenderFrameContext::BeginStream`, in `Pipelines::ComputeSimPipeline::Submit`        |
| fork / parallel-draw secondaries | `Vk::SecondaryPassContext::Begin` (`HeapInheritance` link + per-frame address push, per slot) |

Nothing in the stream re-establishes it: `HeapManager` writes into one pair of
device-addressable heap buffers that are created once and never grow or move, so
the bound ranges stay valid for the life of the command buffer. Pass payloads
cannot clobber the address block either -- `GpuAbi::kScenePushLayout` guarantees
the address window begins after the pass-payload prefix
(`kScenePassPayloadBytes`), and every push is written at its own offset
(`vkCmdPushDataEXT`), never as one whole-block write.

The type system carries the rule, so nothing has to remember it. A stream is
begun by `RenderFrameContext::BeginStream`, which returns a
`Vk::StreamCommandRecorder`: a recorder that cannot exist without the heap state,
because the factory that produces it takes the establishing step as an argument
and runs it before handing the recorder back. `BindHeapsAndPushFrame` is private
to the frame context on top of that -- `BeginStream` is the one thing that can
call it -- so "this stream began without its heap state" is not a state the code
can be written in, rather than a rule someone has to check.

The reason it moved out of the passes is worth keeping in view: a pass that
binds heaps for itself only works because of what happens to run before it on the
same buffer, which is exactly the property that stops holding the day the pass is
reordered, moved to the front of the frame, or forked onto another queue. A pass
is handed a frame and a `Vk::CommandEncoder` and nothing else, so the coupling
has no way back in.

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
  (`HeapLifecycle::Frame` / `Immediate`), rewound by transactions rather than by
  a state flag the caller has to remember to tick: `HeapManager::BeginFrameScope`
  opens the frame partition (the renderer's frame context holds the token) and
  `HeapManager::CreateImmediateScope` opens a bump transaction over the immediate
  partition. A write allocates its pass's whole block from the partition of the
  frame being recorded, which is why a pass no longer declares how many blocks
  per frame it needs. Immediate scopes nest LIFO and each restores the cursor it
  found on exit, so two live scopes hand out disjoint slots instead of resetting
  each other's cursor to zero; the out-of-frame bakes stay honest because
  `ExecuteImmediate` waits on the fence before a scope's range is reused.
* `globalTextures[]` is one offset-addressed region: `ReserveImageRange` returns
  its typed base handle, the set-0 mapping points at that base, and a static
  allocation added before the reservation moves the array instead of landing
  inside it. The hand-counted `SkipStatic*` cursors this replaced are gone.

### Registration, reservation, binding

`HeapManager` hands out typed capabilities, never slot indices and never raw
byte offsets:

| Operation | API | Use |
| --------- | --- | --- |
| Register | `RegisterSampler`, `RegisterSamplerArray`, `RegisterImage`, `RegisterStorageImage`, `RegisterStorageBuffer`, `RegisterUniformBuffer`, `RegisterAccelerationStructure` | Slot assignment and descriptor write in one call: a handle that comes back is already written. |
| Reserve  | `ReserveSampler`, `ReserveImage`, `ReserveSamplerRange`, `ReserveImageRange` | Only when the payload cannot exist yet (a mapping baked into a pipeline precedes the image it addresses). Returns a typed handle, and it is expected to be bound. |
| Bind     | `BindSampler`, `BindImage`, `BindStorageImage`, `BindStorageBuffer`, `BindUniformBuffer`, `BindAccelerationStructure` | Attach or refresh the payload of an existing handle. |

Mapping tables are semantic, not Vulkan. `ApplySamplerMapping`,
`ApplySamplerArrayMapping`, `ApplySampledImageMapping`,
`ApplyBindlessTextureArrayMapping` and `ApplyPushIndexMapping` (plus the
allocation-carrying `ApplyNewSamplerMapping` / `ApplyPairedSamplerMapping` that
`BuildHeapPassBindings` uses) append 32-byte `DescriptorMapping` rows naming a
heap *slot* and a push-constant offset — never a byte offset, never a
`VkDescriptorSetAndBindingMappingEXT`. `HeapManager::Materialize` is the only
place the 96-byte driver struct is produced, and it computes `ResourceOffset()`,
`SamplerOffset()`, `ResourceStride()` and `SamplerStride()` itself, so those
stay private to `HeapManager`. If padding, stride rules or the partition layout
change, nothing outside the heap changes — and nothing persistent holds a driver
struct or a pointer into one.
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
| 10      | texTransLighting   | `HEAP_WITH_CONSTANT_OFFSET` → static image slot |
| 11      | materialSamplers[] | `HEAP_WITH_CONSTANT_OFFSET` → static 9-sampler wrap table |
| 12      | globalTextures[]   | `HEAP_WITH_CONSTANT_OFFSET`, `heapArrayStride = resource stride` |

### Push-data layout

`resources/shaders/descriptor_heap_layout.slang` is the layout authority. Its
`DescriptorHeapPushData` type places the largest ordinary per-pass block first,
then the six frame addresses and the descriptor index. `tools/zshader` compiles
that type in-process and reflects its field offsets into the generated header as
`Slang::kScenePushLayout`, which `src/render/GpuAbi.cpp` exports as
`GpuAbi::kScenePushLayout` -- so an edit to the struct lands in the engine on
the next build instead of needing a hand-kept copy, and nothing re-reads the
module's SPIR-V to derive it. Both mapping creation and `vkCmdPushDataEXT` then
use the derived offsets, including any padding selected by the shader
compiler's layout rules; `src/vulkan` supplies only the generic container
(`Vk::HeapPushDataLayout`) and writes what it is handed. `src/vulkan` never sees the `.slang` source, and
the scene's blob prefix -- `GpuAbi::kScenePassPayloadBytes` and the
`ScenePassPayload` bound on every pass payload -- is policy the render layer
asserts about itself, never a constant the RHI carries.

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

* Scene registry pipelines: materials, shadow (cascade + light atlas), lines,
  CSG stencil passes, particle render, mesh-particle render + shadow, UI
  batches, decals (set 0 + scene set 1 merged into one mapping chain).
* Compute: particle update, mesh-particle update, standalone BDA-based
  skinning (heap-mode push data with no descriptor mapping), HiZ generation
  (one block per mip, written while the mip is recorded), occlusion culling
  (one block per pass), cluster bounds/culling, all five volumetric passes,
  procedural bake (immediate partition).
* Post-processing: ambient, lighting, reflection (their RT/NoRT pipeline
  variants share one block per frame), translucent reflection, bloom (one block
  per chain step), TAA/FXAA/MLAA/SMAA, blit.
* ImGui: there is no renderer backend or ImGui-owned GPU state. Dear ImGui is
  treated as a CPU-side producer; `BlitPass` consumes `ImDrawData`, expands it
  into the current frame `uiVbos`, and draws it with the normal `uiPipeline` and
  bindless texture indices.
* `PipelineBuilder::HeapMappings` / `ComputePipelineBuilder::HeapMappings`
  supply the per-stage mapping structs. Every pipeline builder enables
  descriptor-heap mode, and the implementation chains those mappings into each
  `VkPipelineShaderStageCreateInfo` while adding
  `VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT` via
  `VkPipelineCreateFlags2CreateInfoKHR`. The structs are materialized per build
  by a build-scoped `HeapMappingArena` (`HeapMappingBuilder.hpp`) out of the
  pass's `HeapMappingTable`; `arena.Map(a, b)` composes two tables straight into
  the arena's own inline buffer, which is how a pass brings the shared Set-1
  scene block along without a third table. The buffer holds 32 mappings — every
  set the renderer builds is smaller, and Set 0 + Set 1 together are around 19 —
  so a pipeline build allocates nothing; a wider set spills to one heap block.
* Every pipeline create info uses `layout = VK_NULL_HANDLE`
  (`VUID-VkGraphicsPipelineCreateInfo-flags-11311`); no pipeline-layout handle
  is accepted or stored by the backend.
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

## 5. Backend Compatibility

* All Vulkan pipelines built in `src/render` use descriptor-heap mode. Skinning uses
  heap-mode push data with no descriptor mapping; `HangGpuCS` is a no-binding
  heap pipeline for the optional device-lost diagnostic.
* The backend has one binding model: all pipelines use descriptor heaps, draw
  and dispatch payloads use push data, and `src/vulkan` exposes no traditional
  pipeline-layout, descriptor-set binding, or push-constant fallback API.
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
