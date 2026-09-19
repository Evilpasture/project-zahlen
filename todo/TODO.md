# Refactor plan dumping ground for the repository. 

---


## SECTION 1: WHAT NOT TO DO

These are the strict architectural red lines. None of these patterns should exist in the refactored renderer.

### 1. DO NOT invent enum feature sinks (`ViewKind`, `DestinationKind`, `ViewportMode`)
* **Why:** Any enum that tries to classify what a view is (`Scene3D`, `UIOnly`, `ShadowCascade`) or where it goes (`DefaultWindow`, `Texture`) becomes a feature sink. 100 commits later, when you add OpenXR, cubemap faces, minimaps, or portals, you are forced to add enum values and scatter `switch` statements across every pass.
* **Rule:** A `View` is pure spatial/optical data (camera, matrices, frustum). A destination is pure subresource addressing (`RenderAttachment`).

### 2. DO NOT mirror GPU hardware formats in public enums (`TextureFormat`)
* **Why:** Creating an `enum class TextureFormat { RGBA8_UNORM, D32_SFLOAT_S8_UINT... }` leaks low-level GPU memory packing into the public API. It is an imitation of `VkFormat` that will inevitably bloat to dozens of entries.
* **Rule:** Attachments do not declare formats. A texture’s format is an intrinsic property of its physical VRAM allocation in `src/vulkan/`. Public code only references the subresource (`TextureHandle` + `mipLevel` + `arrayLayer`).

### 3. DO NOT inherit GUI interfaces on the RHI (`RenderContext : public IUISubmitter`)
* **Why:** In your current code (`include/Zahlen/Render.hpp:143`), `RenderContext` inherits from `IUISubmitter`. This tightly couples your hardware renderer to the 2D GUI subsystem.
* **Rule:** `RenderContext` is a standalone facade. 2D GUI code (Clay) produces plain data (`UIDrawData`), and `RenderContext` provides a clean method (`RenderUI`) to draw it.

### 4. DO NOT leak private pipeline headers to callers
* **Why:** Writing `rc.Render<Pipelines::DeferredPbrPipeline>(...)` forces `app/UIEditor.cpp` and `src/engine/RenderSystem.cpp` to `#include <src/render/pipelines/...>`. This violates your hermetic boundary script (`tools/check_subsystem_boundaries.py`).
* **Rule:** Callers only include `<Zahlen/Render.hpp>` and `<Zahlen/View.hpp>`. `RenderContext` exposes opaque dispatch functions (`RenderScene`, `RenderUI`) whose implementations live privately in `src/render/`.

### 5. DO NOT lie about resource dependencies in the Frame Graph
* **Why:** In your current code (`src/render/RenderGraphBuilder.cpp:180`), `MakeShadowPass()` inlines `MainPass1` and falsely declares writes to `Res_SceneColor`, `Res_NormRough`, `Res_Depth`, etc., to hack around single-stream command recording. `MakeMainPass1()` is left as dead code.
* **Rule:** Passes must declare *only* the resources they actually touch. Parallel CPU recording must be handled natively by the graph via `Vk::Fork`.

### 6. DO NOT bind 3D scene execution to the frame lifecycle (`EndFrame()`)
* **Why:** In your current code (`src/render/RenderFrame.cpp:256`), `RenderContext::EndFrame()` unconditionally calls `RecordComputeFrame` and `RecordScene`. This forces `UIEditor.cpp` to run compute cluster culling, volumetric fog, G-Buffer clears, lighting, RTR, and bloom just to draw a 2D window.
* **Rule:** `BeginFrame()` and `EndFrame()` manage *only* GPU synchronization, fences, allocators, and swapchain presentation. Scene and UI rendering must be invoked explicitly.

### 7. DO NOT expose internal transient targets (`InternalHdr`, G-Buffer) to public code
* **Why:** Callers should never know that the 3D pipeline uses an intermediate HDR float16 buffer. If you change the PBR pipeline tomorrow to Forward+ or a Path Tracer, public code shouldn't break.
* **Rule:** Intermediate targets stay private to the compile-time render graph in `src/render/`. Callers only specify the final destination attachment.

---

## SECTION 2: THE REFACTOR PLAN

---

### Step 1: Clean Up Public Types (`include/Zahlen/`)

