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

### 5a. `GeometryManager` — the buffer handle table and allocation

`src/render/GeometryManager.{hpp,cpp}` plus `src/render/GenerationalPool.hpp`.
Owns the buffer table (`meshPool`, 34 sites), `CreateGPUBuffer`, the four
`Create*Buffer` entry points, `UpdateBuffer` and `DestroyBuffer`. `Impl` loses
the pool, the buffer allocator and 2 methods; `RenderInternal.hpp` 1678 → 1597
lines. Seven functions in `RenderResources.cpp` collapsed to one-line forwarders.

`GenerationalPool` came first and on its own: it lived inside
`RenderInternal.hpp`, and a manager cannot include the renderer's private header
without knowing the context exists — which is the one thing DI here forbids. It
is a self-contained template over Core's `ObjectPool`, so it lifted out clean.

**Scope was cut deliberately, three ways:** `materialPool` stayed put (pipeline
state, step 6); the skinned-scratch buffers stayed put
(`CreateSkinnedScratchBuffer` writes `&rtCtx` into the `NativeMesh` for BLAS
cleanup — a dependency on the object rather than on a flag, and `rtCtx` at
`RenderInternal.hpp:1025` is declared after the manager would be constructed);
and the asset caches, particle buffers, entity reconciliation and joint matrices
are 5b.

**Usage flags come from the caller.** Whether a buffer may feed an acceleration
structure depends on `rtCtx`, whose feature is not enabled on hardware without
ray tracing, so adding the bit unconditionally would violate its VUID there.
`Impl::BufferUsageWithRT()` makes that one decision where `rtCtx` lives and the
manager obeys. Injected: `Vk::Context`, `Vk::Allocator`, the transfer staging
ring and command ring, and the deletion queue.

**Verified:** `GeometryManager.cpp` compiles clean under the project warning set
(3,275,264 B, 0 diagnostics), as do `TargetManager.cpp`, `DrawQueueManager.cpp`
and `TextureManager.cpp`. `GenerationalPool.hpp` is pure CPU, so the
stale-handle invariant the whole table rests on actually runs: 12 assertions
covering distinct handles, per-handle resolution, a destroyed handle going dead,
slot reuse *with the old handle still refusing to resolve onto the new
occupant*, null and garbage rejection, and capacity overflow returning an
invalid handle while leaving live entries intact. All pass. All ten governance
checks pass. Member order (`ctx` 365 < `allocator` 374 < `transferRingBuffer`
384 < `transferCmdRing` 387 < `deletionQueue` 393 < `textureManager` 621 <
`geometry` 714) matches the init list.

**Two things found while wiring it up:**
- The mechanical rename produced `geometry.Resolve(h).value_or(nullptr)` against
  a `Resolve` that returned a raw pointer. `Resolve` now returns
  `std::expected<NativeMesh*, ResolveError>` exactly as the pool always did, so
  the 22 `.value_or(nullptr)` sites and the 3 that inspect *why* a handle failed
  are unchanged.
- `CreateVertexBuffer` divided by `stride` with no guard. The manager's version
  clamps to 1, so a zero stride no longer divides by zero. That is the one
  intended behaviour change here.

**Not verified:** the 27 renamed `meshPool` sites and the seven forwarders (they
pull in `GpuAbi.hpp` and the shader cook), and the CMake addition.

### 5b. `GeometryManager`, second half — asset caches and entity ledgers

Extends `GeometryManager` rather than adding a class: the asset caches, the
particle buffer cache and the three per-entity ledgers all answer questions
about buffers, which is what the manager already owns. `Impl` loses 6 more
fields. `ReleaseEntityBuffers` and `ReconcileEntityBuffers` — which were two
copies of the same four-container sweep differing only in their predicate — are
now one private `SweepLedgers(isDead)` with `ReleaseOwner` and `Reconcile` as
its two callers.

Pipeline retirement stayed out. `ClearGPUCaches` walks the material cache to
destroy `materialPool` entries, so the manager exposes `ForEachMaterial` and
`ClearMaterials` — the iteration, not the retirement — and step 6 picks that up.

**The plan for this step was wrong about joints, and they are not moving.**
`jointBuffers` is a member of `struct PerFrameResources` (`RenderInternal.hpp:445`),
the reflection-driven double-buffered bundle that `FlipAll()` walks with
`Reflect::ForEachField`. `UpdateJointMatrices` maps
`frames.jointBuffers[presenter.frameIndex]` — per-frame state indexed by the
presenter. Lifting it out would either break the reflection-driven flip or
inject the presenter into a geometry manager, and neither buys anything.
`AllocateMorphDeltas` is a bump allocator over a persistent arena, which is not
a handle-table entry either. Both stay.

**Verified:** `GeometryManager.cpp` compiles clean under the project warning set
(3,461,968 B, 0 diagnostics), as do the three managers before it. The pool test
from 5a still passes. All ten governance checks pass.

