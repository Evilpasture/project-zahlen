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

### Descriptor Set Definition
Descriptor sets are declared using a static template DSL, defining binding slots, types, and shader stages at compile-time:
```cpp
using MaterialLayout = Vk::DescriptorLayout<
    Vk::SampledImageSlot<0, VK_SHADER_STAGE_FRAGMENT_BIT>, // Texture binding at slot 0
    Vk::SamplerSlot<1, VK_SHADER_STAGE_FRAGMENT_BIT>       // Sampler binding at slot 1
>;
```
You write updates to these descriptor sets using the `Write` interface, which uses compile-time checks to ensure that only `ImageWrite` structs are passed to Image slots, and `SamplerWrite` structs are passed to Sampler slots:
```cpp
MaterialLayout::Write(device, descriptorSet, 
    Vk::ImageWrite{.view = textureView},
    Vk::SamplerWrite{.sampler = linearSampler}
);
```

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
