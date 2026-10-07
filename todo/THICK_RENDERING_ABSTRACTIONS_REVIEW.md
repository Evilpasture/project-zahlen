# Thick rendering abstractions — architecture proposal for review

**Status: review draft only.** No implementation is authorized by this document. The goal is to agree on the target model and its invariants before changing call sites or deleting existing APIs.

## Executive proposal

Move the renderer's public vocabulary from Vulkan construction steps to engine concepts. Callers should describe a shader/pass/material contract or a resource archetype; the Vulkan layer should derive the driver structures, allocations, views, layouts, and creation calls internally.

This is not a proposal to replace fluent builders with `MakeImageCreateInfo2D`, `VkInit`, or another function that still asks the caller to describe Vulkan's structs. It is also not a proposal to remove resource factories, error propagation, ownership, caching, or the render graph. Those are the abstractions that should remain and become thicker.

The proposed direction is:

1. **Pipelines:** express the legal pipeline contract in terms of shader modules, pass attachment contracts, and material/pipeline state. Synthesize Vulkan state internally and create/cache the driver pipeline at runtime.
2. **Images and samplers:** expose engine resource archetypes and intent, not separate VMA allocation, `VkImage`, `VkImageView`, and create-info choreography at render call sites.
3. **Render graph:** continue to own pass hazards and image-state transitions. Image archetypes describe capabilities and purpose; they should not claim a permanent layout that conflicts with graph-managed transitions.
4. **Low-level escape hatches:** retain raw Vulkan structures and operations inside `src/vulkan` where they implement the engine abstractions. Keep exceptional interop, readback, upload, and presentation paths explicit and narrowly scoped rather than leaking driver DTOs into render features.

## Why this is more than deleting builders

Designated initializers make aggregate configuration readable, but they do not by themselves create an engine abstraction. A `VkImageCreateInfo{...}` at every call site still makes that caller responsible for Vulkan image mechanics. The architectural target is to remove those mechanics from the caller's vocabulary, not merely change the syntax used to express them.

Likewise, C++ templates can make a pipeline's *contract* compile-time-checkable, but Vulkan still creates the `VkPipeline` at runtime against a device and driver. The type should constrain and derive the runtime creation; it cannot turn a driver object into a constant-evaluated object.

## Current code inventory and implications

### Pipeline construction

- `src/vulkan/pipeline/PipelineBuilder.hpp/.cpp` currently accepts shader stages, layout, cache, heap mappings, vertex inputs, color/depth formats, topology, polygon/cull/front-face state, depth test/write, blend state, view mask, specialization data, stencil state, and color-write state.
- `src/vulkan/pipeline/PipelineTypes.hpp` already carries compile-time attachment-format sets and `TypedPipeline` wrappers. This is a foundation to build on, rather than a reason to restart the type model.
- Shader stage/module descriptions and Slang reflection already provide shader-side metadata. `PipelineRegistry` owns runtime material pipeline variants and shader reload already rebuilds registered pipelines.
- The render graph expresses resource usage and derives barriers. A separate bridge is still needed to prove that a pass's attachment-format contract reaches pipeline creation without a second manually maintained format list.
- `ZHLN::Vk::Pipeline` is currently the owning Vulkan pipeline-handle type. A type-level graphics-pipeline contract needs a non-conflicting name (for example, `GraphicsPipeline<...>`), or an explicit handle rename as part of the design.

The proposed three inputs—shader, render-pass target, and material flags—are a **hypothesis to validate against every current pipeline**, not yet a proven exhaustive set. The current API exposes more state than those three inputs. Each field must be classified as one of:

- derived from shader reflection or the pass contract;
- a fixed engine invariant;
- represented by a material/pipeline state type or a bounded variant;
- genuinely runtime state that remains in the creation request; or
- unsupported/deprecated after caller and asset migration.

No current behavior should disappear merely because a field did not fit the initial sketch. In particular, inventory vertex-vs-mesh pipelines, vertex input, heap mappings, layout/push-constant compatibility, specialization values, multiview masks, stencil, color-write controls, and hot reload.

**Runtime material state needs a decision.** A non-type template parameter works for compile-time material flags. If material flags come from loaded assets or can vary per material at runtime, blindly instantiating a template for every combination risks a large variant set and compile-time growth. Decide whether the engine has a finite compile-time material family, a runtime pipeline key/cache, or a hybrid (static pass contract plus runtime material variant).

### Images, views, and samplers