**Not verified, and this is a real gap:** the sweep and registry logic could not
be *executed*. `GeometryManager.cpp` compiles but does not link here — it needs
`src/vulkan/core/RenderCore.c`, `Context.cpp`, `Commands.cpp`, VMA's
implementation and SPIRV-Reflect, which is most of the Vulkan module. I got the
undefined-symbol count from 26 to 24 by compiling `volk.c`, `Allocator.cpp` and
`Raytracing.cpp`, then stopped: the remaining 24 would have meant hand-writing
stubs for Vulkan entry points, and a test running against my own stubs proves
nothing about the shipped code. So the sweeps were checked by line-by-line
comparison against the code they replace instead — same predicates, same
containers, same order, and the `!= Invalid` guard that used to be spelled out
at each of the seven mesh buffers now lives inside `Destroy`. The 30-odd renamed
call sites and the CMake addition are unverified for the usual reason.

### 6a. `PipelineRegistry` — the compiled material pipeline table

`PipelineRegistry` owns `materialPool` under the name `_materials`, plus the two
functions that produced entries: `Impl::CreatePipelineMaterial` and the
file-local `BuildMeshVariant`. `Impl` loses 1 field and 1 method. Its five
injected references are the device context, the driver pipeline cache, the
scene registry's heap mapping bundle, GPU diagnostics and the empty pipeline
layout — every one of them a precondition of a correct pipeline: build against
the wrong layout or the wrong heap mappings and the result is a device fault,
not a validation warning.

`PipelineDesc` and `ActiveGBuffer` moved out of `RenderInternal.hpp` into
`src/render/PipelineDesc.hpp` first, the same move step 2 made for the draw
payloads. The plan assumed `PipelineDesc` was blocked on the generated shader
header because it holds `ZHLN_ShaderDesc`; that was wrong. `ZHLN_ShaderDesc` is
the RHI's own C struct at `src/vulkan/core/RenderCore.h:371` — bytes, size and
entry point travelling together. Only the shader *catalog* that fills those
descriptors in is generated, and it stays on the caller's side of this
boundary: the registry is handed a description and never looks up a module. So
the registry is fully compilable in the sandbox, and it is.

The named per-pass pipelines — decal, line, CSG, the particle pair, the post
chain — did not move. They are one-per-pass singletons owned by the code that
records into them, not entries in a table; moving them would relocate a field
without establishing a registry, and `Impl` still owns all 13.

**Verified.** `PipelineRegistry.cpp` compiles at 3,784,832 B with 0 diagnostics
under the full warning set; `GeometryManager` (3,461,968 B), `TargetManager`
(3,464,184 B), `DrawQueueManager` (488,520 B) and `TextureManager` (3,990,272 B)
still compile at 0 diagnostics, so the extraction of `PipelineDesc` and
`ActiveGBuffer` broke no consumer. All ten `configure/check_*.py` exit 0.
`GenerationalPool` now has a second instantiation, so its contract test was
rebuilt against the real `BufferHandle`/`PipelineHandle` types and all four
`Resolve` error branches: `ALL PASS`, 14 assertions, including that a handle
freed and then re-created on the same slot refuses to resolve the new occupant.
Payloads in that test are stand-ins — `NativeMesh`'s destructor reaches VMA and
the thread-local deletion queue, which cannot be linked here. `CreateMaterial`
itself was not executed; it was ported line for line from
`RenderResources.cpp:415`. The `RenderInternal.hpp` field removal, the four
rewired call sites and the CMake addition are unverified for the usual reason.


### 6b. `ShaderReloadRegistry` — shader file to rebuild closure

`ShaderReloadRegistry` owns the table that makes shader hot-reload work: a name,
the normalised source paths it was compiled from, and the closure that rebuilds
it. `Impl` loses the `ShaderReloadRegistration` struct, the `shaderReloads`
vector and both `RegisterShaderReload` overloads; four call sites now go through
`shaderReloads.Register`. The member kept its name, so the diff at those sites is
one token.

It takes **no injected references at all** and makes no Vulkan calls, which put
it in `DestinationRegistry`'s category rather than the GPU managers'. The one
Vulkan thing in the old loop — `vkDeviceWaitIdle` before the first rebuild — stays
in `Impl` and is passed in as `onFirstMatch`, so it still runs once and only when
something actually matched.

Three invariants moved with it and are now written down where they are enforced:
the entry count is snapshotted before iterating, because a rebuild may register a
new entry that was compiled from the old file; each callback is copied before it
is called, because a rebuild commonly re-registers its own name (the CSG group
does) and re-registration replaces the entry in place, moving the closure out
from under the reference the loop holds; and re-registering replaces rather than
appends, which is what keeps index iteration valid. Nothing removes entries — if
a removal is ever added, that last one stops holding.

Also deleted: `PipelineRegistration` and `Impl::RegisterPipeline`, which had no
call site anywhere in the repo. `RegisterAndBuild` in `init/PassDescriptors.hpp`
is the live equivalent and does the same build-then-register.

