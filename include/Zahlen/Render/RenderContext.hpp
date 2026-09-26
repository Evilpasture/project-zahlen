// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Render/RenderContext.hpp
//
// The renderer's dispatch surface: frame lifecycle, asset resolution, opaque
// resource creation, window attachments, the two render verbs. This is what a
// caller holds; everything it takes and returns is spelled in this directory's
// siblings.
//
// PipelineStatsCapture lives here rather than in PipelineStats.hpp because the
// two need each other in a way only one ordering can express: it friends
// RenderContext::Impl, and RenderContext::CapturePipelineStats() returns it by
// value, so each needs the other complete. A forward declaration of a nested
// type of an incomplete class is not available, so they share a file.
#pragma once
#include <Zahlen/Camera.hpp>
#include <Zahlen/Config.hpp>
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/Pair.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/ErrorCode.hpp>
#include <Zahlen/Geometry2D.hpp>
#include <Zahlen/GraphicsSettings.hpp>
#include <Zahlen/Render/FrameResult.hpp>
#include <Zahlen/Render/GpuLayout.hpp>
#include <Zahlen/Render/Info.hpp>
#include <Zahlen/Render/PipelineStats.hpp>
#include <Zahlen/Render/PresentTiming.hpp>
#include <Zahlen/Render/Types.hpp>
#include <Zahlen/Render/View.hpp>
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Vertex.hpp>
#include <Zahlen/gui/UIData.hpp>
#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace ZHLN {

// UIDrawData (the Clay geometry payload RenderUI consumes) lives in
// <Zahlen/gui/UIData.hpp> so the GUI subsystem can produce it without
// including the renderer.

struct Camera;
namespace FS {
class FileSystemWatcher;
}
using FileSystemWatcher = FS::FileSystemWatcher;
class PipelineStatsCapture;
// The renderer's only notion of "something to draw into". Forward-declared,
// never included: it is an engine-internal seam (src/window/PresentationTarget.hpp)
// and nothing here needs it complete. What makes the renderer independent of the
// window system is not the spelling of this parameter but the fact that it is
// this parameter -- a desktop window, a KMS/DRM session and a headless runner all
// hand one over, and nothing below ever asks which.
class PresentationTarget;

// Already-decoded RGBA32F equirect, top row first. The renderer does not
// retain the pointer past SetEnvironmentRadiance. A null pointer or a zero
// extent restores the procedural sky. contentHash 0 with pixels is hashed
// here; a matching non-zero hash skips the rebake. renderSkybox non-zero
// draws cube mip 0; zero omits the background (alpha 0).
struct EnvironmentRadianceDesc {
    const float* rgba         = nullptr;
    uint32_t     width        = 0;
    uint32_t     height       = 0;
    uint64_t     contentHash  = 0;
    int          renderSkybox = 0;
};

class ZHLN_API RenderContext {
  private:
    struct PrivateToken {
        explicit PrivateToken() = default;
    };

  public:
    struct Impl;
    RenderContext(PrivateToken, std::unique_ptr<Impl> impl) noexcept;
    ~RenderContext();

    RenderContext(const RenderContext&)                    = delete;
    auto operator=(const RenderContext&) -> RenderContext& = delete;

    // Pass the engine-owned watcher to enable development shader reloads; when
    // non-null it must outlive the RenderContext.
    [[nodiscard]] static std::expected<std::unique_ptr<RenderContext>, ErrorCode>
        Create(PresentationTarget& target, const RenderConfig& cfg, FileSystemWatcher* fileSystemWatcher = nullptr) noexcept;

    [[nodiscard]] std::optional<Extent2D> GetFramebufferSize() const;

    // --- Frame Lifecycle (GPU synchronization and presentation only)
    //
    // BeginFrame/EndFrame open and close one frame slot: fences, allocators, the
    // transient descriptor partition, presentation. They run *no* rendering, so a
    // 2D-only client never executes a 3D pass and an empty frame costs nothing.