- `ImageBuilder` in `src/vulkan/memory/Allocator.hpp` is mostly a mutable `VkImageCreateInfo`; its setters write individual fields, `Texture2D`/`TextureCube` are presets, and `Build()` delegates to the existing `Image::Create(Allocator&, VkImageCreateInfo, MemoryUsage)` factory.
- `Image` owns the Vulkan image and VMA allocation. `ImageView` owns a view and its view-create metadata. Render code still composes these separately in several places.
- Higher-level types already exist: `TextureResource` bundles an `Image`, `ImageView`, and extent; `RenderTarget<F>`, `RenderTarget3D<F>`, and `MipmappedRenderTarget<F>` bundle images and views; `ImageSlice`/`TypedImage` carry slices, formats, and layout types. These should be audited and reused so the proposal does not add duplicate archetypes.
- `TextureUploader` already owns the upload sequence for 2D, 3D, and cube resources. It is a likely home for resource creation/upload orchestration, but its users should request a texture intent rather than construct image/view/barrier pieces themselves.
- `SamplerBuilder` mutates `VkSamplerCreateInfo`; `Build()` creates the RAII sampler and returns `expected`, while `Info()` is used by some call sites that need sampler-create metadata without building a sampler.

The archetype proposal should cover the real shapes in the codebase, not assume there are only three Vulkan image forms. Review 2D/cube/array/3D images, mip chains and per-mip/per-layer views, render targets, storage use, IBL-generated resources, bindless textures, and external/swapchain images. `Texture2D`, `TextureCube`, `RenderTarget`, and `StorageImage` are candidate domain types, not a final list.

**Do not encode a permanent layout as an archetype invariant unless the resource truly cannot transition.** A texture may be copied, mip-generated, sampled, or used as a storage image over its lifetime. The render graph already tracks layout and access state. Archetypes should capture format, extent, view topology, intended capabilities, and ownership; the graph should remain authoritative for current layout and transitions.

### Render graph and operations outside it

The graph is the strongest existing example of a thick abstraction: passes declare resource uses and the graph derives dependencies and barriers. Extend that principle where pass work is genuinely part of the frame graph. Do not make graph insertion a ritual for operations that are actually asset upload, one-time IBL generation, readback, swapchain handoff, or external interop.

For every manual barrier/copy path, decide whether it is:

1. frame work that belongs in a graph pass;
2. resource-upload or readback work that belongs behind a texture/staging/transfer API; or
3. a narrow low-level interop/presentation exception with an explicit owner and lifetime.

The goal is no render-feature caller manually constructing `ImageBarrierDesc`, `VkImageMemoryBarrier2`, `VkBufferImageCopy2`, or similar driver DTOs. Those structures may still exist inside `src/vulkan` as implementation details of the graph or upload layer. Deleting an internal DTO without moving its responsibility would only move the procedural code.

## Target shape to review

### 1. Pipeline contract

An illustrative shape, not a settled API:

```cpp
template <typename ShaderModule, typename PassContract, MaterialPipelineFlags Flags>
class GraphicsPipeline;

// Runtime factory: uses reflected shader metadata and pass/material contract,
// validates device features, builds/caches VkPipeline, and returns an owner.
auto CreateGraphicsPipeline(const Context&, PipelineCache&, PipelineDiagnostics&)
    -> std::expected<GraphicsPipelineHandle, ErrorCode>;
```

The contract should derive stage/layout/push-data/heap information from reflection where supported, and attachment formats from the render-pass contract. It should encode only meaningful engine variants; viewport/scissor and other genuinely dynamic state remain dynamic. Runtime pipeline creation, caching, diagnostics, and hot reload remain runtime responsibilities.

The first prototype should connect one real render-graph pass (preferably a representative G-buffer/material path) to its concrete shader module and target-format contract, then demonstrate a second material-state variant. This will expose missing contract dimensions before all pipelines are migrated.

### 2. Resource archetypes

Illustrative direction:

```cpp
Texture2D::Create(context, allocator, TextureIntent {
    .extent = extent,
    .format = format,
    .mips = MipPolicy::FullChain,
    .usage = TextureUsage::Sampled | TextureUsage::TransferDestination,
});
```

The actual API should be an engine-intent aggregate or domain-specific overload—not a mirror of `VkImageCreateInfo`. The archetype/factory should own the invariant sequence: choose image properties, allocate memory, create the right view(s), assign debug labels, unwind partial failure, and return one owning resource. Keep low-level `Image`/`ImageView` construction inside the Vulkan/resource implementation for graph internals, unusual view topology, or explicitly justified interop.

