# Renderer refactor — plan and status

The goal is to break `RenderContext::Impl` (1098 lines, `src/render/RenderInternal.hpp:593–1690`)
into managers that are injected with the hardware they need and never reach back
through `RenderContext`. Target shape:

```
RenderContext::Impl  ->  Managers  ->  GPU primitives
```

strictly acyclic, no manager knows `RenderContext` exists, no manager includes
`RenderInternal.hpp`.

Alongside that, the layer boundary the engine already draws for meshes and fonts
has to hold for textures: **`src/render` converts buffers into GPU resources; it
does not open files, parse containers or decode image formats.**

---

## Done

### `TextureManager` — DI, and out of the asset business

`TextureManager` was doing three jobs at once: VFS reader, image decoder and
bindless slot allocator. It called `PrefabFactory::LoadTexture(rc, cwMgr, ...)`,
which called `rc.CreateTexture(...)`, which called back into `Impl` — a cycle
through the public API — and it held a `std::vector<uint32_t> cpuPixels` per
procedural texture in case of a device loss.

It now does one job. Injected at construction with
`(Vk::Context&, Vk::Allocator&, Vk::StagingRingBuffer&, Vk::CommandRing<Graphics,8>&, Vk::HeapManager&)`
from `Impl`'s initializer list, in declaration order, so there is no reach-back
and no cycle. It owns:

- the `globalTextures[]` region (reserves it itself, reports the base through
  `BindlessBaseSlot()`),
- the slot arrays, free list and per-parity pending-release queues that used to
  be loose `Impl` members (`textureImages`, `textureViews`, `nextTextureIndex`,
  `freeTextureIndices`, `pendingTextureFrees`, `textureHeapBase`),
- `Adopt` / `ReleaseSlot` / `BeginFrame` / `Upload2D` / `UploadCube`, which were
  `Impl::AdoptBindlessTexture` / `ReleaseBindlessTexture` / `ReclaimTextureSlots`
  / `WriteTextureSlotToHeap` / `CreateTextureInternal` / `CreateTextureCubeInternal`.

It no longer contains:

- `#include <stb_image.h>` or any decode,
- `#include <Zahlen/AssetManager.hpp>`, `AssetManager`, `LoadSync`, or a path,
- `cpuPixels` — replaced by an 8-byte `pixelHash`, which is all the table needs
  to answer "is this the same content I already have a slot for". The dedupe
  that `recreating_a_procedural_texture_reuses_its_bindless_slot` asserts still
  holds; the renderer is just no longer the second owner of every image's CPU
  copy. Regeneration after a device loss is the caller's job, which is what
  `PrefabFactory::RebuildVulkanResources` already does for the font atlas.

Teardown is self-contained: `Clear()` gives the slots back itself instead of
returning a `std::vector<uint32_t>` for `ClearGPUCaches` to loop over.

**Verified:** `src/render/TextureManager.cpp` compiles clean (clang 21, `-std=c++26`,
`-Wall -Wextra -Wpedantic -Wshadow -Wold-style-cast -Wcast-align -Wunused
-Wnull-dereference -Wimplicit-fallthrough -Wundef`) — 0 errors, 0 warnings. A
separate compile-only harness exercised every method every call site uses
(`ReserveBindlessRegion`, `BindlessBaseSlot`, `Upload2D`, `UploadCube`, `Adopt`,
`Upload`, `RegisterUploaded`, `GetBindlessIndex`, `Unload`, `Clear`,
`OnDeviceLost`, `BeginFrame`, `ReleaseSlot`, `SlotCount`, `Image`, `View`,
`NameSlots`, `Rgba8Format`) and also compiles clean.

**Not verified:** `RenderInternal.hpp` and the other 11 render translation units
this touched. They need `GeneratedGpuTypes.hpp` and `ShaderBindings.hpp`, which
come from building and running `tools/zshader`, so they cannot be compiled in
the sandbox. Every changed call site had its receiver's declared type confirmed
by hand (`Impl&`, `Impl*`, or an `Impl` member function) rather than by grep
alone.

---

## Next, in order

### 1. Finish the texture layering (small, same violation class)

- **`src/render/RenderResources.cpp:794`** still calls `stbi_load_from_memory` on
  `Resource::blue_noise_png` to build the volumetric-fog noise tile. Same
  violation the `TextureManager` decode was: the renderer decoding an image
  format. Move the decode out and hand the renderer pixels, or better, bake the
  tile at cook time and read it through the cooked-texture path.