#### 1.1 Update `include/Zahlen/Types.hpp`
Add `RenderAttachment` as the single universal subresource reference:

```cpp
// include/Zahlen/Types.hpp
#pragma once
#include <cstdint>

namespace ZHLN {

class Window;

// Opaque 64-bit generational handle for GPU textures and render targets
enum class TextureHandle : uint64_t { Invalid = 0 };

/// Universal subresource reference to any renderable GPU target.
/// Fully identifies a swapchain backbuffer, offscreen texture, cubemap face, or mip level.
struct RenderAttachment {
    TextureHandle texture    = TextureHandle::Invalid;
    uint16_t      mipLevel   = 0;
    uint16_t      arrayLayer = 0; // Cubemap face (0..5) or texture array slice

    [[nodiscard]] constexpr bool Valid() const noexcept {
        return texture != TextureHandle::Invalid;
    }
    explicit constexpr operator bool() const noexcept {
        return Valid();
    }
};

static_assert(sizeof(TextureHandle) == 8);
static_assert(sizeof(RenderAttachment) == 16);

} // namespace ZHLN
```

#### 1.2 Create `include/Zahlen/View.hpp`
Define the two clean parameter structs for 3D and 2D rendering: 

```cpp
// include/Zahlen/View.hpp
#pragma once
#include <Zahlen/Camera.hpp>
#include <Zahlen/Types.hpp>
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Vec3.h>

namespace ZHLN {

/// Pure optical, geometric, and destination parameters for rendering a 3D scene.
struct SceneView {
    JPH::Mat44       viewMatrix        = JPH::Mat44::sIdentity();
    JPH::Mat44       projMatrix        = JPH::Mat44::sIdentity();
    JPH::Mat44       viewProjMatrix    = JPH::Mat44::sIdentity();
    JPH::Mat44       invViewProjMatrix = JPH::Mat44::sIdentity();
    JPH::Vec3        worldPosition     = JPH::Vec3::sZero();
    ViewportRect     viewport          = {};
    RenderAttachment target            = {}; // Output subresource
    Frustum          frustum           = {};
    uint64_t         visibilityMask    = ~0ULL;
    uint32_t         frameIndex        = 0;
    float            time              = 0.0f;
};

/// Destination and layout bounds for rendering 2D UI.
struct UIView {
    ViewportRect     viewport   = {};
    RenderAttachment target     = {}; // Output subresource
    uint32_t         frameIndex = 0;
};

} // namespace ZHLN
```

#### 1.3 Refactor `include/Zahlen/Render.hpp`
* Remove `: public IUISubmitter` [15].
* Remove `AddViewport`, `RemoveViewport`, `PresentViewports`, and `SetSceneCameraPrepare`.
* Expose opaque `RenderScene` and `RenderUI` methods.

```cpp
// In include/Zahlen/Render.hpp:
namespace ZHLN {

class Window;
struct SceneDrawList;

/// Immutable 2D UI geometry payload extracted from Clay.
struct UIDrawData {
    std::span<const UIBatch>          batches;
    std::span<const VertexPosition>   positions;
    std::span<const VertexAttributes> attributes;

    [[nodiscard]] constexpr bool Empty() const noexcept {
        return batches.empty() || positions.empty() || attributes.empty();
    }
};

class ZHLN_API RenderContext {
public:
    // --- Frame Lifecycle (Sync & Presentation Only) ---
    [[nodiscard]] RenderResult BeginFrame() noexcept;
    [[nodiscard]] RenderResult EndFrame() noexcept;

    // --- Window Attachment Vending ---
    /// Returns the RenderAttachment for the active swapchain image of the window.
    [[nodiscard]] RenderAttachment GetWindowAttachment(const Window& window) const noexcept;

    // --- Dynamic Render-to-Texture (RTT) ---
    /// Creates an offscreen texture that can be rendered into and sampled in materials.
    [[nodiscard]] auto CreateRenderTexture(uint32_t width, uint32_t height, bool hdr = false) 
        -> std::expected<TextureHandle, ErrorCode>;

    void DestroyRenderTexture(TextureHandle handle) noexcept;

    // --- Opaque Render Dispatches ---
    void RenderScene(const SceneView& view, const SceneDrawList& drawList, const GraphicsSettings& settings) noexcept;
    void RenderUI(const UIView& view, const UIDrawData& uiData) noexcept;
    void DispatchCompute(float dt) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace ZHLN
```

