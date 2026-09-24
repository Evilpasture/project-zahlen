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
   │  - RAII Resource Wrappers (Buffer, Image, Pipeline)       │  <-- src/vulkan/
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

### Vulkan Loading (Volk)

The renderer does **not** link the Vulkan loader. [Volk](https://github.com/zeux/volk) (pinned at `extern/volk`, tag matching the CI SDK version) acquires the loader at runtime and dispatches through its own function pointers:

* `vk*` names are Volk dispatch pointers, not loader prototypes — call sites are unaffected, but every pointer is `NULL` until the loader is acquired. `volk.h` therefore owns the Vulkan includes everywhere (`RenderingPCH.h`, `RenderCore.h`) and must be included *before* any header that pulls in `<vulkan/vulkan.h>`.
* `ZHLN_EnsureVulkanLoader()` (RenderCore.c) is a stateless `volkInitialize()` — idempotent, race-safe. `ZHLN_CreateInstance()` and the pre-instance helpers (`ExtensionBuilder::ForInstance()`, `EnumerateInstanceExtensions()`) call it before touching any dispatch pointer.
* `ZHLN_CreateInstance()` calls `volkLoadInstance()` right after instance creation; `ZHLN_CreateDevice()` calls `volkLoadDevice()` so device-level commands hit the driver's entry points directly, skipping the loader trampolines. The engine is single-device; multi-device would need `volkCreateDeviceTable()` per device.

This keeps tools and executables runnable on machines without a loader installed (clean `ZHLN_EnsureVulkanLoader()` failure instead of a missing-library abort at process start) and removes loader overhead from the hot paths.

### Diagnostics Ownership (Vk::Instance)

The C layer is **stateless** — no counters, no globals. `Vk::Instance` (src/vulkan/core/Instance.hpp) owns the Vulkan instance and the persistent debug messenger, and routes diagnostics into **caller-owned storage**; the library keeps no post-mortem state:

* An observer that needs values to outlive an engine (the test framework) registers a sink — `RenderContext::UseDiagnostics(&validationErrors, &deviceLost)` — before creating engines. Every instance created afterwards increments those atomics **directly**, including teardown-time events fired while the instance is being destroyed, so per-test before/after snapshots bracketing a whole engine lifecycle are exact. There is no retirement fold and none is needed: the storage is the single source of truth and it already outlives the engine.
* `RenderContext::ValidationErrorCount()` / `RenderContext::DeviceLostCount()` are **live views**: the active instance's counters, zero while no engine exists. Workload-scoped snapshots inside a running engine (RenderPerformance, RTR, mesh shaders, …) use these. Unregistered engines count into per-instance members, and those counts die with the instance.
* The instance descriptor carries a `ZHLN_DebugForwarding` (hook + owner pointer); both the pNext messenger (instance create/destroy) and the persistent messenger (runtime) forward error severities into the counting target. The stateless behaviors (stderr logging, the GPU-AV out-of-bounds abort) stay in the C callback.
* `Vk::Instance::IncrementNumericalDeviceLoss()` is the diagnostics increment for `VK_ERROR_DEVICE_LOST` observed on void paths; it bumps the active instance's counter and is unobservable when no engine is live. Nothing reads the counter for control flow -- recovery rides the monadic frame-result chain.
* `Vk::Instance` is move-aware: the C-side forwarding pointer is re-pointed on every move, so builder-to-context transfers keep the hook valid.
* The engine is **single-instance** by design — volk's dispatch tables are process-global and cannot serve two live instances. `Instance::Create()` claims the slot with a compare-and-swap and refuses (returning an invalid instance) while another is live, instead of letting a second one silently steal it. Sequential create/destroy cycles lose nothing.

### One dispatch table per image

Volk's table is **per-image** (`visibility(hidden)` on the pointers, by volk design), while its entry points (`volkInitialize`, `volkLoad*`) are exported. If an executable embeds `zahlen_vulkan`'s archive *and* links `libzahlen_engine.so` (the extras GPU tests do, through the extras targets they link), the executable's copy of those entry points preempts the engine's calls: the loader gets acquired into the *executable's* table while the engine's stays `NULL`, and the first `vk*` call jumps to `0x0`. `cmake/zahlen_engine.map` therefore localizes the RHI's symbols (`volk*`, `ZHLN_*`, `vma*`, `ZHLN::Vk` mangled names) inside the engine `.so`, binding them at link time. Consequences:

* The engine `.so` always initializes and dispatches through **its own** table, regardless of what an executable embeds.
* An executable-embedded copy has its own table, global-level initialized on demand via `ZHLN_EnsureVulkanLoader()` — but it never sees the engine's instance/device pointers, so **executable-side code must not call device-level `vk*` directly**; it goes through the engine's (or renderer's exported) API. Windows PE and macOS two-level namespaces bind intra-image by default and don't need the script.

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

### Images You Do Not Own (`ImageSlice`)
Not every image a pass draws into is ours: a swapchain image belongs to the swapchain, a render texture to the bindless arrays that publish it. Those arrive as a `Vk::ImageSlice` -- handle, view, extent, format -- and become the `TypedImage` a pass records against when the pass says which layout it is in:
```cpp
// `slice` is the image; the layout is the pass's to declare, not the image's to have
const auto image = slice.Assume<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>();
```
`Vk::RenderTarget<F>` is the owning counterpart (VMA allocation, compile-time format, the view created with it), and `Vk::AssumeLayout<L>(target)` converts one of those; `Vk::MakeSlice(...)` builds a slice from the raw pieces a 2D image arrives as.

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
The backend source is compiled as part of `zahlen_vulkan`, where Vulkan handles
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

### Stencil Presets

A `VkStencilOpState` is eight fields, and the two states a pass actually wants —
"stamp a tag" and "test the tag" — differ in two of them. `PipelineBuilder`
carries those two as presets (`pipeline/PipelineBuilder.hpp`), each of which
installs the state on both faces:

```cpp
// CSG Write: replace the stored value with 1 wherever the volume passes,
// colouring nothing while it does.
.ColorWriteEnable(false)
.StencilWriteMask(1)

// CSG Difference: draw only where the cutters did NOT stamp 1.
.StencilCompareMask(VK_COMPARE_OP_NOT_EQUAL, 1)

// CSG Intersection: draw only where the cutters did stamp 1.
.StencilCompareMask(VK_COMPARE_OP_EQUAL, 1)
```

The test is enabled by installing a state — there is no `StencilTest(bool)` to
leave behind or forget, because Vulkan ignores `front`/`back` while
`stencilTestEnable` is false and a pipeline whose state is silently not applied
is exactly the bug that shape invites. Both faces always get the same state;
`StencilOp(front, back)` remains for the rarer pipeline that wants them to
differ.

The C descriptor carries the same single fact (`ZHLN_StencilState*` in
`ZHLN_GraphicsPipelineDesc`, NULL = off), so the enable cannot be re-invented at
the boundary: `ZHLN_CreateGraphicsPipeline` reads it out of the pointer's
presence and refuses a state over a depth format with no stencil aspect
(`zhln_format_has_stencil`) instead of handing the driver a test with nothing to
apply it to. A pass that attaches a stencil view it does not test is still fine —
that is what the format member says, and what
`dynamicRenderingUnusedAttachments` covers (`DESCRIPTOR_HEAPS.md`).

The blend half is preset-shaped in the same way but not yet exposed: the C layer
composes each attachment from one of two named states (alpha, additive), the
caller's write mask, and nothing else. A descriptor naming more colors than
`ZHLN_MAX_COLOR_ATTACHMENTS` is refused by name
(`PipelineBuilderError::TooManyColorAttachments`) rather than blended by a table
shorter than the attachment count.

### Descriptor Heaps (VK_EXT_descriptor_heap)
The scene binding model no longer uses descriptor sets, pools, or set layouts.
Instead the engine owns **one resource heap and one sampler heap** — plain,
device-addressable buffers created with `VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT`
(see `Vk::HeapManager` / `Vk::DescriptorHeap` in `src/vulkan/`):

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
* `Dispatch`, `DispatchHeap`, and `DispatchHeapIndexed` use fixed logical
  domains reflected from `Dispatch.SizeX/Y/Z` (spec-constant IDs 1000-1002).
  Dynamic kernels use explicitly named `*Threads` overloads with runtime
  logical counts. Both paths reflect SPIR-V `LocalSize` from `[numthreads]` and
  derive Vulkan workgroup counts; raw groups require `DispatchGroups`.
* Named UBO / SSBO bindings are reflected from compiled SPIR-V
  (`ReflectedLayout.hpp`); push structs are read at compile time instead
  (`pipeline/SpirvLayout.hpp`), so a hand-written struct that does not mirror
  its `.slang` declaration fails the build. This leaf never sees `.slang`
  source and does not own engine type names, cluster math, or LUT bake policy.
* Per-draw device addresses travel through
  `VK_DESCRIPTOR_MAPPING_SOURCE_PUSH_ADDRESS_EXT`. Offsets come from
  `GpuAbi::kScenePushLayout`, which `src/render/GpuAbi.hpp` reads out of the
  compiled `DescriptorHeapPushData` at compile time and hands to the RHI's
  generic `Vk::HeapPushDataLayout` container as data.
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
SPIR-V (SPIRV-Reflect in `ReflectedLayoutBuilder`), bakes it into a
`VkDescriptorSetAndBindingMappingEXT` table (`HeapBindings.hpp`), and writes
descriptors into the heaps via `HeapManager::WriteHeapParameters` /
`vkWriteResourceDescriptorsEXT`. Each pass names its descriptors as
`Vk::Slot<"name">(value)` arguments (`src/vulkan/pipeline/DescriptorWrites.hpp`):
every value carries the name of the shader binding it fills, resolved against the
names SPIRV-Reflect reported for that pass's set, so argument order carries no
meaning and a binding a configuration drops does not shift the ones after it.
The write allocates the pass's whole block from the frame's transient partition
and returns its base (`Vk::HeapBlockBase`), which the dispatch pushes into the
mapping's index word: descriptors are written where they are consumed, one block
per dispatch, and no pass reserves a per-frame count of variants. Bakes recorded
outside the frame loop allocate from the immediate partition after
`HeapManager::BeginImmediate`. Samplers are initialized once by
`InitHeapPassSamplers` from `Vk::SamplerSlot<"name">` values, matched by name
the same way.

A name that matches nothing is skipped at runtime, which is right for a binding
a configuration dropped and wrong for a typo -- so the names are also checked
before the code links. A pass declares the modules it runs as types
(`<ShaderBindings.hpp>`: one type per cooked module, generated from the modules
by `tools/zshader` and grouped into `Vk::ShaderSet<...>` per descriptor block),
the write and sampler-init sites name
that set (`WriteHeapParameters<Shaders::Lighting>`,
`InitHeapPassSamplers<Shaders::Culling>`, `ComputeChain::Step<Shaders::BloomDown>`),
and `Vk::NamesAreDeclared` / `Vk::NamesCoverDeclarations` (ShaderProgram.hpp)
compare the names against the modules themselves at compile time in both
directions: a name no module of the set declares is a misspelling the compiler
prints with the name in it, and a declared binding the call does not spell is a
descriptor nothing writes. The generated lists are read out of the modules by
SPIRV-Reflect at build time, and the generated `ShaderBytecode.cpp` -- the one
translation unit that `#embed`s the modules -- walks each module's own bytes with
`Vk::SpirvBindings` (OpName and OpDecorate Binding / DescriptorSet in a constant
expression, stopping at the first `OpFunction`) and asserts the two agree, so the
tool and the compiler have to be right together. A binding the shader source declares and the cook strips is
written through `Vk::Unread` / `Vk::UnreadSampler` (DescriptorWrites.hpp), which
says so on purpose; and a stage, an entry point and a descriptor's set/binding
number all come from the module rather than from a second declaration beside it.
The old hand-maintained Python checker over SPIR-V text is gone: a renamed
parameter or an unwritten binding is now a build failure.

### Specialization Constants

A pass's specialization constants are a struct's fields, and the map table is
derived from them: `Vk::Specialization<T>` (`pipeline/Specialization.hpp`) walks
`T` with `Reflect::ForEachFieldInfo<T>` and records one
`VkSpecializationMapEntry` per field, in declaration order -- field N is
`constant_id` N, carrying that field's own `offsetof` and `sizeof` -- so the
ids, offsets and sizes the shader's `[[vk::constant_id(N)]]` declarations are
addressed by cannot drift from the struct the values live in. A site reads:

```cpp
struct SpecData {           // reflection.slang declares 0 and 1 in this order:
    int enableSSR = 0;      //   [[vk::constant_id(0)]] ENABLE_SSR
    int enableRTR = 0;      //   [[vk::constant_id(1)]] ENABLE_RTR
};

Vk::Specialization<SpecData> spec;
Reflect::ForEachFieldInfo<SpecData>(spec);

const std::array variants  = {SpecData {.enableSSR = 0, .enableRTR = 0}, /*...*/};
const auto       specInfos = spec.Infos(variants); // std::span<const VkSpecializationInfo>
```

The walk is a line at the call site, and that is load-bearing. A build without
`-freflection` compiles these sources through `zahlen_transpile_sources`, which
rewrites a `ForEachFieldInfo` call it can see into the per-field calls it stands
for; hidden inside this type's own constructor body the call would compile
against the no-op stand-in and the map would come out empty -- not a build
error, just every variant silently keeping the shader's `= 1` default. The
infos point at `variants` and at the object's own entry table, so both have to
outlive the pipeline build they are handed to, as the arrays they replace did.

An entry whose id a module does not declare is ignored by the driver, so a
module with fewer constants than `T` has fields is fine: the NoRT modules of
`lighting.slang` and `reflection.slang` are built from the same table as their
RT counterparts.

---

## 5. End-to-End Walkthrough

Here is a typical usage pattern for allocating a mesh, configuring a material, and submitting it to the renderer:

### Step 1: Initialize Resources (Initialization Phase)
```cpp
// 1. Create a vertex buffer
std::vector<Vertex> vertices = { ... };
BufferHandle vbo = renderContext.CreateVertexBuffer(vertices.data(), vertices.size() * sizeof(Vertex));

// 2. Create a material (raw shader-blob compilation is the internal
//    ZHLN::PipelineDesc / RenderContext::Impl::CreatePipelineMaterial pair in
//    src/render/RenderInternal.hpp, reserved for the engine's own shaders)
MaterialDesc materialDesc = {
    .doubleSided = false,
    .alphaBlend = false,
    .metallic = 1.0f,
    .roughness = 0.5f
};
Material material = renderContext.CreateMaterial(materialDesc).value();
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
Light lights[1] = { ... };
Renderer::SetLights(renderContext, lights, 1);

// 4. Submit active meshes to the dynamic draw queue
JPH::Mat44 transform = JPH::Mat44::sTranslation({0.0f, 0.0f, 0.0f});
Renderer::Draw(renderContext, material, mesh, transform, transform);

// 5. Submit UI layers
Renderer::DrawUI(renderContext, textMesh, fontAtlasTextureIndex);

// 6. Resolve, Cull, Draw, and Present
renderContext.EndFrame(); // drawQueue is automatically sorted, culled, rendered, and cleared here
```

## Presentation pacing

Each `SwapchainPresenter` owns a `PresentPacer` (`src/vulkan/presentation/PresentPacer.hpp`) that resolves one immutable
`ZHLN::PacingPolicy` at bring-up from device enablement plus surface capabilities, then paces presents accordingly:

| Policy | Present mode | Timing | Meaning |
|---|---|---|---|
| `PacedClosedLoop` | `FIFO_LATEST_READY_KHR` | `VK_EXT_present_timing` targets + feedback | Every present aims at its V-blank; feedback calibrates the next aim. |
| `AdaptiveVBlank` | `FIFO_LATEST_READY_KHR` | none | Stale queued frames are skipped at V-blank, untimed. |
| `Decoupled` | `IMMEDIATE_KHR` | none | V-sync off: uncapped benchmark mode, tearing allowed. |
| `LegacyVBlank` | `MAILBOX_KHR`, else `FIFO_KHR` | none | Classic tear-free V-blank pacing, open-loop. |

The closed loop is provisional until the first swapchain confirms it (actual present mode, timing queue, time domain);
a failure seals the presenter as `AdaptiveVBlank` instead. Rebuilds re-arm the timing state without re-resolving.
When the swapchain exposes no global time domain (a composited desktop), the loop schedules in the stage-local
clock anchored to the dequeue event: timed presents and paced simulation still engage, but the cross-stage slack
margin stays unknown and the fidelity governor stays inert.
The engine reads the pacer through `RenderContext::GetPresentTiming()` / `GetPacedDeltaTime()`:

* fixed-step simulation advances by the display-locked interval instead of the wall clock (closed loop, fixed refresh),
* input is re-pumped and re-translated just before intent (`Engine::PollLateInput`),
* the fidelity governor steps the quality preset down after 90 consecutive sub-2ms-margin frames (never up, never from
  Custom).

Headless sessions resolve `Decoupled` (vsync off) or `LegacyVBlank` with no paced timing behind them.
`ZHLN_NO_AUTO_QUALITY` disables the governor.