    // Begins a frame: std::nullopt when it began, FrameSkipped when there was nothing
    // to draw into (a minimised window -- nothing is wrong), else an error. Check
    // `code.Is(FrameResult::DeviceLost)` before rebuilding the device.
    [[nodiscard]] FrameOutcome<FrameSkipped> BeginFrame() noexcept;

    // Ends a frame: submits and presents every window drawn into. PresentSuboptimal
    // means a present did not go through as asked -- the renderer already rebuilt its
    // swapchain and the frame still counts as drawn.
    [[nodiscard]] FrameOutcome<PresentSuboptimal> EndFrame() noexcept;

    void SetResolution(const Extent2D& resolution);

    // Sub-rectangle of the framebuffer the 3D scene renders into, in pixels, top-left
    // origin. Applied as a fixed-function viewport and scissor on the screen-space
    // scene passes, so nothing outside is rasterized; attachment clears still cover
    // the whole target. Width or height <= 1 restores full-frame rendering. Camera
    // aspect, GPU culling screen space and picking should all use it (GetViewport).
    using ViewportRect = ZHLN::ViewportRect;

    void SetViewport(const ViewportRect& rect) noexcept;
    // Effective scene viewport: the stored rectangle clamped to the framebuffer, or
    // the whole framebuffer when none is active.
    [[nodiscard]] ViewportRect GetViewport() const noexcept;
    // Width / height of GetViewport(), or 1.0 when the viewport is degenerate.
    [[nodiscard]] float GetViewportAspect() const noexcept;
    // Identity, presentation path, and optional-feature status as of Create.
    [[nodiscard]] RenderInfo GetInfo() const noexcept;
    [[nodiscard]] uint32_t   GetFrameIndex() const noexcept;

    // The primary presenter's display-locked frame interval in seconds, when
    // the closed-loop pacer knows it from hardware timing properties (fixed
    // refresh, feedback arrived). Engine::Run paces simulation off this and
    // falls back to the wall clock otherwise, so every other policy,
    // headless sessions, variable refresh, and the bootstrap frames all
    // answer std::nullopt rather than a guess.
    [[nodiscard]] std::optional<float> GetPacedDeltaTime() const noexcept;
    // The primary presenter's pacing strategy and latest display-timing
    // feedback: refresh interval, present slack margin, variable-refresh
    // state. The fidelity governor paces quality scaling off the margin.
    [[nodiscard]] PresentTimingMetrics GetPresentTiming() const noexcept;

    // --- High-Level Asset Resolution & GPU Cache API
    [[nodiscard]] std::optional<Mesh>     GetGPUMesh(AssetID id) const noexcept;
    [[nodiscard]] std::optional<Material> GetGPUMaterial(MaterialID id) const noexcept;
    void                                  RegisterGPUMesh(AssetID id, Mesh mesh) noexcept;
    void                                  RegisterGPUMaterial(MaterialID id, Material mat) noexcept;
    void                                  ClearGPUCaches() noexcept;

    // Reuse or create skinned scratch VBO for an entity without leaking handles
    BufferHandle GetOrCreateSkinnedScratchBuffer(uint64_t entityKey, uint32_t vertexCount);
    BufferHandle CreateStorageBuffer(size_t size);
    // Reuses an owner-scoped particle buffer for one effect subresource. The
    // render lifecycle reclaims it when owner dies or is explicitly despawned.
    BufferHandle GetOrCreateParticleBuffer(Entity owner, uint32_t subresourceKey, uint32_t maxParticles);
    void         SubmitParticleEmitter(BufferHandle gpuBuffer, uint32_t maxParticles, const ParticleEmitterParams& params);
    void SubmitMeshParticleEmitter(BufferHandle gpuBuffer, uint32_t maxParticles, const MeshParticleEmitterParams& params, AssetID mesh, MaterialID mat);