#### 1.4 Remove `include/Zahlen/UISubmitter.hpp` & `include/Zahlen/Viewport.hpp`
* Delete both files.
* In `include/Zahlen/gui/GUI.hpp`, change `EndFrameAndRender` so `EndFrame()` returns `UIDrawData` directly.

---

### Step 2: Implement `Vk::Fork` in the Frame Graph (`src/vulkan/`)

#### 2.1 Update `src/vulkan/graph/RenderGraph.hpp`
Add parallel pass representation and usage merging to the compile-time graph:

```cpp
// Add to src/vulkan/graph/RenderGraph.hpp:

namespace TemplatedDetail {

template <typename List1, typename List2>
struct MergeLists;

template <typename... T1s, typename... T2s>
struct MergeLists<TypeList<T1s...>, TypeList<T2s...>> {
    using type = TypeList<T1s..., T2s...>;
};

template <typename PassT>
struct ExtractPassUsages {
    using type = typename PassT::Usages;
};

template <typename... SubPasses>
struct ExtractPassUsages<ParallelPass<SubPasses...>> {
    using type = typename ParallelPass<SubPasses...>::Usages;
};

} // namespace TemplatedDetail

template <typename... SubPasses>
struct ParallelPass {
    static constexpr auto name = ResourceName("ParallelPassGroup");

    // Compile-time union of all resources touched across all subpasses
    using Usages = typename TemplatedDetail::MergeLists<typename SubPasses::Usages...>::type;

    std::tuple<SubPasses...> subPasses;

    constexpr explicit ParallelPass(SubPasses&&... passes)
        : subPasses(std::forward<SubPasses>(passes)...) {}
};

template <typename... SubPasses>
constexpr auto Fork(SubPasses&&... passes) {
    return ParallelPass<std::decay_t<SubPasses>...>(std::forward<SubPasses>(passes)...);
}
```

In `CompileTimeFrameGraph::ExecutePass`, handle `ParallelPass`:
```cpp
template <size_t PassIndex, typename PassType, typename ProfilerT, typename DiagnosticsT>
void ExecutePass(...) const {
    if constexpr (requires { pass.subPasses; }) {
        // Barriers for all resources across all subpasses have already been submitted.
        // Record all subpasses concurrently across worker threads:
        auto& rec = this->parallelRecorder.Current();
        rec.Reset();

        std::apply([&](const auto&... p) {
            rec.Record(scheduler, [&](Vk::RecordingSlot slot) {
                p.record(slot.cmd);
            }...);
        }, pass.subPasses);

        Vk::ExecuteCommands(cmd, rec.GetCommandBuffers());
    } else {
        pass.record(cmd);
    }
}
```

---

### Step 3: Implement Modular Pipelines (`src/render/`)

Create `src/render/pipelines/` to house private pipeline recipes.

#### 3.1 `src/render/pipelines/UIPipeline.hpp` & `.cpp`
Draws 2D UI quads directly into the destination attachment [10]:

```cpp
// src/render/pipelines/UIPipeline.hpp
#pragma once
#include "../RenderInternal.hpp"
#include <Zahlen/Render.hpp>
#include <Zahlen/View.hpp>

namespace ZHLN::Pipelines {

struct UIPipeline {
    static void Execute(
        RenderContext::Impl& impl, 
        VkCommandBuffer cmd, 
        const UIView& view,
        const UIDrawData& uiData
    ) noexcept;
};

} // namespace ZHLN::Pipelines
```

