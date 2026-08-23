# ZHLN Rendering Subsystem: Developer's Guide

This document outlines the architecture, resource lifetime model, and pipeline execution flow of the ZHLN (Zero-overHead vuLkan abstractioN) rendering subsystem. It is designed to help developers understand how to safely interact with, configure, and extend the renderer.

---

## 1. Architectural Philosophy

ZHLN is built on a **Dual-Layer Compilation Model** to balance low-level driver control, compilation speed, and developer safety.

```
       [ Client Engine Code (ECS Systems, Gameplay, Editor) ]
                                 │
                                 ▼
   ┌───────────────────────────────────────────────────────────┐
   │             C++ Object-Oriented Frontend                  │
   │  - RAII Resource Wrappers (Buffer, Image, Pipeline)       │  <-- src/render/
   │  - Compile-Time Layout Contracts (DescriptorLayout)       │
   │  - Frame loop orchestration (RenderContext, RenderFrame)  │
   └─────────────────────────────┬─────────────────────────────┘
                                 │ (Inlined Type Conversions)
                                 ▼
   ┌───────────────────────────────────────────────────────────┐
   │                 Procedural C Backend                      │
   │  - Thin Vulkan API Abstractions                           │  <-- RenderCore.h
   │  - Hardware Selection & Swapchain Infrastructure          │
   │  - State transitions, command submission, and sync        │
   └───────────────────────────────────────────────────────────┘
```

* **The C Backend (`RenderCore.h`):** Exposes a stateless, procedural C23 API. It handles the raw Vulkan boilerplates (instance creation, device selection, swapchain recreation, synchronization primitives). It does not allocate memory on the heap and remains independent of C++ engine structures.
* **The C++ Frontend (`RenderCore.hpp`):** Wraps raw Vulkan handles in strongly-typed RAII structures. It leverages C++23 type-safety features to validate descriptor bindings, vertex layouts, and image transitions at compile time, eliminating runtime state validation.

---

## 2. The Lifetime Model (RAII)

Vulkan requires explicit, manual destruction of every allocated resource. ZHLN mitigates the risk of memory leaks and double-frees through three strict rules:

### Rule 1: No Naked Handles
Naked Vulkan handles (`VkBuffer`, `VkImage`, `VkPipeline`) are rarely exposed directly to engine systems. They are always owned by a lifetime wrapper:
* **`Handle<T, Deleter>`:** Manages non-logical-device resources (e.g., `VkSurfaceKHR`, `VkInstance`).
* **`DeviceHandle<T, Deleter>`:** Manages logical-device-bound resources. It holds a reference to the owning `VkDevice` to execute its destructor when the wrapper goes out of scope.

### Rule 2: Move-Only Semantics
All C++ resource wrappers delete their copy constructor and copy assignment operators. They can only be moved. When a resource is transferred, the original container is nullified via `std::exchange()`, preventing double-destruction when the temporary goes out of scope:
```cpp
// Correct Transfer
Vk::Buffer gpuBuffer = std::move(stagingBuffer); 

// Compiler Error
Vk::Buffer illegalCopy = gpuBuffer; 
```

### Rule 3: Memory-Managed Allocations (`Buffer` & `Image`)
Physical resources (vertex buffers, uniform buffers, textures) require both a Vulkan handle and a Vulkan Memory Allocator (`VmaAllocation`) handle.
* The `Buffer` and `Image` classes manage **both** handles simultaneously.
* Destroying a `Buffer` or `Image` automatically frees its allocated GPU memory via `vmaDestroyBuffer` or `vmaDestroyImage`.
* Mapped CPU-visible memory is managed via `Buffer::Map()`, which returns a scoped `MappedRegion` that automatically flushes the cache-lines and unmaps the memory when it goes out of scope.

---

## 3. Image Layouts & Render Passes

One of Vulkan's steepest requirements is managing image layout transitions (e.g., transforming a texture from a `TRANSFER_DST` layout during upload to a `SHADER_READ` layout during drawing). ZHLN handles this using a compile-time type contract.

### State Transitions (`TypedImage<Layout>`)
Instead of tracking layout states at runtime with mutable variables, layouts are baked into the type of the image wrapper:
```cpp
// Represents an image whose current state on the GPU is undefined
Vk::TypedImage<VK_IMAGE_LAYOUT_UNDEFINED> rawTexture;

// Compiling this function records a pipeline barrier and yields a new type:
auto readableTexture = Vk::Transition<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, rawTexture);
```
If you attempt to bind a `TypedImage<VK_IMAGE_LAYOUT_UNDEFINED>` to a render pass that expects a shader-readable image, the C++ compiler will generate a compilation error.

