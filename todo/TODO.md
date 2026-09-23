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

It no longer contains `#include <stb_image.h>`, `AssetManager`, a path, or
`cpuPixels` — that last replaced by an 8-byte `pixelHash`, which is all the table
needs to answer "is this the same content I already have a slot for". The dedupe
`recreating_a_procedural_texture_reuses_its_bindless_slot` asserts still holds;
the renderer just stopped being the second owner of every image's CPU copy.
`Load()` and `RebuildGPUResources(AssetManager&)` are gone; `Load()` had no
caller anywhere in the tree, so the only path from `src/render` into the VFS went
with it.

**Locking is documented, and deliberately narrow.** `_mutex` guards `_textures`
and nothing else. The record table is the one structure with two kinds of caller
— mutators from whatever thread owns the asset, readers from the draw-building
path that the parallel recorder fans out across worker threads — so it locks.
The GPU layer (`_slotImages`, `_slotViews`, `_nextSlotIndex`, `_freeSlots`,
`_pendingFrees`, `_bindlessBaseSlot`, `_frameIndex`) has one caller and is
render-thread-only by construction. The header records that as a load-bearing
assumption and names what has to change if a manager ever uploads from a worker.

**Verified:** `src/render/TextureManager.cpp` compiles clean under clang 21,
`-std=c++26`, with `-Wall -Wextra -Wpedantic -Wshadow -Wold-style-cast
-Wcast-align -Wunused -Wnull-dereference -Wimplicit-fallthrough -Wundef` — 0
errors, 0 warnings. A separate compile-only harness exercising all 18 public
methods also compiles clean. Confirmed running normally on real hardware.

**Not verified in the sandbox:** `RenderInternal.hpp` and the other render
translation units — they need `GeneratedGpuTypes.hpp` and `ShaderBindings.hpp`
from `tools/zshader`. Every changed call site had its receiver's declared type
confirmed by hand rather than by grep.

---

### `DrawCommands.hpp` — the payload types out of `RenderInternal.hpp`

The mandatory prerequisite for every manager that owns a queue.
`~Impl()` is defined inline in `RenderInternal.hpp`, so
`std::unique_ptr<Incomplete>` fails in its destructor and the
forward-declare-plus-pointer escape from the include cycle does not work for a
manager held by `Impl`. (`ForkReplayer` works because it is complete by then.)
The payload types had to move before any manager header could be included by a
render source without dragging all of `Impl` in behind it.

Moved: `NativeMesh`, `NativeMaterial`, `DrawCommand`, `CSGDrawCommand`,
`ParticleEmitterCommand`, `DecalDrawCommand`, `LineSegment`,
`MeshParticleEmitterCommand`, `RenderQueues`. `RenderInternal.hpp` is 115 lines
shorter and includes the new header at line 20.

Left behind deliberately: `ShaderStage` / `ShaderStageSource` /
`MakeStageSource` (shader plumbing, interleaved between the payload structs),
the `kGpuCullingMax*` budgets, `WorkerCmdContext` (parallel-recorder plumbing),
`SceneResources`, and the one `ClearColorOf<Res_TransLighting>` specialization
— the primary template lives in `src/vulkan/graph/RenderGraph.hpp:749`, so that
pair belongs with step 4, not with draw payloads.

Two include rules are what make the header compile-checkable on its own:
- It includes `<Zahlen/Render/GpuLayout.hpp>`, where `InstanceData` comes from
  (`GpuLayout.hpp:36`). `DrawCommand` holds `InstanceData` **by value**, so the
  generated header is a real dependency of the payload's layout, not an
  incidental include.
- It does **not** include `src/render/GpuAbi.hpp`. That is the header that makes
  a render TU uncompilable without the shader cook — `GpuAbi.hpp:52` does
  `#embed ZHLN_GPU_ABI_MODULE` and consteval-parses the cooked module. A header
  that needs only the *structs* compiles against a stubbed
  `<GeneratedGpuTypes.hpp>`; one that pulls in `GpuAbi.hpp` cannot.