```cpp
// src/render/pipelines/UIPipeline.cpp
#include "UIPipeline.hpp"

namespace ZHLN::Pipelines {

void UIPipeline::Execute(
    RenderContext::Impl& impl, 
    VkCommandBuffer cmd, 
    const UIView& view,
    const UIDrawData& uiData
) noexcept {
    if (uiData.Empty() || !view.target.Valid()) {
        return;
    }

    // 1. Submit UI geometry directly to UIRenderer buffers
    impl.uiRenderer.SubmitUI(
        uiData.batches.data(), static_cast<uint32_t>(uiData.batches.size()),
        uiData.positions.data(), uiData.attributes.data(),
        static_cast<uint32_t>(uiData.positions.size())
    );

    // 2. Resolve destination attachment to concrete VkImage
    auto resolvedTarget = impl.ResolveAttachment(view.target);
    if (!resolvedTarget) return;
    auto target = *resolvedTarget;

    // 3. Transition target to COLOR_ATTACHMENT_OPTIMAL
    Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(
        cmd, target.handle, VK_IMAGE_ASPECT_COLOR_BIT
    );

    const auto vp = (view.viewport.width > 0 && view.viewport.height > 0)
        ? view.viewport
        : ViewportRect{0, 0, target.extent.width, target.extent.height};

    // 4. Record UI dynamic rendering pass
    Vk::DynamicPass(VkExtent2D{target.extent.width, target.extent.height})
        .Viewport(static_cast<float>(vp.x), static_cast<float>(vp.y), static_cast<float>(vp.width), static_cast<float>(vp.height))
        .AddColor(target, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE)
        .Execute(cmd, [&]() {
            impl.BindHeapsAndPushFrame(cmd);
            impl.uiRenderer.Record(cmd, target.extent.width, target.extent.height, view.frameIndex);
        });

    impl.uiRenderer.Clear();
}

} // namespace ZHLN::Pipelines
```

#### 3.2 `src/render/pipelines/DeferredPbrPipeline.cpp`
Build the 3D graph using `Vk::Fork` with honest resource dependencies:

```cpp
// In src/render/pipelines/DeferredPbrPipeline.cpp
#include "DeferredPbrPipeline.hpp"
#include "../RenderInternal.hpp"

namespace ZHLN::Pipelines {

template <AAMode Mode>
auto BuildPbrGraph(const PassFactory& factory, const RenderAttachment& target) {
    auto corePasses = std::tuple {
        // Honest Parallel Execution:
        // ShadowPass declares ONLY Res_ShadowMap, Res_ShadowAtlas
        // MainPass1 declares ONLY G-Buffer attachments
        Vk::Fork(
            factory.MakeShadowPass(), // Writes: Res_ShadowMap, Res_ShadowAtlas
            factory.MakeMainPass1()   // Writes: Res_SceneColor, Res_Velocity, Res_NormRough, Res_Emissive, Res_Depth
        ),
        factory.MakeHiZGeneratePass(),
        factory.MakeMainPass2(),
        factory.MakeDecalPass(),
        factory.MakeViewmodelPass(),
        factory.MakeTranslucentPrePass(),
        factory.MakeGtaoPass(),
        factory.MakeLightingPass(),
        factory.MakeRtrHalfTracePass(),
        factory.MakeReflectionPass(),
        factory.MakeTranslucentReflectionPass(),
        factory.MakeForwardPass(),
        factory.MakeHdrDenoisePass(),
        factory.MakeBloomPass()
    };

    auto tailPasses = factory.MakeTailPasses<Mode>(target);

    return std::apply(
        [](auto&&... passes) { return Vk::CompileTimeFrameGraph(std::move(passes)...); },
        std::tuple_cat(std::move(corePasses), std::move(tailPasses))
    );
}

void DeferredPbrPipeline::Execute(
    RenderContext::Impl& impl,
    VkCommandBuffer cmd,
    const SceneView& view,
    const SceneDrawList& drawList,
    const GraphicsSettings& settings
) noexcept {
    impl.UploadSceneDrawData(view, drawList);

    PassFactory factory {
        .self = impl,
        .fIdx = view.frameIndex & 1u,
        .pc = {
            .invViewProj = view.invViewProjMatrix,
            .viewProj    = view.viewProjMatrix,
            .camPos      = {view.worldPosition.GetX(), view.worldPosition.GetY(), view.worldPosition.GetZ(), view.time},
            .giMode      = settings.post.mode,
            .aoRadius    = settings.post.aoRadius,
            .aoBias      = settings.post.aoBias,
            .aoPower     = settings.post.aoPower,
            .giIntensity = settings.post.giIntensity,
            .giSamples   = settings.post.giSamples,
            .enableSSR   = settings.post.enableSSR,
            .enableRTR   = (settings.rayTracing.enableReflections && impl.rtCtx.Valid()) ? settings.post.enableRTR : 0
        },
        .lightVariant = (settings.rayTracing.enableReflections && impl.rtCtx.Valid()) ? 1u : 0u,
        .reflVariant  = (settings.post.enableSSR ? 1u : 0u) | ((settings.rayTracing.enableReflections && impl.rtCtx.Valid()) ? 2u : 0u)
    };

    Reflect::DispatchEnum(settings.antiAliasing.mode, [&]<AAMode Mode>() {
        auto graph = BuildPbrGraph<Mode>(factory, view.target);
        graph.Execute(cmd, impl.GetGraphBinder(), view.frameIndex);
    });
}

} // namespace ZHLN::Pipelines
```