    // --- Opaque Resource Creation API
    // Uploads immutable data that shaders reach only through its device
    // address (VK_BUFFER_USAGE_STORAGE_BUFFER_BIT). Use this, not
    // CreateVertexBuffer, for BDA-only streams such as the VK_EXT_mesh_shader
    // meshlet descriptors: they are never bound as vertex or index buffers,
    // so tagging them VERTEX_BUFFER_BIT misdescribes them to the driver and
    // to tooling.
    //
    // Note that STORAGE_BUFFER_BIT is not what makes a BDA read legal --
    // SHADER_DEVICE_ADDRESS_BIT is, and CreateGPUBuffer always sets it. This
    // exists for correct intent, and so the streams can be bound as storage
    // descriptors later (e.g. a compute pass writing meshlet indirect args).
    auto CreateStorageBuffer(const void* data, size_t size, uint32_t stride = sizeof(uint32_t)) -> BufferHandle;

    auto CreateVertexBuffer(const void* data, size_t size, uint32_t stride = sizeof(VertexPosition)) -> BufferHandle;
    auto CreateIndexBuffer(const void* data, size_t size) -> BufferHandle;
    void DestroyBuffer(BufferHandle handle);
    void UpdateBuffer(BufferHandle handle, const void* data, size_t size) noexcept;
    auto CreateConstantBuffer(size_t size) -> BufferHandle;
    // Compiles a material from the engine's built-in scene shaders.
    // Translucent materials (alphaBlend/additiveBlend) use the Forward
    // variant, everything else the G-buffer variant.
    [[nodiscard]] std::expected<Material, ErrorCode> CreateBasicMaterial(bool doubleSided = false, bool alphaBlend = false, bool additiveBlend = false);
    [[nodiscard]] std::expected<Material, ErrorCode> CreateMaterial(const MaterialDesc& desc);

    auto CreateSkinnedScratchBuffer(uint32_t vertexCount) -> BufferHandle;

    void                       UploadDebugVertices(const void* posData, size_t posSize, const void* attrData, size_t attrSize, uint32_t vertexCount) noexcept;
    [[nodiscard]] BufferHandle GetDebugMeshBuffer() const noexcept;

    // --- Window Attachments: acquiring and asking
    //
    // A window is a destination, not a mode: the renderer hands out the subresource
    // for the image it acquired this frame and the caller decides what to render into
    // it; EndFrame presents it. Headless windows hand out the offscreen color target,
    // so the same call site works with no window system at all.

    // Acquires this frame's attachment for a window: creates its destination on the
    // first frame that draws into it, acquires the swapchain image (or the headless
    // color target), registers the descriptors it is drawn through, and opens the
    // destination's command buffer -- the stream every pass aimed at it records into.
    //
    // The attachment is optional because "nothing to draw into this frame" is an
    // answer, not a failure; failures (surface, presenter bring-up, the acquire, a
    // call outside BeginFrame/EndFrame) arrive in the error slot, so the caller
    // decides what is worth logging.
    [[nodiscard]] auto AcquireTarget(const PresentationTarget& target) noexcept -> FrameOutcome<RenderAttachment>;

    // The attachment this frame already acquired for a window, and nothing else: a
    // query in the strict sense -- no image acquired, nothing waited on, no command
    // buffer opened, no state left changed. This is what a pass resolves its own
    // target against, so what it draws into cannot depend on which window was asked
    // about last.
    [[nodiscard]] std::optional<RenderAttachment> GetTargetAttachment(const PresentationTarget& target) noexcept;

    // Releases the swapchain and present resources of a window about to be destroyed.
    // Idempotent; an unknown window is a no-op.
    void ReleaseTarget(const PresentationTarget& target) noexcept;

    // --- Dynamic Render-to-Texture (RTT)
    // Creates an offscreen texture that can be rendered into and sampled in materials.
    // The handle addresses it as a RenderAttachment *and* resolves to a bindless slot,
    // so `CreateMaterial` may bind it.
    [[nodiscard]] auto CreateRenderTexture(uint32_t width, uint32_t height, bool hdr = false) -> std::expected<TextureHandle, ErrorCode>;
    void               DestroyRenderTexture(TextureHandle handle) noexcept;