### The Dynamic Pass Builder
Render passes are recorded using a fluent, builder-style interface that wraps Vulkan 1.3's Dynamic Rendering API:
```cpp
Vk::DynamicPass(colorRenderTarget.extent)
    .AddColor(colorRenderTarget, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, clearColor)
    .AddDepth(depthRenderTarget, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_DONT_CARE, 1.0f)
    .Execute(cmd, [&]() {
        // Record draw commands here...
    });
```
This encapsulates `vkCmdBeginRendering`, sets up the dynamic viewports, scissors, and execution states, and automatically calls `vkCmdEndRendering` when the lambda finishes executing.

### Automatic frame-graph instrumentation

`CompileTimeFrameGraph` can instrument every pass at its execution boundary. Pass
recording lambdas should not contain profiler scopes or breadcrumb calls:

```cpp
enum class Stage : uint8_t { GBuffer, Lighting, PostProcess };
Profiler::GpuProfiler<Stage> profiler;
Vk::GPUDiagnostics diagnostics;

// The pass name is the single source of truth for both systems.
auto graph = Vk::CompileTimeFrameGraph(
    Vk::MakePass<"Lighting", Vk::ShaderRead<GBuffer>, Vk::ColorWrite<Hdr>>(
        [](auto& ctx) { RecordLighting(ctx.Cmd()); }
    )
);

graph.Execute(cmd, binder, frameIndex, &profiler, &diagnostics);
```

Immediately before each pass, the graph writes a checkpoint with
`PassType::name`. It then uses `Reflect::StringToEnum<Profiler::StageType>` to
resolve a profiling stage with the same name, writes its start timestamp,
records graph barriers and pass commands, and writes the end timestamp. A stage
enum may intentionally be a subset of the graph: a pass with no matching enum
still receives a breadcrumb but consumes no timestamp queries. Both pointers are
optional, so an uninstrumented execution has no runtime instrumentation calls.

The built-in path uses allocation-free `VK_EXT_debug_utils` labels. It creates
no marker buffers and consumes no VRAM. No proprietary diagnostics SDK is
included or linked, and none of this native machinery is exposed through
`Render.hpp` or gameplay code.

A renderer integrator can provide a native backend at build time with
`ZHLN_GPU_DIAGNOSTICS_BACKEND_SOURCE`; optional SDK binaries are supplied via
`ZHLN_GPU_DIAGNOSTICS_BACKEND_LIBRARIES`. Both remain outside source control.
The backend source is compiled as part of `zahlen_render`, where Vulkan handles
already belong, and implements the single internal factory:

```cpp
#include "Rendering.hpp" // Renderer integration code only.

namespace ZHLN::Vk {
GPUCrashTrackerCallbacks CreateConfiguredGPUCrashTracker(
    GPUVendor vendor,
    VkDevice device,
    VkPhysicalDevice physical,
    DiagnosticConfig config
) {
    if (vendor == GPUVendor::NVIDIA) {
        return MakeGPUCrashTrackerCallbacks(
            MyAftermathBackend::Create(device, physical, config)
        );
    }
    return {};
}
} // namespace ZHLN::Vk
```

The renderer invokes this factory once, after native device creation. Gameplay
only sees the original object API: `RenderContext::Create(window, config)`.

---

## 4. Resource Binding & Layouts

### Vertex Layout Definition
Vertex structures are declared as standard C++ structs and reflected using the `ZHLN_REFLECT_VERTEX` macro:
```cpp
struct CustomVertex {
    float position[3];
    float uv[2];
};
ZHLN_REFLECT_VERTEX(CustomVertex, position, uv);
```
This automatically registers the vertex stride, input rate, and attribute locations (mapping `position` to `location = 0` and `uv` to `location = 1`) with any pipeline configured to use `CustomVertex`.

### Descriptor Heaps (VK_EXT_descriptor_heap)
The scene binding model no longer uses descriptor sets, pools, or set layouts.
Instead the engine owns **one resource heap and one sampler heap** — plain,
device-addressable buffers created with `VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT`
(see `Vk::HeapManager` / `Vk::DescriptorHeap` in `src/render/`):

* Descriptors are produced on the host with `vkWriteResourceDescriptorsEXT` /
  `vkWriteSamplerDescriptorsEXT` and written directly into the mapped heap
  memory (with a cache flush). They are opaque bytes — there are no
  `VkDescriptorSet` objects at all.
* Command buffers consume the heaps after `vkCmdBindResourceHeapEXT` /
  `vkCmdBindSamplerHeapEXT`. The tail of each heap buffer is reserved for the
  implementation (`minResourceHeapReservedRange` / `minSamplerHeapReservedRange`).