#### 3.3 `src/render/RenderFrame.cpp`
Implement the `RenderContext` facade and strip 3D rendering out of `BeginFrame` and `EndFrame`:

```cpp
// src/render/RenderFrame.cpp
#include "RenderInternal.hpp"
#include "pipelines/DeferredPbrPipeline.hpp"
#include "pipelines/UIPipeline.hpp"
#include "pipelines/ComputeSimPipeline.hpp"

namespace ZHLN {

void RenderContext::RenderScene(const SceneView& view, const SceneDrawList& drawList, const GraphicsSettings& settings) noexcept {
    Pipelines::DeferredPbrPipeline::Execute(*_impl, _impl->current_cmd, view, drawList, settings);
}

void RenderContext::RenderUI(const UIView& view, const UIDrawData& uiData) noexcept {
    Pipelines::UIPipeline::Execute(*_impl, _impl->current_cmd, view, uiData);
}

void RenderContext::DispatchCompute(float dt) noexcept {
    Pipelines::ComputeSimPipeline::Execute(*_impl, _impl->current_compute_cmd, dt);
}

auto RenderContext::BeginFrame() noexcept -> RenderResult {
    // 1. Wait for fence for current frame slot
    auto wait_res = _impl->session.sync.Wait(_impl->session.frameIndex ^ 1);
    if (wait_res == VK_ERROR_DEVICE_LOST) {
        return std::unexpected(RenderFrameResult::DeviceLost);
    }

    // 2. Step timeline & recycle resources
    _impl->session.sync.StepTimeline(_impl->session.frameIndex);
    _impl->deletionQueue.BeginFrame(_impl->session.frameIndex);
    _impl->ReclaimTextureSlots(_impl->session.frameIndex);

    // 3. Reset primary command buffer and begin recording
    const auto cmd = _impl->session.pools.Cmd(_impl->session.frameIndex);
    _impl->current_cmd = cmd;
    _impl->session.pools[_impl->session.frameIndex].Reset();

    ZHLN_BeginCommandBuffer(cmd);
    return {};
}

auto RenderContext::EndFrame() noexcept -> RenderResult {
    const auto cmd = _impl->current_cmd;
    ZHLN_EndCommandBuffer(cmd);

    // 1. Submit graphics command buffer
    auto submit_res = Vk::QueueSubmit(
        _impl->ctx.GraphicsQueue(), cmd,
        VK_NULL_HANDLE, 0, VK_PIPELINE_STAGE_2_NONE,
        _impl->session.sync[_impl->session.frameIndex].render_finished, 0, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT,
        _impl->session.sync[_impl->session.frameIndex].in_flight
    );

    if (!submit_res) {
        return std::unexpected(RenderFrameResult::DeviceLost);
    }

    // 2. Present all windows that received draw commands
    for (auto* win : _impl->windowsToPresentThisFrame) {
        _impl->PresentWindow(*win);
    }
    _impl->windowsToPresentThisFrame.clear();

    // 3. Advance double-buffer index
    _impl->session.frameIndex ^= 1u;
    return {};
}

} // namespace ZHLN
```

---

## Phase 4: Caller Migration

### 4.1 `app/UIEditor.cpp`
The UI editor now executes zero 3D deferred passes and zero compute shaders:

```cpp
// app/UIEditor.cpp
#include <Zahlen/Kernel.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/View.hpp>
#include <Zahlen/gui/GUI.hpp>

while (kernel->IsRunning()) {
    kernel->ProcessEvents();

    auto& rc = kernel->GetRenderContext();
    if (auto begin = rc.BeginFrame(); !begin) {
        continue;
    }

    // 1. Build Clay UI commands into UIDrawData payload
    GUI::Context gui(registry, kernel->GetWindow().GetSize());
    gui.BeginFrame(session.dt);
    DrawWorkspace(gui, session);
    UIDrawData editorUiData = gui.EndFrame();

    // 2. Render UI directly to the primary window attachment
    UIView editorView {
        .viewport = {0, 0, kernel->GetWindow().GetSize().width, kernel->GetWindow().GetSize().height},
        .target   = rc.GetWindowAttachment(kernel->GetWindow())
    };
    rc.RenderUI(editorView, editorUiData);

    // 3. Render preview UI to secondary window (if open)
    if (session.previewWindow != nullptr) {
        GUI::Context previewGui(session.previewGui, session.previewWindow->GetSize());
        previewGui.BeginFrame(session.dt);
        DrawPreviewTree(previewGui, session);
        UIDrawData previewUiData = previewGui.EndFrame();

        UIView previewView {
            .viewport = {0, 0, session.previewWindow->GetSize().width, session.previewWindow->GetSize().height},
            .target   = rc.GetWindowAttachment(*session.previewWindow)
        };
        rc.RenderUI(previewView, previewUiData);
    }

    rc.EndFrame();
}
```

### 4.2 `src/engine/system/RenderSystem.cpp`
Clean separation between 3D scene rasterization and 2D HUD overlays:

```cpp
// src/engine/system/RenderSystem.cpp
#include <Zahlen/Engine.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/View.hpp>

std::expected<void, ErrorCode> RenderSystem::Update(Engine& engine, float dt) {
    auto& rc = engine.GetRenderContext();
    const auto settings = SyncGraphicsSettings(engine);

    // 1. Run compute simulations if emitters are active
    if (engine.HasActiveEmitters()) {
        rc.DispatchCompute(dt);
    }

    // 2. Render 3D Scene View -> Primary Window
    SceneView sceneView {
        .viewMatrix        = engine.GetCamera().GetViewMatrix(),
        .projMatrix        = engine.GetCamera().GetProjectionMatrix(rc.GetViewportAspect()),
        .viewProjMatrix    = engine.GetCamera().GetViewProjMatrix(),
        .invViewProjMatrix = engine.GetCamera().GetInvViewProjMatrix(),
        .worldPosition     = engine.GetCamera().position,
        .viewport          = rc.GetViewport(),
        .target            = rc.GetWindowAttachment(engine.GetWindow()),
        .frustum           = engine.GetCamera().frustum,
        .frameIndex        = static_cast<uint32_t>(engine.GetCurrentFrame()),
        .time              = engine.GetTotalTime()
    };
    rc.RenderScene(sceneView, CollectSceneDrawData(engine), settings);

    // 3. Render 2D UI HUD Overlay over the same window (if any UI was submitted)
    if (auto uiData = engine.GetPendingUIData(); !uiData.Empty()) {
        UIView hudView {
            .viewport = rc.GetViewport(),
            .target   = rc.GetWindowAttachment(engine.GetWindow())
        };
        rc.RenderUI(hudView, uiData);
    }

    return {};
}
```

---

## 5. Architectural Comparison Matrix

| Property | Before Refactor | After Refactor |
| :--- | :--- | :--- |
| **Pass Dependencies** | `ShadowPass` inlines `MainPass1` and lies about G-Buffer writes. `MakeMainPass1` dead code. | `Vk::Fork(ShadowPass, MainPass1)` compiles honest disjoint usage lists. Both run concurrently. |
| **UI Editor Overhead** | `UIEditor` executes 17 3D passes (compute, culling, lighting, GTAO, TAA, Bloom). | `UIEditor` calls `RenderUI()`, executing 1 dynamic pass directly to the swapchain. |
| **Subsystem Isolation** | `RenderContext` inherits `IUISubmitter`. Pipelines leaked via templates. | `RenderContext` is an opaque facade. Pipelines are private to `src/render/`. |
| **Destination Model** | Hardcoded monolithic window swapchain with hacks for secondary windows. | Universal `RenderAttachment` (`TextureHandle`, `mipLevel`, `arrayLayer`). |
| **Future Extensibility** | Adding OpenXR, cubemap probes, or portals creates enum bloat. | **Zero enum changes.** OpenXR or probes simply vend subresource `RenderAttachment`s. |

---

## 6. Addendum: the attachment query and the frame's streams