`SamplerBuilder` should follow the same rule: callers express sampler intent, while a single factory owns the Vulkan create-info mapping and RAII/error behavior. A data-only aggregate is acceptable; a second fluent builder or a renamed Vulkan-init helper is not the goal.

## Migration phases proposed

### Phase 0 — inventory and invariants (planning)

- Inventory all graphics and compute pipeline build sites and record every current pipeline-state dimension, shader reflection source, target-format source, hot-reload behavior, and pipeline-cache key.
- Inventory image/view creation and sampler use sites; classify texture, cube/array/3D, render-target, storage, IBL, upload, readback, and external-image cases.
- Capture current defaults and output behavior before removal. In particular, the sampler builder's defaults are meaningful and differ from zero-initialized Vulkan fields; image type, extent, layers, mips, flags, usage, tiling, samples, and initial state must also be preserved.
- Map every manual barrier/copy to graph work, upload/readback work, or a documented exception.

### Phase 1 — one vertical proof

- Prototype one typed pipeline contract tied to a real render-graph pass, reflected shader, and material-state variant.
- Prototype one sampled texture/cube upload that returns a single owning archetype and is usable by the existing bindless path.
- Prove error propagation and cleanup on partial view/allocation/pipeline failure, plus shader reload and runtime format behavior.
- Keep old and new creation paths side by side only during this proof; compare descriptors/state and image output.

### Phase 2 — migrate consumers

- Migrate pass/material pipeline registrations to contracts while keeping `PipelineRegistry` as the runtime owner/cache until a replacement is demonstrated.
- Migrate texture and render-target consumers to archetypes; stop exposing `ImageView::Create` and Vulkan create-info structs to render-feature code.
- Route ordinary pass barriers through RenderGraph. Route uploads/readbacks through resource/transfer APIs rather than manually composing Vulkan copy/barrier structs at render call sites.

### Phase 3 — remove pseudo-abstractions and enforce the boundary

Only after consumers have migrated:

- remove `PipelineBuilder`, `ComputePipelineBuilder`, `ImageBuilder`, and `SamplerBuilder` if no call sites or required invariants remain;
- remove redundant create-info DTOs and public helper APIs only when their responsibility has moved to the contract/archetype/graph implementation;
- add configure-time/source checks preventing render-feature code from constructing Vulkan `*CreateInfo` structs or calling low-level creation/barrier/copy entry points directly, with narrow documented exceptions for interop/presentation;
- add focused contract tests and run the platform build, Vulkan validation, shader reload, and representative image regressions.

## Acceptance criteria

- Render-feature and material call sites speak in shader/pass/material and texture/render-target intent; Vulkan create structs, VMA details, image-view construction, and barrier/copy DTOs are confined to the Vulkan/resource implementation or documented exceptions.
- A pipeline's shader/target/material contract is checked at compile time wherever the information is truly static; runtime render-target formats, driver creation, caching, and hot reload continue to work where they are dynamic.
- Pipeline variants are bounded and cacheable; compile time and binary size remain reasonable.
- Image allocation, views, ownership, debug labels, partial-failure cleanup, and error categories remain correct.
- The render graph remains the source of truth for hazards/layouts; archetypes do not impose incompatible permanent layouts.
- Existing GPU-visible behavior is preserved, verified with Vulkan validation and representative rendered-image tests.
- No intermediate `MakeImageCreateInfo2D`/`VkInit`-style helper is accepted as the final architecture if callers still reason in raw Vulkan structures.

## Review questions

1. Which pipeline fields are truly derivable from shader reflection, pass formats, or fixed engine conventions? Which remain explicit? Is the proposed three-input contract complete for current code?
2. Are material state flags compile-time, or do asset/runtime materials require a bounded runtime pipeline key/cache? What is the variant-count budget?
3. Can every relevant RenderGraph pass provide target formats as types today? Which targets (swapchain, window resize, plugins) are inherently runtime-formatted?
4. Should the type-level contract be named `GraphicsPipeline`/`ComputePipeline` to avoid collision with the existing `Vk::Pipeline` owning handle?
5. Which resource archetypes are actually required? Can `TextureResource`, `RenderTarget<F>`, `RenderTarget3D`, and `MipmappedRenderTarget` be consolidated or extended rather than duplicated?
6. Which resource operations should be graph nodes, and which are legitimate upload/readback/presentation exceptions?
7. Should `Texture2D` encode only creation capabilities and ownership while the graph owns layout, or are there resources whose layout really is fixed for their full lifetime?
8. What is the minimum representative vertical slice to prove before migrating every pass and image call site?