**Verified:** `DrawCommands.hpp` compiles standalone against a stub
`GeneratedGpuTypes.hpp` carrying the nine structs `GpuLayout.hpp` aliases —
exit 0, 0 diagnostics, with all four pre-existing `static_assert`s
(`is_trivially_copyable_v<DrawCommand>` and friends) holding. Every consumer
(`RenderDrawCommands.cpp`, `RenderPasses.cpp`, `RenderResources.cpp`) reaches
the types through `RenderInternal.hpp`, confirmed file by file. The stub needs
Jolt on the include path too, since `LineSegment` and `DecalDrawCommand` are
JPH math — that is the one extra dependency beyond the Vulkan set.

**Not verified:** `RenderInternal.hpp` itself, which still needs the cook.
Braces balance (166/166) and nothing that should have stayed went missing, but
that is a static check, not a compile.

### 1a. Blue noise, cooked instead of decoded

Done. `src/render` no longer decodes an image format, and `extern/stb` is off the
render target's include path — so "no decoder in the renderer" is now enforced by
the build rather than caught in review.

It could not become a VFS asset, and the reason is worth recording because it
constrains anything else the renderer owns at startup: `InitializeBlueNoiseTexture`
runs inside `RenderContext::Create`, and the Kernel constructs its `AssetManager`
(`src/engine/Kernel.cpp:116`) and mounts `data/base.pak` (:122) only *after* that
returns. There is no asset system to read from yet. Routing it through the pak
would mean restructuring Kernel init and handing `RenderContext::Create` an
asset source — putting back the dependency that was just removed.

So the fix was to move the decode, not the delivery. `configure/cook_blue_noise.py`
turns the PNG into raw 8-bit RGBA at build time; `cmake/ShaderCompilation.cmake`
runs it as a custom command feeding the existing `--blob` mechanism, and
`InitializeBlueNoiseTexture` memcpys a block whose layout it asked for. The tile
is square by definition, so the extent comes off the byte count and the count is
what validates the blob.

Overhead went *down* at both ends. The tile is 1024x1024 of high-frequency
noise, so it barely compresses: 4202841 bytes as PNG against 4194304 raw, within
8KB. And the startup decode is gone.

The cook is stdlib-only on purpose — the build finds an interpreter and promises
no third-party packages, so a Pillow dependency here would make configure depend
on something configure does not install.

Still the same class of impurity, for later: `RenderInitHeaps.cpp:348` steps over
`ltc_mat`'s 128-byte DDS header by offset arithmetic. A real cooked-texture
container would cover it. `CookedTextureHeader` (`include/Zahlen/AssetManager.hpp:33`)
still has no writer and no reader, and `zcook CookTexture`
(`tools/zcook/Cook.cpp:123`) is a verbatim byte copy that nothing in the build
calls — that is where a texture-asset pipeline should land, and blue noise could
then move to the pak once something uploads renderer resources after init.

---

### 3. `DrawQueueManager` — queues and CPU sorting only

`src/render/DrawQueueManager.{hpp,cpp}`. Owns the six queues by value, the three
sort scratch arrays, `Sort()` and `Clear()`. `Impl` keeps the member name
`queues`, so all 71 access sites became a mechanical rename —
`queues.drawQueue` → `queues.Draws()`, and so on across
`RenderPasses.cpp` (35), `RenderDrawCommands.cpp` (13), `RenderFrame.cpp` (12),
`RenderGraphBuilder.cpp` (6), `RenderResources.cpp` (3), `RenderSetup.cpp` (2).
`Impl` lost four members and one method; `RenderInternal.hpp` no longer mentions
`SortItem`, `RadixSort64` or `RenderQueues` outside comments.

Queues are handed out by `Array<T>&` (const and non-const) rather than through
`Push`/`Pop`, because the passes mutate in bulk — `PrepareSceneFrame` resizes
`Draws()` to the culling budget and `ClearDrawQueues()` clears only two of the
six. An accessor per queue is the honest surface; a method per operation would be
a longer way to spell the same thing.

`FlushLineQueue()` stayed in `RenderDrawCommands.cpp` as planned — it needs
`linePipeline`, `frames.lineVbos[]` and the instance index, and the manager stays
free of all of it.