The snippets above predate this, and two of them are now wrong: there is no
`RenderContext::Impl::current_cmd`, and `GetWindowAttachment` is a query.

- **Acquiring is a verb, asking is not.** `RenderContext::AcquireTarget(window)`
  takes this frame's image for a window and opens the destination's command
  buffer. `RenderContext::GetWindowAttachment(window)` returns what the frame has
  already acquired, and nothing else: no acquire, no fence wait, no
  `vkBeginCommandBuffer`, no state a later call could see as changed. A caller
  may ask about any window at any point in a frame without changing the frame.
- **A command buffer belongs to a destination.** `DestinationRegistry::WindowEntry`
  owns a `DestinationRecording`, opened once per frame by the acquire that makes
  the destination drawable and ended by the present, the frame's guard, or its
  own destructor. Passes resolve `view.target` to a destination and record into
  that destination's stream, so a pass can only ever write what it was aimed at.
  A record with no window (a render texture) has no submission of its own and
  rides the frame's active destination.
- **Where the snippets say `GetWindowAttachment` at a call site**, read
  `AcquireTarget` and check the two-level result: an error means the window could
  not become a destination, an empty optional means it has nothing to draw into
  this frame.
- **A destination's contents are a receipt, not two flags.** `Record` no longer
  carries `writtenThisFrame` / `backgroundFilled`; it carries
  `std::optional<Rendered>` and answers `GetRenderedContent()` with the frame
  vocabulary (`FrameOutcome<Rendered>`): an error when the record holds no image
  at all, `std::nullopt` when nothing has touched the image this frame (its
  contents are undefined), and otherwise the receipt -- which pass wrote it
  (`Rendered::By::Scene` / `UI`) or `FrameFill` when the frame's own fallback
  clear is all it got, with `Drawn()` saying which of those it was. Writers are
  `NoteWritten(attachment, by, layout)` for a pass, and the presentation step
  for the frame (`ReconcileDestination`, called per destination from
  `PresentUsedWindows`); a reader that wants the frame rather than the pixels
  (the capture path) asks the receipt instead of deriving the answer from flags.
- **There is no fill pass.** `FillUnwrittenDestinations` -- a sweep over every
  destination between the frame's passes and its presents, mutating records to
  make the frame presentable -- is gone. A destination is closed by the same
  step that decides to show it: `ReconcileDestination` answers
  `FrameOutcome<Rendered>` (no image left to present / no stream to close it
  with / written, by a pass or by the frame), and a destination the frame cannot
  speak for is not presented at all instead of being presented as a frame that
  never happened.

- **Specialization constants are a struct's fields.** The hand-written
  `VkSpecializationMapEntry` arrays, `offsetof`s and per-variant
  `VkSpecializationInfo` loops at `RenderInitPostProcess.cpp` (lighting,
  reflection) and `RenderProcedural.cpp` (the bake) are gone.
  `Vk::Specialization<T>` (`pipeline/Specialization.hpp`) records one entry per
  field in declaration order -- field N is `constant_id` N, with that field's
  own offset and size -- and `spec.Infos(variants)` is the
  `std::span<const VkSpecializationInfo>` the pipeline builders take. The walk
  (`Reflect::ForEachFieldInfo<SpecData>(spec)`) stays a line at the call site on
  purpose: a build without `-freflection` reaches it through
  `zahlen_transpile_sources`, which rewrites a call it can see, and hidden in the
  header it would compile against the no-op stand-in and leave the map empty --
  a silent behaviour change rather than a build failure.

- **Stencil state is a preset, not eight fields.** The three hand-written
  `VkStencilOpState` literals in `RenderInitScenePipelines.cpp` (CSG write /
  difference / intersection) are gone: `StencilWriteMask(ref, mask)` and
  `StencilCompareMask(op, ref, mask)` on `PipelineBuilder` install both faces
  and turn the test on with the state, so the enable flag cannot be forgotten
  and `StencilTest(bool)` no longer exists to be left behind. `StencilOp(front,
  back)` stays as the escape hatch for a pipeline whose faces differ. The blend
  half of "fluent blend state" is not in this shape yet: `AlphaBlend()` /
  `AdditiveBlend()` are still two booleans the C layer turns into a fixed
  `VkPipelineColorBlendAttachmentState` (RenderCore.c), so there is no
  hand-written blend struct at any call site and nothing to delete.