    // --- Opaque Render Dispatches
    //
    // The only entry points that record 3D/2D work. Their implementations live in
    // src/render/pipelines/: no pipeline header, graph type or pass list crosses this
    // boundary.

    // Renders the queued scene draws (Draw/DrawCSG/DrawDecal/DrawLine and the particle
    // emitters) into `view.target`. `settings` is applied on the way in, so it must be
    // the frame's canonical state.
    //
    // Result: the error slot on hard failure; std::nullopt when the scene was drawn;
    // FrameSkipped when the view's target is not this frame's destination (retired
    // under the frame, or never acquired) -- the frame still ends and presents what
    // it has, so a skip is information, not a failure.
    [[nodiscard]] auto RenderScene(const SceneView& view, const GraphicsSettings& settings) noexcept -> FrameOutcome<FrameSkipped>;
    // Draws a Clay-geometry payload into `view.target`, preserving its contents -- so
    // it is safe over a target a scene was just rendered to (HUD overlay).
    //
    // Same result vocabulary as RenderScene: FrameSkipped when the payload is empty,
    // the target does not resolve, or it has no recording open this frame.
    [[nodiscard]] auto RenderUI(const UIView& view, const UIDrawData& uiData) noexcept -> FrameOutcome<FrameSkipped>;
    // Records and submits this frame's GPU compute simulations -- the renderer's own
    // set (cluster bounds, cluster culling, volumetric fog, particle updates), stepped
    // by `dt` seconds. Dispatches nothing user-supplied. Must precede RenderScene when
    // a 3D scene is drawn; a UI-only frame never pays for it.
    //
    // Result: the error slot when the compute submit fails -- a lost device among them
    // -- so the frame's caller propagates it this frame instead of finding out one
    // frame late at a fence wait.
    [[nodiscard]] auto DispatchSimulations(float dt) noexcept -> RenderResult;

    void DrawLine(JPH::Vec3Arg start, JPH::Vec3Arg end, JPH::Vec4Arg colorStart, JPH::Vec4Arg colorEnd) noexcept;
    void DrawLine(JPH::Vec3Arg start, JPH::Vec3Arg end, JPH::Vec4Arg color) noexcept {
        DrawLine(start, end, color, color);
    }

    [[nodiscard]] uint32_t GetBindlessIndex(TextureHandle handle) const noexcept;

    [[nodiscard]] auto          CreateTexture(const void* data, uint32_t width, uint32_t height, bool isSRGB = true) -> std::expected<uint32_t, ErrorCode>;
    [[nodiscard]] auto          CreateTextureCube(const void* const* faceData, uint32_t width, uint32_t height) -> std::expected<uint32_t, ErrorCode>;
    [[nodiscard]] TextureHandle RegisterTexture(std::string_view name, uint32_t bindlessIndex, bool isSRGB = true);
    // Releases the bindless slot behind a registered handle. The record goes
    // immediately (later GetBindlessIndex calls resolve to the white fallback) and the
    // slot is recycled once the frames that could still read its descriptor retire, so
    // streaming loops stop eating the 32768-entry index space. Unknown handles and the
    // engine's fallbacks are a no-op.
    void UnloadTexture(TextureHandle handle);

    // Generates a texture through a CPU-side callback filling
    // `void(uint32_t* pixels, uint32_t width, uint32_t height)`.
    template <typename Func>
    [[nodiscard]] auto CreateTextureProcedural(uint32_t width, uint32_t height, bool isSRGB, Func&& callback) -> std::expected<uint32_t, ErrorCode> {
        std::vector<uint32_t> pixels(static_cast<size_t>(width * height));
        callback(pixels.data(), width, height);
        return CreateTexture(pixels.data(), width, height, isSRGB);
    }

