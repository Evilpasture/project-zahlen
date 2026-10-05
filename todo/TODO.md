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

Left behind deliberately: the `kGpuCullingMax*` budgets, `WorkerCmdContext`
(parallel-recorder plumbing), `SceneResources`, and the one
`ClearColorOf<Res_TransLighting>` specialization — the primary template lives in
`src/vulkan/graph/RenderGraph.hpp:749`, so that pair belongs with step 4, not
with draw payloads. (`ShaderStage` / `ShaderStageSource` / `MakeStageSource` were
left behind with them; they belong to the Vulkan layer, not to draw payloads,
and went in the entry below.)

**Update (seventh cut, the ABI leaves the public tree):** the structs are generated
now, not hand-written. `tools/zshader` emits `Particle`, `ParticleEmitterParams`,
`MeshParticleEmitterParams`, `Light` and `FrameUniforms` from the reflected layout
itself -- members at the offsets Slang seated them, the `_padN` between them, the
`alignas` a lane carries, and the per-member asserts -- and
`include/Zahlen/Render/RenderData.hpp` is gone with the alias branch that named it.
Nothing under `include/` names a generated type, so the engine authors its own
terms: `Zahlen/ParticleEmitterDesc.hpp` for the component parameters and
`Render/FrameData.hpp` (`FrameData`, `LightDesc`) for the frame, and
`src/render/GpuPack.cpp` packs all of them at the submit boundary -- emitters, mesh
emitters, lights (view-space position included) and the frame. `RenderContext` lost
`ParticleStride`/`MeshParticleStride`/`UploadParticles` and gained
`CreateParticleBuffer`/`CreateMeshParticleBuffer`/`DrawBillboards`, and the VFX
`RenderBatchGPU` stopped owning four storage buffers: it describes world-space quads
and hands over a `TextureHandle` and a blend, so `ExplosionComponent` holds no GPU
handles and `ExplosionSystem::Release` and its cleanup pass are gone with them (the
one `TestExplosion` assertion that used a handle to prove the effect was alive now
holds the simulation's own state instead). The `GpuLayout.hpp` rule below still
holds, now for every struct: `InstanceData`, `Particle3D`, `ClusterBounds`,
`ClusterVolume` and the five above are all generated. `GPUMeshlet` is unchanged --
still hand-written, still the one `[CxxSkip]`, for the reason recorded below.

Two include rules are what make the header compile-checkable on its own:
- It includes `"GpuLayout.hpp"` (renderer-internal since the public-API
  redesign; it was `<Zahlen/Render/GpuLayout.hpp>`), where `InstanceData` comes
  from. `DrawCommand` holds `InstanceData` **by value**, so the generated header
  is a real dependency of the payload's layout, not an incidental include -- and
  that is fine: this header is renderer-internal too.
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

Update: the redesign moved the generated types out of the public API entirely,
so only renderer translation units need this stub now -- the engine, its
extensions and its tests compile without the shader cook having run. Also: with an
empty-struct `GeneratedGpuTypes.hpp` stub, Jolt cloned from
GitHub (`jrouwe/JoltPhysics`, `-DJPH_DOUBLE_PRECISION -DJPH_OBJECT_STREAM`),
and the umbrella included first, a real render TU that avoids
`ShaderBindings.hpp` compiles as-is — `src/render/GeometryManager.cpp` did so
when `NativeMesh` became `Vk::AccelerationStructure`-owning. TUs that include
the generated `ShaderBindings.hpp` directly still need the zshader cook.

**Not verified:** `RenderInternal.hpp` itself, which still needs the cook.
Braces balance (166/166) and nothing that should have stayed went missing, but
that is a static check, not a compile.

### The shader stage sources — out of `RenderInternal.hpp`, into `src/vulkan`

The other half of what was left behind with the payloads, and it turns out to be
the part that never belonged in `src/render` at all: `VkShaderStageFlagBits` and
`Vk::ShaderProgram` are both Vulkan-layer types, and the rest of the struct is a
path, a byte span and an entry point — nothing about scene rendering, materials
or render passes. `ShaderStageSource` now lives in
`src/vulkan/pipeline/ShaderStages.hpp`, beside the `ShaderStagesView` /
`OwnedShaderStages` pair it feeds.

Three things went with it:

* `enum class ShaderStage` and `StageFlagOf()` are gone. The enum duplicated
  `VkShaderStageFlagBits` and stopped at vertex/fragment/compute, so a task or
  mesh module could not be named through it at all. `ShaderStageSource` is
  templated on the Vulkan flag itself, and `TaskStageSource` / `MeshStageSource`
  are the same template — the seam the mesh pipeline path can be re-routed onto
  later, if the cascade path wants the dev-mode disk override too.
* `MakeStageSource<Stage, Module>()` is `MakeStageSource<Module>()`. The module
  already carries its stage in `Module::Stage`, so the caller was restating what
  the compiler had, and the `static_assert` that caught the two disagreeing
  existed only to defend that redundancy. The stage is part of the source's type
  now, so a mismatch is a type error rather than an assertion.
* `LoadShaderData` still takes the source by const reference and still deduces
  the stage; only the parameter type changed, to the Vulkan flag.

`RenderInternal.hpp` is 32 lines shorter (1035 -> 1003). 57 call sites in
`RenderProcedural.cpp`, `RenderInitHeaps.cpp`, `RenderInitPostProcess.cpp` and
`RenderInitScenePipelines.cpp`, plus the two `LoadAndCreate*` declarations,
`RenderInit.cpp` and `init/PassDescriptors.hpp`'s `GraphicsPassDesc`. Paths,
entry points and fallbacks are untouched, so the dev-mode disk override and the
hot reload behave exactly as before.

**Verified:** `ShaderStages.hpp` compiles clean, links and runs under GCC 16.2.0
(`-std=c++26 -freflection`; the chain refuses to compile without the reflection
flag — `include/Zahlen/Core/Reflection/Core.hpp:101`) with `Vulkan-Headers` and
`volk` on the include path and stand-ins for what `tools/zshader` emits. Checked
in that harness: all five stages deduce; `MakeStageSource()`'s fields match the
module's own, member by member; the source stays a trivially copyable aggregate,
which is what the hot-reload lambda capture in `PassDescriptors.hpp` needs;
assigning a compute module to a `VertexStageSource` is a compile error; and
`MakeStageSource<ShaderStage::Vertex, M>()` no longer parses, because the enum is
gone.

**Not verified in the sandbox:** the render translation units that instantiate
it — they include `<ShaderBindings.hpp>` and need the zshader cook, the same wall
as everything else in `src/render`. Every call site was rewritten mechanically
and counted before and after (57 per file set, equal), and a repo-wide grep
confirms nothing still names the removed enum or the old two-argument form.

### `Buffer::Map()` — a `std::expected`, and a mapping nothing unmaps

`Buffer::Map()` now returns `std::expected<MappedRegion, ErrorCode>` instead of a
`MappedRegion` whose public `void* data` a caller could forget to test, and
`MappedRegion` exposes `Data()` / `Size()` / `As<T>()` / `AsSpan<T>()`. Every call
site reads

```cpp
auto mapped = buffer.Map(alloc);
if (!mapped) return std::unexpected(mapped.error());
std::memcpy(mapped->Data(), data, bytes);
```

so a failed map travels in the caller's own error channel — `ErrorCode` is
type-erased, so it propagates unchanged out of any `std::expected<..., ErrorCode>`
return — instead of as a null pointer that each of the 23 sites had to remember to
check. Nineteen of them are in `src/render`; the rest are `UploadToBuffer()`,
`StagingRingBuffer::Init()`, `StagingContext::UploadImage2D()` and
`DescriptorHeap::Init()`, the last two of which now hand the buffer's map error to
their caller rather than folding it into their own mapping enumerator.

The other half is what `MappedRegion` no longer pretends to do. Every buffer the
engine maps is created `CPUOnly`, `CPUToGPU` or `GPUToCPU`, and `Buffer::Create()`
sets `VMA_ALLOCATION_CREATE_MAPPED_BIT` for all three: the allocation is
persistently mapped and `Map()` was already returning VMA's own pointer. The old
destructor called `vmaUnmapMemory()` on it anyway. VMA's contract for the flag is
that the mapping it creates — the "0-th" one — must not be released, and
internally a persistently mapped allocation keeps its own map count at 0 while
the memory block's count was bumped once when VMA made the mapping. So each
region that went out of scope tripped VMA's `Unmapping allocation not previously
mapped` assertion wherever assertions are on, and in a release build decremented
the block's count behind VMA's back — one unbalanced unmap away from an actual
`vkUnmapMemory()` on memory `Buffer::_mappedData` still hands out. The
persistent-only API drops the pretence: `Map()` never calls
`vmaMapMemory()`, the destructor only flushes (all non-coherent memory asks for),
the mapping constructor is private under `friend class Buffer`, and a buffer with
no persistent mapping is `BufferMapError::NotPersistentlyMapped` rather than a
second, unexercised code path. A dynamic-map/owned-region split was considered
and deliberately left until something needs it.

`EnvironmentBakeError::RadianceUploadFailed` and `ScreenshotError::ReadbackFailed`
described nothing but a failed map; both are gone and their sites propagate the
`BufferMapError`. `src/vulkan/RENDER.md`'s Rule 3 was updated to match. Left for
its own step: `StagingRingBuffer::Allocation::mappedData`, the same
raw-pointer-and-null-check pattern at the four sites that consume it.

The call sites were then checked again the hard way: the engine's first build of this
patch rejected `RenderResources.cpp`'s `mapped.As<const uint8_t>()` — one read that had
kept the old dot, past its own `if (!mapped)` guard, because `.data` was what every sweep
had been written to find and `As()` was not. `verify/check_map_sites.py` now resolves each
`.Map()` call to the variable that receives it and rejects any member reached through `.`
that `std::expected` does not have, which is the whole class rather than the one spelling.

**Verified:** the real headers compile, link and run under GCC 16.2.0
(`-std=c++26 -freflection`, `Vulkan-Headers` + `volk` on the include path)
together with the `MappedRegion` definitions lifted verbatim out of
`Allocator.cpp`: `Map()`'s return type, every accessor's type, move-only-ness,
and both call-site spellings (`if (!mapped) return std::unexpected(mapped.error());`
and `.and_then`). A second program runs a live region at 25 points: the
destructor flushes exactly once with `VK_WHOLE_SIZE` and never unmaps,
`AsSpan<T>(n)` floors to whole elements and clamps to the buffer's size, moves
transfer without double-flushing, a zero-sized region spans nothing, and an empty
region (what the two long-lived holders reset to) is harmless. Negative checks:
forging a region outside `Buffer::Map()` is "is private within this context", and
`region.data` is "has no member named 'data'". Repo-wide: 23 `.Map(` call sites in
11 files, every use of every result is `!region`, `region.error()` or `region->…`, no
`vmaMapMemory(`/`vmaUnmapMemory(` call left, no reference to either removed enumerator,
and the patch applies clean on the tree it is cut against — `src/`'s hunks also apply to the pristine base on their
own, the `todo/TODO.md` hunk layers onto the shader-stage entry above.

**Not verified in the sandbox:** the render translation units themselves. They
need the Jolt/glfw/slang submodules, the Vulkan SDK and the generated
`ShaderBindings.hpp`, none of which are vendored here — the same wall as every
other entry, and the reason the checks above compile the real headers with
stand-ins. `Allocator.cpp` is covered only through its extracted
definitions: the file as a whole needs `vk_mem_alloc.h` and the engine's
`Rendering.hpp` graph at link time.

### `GeometryManager`: a `BufferSource`, and one creator instead of five

`GeometryManager` had five ways in, and two of them disagreed about argument
order:

```cpp
CreateBuffer(size_t size, const void* data, Vk::BufferUsage usage);              // (size, data, usage)
CreateVertexBuffer(const void* data, size_t size, uint32_t stride, ...);         // (data, size, stride, ...)
CreateIndexBuffer(const void* data, size_t size, Vk::BufferUsage usage);
CreateStorageBuffer(size_t size, Vk::BufferUsage usage);
CreateStorageBuffer(const void* data, size_t size, uint32_t stride, ...);
```

They differ in exactly two ways — which usage bits they add, and what they divide
the byte count by to get an element count — and both of those are things the caller
knows. So the payload became a named type and the five became one:

```cpp
struct BufferSource {
    std::span<const std::byte> bytes          = {};
    size_t                     allocationSize = 0; // read when `bytes` is empty
    uint32_t                   stride         = 1; // bytes per element
    [[nodiscard]] auto TotalSize() const noexcept -> size_t;      // bytes ? size : allocationSize
    [[nodiscard]] auto ElementCount() const noexcept -> uint32_t; // 0 for an allocation-only source
};

[[nodiscard]] auto CreateBuffer(const BufferSource& source, Vk::BufferUsage usage) -> std::expected<BufferHandle, ErrorCode>;
```

`RenderResources.cpp`'s four forwarders pass one designated initializer each
(`{.bytes = bytes, .stride = stride}`, `{.allocationSize = size}`), and the public
`RenderContext` API is untouched — its ~40 call sites in `MeshBuilder`, the glTF
importer, terrain, VFX and the tests still say `CreateVertexBuffer(std::span {…})`,
because the typed span overloads already carried the stride for them. The one
internal caller of the raw form was `RenderResources.cpp` itself.

**Strict where it was silently forgiving.** The old creators normalised their way
past bad input: `stride == 0` became `1`, a byte count that was not a whole number
of elements floored, and a zero-size request reached `vmaCreateBuffer` (Vulkan
requires `size > 0`, so the outcome depended on the driver and the validation
layer). Now a source that contradicts itself is a `BufferSourceError` — one type,
four reasons, checked in that order so which error wins is not an accident:

| Rejected | Why it is a mistake |
| --- | --- |
| `EmptySource` | neither bytes nor an allocation size: there is nothing to create |
| `ZeroStride` | an element of zero bytes makes the count meaningless (`ElementCount()` would be 0) |
| `ConflictingSize` | bytes *and* an allocation size; `TotalSize()` prefers the bytes, so the other is dead input |
| `RaggedElements` | the byte count is not a whole number of elements — the case where flooring used to hide a bad stride |

`BufferSource` is passed by `const&` and the caller keeps ownership of the bytes
until the copy is recorded, as before.

**Ownership stated, not implied.** `Adopt()` and the `NativeMesh` constructor it hands
off to take the buffer by value rather than by `Vk::Buffer&&`. `Vk::Buffer` is move-only
(copy construction and copy assignment are deleted), so this is still exactly one move
and cannot silently become a copy — what changes is that the signature says who owns the
buffer after the call. It also brings the two in line with the renderer's other adopt
path, `TextureManager::Adopt(Vk::Image, Vk::ImageView)`, which has always taken its
resources by value, and it makes `NativeMesh`'s constructor consistent with its own
`Vk::AccelerationStructure` parameter. The failure path is unchanged in effect: when the
pool is full, `Create` never touched the argument, so `Adopt` still destroys the buffer
it was given — and because the caller's object was moved-from on the way in, the caller's
own `DestroyBuffer` is the no-op it already was.

**The ring's null pointer, folded in.** `GeometryManager.cpp` had the
`Allocation::mappedData == nullptr` test at both of its `Allocate()` sites — the
same "null means no room" pattern `Buffer::Map()` used to have. Both now go through
one `AllocateStaging()` helper in the same TU that turns it into
`StagingError::StagingSpaceExhausted`, so the call sites read `if (!staging)` like
every other expected-valued call in the engine, and `MemoryMappingFailed` (which
described a *mapping* failure and never was one here) is no longer borrowed to mean
"the ring had no room". The helper lives in this TU rather than in
`StagingRingBuffer` on purpose: the ring's other consumers — `RenderResources.cpp`
and `TextureUploader.hpp` (3 sites) — still read `mappedData` directly, and this
step deliberately did not rewrite them. Moving the check to `Allocate()` itself and
deleting this helper is the remainder of that follow-up.

**Verified:** the real `GeometryManager.hpp` compiles under GCC 16.2.0
(`-std=c++26 -freflection`) with the engine's Jolt headers at the pinned SHA and a
stand-in for the generated GPU ABI header (the cook is not available here; see
`verify/codegen_stub/`), and 19 running checks pass over it — the accessors on the
literal shapes the callers pass, and the whole validation table above, including
which error wins when a source breaks two rules at once. The same 19 checks also
pass under `-DNDEBUG`: the kit counts checks explicitly instead of using `assert()`,
because an assert compiled out is a check that silently passes. The one-creator API
and the absence of the four old call shapes are compile-time claims
(`static_assert` on the member pointer type, and concepts for each old signature
that have to stay unsatisfiable), and two negative TUs confirm the diagnostics. The
ring helper runs against a stand-in `StagingRingBuffer` with the engine's shape: a
successful allocation comes back untouched, and a null one becomes
`StagingSpaceExhausted` with nothing dereferencing it. Structurally: one creator in
the header, four `geometry.CreateBuffer()` call sites (all designated
initializers), one `mappedData` null test left in `GeometryManager.cpp`. The two
earlier kits of this series still pass.

**Not verified in the sandbox:** the render translation units still cannot be
built here — Jolt is only headers, glfw/slang are absent, and `GeneratedGpuTypes.hpp`
is stubbed — so `GeometryManager.cpp`'s body is checked through its extracted
helpers, and each call site's expected-handling is checked by inspection of
`RenderResources.cpp`'s four forwarders against the public header they implement.
`RenderContext::CreateStorageBuffer(size_t)` now reports Invalid for `size == 0`
before Vulkan sees it, where it previously depended on the driver rejecting a
zero-size buffer; nothing in the tree passed zero.

### TextureSystem: removed, not left as a husk

`TextureSystem` was two files, two members and no work. `Update()` had an empty
body, and `ResolveIndex(Engine&, TextureHandle)` forwarded one line to
`RenderContext::GetBindlessIndex` — which has thirty-odd call sites across the
renderer and the tests, none of them through this. The forwarder had no callers at
all.

It was registered in the update graph (`updateGraph.AddSystem<&TextureSystem::Update>()`)
anyway, so every frame scheduled a node that declared no component access and did
nothing. The history says why it looked that way: `Update()` was a placeholder for
async texture streaming and mip fading, and the comment that said so — the one that
would have explained the empty body — was deleted in the #91 renderer refactor, which
left `Update(SystemContext&, float)` empty with unnamed parameters; #92 then dropped
the parameters entirely. What survived was a registration whose only remaining claim,
"this system exists", stopped being true several refactors ago.

Removed: both files, the `system/TextureSystem.cpp` entry in
`src/engine/CMakeLists.txt`, the include and the `AddSystem` line in
`SystemWiring.cpp`, and the two test comments that still named it as one of the
examples of a system declaring no conflicting access (`HeadlessEngineFixture.hpp`,
`TestRenderPipelines.cpp` — the incident they describe is real and `CullingSystem` /
`DecalSystem` still carry it). The engine's `Engine.hpp` exposes no texture API, so
nothing was left declaring something that had been implemented; texture lookups go
through `RenderContext::GetBindlessIndex` directly. The update graph now registers
five systems, all of which do work.

**Verified:** `src/engine/SystemWiring.cpp`, `tests/helpers/HeadlessEngineFixture.hpp`
and `tests/render/TestRenderPipelines.cpp` all compile under GCC 16.2.0
(`-std=c++26 -freflection`, the engine's Jolt defines), so the deleted include was not
load-bearing in the TU that used it; naming `ZHLN::TextureSystem` anywhere is now a
compile error (`'TextureSystem' is not a member of 'ZHLN'`); nothing in the tree, of
any file type, still contains the string. A new check, `verify/check_system_husks.py`,
judges every system the graphs register — each `AddSystem<&X::Y>()` must resolve to a
definition with a non-empty body, both engine build lists must match the files on disk,
and the registration count must be the expected one. That check is control-tested by
planting an empty-bodied system in a scratch copy of `src/engine`, where it fails and
names it; the tree as shipped passes it with all nine systems judged. The three earlier
kits of this series still pass.

**Not verified in the sandbox:** that the engine links and runs — `zahlen_engine` is
not buildable here for the same reasons as every other entry. Removing a graph node
cannot reorder the remaining ones in a way this checked (a system that declares no
component access has no edges to reorder), but the update graph's compiled shape is
something only a real build can confirm.

### `World::GetCullingStats()`: an invariant instead of an assertion

```cpp
auto World::GetCullingStats() -> CullingStats& {
    auto stats = _impl->registry.GetSingleton<Components::CullingStatsComponent>();
    ZHLN::Assert(stats.has_value(), "the scene has no CullingStatsComponent singleton: InitializeDefaultScene creates it");
    return stats->stats;
}
```

A reference-returning getter whose value is guarded by a runtime assertion, and the
assertion names a function the caller may never have called: ask for the counters
after `World::Create()` but before `InitializeDefaultScene`, and the assert fires. It
was worse than it looks. `ZHLN::Assert` is `InternalPanic` only when `isDev`;
otherwise it is `[[assume(false)]]` and the line below it dereferences an empty
`Optional` — so a ship build does not abort, it has undefined behaviour. The engine
uses static reflection elsewhere to make this class of mistake a compile error (a
system declaring `Res<T>` against a graph that cannot provide `T` does not compile),
but a singleton in a sparse set is a runtime property, so this one had been left to
an assertion.

The second half of the problem only shows up a minute later: `Engine::ClearScene()`
reaches `SceneCleanupSystem::ClearAll`, which calls `Registry::Clear()` — every
component pool is cleared and every entity recycled. So even creating the singleton in
`World::Create()` would not have made the invariant true for the life of a World; the
first scene reset would take it away again. The registry already has the idiom for
this: `GetOrEmplaceSingleton`, which the input state has used since it was created in
`World::Create` (Engine.cpp:258-285).

What changed:

* `World::Create()` now creates `CullingStatsComponent` next to `InputStateComponent`,
  so the counters exist from construction — for the crash dump, which reads the
  registry directly (Engine.cpp:161, `Optional`-safe today), not just for callers of
  the getter.
* `World::GetCullingStats()` is `registry.GetOrEmplaceSingleton<Components::CullingStatsComponent>().stats`:
  total, no assertion, and self-healing after a scene reset. A zeroed `CullingStats` is
  the honest value for "no cull has run yet" — it is the same value the render tests
  assign when they reset the counters.
* `InitializeDefaultScene` keeps creating it, now with a comment saying why: it is the
  re-seed after a clear, not the original source.
* The culler's own assertion stays, reworded: `World::Create()` creates it and
  InitializeDefaultScene re-seeds it, so it is a tripwire for the one path left — a
  graph run against a registry that was cleared and never re-seeded. It cannot create
  the component itself: the culler reaches it through its declared query
  (`Components::CullingStatsComponent&` in the query signature, which is what gives the
  graph its hazard edge), and structural changes have to be declared, which would
  serialise the culler against every other component access.
* `Components::CullingStatsComponent`'s comment gained the invariant.

Two paths were rejected deliberately. A plain `World::Impl` member (like
`visibleEntities`) would break the graph-declared write access and the crash dump's
registry read. Returning `ZHLN::Optional<CullingStats&>` would be honest about
fallibility, but the culler's contract is that a scene without counters cannot cull —
it has nowhere to report what it did — so the optional would only move the same
decision one call deeper, at the cost of `*` at six sites, one of which writes
(`RenderSystem.cpp:550` builds the totals).

`World::GetCamera()` keeps its assertion, and that difference is now written down in
both places. A world with no main camera yet is a state the renderer reports as
`NoMainCamera` before it calls this, so asking for `Camera&` is opting into a
contract; the culling counters were not a contract, they were the engine's own timing.

**Verified:** the three changed translation units compile under GCC 16.2.0
(`-std=c++26 -freflection`, the engine's Jolt defines) — `World.cpp` itself among them.
The accessors' signatures are pinned by `static_assert` (`GetCullingStats()` returns
`CullingStats&`, not `Optional`; `GetCamera()` returns `Camera&`). The behaviour the
total accessor rests on is checked at run time against the **real** `ECS::Registry`
with the **real** `Components::CullingStatsComponent` (ECS.cpp, Mutex.cpp and Log.cpp
built and linked, with the fiber scheduler stubbed to abort if a single-threaded check
ever reaches it): `GetSingleton()` is empty on an unseeded world — the state the old
code asserted on and then dereferenced — `GetOrEmplaceSingleton()` returns a valid
zeroed component, is idempotent (same component, no second entity), is where writes
land, and after `Registry::Clear()` re-creates a *fresh* zeroed component rather than
resurrecting the previous scene's counters. 13 checks, run twice: normally and under
`-DNDEBUG`. Structurally: the accessor creates on demand and does not assert,
`World::Create()` seeds the singleton, `World.cpp` has exactly one assertion left and
it is `GetCamera()`'s, `InitializeDefaultScene` still re-seeds, and the culler's
tripwire names the real creator. Control: with the accessor reverted to the
assert-guarded version in a scratch copy, that check fails on both counts. The four
earlier kits of this series still pass.

**Not verified in the sandbox:** the seeding itself cannot be exercised — `World::Create`
constructs a `PhysicsContext`, and Jolt is headers-only here, so no `World` is ever
built. What CI settles: that a freshly created World already carries the counters, and
that the crash dump's direct registry read finds them without the getter having been
called first.

### `EventBus`: the queues own themselves

```cpp
struct Queue {
    uint32_t hash     = 0;
    void*    storage  = nullptr;
    void (*destroy)(void*) = nullptr;   // ~IEventQueue(), written out by hand
};

template <typename T>
void Push(T event) {
    auto* q = static_cast<std::vector<T>*>(Ensure<T>().storage);
    q->push_back(std::move(event));
}
```

The bus erased its queues by hand: the entry was a `void*` to a separately allocated
`std::vector<T>` plus a function pointer that deleted it. Every part of that is a `virtual`
destructor written out in a place the compiler cannot check, and it is why the class deleted
all four copy/move operations -- a defaulted move would have left two buses holding the same
pointers.

The allocation story is worth stating plainly, because the obvious reading of that code is
wrong: the hand-written erase does **not** cost an extra allocation. Measured on the same
scenario (a counting `operator new`), the old header and the new one both allocate three
times for a first `Push<T>` -- an entry, the queue object, the vector's buffer -- and the same
number of bytes. `Find()`'s linear scan is also unchanged: one entry per event type a host
uses, so it is four integer compares here either way. What the refactor buys is not
throughput:

* ownership stated in the type system -- `std::unique_ptr` to a private `Queue` base with a
  `virtual ~Queue()`, instead of `void*` plus a `delete`-shaped function pointer -- so the
  destructor, the deleted copy and the *restored* move operations are the compiler's;
* a bus can be moved again (the old one could not be, by construction);
* entries are 16 bytes instead of 24 (the hash no longer pads against two pointers);
* a `Drain` bug the old shape made easy to miss. It walked the queue and cleared it
  afterwards, so an event a callback pushed during the walk was cleared with it -- silently
  lost when the vector had spare capacity, undefined behaviour when it did not. `Drain` now
  takes the queue's buffer (moving a `std::vector` leaves the source empty), so the walk
  covers exactly what was pending when it started and in-drain pushes are handed to the next
  drain.

The boundary question this raised, and the answer: the engine never names `EventBus` --
`grep -rn EventBus src/ modules/` is empty; its consumers are `app/UIEditor.cpp`,
`extensions/UI/` (whose `ActionRegistry` holds an `EventBus*` and pushes on `Invoke`) and
`tests/extras/TestUITree.cpp`, two of them through `UI/UITree.hpp`'s include of it. So should
it live in `extensions/` instead of `include/Zahlen/ecs/`? It stays in Core:
`ARCHITECTURE.md`'s rule is directional (optional layers may consume Core; Core must never
consume them) and says nothing about Core being *used* by Core, and a Core header consumed
only by the optional layers is established -- `Core/EnumFlags.hpp`, `Audio/AudioTypes.hpp`,
`physics/PhysicsHandles.hpp`, `gui/TextBuffer.hpp`, `Threading/Channel.hpp` are all named
zero times in `src/`. Moving it would also mean inventing a home: `extensions/` is one
directory per capability target, so a one-header utility would need a new `extensions/Common`
+ `zahlen_common`, a new include root, and link changes in the app, the UI schema and the
extras tests -- and would put an event bus out of Core's reach for good. Nothing includes it
on a caller's behalf (not even `ecs/ECS.hpp`), so it was already opt-in; the header now says
why it lives where it does.

`Clear<T>()` is gone (zero callers anywhere; `Drain<T>` is how a type is consumed) and the
bus-wide `Clear()` stays. One consequence of the restored move worth knowing about: a host
that keeps an observer of its bus -- `ActionRegistry` does -- must call `SetEventBus` again if
it moves the struct the bus lives in. In-tree nothing moves one (`Session session;` is built
in place and passed by reference), and the same caveat applies to any observer holding a
pointer.

**Verified:** the header compiles standalone under the repo's own flags
(`-std=c++26 -freflection -fno-exceptions -fno-rtti`, plus `-Wall -Wextra -Werror`), which is
also what shows the new `virtual` destructor needs nothing from RTTI. The surface the
consumers are built against is pinned by `static_assert`s: `Push(T)` by value (including the
`bus.Push(*static_cast<const T*>(p))` an `ActionRegistry` emits), `View<T>()` returning
`std::span<const T>`, `Drain<T>(fn)` handing the callback a `T&`, the bus-wide `Clear()`, copy
still deleted, move now available -- and a caller of the dropped `Clear<T>()` no longer
compiles. The promises run against the real `GetTypeHash` under AddressSanitizer and
UndefinedBehaviorSanitizer, and again under `-DNDEBUG`: 42 checks -- an untouched bus
allocates nothing (including `View`/`Drain` of a type never pushed), order is preserved,
`Drain` visits and drops exactly what was pending and *takes* the buffer rather than copying
it, in-drain pushes survive to the next drain, queue addresses survive sibling pushes, moving
transfers and empties, and every allocation is freed. Controls: with `Drain` patched to copy
instead of take -- the first draft of this refactor, which copied because the finder it went
through returned a const queue -- 8 checks fail, caught by both the emptiness and the
allocation assertions; the old header, extracted from git, loses the event pushed during its
own drain; and the naive fix for *that* (defaulting the old header's moves) is measured
leaking 3 allocations on move assignment. One correction to the analysis that prompted this:
it is not a double free. Moving a `std::vector` empties the source, so a defaulted move
*construction* is accidentally safe and the old destructor then frees nothing; move
*assignment* overwrites entries that own their storage through a `void*` and have no
destructor, so the destination's queues leak. Either way the class rested on a property of
`std::vector` it never stated -- the refactored one states it, because a moved-from
`unique_ptr` is null by construction.

**Not verified in the sandbox:** that the consumers still build -- `extensions/UI/UITree.hpp`
reaches Clay and the GUI context, which are not available here, so `app/UIEditor.cpp` and the
extras test cannot be compiled; every call shape they use is pinned in
`verify/eventbus_api_pin.cpp` instead, and CI is the arbiter.

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

### 8. Kill `RayTracingContext` — done, at exactly the planned shape

`ZHLN_RayTracingContext`, `ZHLN_InitRayTracingContext` and the C++
`Vk::RayTracingContext` class are gone. The object existed to hold five Volk
globals process-wide plus a `VkDevice`, and every cost it imposed — the
`NativeMesh` back-pointer, the forward-reference block on
`CreateSkinnedScratchBuffer`, the `BufferUsageWithRT` thunk — protected the
shim, not the renderer.

What replaced it, point for point from the plan:

1. **Capability is a device predicate.** `ray_tracing_enabled` joined
   `ZHLN_Device`, computed in `ZHLN_CreateDevice` from the *enabled* extension
   list (`ZHLN_NameListed` over `active_exts`), and `Vk::Context` answers it
   through `RayTracingSupported()`. Both refinements the plan called out are
   in: it is a per-device flag set once at creation rather than a probe of the
   Volk globals, and it keeps the old gate's *strength* — all three of
   `acceleration_structure`, `ray_query` and `deferred_host_operations` have
   to be in the enabled list, which is precisely what the 19 `rtCtx.Valid()`
   sites meant (ray query contributes no entry points, so a pointer probe
   could never have expressed it).
2. **`NativeMesh` dropped `rtCtx`.** `~NativeMesh` destroys the BLAS through
   the Volk-backed free function using the `device` member it already carried.
   The stamp is now structural: `GeometryManager::Adopt` writes the device
   into every pooled mesh, so the invariant "a mesh with a BLAS knows its
   device" lives at the single entry point to the table instead of at two
   call sites.
3. **BLAS/TLAS work is free functions.** `src/vulkan/diagnostics/Raytracing.hpp`
   now declares seven free functions in `ZHLN::Vk` (`GetBLASSizes`,
   `GetTLASSizes`, `CreateAccelerationStructure`,
   `DestroyAccelerationStructure`, `GetAccelerationStructureAddress`,
   `BuildBLAS`, `BuildTLAS`); the builds take a `VkCommandBuffer`, the
   device-scoped ones take a `VkDevice`, and the C layer underneath calls the
   Volk globals directly — single engine, single `volkLoadDevice`, so there
   is no dispatch table to carry. The C signatures lost their `ctx` parameter
   and nothing else; bodies are line-for-line the old ones minus the
   indirection.
4. **`Vk::RayTracingContext` deleted.**

**The unblock the step existed for.** `CreateSkinnedScratchBuffer` and
`skinnedScratchMap` moved into `GeometryManager`
(`CreateSkinnedScratchBuffer` / `GetOrCreateSkinnedScratchBuffer` /
`ReleaseSkinnedScratchBuffers` plus the map), because their only reach
outside the manager was the rtCtx pointer write. `Impl::BufferUsageWithRT()`
went with them: the ray-tracing build-input bit is now decided where the
allocation happens — `CreateBuffer` and `CreateSkinnedScratchBuffer` add it
when `_ctx.RayTracingSupported()` — so the five call sites that spelled the
thunk pass plain usage flags, and the VUID guard (never add the bit on a
device without the feature) is expressed once. `Impl` lost `rtCtx`,
`skinnedScratchMap` and `BufferUsageWithRT`; `RenderInit.cpp` logs from the
device predicate instead of re-probing the physical device and initialising a
context.

**Verified, as far as this sandbox reaches.** The sandbox has neither clang
nor submodule contents (and no package network), and GCC 12 cannot parse the
file's pre-existing C23 (`constexpr`, enum base, `nullptr`), so the C layer
was checked through a shim harness: the three pre-existing constructs
mechanically replaced — they are CI-proven, and no line of the new code
needed a shim. The modified `RenderCore.h` compiles standalone at 0
diagnostics under `-Wall -Wextra`; the RT implementation region extracted
verbatim from `RenderCore.c`, plus the new `ZHLN_CreateDevice` block,
compile, link against the real `volk.c` and Vulkan headers, and run — the
harness asserts the flag comes out true with all three extensions enabled and
false with two, and locks all seven new C signatures through function-pointer
assignments. `Raytracing.cpp` compiles clean under g++ `-Wall -Wextra`
against the same headers (the only two diagnostics reproduce identically
against the pre-change header, so they are not this change's). All ten
`configure/check_*.py` pass, including the include-provenance check, which
caught and fixed a missing `<Zahlen/Vertex.hpp>` in `GeometryManager.cpp`.
Every call site of the seven AS functions was grepped back to its receiver,
and braces balance in all eleven edited files.

**Not verified:** the eight render translation units that name the new API
(`RenderInternal.hpp`, `RenderResources.cpp`, `RenderFrame.cpp`,
`RenderGraphBuilder.cpp`, `RenderInit.cpp`, `init/RenderInitPostProcess.cpp`,
`init/RenderInitScenePipelines.cpp`, `DrawCommands.hpp`'s consumers) — they
need the generated shader headers — and a real build; CI decides those. Every
renamed site had its receiver's declared type confirmed by hand. The one
semantic delta to watch at runtime is that `~NativeMesh` now gates on
`device != VK_NULL_HANDLE` rather than `rtCtx != nullptr` — equivalent in
practice, because a BLAS is only ever set on a mesh whose adoption stamped
the device.


## Next


### 9. `PresentUsedWindows` — the architectural fault line of presentation

**Status: DONE in `84f0fc5`** with the user's guidance: direction A landed as a
`ReconcileReceipt { Rendered rendered; AttachmentLayout layout; }` returned by
`ReconcileDestination` (the layout stays in the frame vocabulary; the
demotion to `VkImageLayout` remains the presentation step's), deleting the
seven-line re-resolve; direction B landed as named `FrameSync` accessors
(`ComputeTimeline` / `ImageAvailable` / `RenderFinished`), the raw
`ZHLN_FrameSync` no longer leaving the class into orchestration; direction C
per the user's answer — `DeviceLost` bails immediately, every other present
failure records the first error, retires that window's acquisition, lets the
remaining windows present and advance, and reports the first error after the
loop (bailing left their parity permanently desynchronised); directions D/E
kept as-is. The analysis that preceded it, kept for the record — every quote
re-verified against the code as of `35a69eb`; where the original complaint
misremembered the code, the correction is inline:

The whole file is `src/render/RenderPresentation.cpp` (240 lines, two
functions). Its own header comment (:5-18) already stakes out the position any
rework has to respect: the transition/submit/present belong to
`Vk::SwapchainPresenter::Present`, the recovery belongs to the registry and
this file, and multi-queue ordering is "the renderer's because it is about the
*frame*, which the RHI's presenter does not know and should not want to."

**The observation.** Everything upstream — compile-time frame graph, typestate
handles, monadic `std::expected` chains — drains into this one function, and
here it collides with Vulkan WSI and the OS windowing layer. Five phenomena
stand out, each with its reason:

**1. The accumulator instead of a monadic fold.**
`FrameOutcome<T>` is `std::expected<std::optional<T>, ErrorCode>`
(`include/Zahlen/Render/FrameResult.hpp:31-32`); `PresentSuboptimal` is an
empty tag struct (:48). The loop over `destinations.Windows()`
(RenderPresentation.cpp:93) cannot bail on one window's soft failure, because
the other windows still need to present — so :91 declares
`std::optional<PresentSuboptimal> result {}`, :226 folds the worst-case soft
warning into it, :237 returns it.
*Correction to the original read:* the function **does** bail early — on hard
errors, :180 `return std::unexpected(presented.error())` leaves every
remaining window unpresented for the frame. Only soft results fold; that works
because `SwapchainPresenter::Present` maps OUT_OF_DATE/SUBOPTIMAL into the
*value* slot (`SwapchainPresenter.cpp:356`), so the error slot only ever
carries real failures. The comment at :221-224 states the rule. Whether the
hard-error bail is right (see open question 1) is part of the resolution.

**2. Three eras of synchronization in one block (:130-141).**
- Transfer queue: `Vk::StagingRingBuffer transferRingBuffer`
  (RenderInternal.hpp:357), modern C++ RHI.
- Compute queue: a loose `bool computeSubmitted` on `Impl`
  (RenderInternal.hpp:782, reset at :807).
- Sync objects: `destPresenter.sync` is `FrameSync<2>`
  (SwapchainPresenter.hpp:93) whose `operator[]` hands out
  `const ZHLN_FrameSync&` — the raw C struct of four handles
  (RenderCore.h:281-286) — and the block reads
  `sync[slot].compute_timeline` directly.
- Assembly: Vulkan 1.3 `VkSemaphoreSubmitInfo` via
  `Vk::MakeSemaphoreSubmitInfo`, into `std::array<…, 3>` of which at most two
  slots are ever filled.

The frame graph only orders the graphics queue — the async-compute ordering is
done here by hand, exactly as the ComputeSimPipeline comment admits
(`src/render/pipelines/ComputeSimPipeline.cpp:26-28`). The escape hatch is
even documented on `Present` itself: "`extraWaits` is how the caller orders
this behind the other queues it used this frame; the presenter has no opinion
about those" (SwapchainPresenter.hpp:133-138).

**3. The layout extraction (:148-154).**
To tell the presenter "transition from color-attachment to present", the code
re-fetches `dest.imageIndex`, indexes `dest.recordHandles`, validates the
64-bit tagged `DestinationRegistry::Handle`, indexes
`destinations.Records()`, reads the custom `trackedLayout`
(`AttachmentLayout`, `src/vulkan/graph/DynamicRendering.hpp:102-113`), and
demotes it through `ToVkImageLayout` (:120-136).
*Defense first:* the ceremony is intentional — the consteval static_assert at
DynamicRendering.hpp:138-156 makes `PRESENT_SRC_KHR` unnameable by any pass,
and the presenter alone does the present transition (its doc says so). The
real wart is that **the same handle was already resolved one paragraph
earlier**: `ReconcileDestination` (:29-87) resolves the record (:39-42) and
even writes `record.trackedLayout` (:75); the bounds re-check at :149-150
repeats validation that reconciling already guaranteed. The receipt could
carry the leaving layout and the whole extraction disappears.

**4. The device-lost side channel (:168-180).**
`Vk::Instance::NotifyDeviceLost()` mutates the active instance's
`_deviceLostTarget` atomic directly (`src/vulkan/core/Instance.cpp:198-205`).
*Correction:* this is not an undocumented hole — RENDER.md:51 names it the
designed observer sink ("unobservable when no engine is live"), and the
house rule (RenderCore.cpp:10-16) is that **callers acting on a lost device
notify**, while the one mapping (`Vk::ToFrameError`) only names the error.
The present path here is one of five explicit notify sites
(RenderDestinations.cpp:194/388/400, RenderPresentation.cpp:172,
ComputeSimPipeline.cpp:38); the init path deliberately does not notify
(Context.cpp:311-318 — there a lost device only prints and exits). The
purity break is deliberate (one present failure the frame loop cannot carry
on past) and consistent with the pattern everywhere else.

**5. The rebuild swallow (:210-218).**
*Correction:* `Rebuild` already returns `std::expected<void, ErrorCode>`
(SwapchainPresenter.hpp:115); the swallow is at the call site, where the error
slot is discarded into a log line. The window is still retired, its records
cleared, and its generation cached (:219-223), so the next frame re-vends
anyway. Rationale stands: a user dragging a window corner floods resize
events, and a momentarily 0×0/minimized surface must not become a fatal
engine error. "Retry next frame" is the oldest trick in the book, and here it
is the right one.

**Resolution directions (resolved — outcomes in the status block above):**

- **A. Carry the leaving layout in the reconcile receipt.** Have
  `ReconcileDestination` (or the `DestinationRegistry::Rendered` receipt)
  surface the `trackedLayout` it already has; delete the re-resolve at
  :148-154. The invariant (passes cannot name PRESENT_SRC) stays untouched —
  only the second lookup dies. Need to confirm the `Rendered` struct's shape
  and its other consumers first.
- **B. Shrink the raw-sync leak.** Give `FrameSync<N>` a named accessor
  (`ComputeTimeline(slot)` / consumer-stage constant) so the raw C struct no
  longer leaves the class, and gather the :130-141 block into one small
  `extraWaits` builder. This does **not** invent a multi-queue DAG — it
  narrows the one place that legitimately knows about all queues. A real
  unified queue DAG in the frame graph is a separate, much larger item; park
  it under Later if it ever gets wanted.
- **C. Leave the device-lost notification where it is.** Verified against all
  five notify sites: the pattern is "the caller acting on a lost device
  notifies", and `ToFrameError` stays pure (Context.cpp:311-318 shows the
  init path deliberately not notifying). Consolidating notification into the
  mapping would change that documented behaviour. The side channel is the
  design, not a wart — no action.
  *Update:* user verdict reversed this — the counter is fine as a diagnostic,
  but the NAME implies someone reacts to it (nobody does), and two teardown
  comments assert a mechanism that does not exist. Resolution parked as
  item 10.
- **D. Keep the fold, name it.** `result` is an honest reduction, not a
  broken monad; a two-line comment saying "reduction over windows: hard
  errors bail, soft results accumulate" beats restructuring. Only promote it
  to a named combinator if a second user appears.
- **E. Keep the rebuild swallow;** optionally print the discarded
  `ErrorCode` in the log line, since it is right there.

**Open questions — both answered by the user (outcomes in the status block):**
1. Branch on DeviceLost: bail immediately (every window's device is gone);
   for any window-local error do NOT bail — record the first error, mark the
   window unacquired, let the remaining windows present and advance, return
   the first error at the end of the loop.
2. Yes — `FrameSync` is internal RHI (src/vulkan/execution/, not
   include/Zahlen/); adding accessors and hiding the raw struct is fully in
   scope.

### 10. `NotifyDeviceLost` — purged: now `IncrementNumericalDeviceLoss` (done)

**Status: DONE in `e78b122`.** User picked the name: `IncrementNumericalDeviceLoss()` —
the bluntest possible statement of the mechanism (it increments a number; nothing
reacts). The audit that preceded it, kept for the record:

**What the audit confirmed.**
- `BeginFrame` never reads the counter: it waits fences only and maps the wait
  result (RenderFrame.cpp:381-398). The counter plays no part in control flow
  anywhere in the frame loop.
- The mid-frame public API is `void` and swallows: `RenderScene` / `RenderUI`
  / `DispatchCompute` (RenderContext.hpp). The compute swallow is
  ComputeSimPipeline.cpp:33-44 — on a failed submit, `computeSubmitted` stays
  false (only set at :45), so the present skips the timeline wait and the
  frame proceeds; the loss surfaces at the NEXT frame's fence wait. Not a
  black hole, but detection is one frame late and the error's specificity is
  gone.
- `AcquireTarget`'s notify (RenderDestinations.cpp:194) is genuine
  belt-and-suspenders: the same `DeviceLost` error also leaves monadically at
  :202, so the counter there is redundant with the return value.

**What the audit overturned.**
- Recovery is implemented, and it rides the monadic chain end to end:
  SystemWiring.cpp:179-193 (`Present` checks
  `render_res.error().Is(FrameResult::DeviceLost)`) →
  `Engine::HandleDeviceLost` (Engine.cpp:213-230: Kernel rebuild, then
  `PrefabFactory::RebuildVulkanResources`, then the registered
  `deviceLostCallbacks` at :591) → `Kernel::HandleDeviceLost`
  (Kernel.cpp:278-291: `OnDeviceLost()`, destroy the RenderContext, recreate
  it). `ProvokeDeviceLost` + the hang_gpu pipeline exist to exercise exactly
  this. So "a recovery architecture that was never actually implemented" is
  wrong — what is missing is only the implication the NAME suggests.
- The counter is documented diagnostics ownership, not a fig leaf for dropped
  errors: RENDER.md "Diagnostics Ownership (Vk::Instance)", `DiagnosticsSink`
  (Instance.hpp:21-34, caller-owned storage surviving engine death),
  `RenderContext::UseDiagnostics` (RenderContext.hpp:290), and a real
  consumer: tests/render/TestRTRPBRReflection.cpp:282/327/347 snapshots
  `DeviceLostCount()` around provocation.

**The actual defect.**
`NotifyDeviceLost` reads as "someone is told and will react". Nobody reacts —
it increments a diagnostics observation counter. Worse, two teardown comments
assert the nonexistent mechanism: RenderDestinations.cpp:384-386 ("the next
frame's BeginFrame wait only reports what the instance's lost-device state
already says") and :397-398 ("hand a lost device to the instance state the
next frame reads"). BeginFrame reads nothing of the sort — the next frame
surfaces the loss through its OWN fence/present `VkResult`, not through the
counter. The capture itself is legitimate (those teardown paths are `void`;
the event would otherwise be unobservable), but its stated purpose is false.

**Rename plan — executed in `e78b122` with the user-chosen name
`IncrementNumericalDeviceLoss()`** (readers stayed as-is; `DeviceLostCount`
was already honest). Touchpoints, as landed:
- Instance.hpp:99-101 (decl + comment: say "diagnostics observation only;
  recovery rides the monadic VkResult chain").
- Instance.cpp:198 (definition + comment).
- Five call sites: RenderDestinations.cpp:194/388/400,
  RenderPresentation.cpp:172, ComputeSimPipeline.cpp:38.
- RenderCore.cpp:13 (comment naming it), RENDER.md:51.
- Comment rewrites at RenderDestinations.cpp:384-386 and :397-398 — landed:
  the capture is observability for void paths; the frame loop learns of the
  loss from its own fence wait, not from the counter.

**Open question 2** (the void-pipeline gap) was promoted to item 11.

### 11. The void frame APIs — grievance filed, then resolved

**Status: RESOLVED** per the user's answers to all three open questions.
`RenderScene` and `RenderUI` now return `[[nodiscard]] FrameOutcome<FrameSkipped>`
(the exact BeginFrame vocabulary), `DispatchCompute` is renamed
`DispatchSimulations(float dt)` returning `[[nodiscard]] RenderResult` — the
name no longer suggests user-supplied compute, and the doc says it dispatches
the renderer's own simulation set stepped by `dt`. `ComputeSimPipeline::Submit`
propagates a failed `QueueSubmit` as `std::unexpected(err)` (the diagnostics
increment stays beside it), so a lost device on the compute queue reaches
`SystemWiring::Present` THIS frame and triggers `Engine::HandleDeviceLost`
instead of surfacing one frame late at a fence wait. `RenderSystem::RenderMain`
propagates hard errors from all three and consumes scene/UI skips knowingly
(EndFrame still closes the unwritten destination with the background).
TestUI asserts drawn-ness where pixels are checked and no-hard-error on the
render-texture destination; ARCHITECTURE.md's example consumes the result;
app/UIEditor.cpp (found by the user's compile — the survey had missed `app/`)
logs a hard failure and keeps the editor alive.
The grievance as filed, kept for the record:

**The grievance.** `RenderContext`'s frame lifecycle is monadic at the edges
and `void` in the middle. The three entry points that record work all return
nothing (include/Zahlen/Render/RenderContext.hpp:223/226/230):

```cpp
void RenderScene(const SceneView& view, const GraphicsSettings& settings) noexcept;
void RenderUI(const UIView& view, const UIDrawData& uiData) noexcept;
void DispatchCompute(float dt) noexcept;
```

`BeginFrame`/`EndFrame` return `FrameOutcome`, so the recovery chain
(SystemWiring.cpp:182-190 → `Engine::HandleDeviceLost`) only ever sees what
those two surface. The middle third cannot feed it: failures inside these
three are logged, skipped, or — in the compute case — swallowed outright, and
a device loss there is detected one frame late at the next fence wait with the
specificity gone. The `IncrementNumericalDeviceLoss` rename (item 10) made the
diagnostics counter's name honest; the hole itself is still here.

**What the void swallows, per function (verified).**
- `RenderScene` (RenderFrame.cpp:572-636): a destination that does not resolve
  or has no recording open becomes a log line plus `return` (:577-622) — the
  scene is silently skipped and the caller cannot tell drawn from skipped.
  The deferred pipeline executes only if both checks pass; whatever it records
  never reports either.
- `RenderUI` (:637-641): a fire-and-forget forward into `UIPipeline::Execute`.
- `DispatchCompute` (:644-646): one line forward into
  `ComputeSimPipeline::Submit`, which swallows a failed `QueueSubmit`
  (ComputeSimPipeline.cpp:33-44) — log or diagnostics increment, `return;`.
  Because `computeSubmitted` is only set on success (:45), the present then
  skips the timeline wait and the frame proceeds on the graphics queue as if
  the compute work had happened.

**The `DispatchCompute(float dt)` naming grievance (verified).**
The name reads as "dispatch compute work you supply". The implementation
records and submits a FIXED internal simulation set — cluster bounds, cluster
culling, volumetric fog, particle updates (ComputeSimPipeline.cpp:21-23) — and
`dt` is stashed into `impl.currentDt` (:18) for the simulation push constants.
Nothing user-supplied enters it. The header comment
(RenderContext.hpp:227-229) describes the set but never says what `dt` is, and
the implementation and the engine's call site (RenderSystem.cpp:419) are
silent. User's verdict, verbatim: "if it says DispatchCompute I'm supposed to
be feeding it math, but apparently it is as effective calling internal code."

**Resolution directions (resolved — outcomes in the status block above):**
1. Give the three entry points results: `FrameOutcome` (or at least
   `std::expected<void, ErrorCode>`) so a failed compute submit or a skipped
   scene can propagate THIS frame instead of surfacing at the next fence wait.
   Caller survey (done, then corrected on real hardware): `RenderScene` and
   `DispatchSimulations` each have exactly ONE caller — RenderSystem.cpp:442
   and :419. `RenderUI` is called there (:448), four times in
   tests/render/TestUI.cpp, documented as public API shape in
   include/ARCHITECTURE.md:573, AND — caught only by the user's compile —
   twice in app/UIEditor.cpp (:584 preview, :854 editor frame). The sandbox
   survey missed `app/` because the grep was scoped to src/include/tests/extras/
   examples; the fix landed in `app/` as a logged, keep-the-editor-alive
   consumption. See Standing constraints.
2. Rename `DispatchCompute` to something that names what it IS — a fixed
   internal simulation step. Candidates to argue over: `RunSimulations(dt)`,
   `SubmitSimFrame(dt)`, `SimulateFrame(dt)`; and document `dt` (the frame
   timestep the simulations step by) wherever the function is declared.
3. `RenderScene`'s skip paths: decide whether "scene skipped" should surface
   as a value (a `FrameSkipped`-style tag in the return) rather than dying in
   a log line — drawn-vs-skipped is information the caller can act on.

**Open questions — all answered by the user (outcomes in the status block):**
1. Return shape: `FrameOutcome<FrameSkipped>` for RenderScene/RenderUI (the
   BeginFrame vocabulary), `RenderResult` for the compute pass.
2. Replacement name: `DispatchSimulations(float dt)`.
3. Policy: break the consumers and update them cleanly (one engine system,
   TestUI, ARCHITECTURE.md).

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
| 8 | ~~Kill `RayTracingContext`~~ **done** | Device predicate on `Vk::Context`; AS work is free functions; skinned scratch + RT usage bit moved to `GeometryManager`. |

---

## Later

Deliberately parked: engine/ECS scope, not renderer, so it does not sit in
`## Next`. Recorded here so the shape of the idea survives until there is time
for it. The enabler is already in the build — static reflection, on for every
engine target through `zahlen_enable_reflection` — and the engine consumes it
exclusively through the `ZHLN::Reflect` abstraction
(`include/Zahlen/Core/Reflection/`); this plan follows the same rule, so none
of the machinery below spells reflection tokens outside that module.

### Compile-time system argument injection — reflect the signature, not a container

The pattern popularized by Bevy and Flecs: systems declare what they need in
their *signature*, and the graph supplies it. Two pieces are worth borrowing;
two are traps.

**The enterprise IoC container is the trap.** C#/Java-style DI — deep object
trees, `container.Resolve<T>()`, interface-plus-virtual for every dependency,
singleton/scoped/transient lifetimes — is an anti-pattern here for three
hardware reasons:

- Destruction order is strict and hardware-enforced: World (ECS, ragdolls) →
  Physics (Jolt) → Kernel (GPU context, swapchains) → GLFW. A generic
  container as the composition root surrenders deterministic destruction
  order and invites driver segfaults on exit or device loss. `Engine`,
  `Kernel` and `World` stay composed by hand.
- The engine is data-oriented: contiguous component arrays and linear passes,
  not a network of interconnected singleton services.
- `SystemGraph` parallelizes worker fibers from explicit hazard analysis
  (`ComponentAccess: Read/Write` in each `SystemInfo::access_pattern`). A DI
  container treats dependencies as opaque black boxes, which is exactly the
  information the scheduler needs spelled out.

Component-level DI (`[Inject]` on ECS components) is out for the same reason:
components stay plain data.

**Worth doing, part 1: system parameter injection.** Today the contract is a
god-object: `SystemFunc = void (*)(ZHLN::SystemContext&)`
(`include/Zahlen/ecs/SystemGraph.hpp`), so every system receives the whole
context even when it needs one field — `SystemWiring.cpp` hand-extracts
(`sys.ResolveTransforms(ctx.registry)`), and unit-testing an audio system
means standing up a `SystemContext` whose render/physics/camera pointers all
have to be plausible. `SystemContext` carries `ECS::Registry&` plus nullable
services (`render`, `physics`, `audio`, `camera`, `culling`, `articulation`,
`bonePosePostProcessor`, the two `visibleEntities` arrays) and scalars
(`frame`, `alpha`, `dt`).

The migration: a system names only its dependencies —

```cpp
void TransformSystem(ECS::Registry& reg);
void AudioSystem(ECS::Registry& reg, AudioContext& audio, FrameDt dt);
```

— and a reflection-generated thunk replaces the hand-written wrapper. The one
new reflection piece is a "parameters of a function" primitive in
`ZHLN::Reflect` — same module and same one-header-one-home rule as
`ForEachFieldInfo` — which hands the thunk generator the parameter types at
compile time; a per-type `if constexpr` resolver maps each one to the matching
`SystemContext` member, and an index-sequence expansion calls the function.
This is the same shape as `PushConstantLayoutMatches`, which already walks
`Reflect::ForEachFieldInfo` without the pipeline layer touching reflection
directly. The thunk IS a `SystemFunc`, so graph execution, scheduling and
profiling see no change; registration becomes
`.update_func = MakeSystemThunk<AudioSystem>()`. Nothing runs that did not
run before — zero runtime cost, and an unknown parameter type is a
`static_assert`, not a runtime miss.

Constraints the thunk design must respect:

- Services are nullable *on purpose* (`SystemContext`'s own contract: graphs
  must stay executable in reduced environments — ECS-only unit tests,
  headless logic stepping). Injecting a `RenderContext&` therefore has to
  fail at compile time for any graph that can run without one, not
  dereference a null pointer at run time.
- Scalars collide by type (`dt` vs `alpha` are both `float`), so ambient
  values travel as small tagged types (`FrameDt`, `FrameAlpha`, `FrameIndex`)
  — clearer at the call site than positional guessing.
- The reflection boundary holds: `configure/check_reflection_boundary.py`
  keeps reflection tokens confined to `include/Zahlen/Core/Reflection/`, so
  `SystemGraph.hpp` and `SystemWiring.cpp` consume the new parameter
  primitive through `ZHLN::Reflect` and never grow tokens of their own —
  `ShaderProgram.hpp` consuming `ForEachFieldInfo` is the precedent to copy.
- This part needs no ECS change at all: `SystemContext.hpp`,
  `SystemGraph.hpp`, `SystemWiring.cpp` (plus the one new `Reflect`
  primitive). That is why it goes first.

**Worth doing, part 2: auto-deducing graph hazards.** The manual half of the
status quo is the synchronization declaration, e.g.
`SystemWiring.cpp`'s TransformSystem entry —

```cpp
.access_pattern = {Read<Components::HierarchyComponent>(), Read<Components::TransformComponent>(), Write<Components::WorldTransformComponent>()},
```

— and a system that starts writing `HierarchyComponent` without updating its
`access_pattern` is a silent race on the worker fibers. The deduction Bevy
does is to read access off the query's constness. Reflection reads
*signatures*, not bodies, and today the accesses live in the body
(`reg.GetEntitiesWith<>`, `reg.GetRawArray<>`, `reg.Get<>`) — invisible to
anything a signature walk can see. So the deduction is only possible once
systems declare their access as a parameter type: a query/view whose template
arguments carry constness (`Query<const Hierarchy, const Transform, WorldTransform>`
shape), from which the `ComponentAccess` array is generated at compile time
and the manual one becomes a `static_assert`-checked relic.

The honest caveat: part 2 is an ECS API migration, not reflection glue —
`GetEntitiesWith`/`GetRawArray` call sites move onto the query type. Part 1
delivers the testing and boilerplate wins on its own and is the place to
start; part 2 follows once a query parameter is worth having for its own
sake.

| Approach | Verdict |
| :--- | :--- |
| Enterprise IoC container (`Resolve<T>`, service locators, interface injection) | ❌ Surrenders destruction order, hides lifetimes, breaks the fiber scheduler's hazard analysis. |
| Component-level DI (`[Inject]` in ECS components) | ❌ Components stay plain data. |
| System parameter injection (reflect the signature, generate the thunk) | ✅ Decouples systems from `SystemContext`, makes single-context unit tests trivial, deletes wrapper boilerplate. Zero runtime cost. |
| Automatic hazard deduction (`Read/Write` off query constness) | ✅ Kills the manual `access_pattern` drift hazard — after systems declare access through a query parameter. |

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
- Caller surveys for a public-API change grep EVERY target the build compiles —
  `src/`, `include/`, `tests/`, `extras/`, `examples/`, and `app/`. Item 11's
  survey missed `app/UIEditor.cpp` and the user's compiler caught it; `app/` is
  a first-class consumer of the public API, not an afterthought.

## Verification note

CI is the arbiter; it compiles and runs on real hardware. In the sandbox
`src/render` cannot be compiled wholesale: it needs `GeneratedGpuTypes.hpp` and
`ShaderBindings.hpp` from `tools/zshader`, and the 15 `extern/` submodules are
empty. (Since the redesign only `src/render` needs the first of those: the engine
and its extensions no longer reach it.) What *can* be checked there is any render source that includes neither
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

---

## Golden-image regression tests — deterministic camera, Godot reference, per GLB import

The RemoteGLBSample floor shimmer (some models flicker, more often the
skinned/animated ones) survived every sample-side A/B switch: the shadow map
held at its allocated 2048 (a 4096 request could force a declined realloc,
leaving the PCSS shader at half its designed width), `S` (SSR off) and `H`
(subject hidden, with its shadow casters and reflections) all changed nothing.
The remaining suspects are engine-side, and guessing is over. The next step is
a test that reproduces the flicker deterministically, on every GLB import.

The idea: generate golden images with an independent renderer at the exact
transforms and camera data, then compare a Zahlen render made with the exact
same data.

* **Golden side** — a bash script runs Godot headlessly (Godot is installed on
  the user's machine; not in this sandbox). For each GLB: load it, place the
  camera at the same transform the sample's turntable computes (distance
  `r/tan(fov/2)*1.35`, same floor extent and centre, same sun direction /
  colour / intensity), render N frames at a fixed time step, and write one PNG
  per frame.
* **Test side** — a Zahlen test imports the same GLB (fetched through
  `extensions/net/RemoteAsset`, see the prework below), applies the exact same camera
  matrices (no user orbit; TAA jitter deterministically seeded or disabled;
  animation time pinned for skinned models), renders the same N frames, and
  compares.
* **Two assertions.**
  1. *Correctness*: frame k against the Godot golden of frame k. A structural
     comparison (per-region means, silhouette overlap) — not pixel-exact: two
     different PBR pipelines never match pixel-for-pixel, but a missing model,
     a wrong scale, a clipped floor or a broken material shows in every
     region.
  2. *Flicker*: a static scene must settle. With the camera fixed, compare
     frame k against frame k+1 over the floor region across the N frames; a
     mean delta above a small threshold is the shimmer itself. This one is
     self-referential (it needs no Godot) — the goldens only prove both sides
     rendered the same scene in the first place.
* **Per import** — a new row in the crawled list (or a local fixture
  directory) runs the test, and the result gates the import.

Prework, landed on this branch: the fetch machinery the sample used to get its
GLBs had to move out of `samples/RemoteGLBSample.cpp` so a *test* — not just a
sample — can fetch and cache remote assets: `URLResolver`, `DiskCache` and
`AsyncAssetFetcher` in `ZHLN::Remote` (`extensions/net/RemoteAsset`, target
`zahlen_remote_asset`), the GitHub git-trees crawl as `ZHLN::GitHub`
(`extensions/net/GitHub`, target `zahlen_github`, which the test needs to enumerate
the GLBs), and the sample rewired onto them. The sample keeps only showroom
policy: the dropdown, the turntable, the studio rig, the HUD.

Open questions for the Godot side:

* the comparison metric — per-region SSIM, or mean plus silhouette IoU;
* how the skinned models' animation time gets pinned in both renderers (the
  engine's loop time is not externally addressable yet);
* how much of the studio look Godot must reproduce — the goldens need to frame
  the scene, not match its shading.

---

## Fidelity: HDR-driven IBL (plan for review)

Implemented. The asset layer decodes raw `.hdr` and cooked `ZRD1` (RGBA32F).
`EnvironmentMapComponent` carries the path and `renderSkybox`; `RenderSystem`
resolves it and passes floats to the renderer, which never parses the file.
SH and the specular prefilter sample the equirect (no solid-angle jacobian,
no `lightDir` rotation, exposure stays a shade-time knob). The prefiltered
cube is RGBA16F for an HDR bake and stays UNORM for the procedural path.
Background modes are stamped on `FrameUniforms::environmentMode` (0
procedural, 1 cube mip 0, 2 omit / alpha 0). The suite captures PAM so that
alpha reaches the metric; `CaptureScreenshotPPM` stays P6 for every other
caller. An unauthored 180-intensity sun is suppressed while the component is
set. Live specular LOD remains `roughness * 5.0`.

Decisions that landed with it: the renderer never parses the file (no stb in
`src/render`); the bake samples the equirect directly rather than resampling
to a cube first; there is no solid-angle jacobian (Karis `NdotL`); exposure
stays `ambientExposure` at shade time; specular LOD stays `roughness * 5.0`.
`zcook` writes `ZRD1` for `.hdr` and copies every other texture verbatim.
Re-run the suite to see the score move.

Success measure: re-run `run_fidelity.py`; the near-misses (-1 to -2 dB) should
close toward the -22 dB convention once the same HDR lights the scene, with the
residual gap attributable to unsupported `KHR_materials_*` extensions.