- **`PrefabFactory::LoadTexture`** (`src/engine/PrefabFactory.cpp:178`, declared
  `include/Zahlen/PrefabFactory.hpp:54`) now has **zero callers in the tree**.
  It is on the correct side of the boundary — engine reads the VFS, decodes with
  stb, hands pixels to `RenderContext` — so it is the natural home for the
  pipeline below. Either retire it or grow it into the real texture loader:
  `CookedTextureHeader` unpack in production, `stb_image` in dev, decoded pixels
  out, `rc.CreateTexture` + `rc.RegisterTexture` in. Decide which before anything
  else starts calling it, so there is one path and not two.
- **The missing piece:** nothing in the engine currently loads a texture by
  path. `CookedTextureHeader` is declared next to `CookedMeshHeader` and
  `CookedFontHeader` in `include/Zahlen/AssetManager.hpp` and has no reader. A
  `TextureLoader` beside `src/gui/FontLoader.cpp` — decode into a
  `DecodedImage { const void* pixels; uint32_t width, height; VkFormat format; }`,
  cache through `FS::AssetCache<DecodedImage>`, then one call into the renderer —
  closes the gap and matches how fonts and meshes already work.

### 2. Extract the draw-command payload types

`src/render/DrawCommands.hpp`: move `NativeMesh`, `NativeMaterial` and the draw
command structs (currently `RenderInternal.hpp:358–591`) into their own header.

This is a prerequisite, not a preference. The forward-declare-plus-`unique_ptr`
escape from the include cycle does not work for a manager held by value in
`Impl`: `~Impl()` is defined inline in `RenderInternal.hpp`, so
`std::unique_ptr<Incomplete>` fails in its destructor. (`ForkReplayer` works
because it is complete by then.) The payload types have to move before any
manager header can be included by a render source without dragging the whole of
`Impl` in with it.

### 3. `DrawQueueManager`

Queues, lights, sort scratch. Absorbs `RenderDrawCommands.cpp`.
`SortDrawQueue()` is pure CPU (`queues.drawQueue` + three scratch arrays,
`RadixSort64`) and moves wholesale. `FlushLineQueue()` is not — it needs
`linePipeline`, `frames.lineVbos[frameIndex]`, `frames.lineVboAddresses[frameIndex]`
and `frames.instanceDataBuffers[frameIndex]`, so either those get injected too
or that one function stays in `Impl`. Decide when it is written, not now.

Note: `DrawCommand` holds `InstanceData` **by value**, and `InstanceData` is
`ZHLN::GeneratedGpu::InstanceData` from `tools/zshader`. So this manager cannot
be header-dependency-free the way `TextureManager` is. That is fine — it just
means step 2 has to happen first.

### 4. `TargetManager`, then `GeometryManager`

- `TargetManager` — `GraphResources`, `RecreateTargets`, `ResizeShadowTargets`;
  absorbs `RenderInitTargets.cpp`.
- `GeometryManager` — mesh/material tables, `Create{Vertex,Index,Storage}Buffer`,
  `DestroyBuffer(handle, DeletionQueue&)`, skinned scratch, `UpdateJointMatrices`,
  `OnDeviceLost`; absorbs `RenderResources.cpp` and `ReconcileEntityBuffers`.

### 5. `PipelineRegistry`

The passes plus shader hot-reload (`RenderInitScenePipelines.cpp`,
`RenderInitPostProcess.cpp`).

### 6. The hardware bundle

Once three or more managers take the same five references, fold them into one
`struct GpuHardwareContext { Vk::Context&; Vk::Allocator&; Vk::HeapManager&;
StagingRingBuffer& staging; StagingRingBuffer& transfer; Vk::DeletionQueue&; }`
in `RenderInternal.hpp`, taken as `const GpuHardwareContext&`. Not before: two
call sites do not justify a type, and `TextureManager` proves the explicit form
reads fine.

---

## Standing constraints

- `src/vulkan` is a leaf: Vulkan, VMA, Volk. No engine or scene concepts, and no
  new abstraction pushed into the RHI to make a manager work. `Vk::Context`
  stays lean through all of this.
- `src/render` is the renderer: scene passes, frame graph, shaders, scene state.
- Managers take their dependencies by constructor injection. No callbacks into
  `RenderContext`, no `RenderInternal.hpp` include.
- If a boolean feels awkward to move down without inventing an abstraction, it
  stays in `Impl`.
- A good abstraction does not enumerate, probe and wrap every optional feature
  in its public interface.

## Verification note

CI is the arbiter. In the sandbox, `src/render` cannot be compiled: it needs
`GeneratedGpuTypes.hpp` and `ShaderBindings.hpp` from `tools/zshader`, and the
15 `extern/` submodules are empty. What *can* be checked there is any render
source that does not include `GpuAbi.hpp` or `<ShaderBindings.hpp>` — which is
exactly why `TextureManager.cpp` was compilable and the rest were not. Prefer
splitting work so the parts that can be compiled are the parts that get compiled.