    void     UpdateJointMatrices(uint32_t offset, const JPH::Mat44* matrices, uint32_t count);
    uint32_t AllocateMorphDeltas(uint32_t count, const float* deltas);

    ZHLN::Array<ZHLN::Pair<uint64_t, BufferHandle>>& GetTracked2DEmitters() noexcept;
    ZHLN::Array<ZHLN::Pair<uint64_t, BufferHandle>>& GetTracked3DEmitters() noexcept;

    // Records a buffer outside ECS storage; the owner association survives a plain
    // Registry::Destroy so the render lifecycle can reclaim it later.
    void TrackEntityBuffer(Entity owner, BufferHandle buffer);
    // Releases every buffer attributed to owner, particle ledgers included;
    // DespawnEntity uses this for immediate ordered teardown.
    void ReleaseEntityBuffers(Entity owner);
    // Reclaims tracked buffers whose ECS owner has already died.
    void               ReconcileEntityBuffers(EntityAliveQuery alive);
    [[nodiscard]] auto GetTrackedEntityBufferCount() const noexcept -> size_t;

    // Validation-layer errors observed by the ACTIVE engine (zero when none exists).
    // Snapshot it around a workload to assert the workload is VUID-clean; observers
    // that need values to outlive an engine register their own storage with
    // UseDiagnostics().
    [[nodiscard]] static uint32_t ValidationErrorCount() noexcept;

    // Device-lost / hang events observed by the ACTIVE engine. See
    // ValidationErrorCount().
    [[nodiscard]] static uint32_t DeviceLostCount() noexcept;

    // Registers caller-owned diagnostics storage (both or neither; nullptrs revert to
    // per-instance counting). Every engine created afterwards increments these atomics
    // directly, including teardown-time validation events, so deltas across an
    // engine's whole lifecycle are exact. The storage must outlive those engines, and
    // registration must precede their creation: the sink is resolved once per
    // instance.
    static void UseDiagnostics(std::atomic<uint32_t>* validationErrors, std::atomic<uint32_t>* deviceLost) noexcept;

    // Injects a diagnostic GPU breadcrumb into the active frame's command stream.
    void WriteCheckpoint(std::string_view name) noexcept;

    // Starts a scoped GPU pipeline-counter capture, live while the returned object is
    // alive; inactive (converts to false) when the device has no statistics queries.
    // Statistics queries make drivers serialize counter bookkeeping, so this is a
    // measurement tool, not always-on telemetry.
    [[nodiscard]] PipelineStatsCapture CapturePipelineStats() noexcept;

    // Triggers hardware fault diagnostic dumps and unblocks GPU crash handlers.
    void OnDeviceLost() noexcept;

    RenderResult BuildMeshBLAS(Mesh& mesh) noexcept;

    // Legacy explicit resize for tools/tests: equivalent to a GraphicsSettings delta
    // on shadows.resolution, the path ApplySettings uses internally.
    [[nodiscard]] std::expected<void, ErrorCode> SetShadowResolution(uint32_t resolution);
    void                                         ProvokeDeviceLost();

    auto BakeProceduralTexture(uint32_t width, uint32_t height, uint32_t variantIdx, float scale, float randomness) -> std::expected<uint32_t, ErrorCode>;
    TextureHandle CreateProceduralTexture(std::string_view name, uint32_t width, uint32_t height, bool isSRGB, const uint32_t* pixels);

    [[nodiscard]] std::expected<void, ErrorCode> CaptureScreenshotPPM(std::string_view outputPath) noexcept;

    // Already-decoded RGBA32F equirect. Null (or a zero extent) restores the
    // procedural sky. The renderer does not open the file or parse a format.
    // Call after the previous frame's fence has been waited (RenderSystem does
    // this just after BeginFrame) and before this frame records: the bake
    // replaces the images the reflection pass samples.
    [[nodiscard]] std::expected<void, ErrorCode> SetEnvironmentRadiance(const EnvironmentRadianceDesc& desc) noexcept;