* Legacy set/binding decorations in the (unchanged) Slang shaders are remapped
  onto the heaps at **pipeline creation** through
  `VkShaderDescriptorSetAndBindingMappingEXT` chains
  (`ZHLN_GraphicsPipelineDesc::descriptor_heap`, `PipelineBuilder::HeapMappings`).
  No shader changes were required.
* Per-draw data travels through `vkCmdPushDataEXT` (`Vk::PushData` /
  `CommandEncoder::PushDrawData`); legacy `push_constant` blocks in SPIR-V read
  the push-data blob directly.
* Compute dispatch APIs take logical thread counts. `ComputePass` reflects the
  SPIR-V `LocalSize` emitted from Slang's `[numthreads]` and derives Vulkan
  workgroup counts, so host code never duplicates shader-local dimensions.
  Fixed-domain kernels can also publish their logical dispatch size through
  reserved specialization-constant metadata, removing the domain from C++.
* The per-frame scene buffers (frame UBO, lights, instances, joints, morph
  deltas) are selected with `VK_DESCRIPTOR_MAPPING_SOURCE_PUSH_ADDRESS_EXT`
  mappings. Each heap segment pushes their device addresses once at the field
  offsets reflected from `DescriptorHeapPushData` in
  `descriptor_heap_layout.slang` (`RenderContext::Impl::BindHeapsAndPushFrame`).
* The bindless `globalTextures[]` array is a contiguous region of the resource
  heap pinned by a `HEAP_WITH_CONSTANT_OFFSET` mapping
  (`RenderContext::Impl::WriteTextureSlotToHeap`); instance-data texture
  indices remain unchanged.

State-model caveat: heap/push-data commands and legacy descriptor-set/push-
constant commands invalidate each other within a command buffer. Ported passes
call `BindHeapsAndPushFrame()` at the start of their segment; the remaining
legacy passes (HiZ, cluster culling, volumetric, post-processing, ImGui) still
use descriptor sets and are ordered so their invalidations are harmless.

### Descriptor Bindings (heaps only)
The descriptor-set DSL (`DescriptorLayout<...>`, descriptor pools, set
layouts) has been removed: every pass now reflects its binding structure from
SPIR-V (SPIRV-Reflect in `UnsafeReflectedLayoutBuilder`), bakes it into a
`VkDescriptorSetAndBindingMappingEXT` table (`HeapBindings.hpp`), and writes
descriptors into the heaps via `WriteHeapBindings` /
`vkWriteResourceDescriptorsEXT`. Pass argument order mirrors the shader's
set-0 declaration order; `SkipWrite` marks trailing sampler slots.

---

## 5. End-to-End Walkthrough

Here is a typical usage pattern for allocating a mesh, configuring a material, and submitting it to the renderer:

### Step 1: Initialize Resources (Initialization Phase)
```cpp
// 1. Create a vertex buffer
std::vector<Vertex> vertices = { ... };
BufferHandle vbo = renderContext.CreateVertexBuffer(vertices.data(), vertices.size() * sizeof(Vertex));

// 2. Create a material
PipelineDesc materialDesc = {
    .vertexShaderData = vertexShaderCode,
    .vertexShaderSize = vertexShaderSize,
    .fragShaderData = fragmentShaderCode,
    .fragShaderSize = fragmentShaderSize,
    .doubleSided = false,
    .alphaBlend = false
};
Material material = renderContext.CreateMaterial(materialDesc);
material.albedoIndex = renderContext.CreateTexture(pixels, width, height);

Mesh mesh = { .vertexBuffer = vbo, .vertexCount = vertices.size() };
```

### Step 2: Record and Render (The Frame Loop)
```cpp
// 1. Process Window Events and Begin the Frame
renderContext.BeginFrame();

// 2. Set Scene View-Projection Matrices
Renderer::SetMatrices(renderContext, camera.GetViewProj(), camera.GetUnjitteredViewProj());

// 3. Populate and submit lights
GPULight lights[1] = { ... };
Renderer::SetLights(renderContext, lights, 1);

// 4. Submit active meshes to the dynamic draw queue
JPH::Mat44 transform = JPH::Mat44::sTranslation({0.0f, 0.0f, 0.0f});
Renderer::Draw(renderContext, material, mesh, transform, transform);

// 5. Submit UI layers
Renderer::DrawUI(renderContext, textMesh, fontAtlasTextureIndex);

// 6. Resolve, Cull, Draw, and Present
renderContext.EndFrame(); // drawQueue is automatically sorted, culled, rendered, and cleared here
```