**One behaviour change, found by running the sort.** `RenderQueues::Clear()` was
`Reflect::ForEachField` over the six members. A runtime test of the manager
showed it clearing *nothing*: a build without generated descriptors visits zero
fields, so `Clear()` — called every `EndFrame` — silently leaves the previous
frame's draws queued. The real build has reflection, so this was not a live bug,
but a frame-lifecycle function that can fail by doing nothing is not worth the six
lines it saves. `Clear()` now names the six members; `RenderQueues` is a plain
aggregate and `DrawCommands.hpp` dropped its `Reflection/Structs.hpp` include.

**Verified:** `Sort()` is pure CPU, so unlike the rest of this refactor it runs.
A harness builds, links and executes it — ordering by `SortKey(material, mesh)`
over an unsorted queue with repeated materials, idempotence, the empty-queue
early return, all six accessors, `Clear()`, const access: 12 assertions, all pass.
`DrawQueueManager.cpp` and `TextureManager.cpp` compile clean under
`-Wall -Wextra -Wpedantic -Wshadow -Wold-style-cast -Wcast-align -Wundef`.
All ten `configure/check_*.py` pass under the exact no-argument invocation
`GovernanceChecks.cmake` uses. **Not verified:** the six callers
(`RenderPasses.cpp` and the rest pull in `GpuAbi.hpp` and the shader cook, so
they do not compile here), and the CMake source-list addition.

### 4. `TargetManager` — every render target and the shadow cascade cluster

`src/render/TargetManager.{hpp,cpp}`. Owns `GraphResources` (the 32-target
reflected bundle plus its `ReflectMetadata`), the 30 `Res_*` tags, and the shadow
cluster: the previous cascade map, both cascade view arrays, the punctual views
and the atlas's cube/2D views with their create infos. `Impl` loses 10 fields and
3 methods; `RenderInternal.hpp` 1801 → 1678 lines.

Injected: `Vk::Context&`, `Vk::Allocator&`, `Vk::CommandRing<Graphics,8>&` — the
same three-ref shape as `TextureManager`, not `DestinationRegistry`'s
zero-dependency one, because targets are GPU allocations. The plan note calling
`DestinationRegistry` the proven pattern was right about *held by value, no
reach-back* and wrong about the rest: that registry makes no Vulkan calls at all.

**The `src/vulkan` contract is what made this interesting.**
`ResourceBinder::AutoBind` (`src/vulkan/graph/RenderGraph.inl:484`) requires
`typename ContextImpl::GraphResources` *and* a data member literally named
`graphResources`. So `Impl` keeps both — a nested `using GraphResources =
TargetManager::GraphResources;` and `GraphResources& graphResources =
targets.Graph();`. The Vulkan module never learns a manager exists, and all 139
`graphResources.X` reads across nine files keep compiling untouched.

Layout transitions are *recorded*, not submitted: `RecordInitialLayouts(cmd)`
takes the caller's command buffer so target recreation stays inside the one
immediate submission that also clears the accumulation history and transitions
the presentation depth. Splitting that would add a device wait to every resize.

`CreateDefaultTarget` was an `Impl` member, which made target creation reachable
only from the thing the split exists to stop owning it. It is now the free
function `CreateColorTarget(allocator, ctx, ext, extra)` in `TargetManager.hpp`,
used by both the manager and the accumulation-history allocation.
`ShadowResolutionError` moved with `ResizeShadows` into the header.

**Two deliberate changes worth watching at runtime:**
- `InitShadows` submits through the 2-arg `ExecuteImmediate` (straight to the
  queue) where `InitShadowResources` used the 3-arg staging-ring overload. The
  body only records `TransitionLayout`, so nothing stages data and the staging
  timeline has nothing to stamp; `ResizeShadows` already used the 2-arg form for
  the same transitions. This avoids injecting a staging dependency the manager
  does not use, but it is a change in how that submit reaches the queue.
- Allocation order in `RecreateTargets` is unchanged (accumulation buffers, then
  the reflected bundle), so a failure still short-circuits the same way.