    // --- OOP Idiomatic State & Command Submission APIs
    // Current optical state of the frame's view; RenderScene overwrites the
    // camera-derived slice (view/proj/invViewProj, camPos) from its SceneView.
    void SetMatrices(const JPH::Mat44& viewProj, const JPH::Mat44& unjitteredViewProj) noexcept;
    // Frame-uniform state belonging to the *frame*, not one view: sun/sky/probe values,
    // TAA jitter and previous-frame matrices, the cascade shadow matrix, dt. Kept until
    // changed, so a view only describes the current optics.
    void SetFrameData(const Camera& cam, const FrameUniforms& uniforms, const JPH::Mat44& shadowProjView, float dt = 0.0166f) noexcept;

    // Writes view/proj and camPos into the live FrameUniforms slot (no cascade rebuild).
    void BindCamera(const Camera& cam, Extent2D viewSize) noexcept;
    // Drops the queued draws without rendering them, for callers building a second
    // view's draw list on the same queue.
    void ClearDrawQueues() noexcept;

    // --- Canonical graphics configuration
    // Single entry point for graphics configuration: diffs `newSettings` against the
    // current state and reacts to deltas (resizing cascade shadow targets when
    // shadows.resolution changes); plain knob changes flow into the next frame's
    // uniforms, push constants and pipeline-variant selection. Call between BeginFrame
    // batches -- RenderSystem's ECS sync point does it once per frame.
    void ApplySettings(GraphicsSettings newSettings) noexcept;

    // Snapshot of the renderer's canonical GraphicsSettings (last applied).
    [[nodiscard]] const GraphicsSettings& GetSettings() const noexcept;

    // Legacy bridge for tools/tests: overwrites only the post/GI slice. Prefer the ECS
    // settings components -- RenderSystem re-applies the collected state every frame.
    void SetGISettings(const GISettings& settings) noexcept;
    // Legacy bridge for tools/tests: overwrites the AA state. The ECS
    // AASettingsComponent is authoritative and re-applied every frame.
    void SetAAState(const AAState& state);
    void SetLights(const Light* lights, uint32_t count) noexcept;
    void Draw(const Material& material, const Mesh& mesh, const DrawParams& params) noexcept;
    void DrawCSG(const Material& eyeMaterial, const Mesh& eyeMesh, const CSGDrawParams& params) noexcept;
    void DrawDecal(const DecalParams& params) noexcept;

  private:
    std::unique_ptr<Impl> _impl;
};

// Scoped GPU pipeline-counter capture, created by RenderContext::CapturePipelineStats().
// The destructor stops the capture, so no loose on/off flag can be left behind.
// Counters accumulate over COMPLETED frames and retrieval lags one (a frame's counters
// are pulled at the next frame's begin), so tick one extra frame after the measured
// work before Consume(). Move-only; must not outlive its RenderContext.
class ZHLN_API PipelineStatsCapture {
  public:
    PipelineStatsCapture() noexcept = default;
    PipelineStatsCapture(PipelineStatsCapture&& other) noexcept;
    auto operator=(PipelineStatsCapture&& other) noexcept -> PipelineStatsCapture&;
    ~PipelineStatsCapture() noexcept;

    PipelineStatsCapture(const PipelineStatsCapture&)                    = delete;
    auto operator=(const PipelineStatsCapture&) -> PipelineStatsCapture& = delete;

    // False when the device offers no statistics queries and the capture never
    // started.
    explicit operator bool() const noexcept {
        return _impl != nullptr;
    }

    // Counter sums over the frames completed since the previous Consume() (or since the
    // capture started), resetting the accumulator.
    [[nodiscard]] GpuPipelineCounters Consume() noexcept;

  private:
    friend class RenderContext;
    explicit PipelineStatsCapture(RenderContext::Impl* impl) noexcept: _impl(impl) {
    }

    RenderContext::Impl* _impl = nullptr;
};

} // namespace ZHLN