**Verified, and executed rather than just compiled** — this is the first manager
since `DrawQueueManager` that links in the sandbox, because it touches no Vulkan.
`/tmp/h/srr_test` runs nine groups covering: a match running the closure with
`onFirstMatch` firing once for two matches; a non-match leaving the device alone;
re-registration replacing in place; the self-re-registering callback that used to
risk a moved-from `std::function`; an entry appended mid-dispatch being skipped
this pass and running on the next; `..` path normalisation matching the watcher's
form; and the four rejected registrations. `ALL PASS`. `ShaderReloadRegistry.cpp`
compiles at 0 diagnostics under the full warning set in **both** `-DZHLN_DEV_MODE`
and release, since `Register` is gated on `isDev` internally and one caller
registers unconditionally. The other five managers still compile at 0
diagnostics, the `PipelineRegistry` contract test still passes, and all ten
`configure/check_*.py` exit 0.

Not verified: `RenderResources.cpp`, `init/PassDescriptors.hpp` and
`init/RenderInitScenePipelines.cpp` cannot be compiled here (generated shader
ABI), so the four rewired call sites and the CMake addition are checked by
inspection — including that `shaderReloads` sits in the public part of
`struct RenderContext::Impl`, which is what lets `PassDescriptors.hpp` reach it
directly.


## Next


### 6c. The 13 named per-pass pipelines — still recommend leaving them

`RenderInitScenePipelines.cpp` and `RenderInitPostProcess.cpp` build the named
members: the shadow pair and its mesh twin, decal, line, the particle pair, the
three CSG pipelines, plus their raw layout aliases. Each is a singleton owned by
the one pass that records into it, and each is already hot-reloadable through the
registry that 6b extracted. Moving them into `PipelineRegistry` would put keyed
table entries and unkeyed singletons in one class and change no call site's
shape; they would move as a group only if a pass object materialised to own them,
and nothing calls for one yet.

### 7. `GpuHardwareContext` — **recommend dropping this from the plan**

The plan gated this on three or more managers taking the same references. That
threshold is met by exactly two of its six fields. Counted across all four
GPU-touching managers:

| Injected reference | Managers taking it |
| :--- | :--- |
| `Vk::Context&` | 3 — Texture, Target, Geometry |
| `Vk::Allocator&` | 3 — Texture, Target, Geometry |
| `CommandRing<Graphics, 8>&` | 2 — Texture, Target |
| `StagingRingBuffer&` | 1 — Texture |
| `CommandRing<Transfer, 8>&` | 1 — Geometry |
| `HeapManager&` | 1 — Texture |
| `Vk::DeletionQueue&` | 1 — Geometry |

A bundle of six fields where four are used once would be paid for by every
reader of every constructor signature to save nothing: the four single-use
references would still be named individually, just as members. `PipelineRegistry`
made the point again from the other direction — it takes five references, four
of which no other manager takes, and its signature is clearer spelled out than
it would be as a bundle plus three stragglers.

The condition the plan set is not going to be reached by the work that is left,
because the remaining work is passes, not new managers. So: close it unless a
fifth GPU manager appears that shares the `StagingRingBuffer`/`HeapManager`
pair.

| Step | Action | Boundary |
| :--- | :--- | :--- |
| 1a | ~~Blue-noise decode out of `src/render`, drop `extern/stb`~~ **done** | Cooked at build time; VFS route blocked by Kernel init order. |
| 2 | ~~Extract `DrawCommands.hpp`~~ **done** | Payload types only; no `GpuAbi.hpp`, no target types. |
| 3 | ~~`DrawQueueManager`~~ **done** | Queues + CPU sort. No buffer mapping, no pipeline. |
| 4 | ~~`TargetManager`~~ **done** | `GraphResources`, target recreation, shadow resize. |
| 5a | ~~`GeometryManager`~~ **done** | Buffer table + allocation. No pipelines, no RT. |
| 5b | ~~`GeometryManager`, second half~~ **done** | Asset caches + entity ledgers. Joints stay: per-frame state. |
| 6a | ~~`PipelineRegistry`~~ **done** | Material pipeline table + `PipelineDesc` extracted. |
| 6b | ~~`ShaderReloadRegistry`~~ **done** | Shader hot-reload table. No injected refs; tested. |
| 6c | 13 named per-pass pipelines | **Recommend leaving**: per-pass singletons, already reloadable. |
| 7 | ~~`GpuHardwareContext`~~ **recommend dropping** | Threshold met by 2 of 6 fields; 4 are single-use. |

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
`GpuAbi.hpp` nor `<ShaderBindings.hpp>` — which is why `TextureManager.cpp` was
compilable early on and why step 2's header is worth keeping free of `GpuAbi.hpp`.
Note that the generated header is narrower than it looks: `ZHLN_ShaderDesc`
comes from `src/vulkan/core/RenderCore.h`, so a type that *holds* a shader
description is not blocked by it, only one that *names* a module from the
catalog. `PipelineRegistry` compiles on that basis. Prefer splitting work so the parts
that can be compiled are the parts that get compiled.

Formatting is matched by hand to the surrounding code. `clang-format` 23 is not
the version this tree was formatted with: run over these files it collapses the
`HeapMappingBuilder` chains that `RenderInitHeaps.cpp` keeps multi-line in four
places, so applying it adds churn to lines no edit touched.