**Verified:** `TargetManager.cpp` compiles clean under the project warning set
(3,464,184 B, 0 diagnostics). A contract test builds a stand-in `Impl` shaped
exactly like the new one and runs the **real** `ResourceBinder::AutoBind` from
`src/vulkan` against it — that test is what caught `GraphResources` having been
spliced in at namespace scope instead of nested, which `TargetManager.cpp` alone
did not detect because unqualified lookup inside the class found it anyway.
`DrawQueueManager.cpp` and `TextureManager.cpp` still compile clean. All ten
`configure/check_*.py` pass. Member declaration order
(`ctx` 459 < `allocator` 468 < `graphicsCmdRing` 480 < `targets` 498 <
`textureManager` 715) matches the constructor's init list, so no `-Wreorder`.
**Not verified:** the nine callers of `graphResources`/the 42 renamed shadow
sites (they pull in `GpuAbi.hpp` and the shader cook), and the CMake addition.

## Next


### 5. `GeometryManager`

`meshPool`, `materialPool`, `assetMeshMap` / `assetMaterialMap`,
`Create{Vertex,Index,Storage}Buffer`, `DestroyBuffer(handle, DeletionQueue&)`,
skinned scratch, joint and morph buffers, `UpdateJointMatrices`, `OnDeviceLost`.
Absorbs the rest of `RenderResources.cpp` and `ReconcileEntityBuffers`, and takes
the line-vertex upload from step 3 if that belongs here rather than in
`PrepareSceneFrame`.

### 6. `PipelineRegistry`, then the hardware bundle

The passes plus shader hot-reload (`RenderInitScenePipelines.cpp`,
`RenderInitPostProcess.cpp`).

`GpuHardwareContext` lands last, and only once three or more managers take the
same references: `struct GpuHardwareContext { Vk::Context&; Vk::Allocator&;
Vk::HeapManager&; StagingRingBuffer& staging; StagingRingBuffer& transfer;
Vk::DeletionQueue&; }`, taken as `const GpuHardwareContext&`. `TextureManager`
proves the explicit five-parameter form reads fine, so two call sites do not
justify a type.

| Step | Action | Boundary |
| :--- | :--- | :--- |
| 1a | ~~Blue-noise decode out of `src/render`, drop `extern/stb`~~ **done** | Cooked at build time; VFS route blocked by Kernel init order. |
| 2 | ~~Extract `DrawCommands.hpp`~~ **done** | Payload types only; no `GpuAbi.hpp`, no target types. |
| 3 | ~~`DrawQueueManager`~~ **done** | Queues + CPU sort. No buffer mapping, no pipeline. |
| 4 | ~~`TargetManager`~~ **done** | `GraphResources`, target recreation, shadow resize. |
| 5 | `GeometryManager` | Pools, asset caches, buffer creation, scratch. |
| 6 | `PipelineRegistry`, then `GpuHardwareContext` | Passes and hot-reload; bundle last. |

---

## Standing constraints

- `src/vulkan` is a leaf: Vulkan, VMA, Volk. No engine or scene concepts, and no
  new abstraction pushed into the RHI to make a manager work. `Vk::Context`
  stays lean through all of this.
- `src/render` is the renderer: scene passes, frame graph, shaders, scene state.
  No file format, no container, no decoder.
- Managers take their dependencies by constructor injection. No callbacks into
  `RenderContext`, no `RenderInternal.hpp` include.
- If a boolean feels awkward to move down without inventing an abstraction, it
  stays in `Impl`.
- A good abstraction does not enumerate, probe and wrap every optional feature
  in its public interface.

## Verification note

CI is the arbiter; it compiles and runs on real hardware. In the sandbox
`src/render` cannot be compiled wholesale: it needs `GeneratedGpuTypes.hpp` and
`ShaderBindings.hpp` from `tools/zshader`, and the 15 `extern/` submodules are
empty. What *can* be checked there is any render source that includes neither
`GpuAbi.hpp` nor `<ShaderBindings.hpp>` — which is exactly why
`TextureManager.cpp` was compilable and the rest were not, and why step 2's
header is worth keeping free of `GpuAbi.hpp`. Prefer splitting work so the parts
that can be compiled are the parts that get compiled.

Formatting is matched by hand to the surrounding code. `clang-format` 23 is not
the version this tree was formatted with: run over these files it collapses the
`HeapMappingBuilder` chains that `RenderInitHeaps.cpp` keeps multi-line in four
places, so applying it adds churn to lines no edit touched.
