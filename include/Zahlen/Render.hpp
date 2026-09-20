// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "Common.h"
#include <Zahlen/Config.hpp>
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Core/Pair.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/FrameResult.hpp>
#include <Zahlen/GraphicsSettings.hpp>
#include <Zahlen/Types.hpp>
#include <Zahlen/View.hpp>
#include <Zahlen/Window.hpp>
#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace ZHLN {

// Renderer capability errors: backend-neutral, so content/asset code can branch on
// "this GPU lacks the optional feature" without knowing about Vulkan.
enum class RenderFeatureError : uint8_t {
    FeatureNotSupported ZHLN_ANNOTATION(ZHLN::Description<"The requested render feature is not supported on this device">{}) = 1,
};

namespace Shadows {
inline constexpr float NearClip   = 0.1f;
inline constexpr float BaseOffset = 150.0f;
inline constexpr float BaseDepth  = 300.0f;
inline constexpr float FarOffset  = 500.0f;
inline constexpr float FarDepth   = 1000.0f;
} // namespace Shadows

/// How finished frames reach a display, chosen once at device creation. Kept distinct
/// from "headless": a windowed session with no window-system integration (macOS has no
/// native Vulkan WSI) presents through the host-GL blit rather than masquerading as a
/// CI run.
enum class PresentationMode : uint8_t {
    /// Standard Vulkan WSI: VkSurfaceKHR + VkSwapchainKHR.
    NativeSwapchain,
    /// Offscreen Vulkan render target copied out and blitted through the
    /// HostBlit plugin's own OpenGL window (macOS).
    HostBlit,
    /// No window and no presenter at all: CI / servers / --headless.
    OffscreenOnly,
};

/// Physical-device class, mirroring the backend's device-type enum without naming it.
enum class PhysicalDeviceType : uint8_t {
    Other         = 0,
    IntegratedGPU = 1,
    DiscreteGPU   = 2,
    VirtualGPU    = 3,
    CPU           = 4,
};

/// Snapshot of renderer identity and optional-feature status. `rendererName` and
/// `gpuName` stay valid for the lifetime of the producing RenderContext.
struct RenderInfo {
    std::string_view   rendererName         = {};
    std::string_view   gpuName              = {};
    PhysicalDeviceType deviceType           = PhysicalDeviceType::Other;
    PresentationMode   presentationMode     = PresentationMode::OffscreenOnly;
    bool               meshShadingSupported = false;
    bool               meshShadingActive    = false;
    bool               rayTracingSupported  = false;
};

/// A fallible renderer operation with only success and error as outcomes
/// (BuildMeshBLAS). The frame verbs return FrameOutcome<T> instead, because they have
/// a non-failure to report -- see Zahlen/FrameResult.hpp.
using RenderResult = std::expected<void, ErrorCode>;

// UIDrawData (the Clay geometry payload RenderUI consumes) lives in Types.hpp
// so the GUI subsystem can produce it without including the renderer.

/// Material recipe for RenderContext::CreateMaterial: pipeline-state flags
/// plus the PBR factors and texture bindings of one scene material.
struct MaterialDesc {
    // Pipeline configuration
    bool doubleSided   = false;
    bool alphaBlend    = false;
    bool additiveBlend = false;

    // PBR factors (using std::array eliminates memcpy)
    uint32_t             alphaMode   = 0;
    float                alphaCutoff = 0.5f;
    float                metallic    = 1.0f;
    float                roughness   = 1.0f;
    std::array<float, 4> baseColor   = {1.0f, 1.0f, 1.0f, 1.0f};
    std::array<float, 4> emissive    = {0.0f, 0.0f, 0.0f, 1.0f};

    // Texture bindings
    TextureHandle albedoMap   = TextureHandle::Invalid;
    TextureHandle normalMap   = TextureHandle::Invalid;
    TextureHandle pbrMap      = TextureHandle::Invalid;
    TextureHandle emissiveMap = TextureHandle::Invalid;
};

struct DrawParams {
    JPH::Mat44           transform        = JPH::Mat44::sIdentity();
    JPH::Mat44           prevTransform    = JPH::Mat44::sIdentity();
    float                cullRadius       = 1.0f;
    std::array<float, 3> localCenter      = {0.0f, 0.0f, 0.0f};
    uint32_t             jointOffset      = 0;
    uint32_t             morphOffset      = 0;
    uint32_t             activeMorphCount = 0;
    const float*         morphWeights     = nullptr;
    DrawFlags            flags            = DrawFlags::None;

    BufferHandle skinnedVertexBuffer = BufferHandle::Invalid;

    float roughness = -1.0f;
    float metallic  = -1.0f;

    std::array<float, 4> colorOverride    = {1.0f, 1.0f, 1.0f, -1.0f}; // alpha < 0 means disable override
    std::array<float, 4> emissiveOverride = {0.0f, 0.0f, 0.0f, -1.0f}; // alpha < 0 means disable override
};

struct CSGCutterParams {
    Mesh         mesh;
    Material     material;
    JPH::Mat44   transform           = JPH::Mat44::sIdentity();
    JPH::Mat44   prevTransform       = JPH::Mat44::sIdentity();
    float        cullRadius          = 1.0f;
    CSGOperation operation           = CSGOperation::Difference;
    uint32_t     jointOffset         = 0;
    BufferHandle skinnedVertexBuffer = BufferHandle::Invalid;
    DrawFlags    flags               = DrawFlags::None;
};

struct CSGDrawParams {
    DrawParams                   eyeParams;
    ZHLN::Array<CSGCutterParams> cutters; // Stably using your custom Array container
};

struct DecalParams {
    JPH::Mat44    transform    = JPH::Mat44::sIdentity();
    JPH::Mat44    invTransform = JPH::Mat44::sIdentity();
    TextureHandle albedoMap    = TextureHandle::Invalid;
    TextureHandle normalMap    = TextureHandle::Invalid;
    float         roughness    = 0.5f;
    float         metallic     = 0.0f;
};

/// GPU pipeline counters summed over every profiled pass of the captured frames
/// (hardware VK_QUERY_TYPE_PIPELINE_STATISTICS; see CapturePipelineStats). Counters the
/// device does not support stay 0.
///
/// The ratios this exists to measure -- clipping: 1 - clipperPrimitivesOut /
/// clipperInvocations; meshlet culling: meshInvocations against the meshlets the scene
/// issued.
struct GpuPipelineCounters {
    uint64_t iaPrimitives         = 0;
    uint64_t vsInvocations        = 0;
    uint64_t clipperInvocations   = 0; // primitives fed to the clipper
    uint64_t clipperPrimitivesOut = 0; // primitives that survived clipping
    uint64_t gsInvocations        = 0;
    uint64_t gsPrimitives         = 0;
    uint64_t fsInvocations        = 0;
    uint64_t csInvocations        = 0;
    uint64_t taskInvocations      = 0; // task workgroups launched (needs mesh shading)
    uint64_t meshInvocations      = 0; // mesh workgroups executed post-culling
};

struct Camera;
class FileSystemWatcher;
class PipelineStatsCapture;

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

    /// Pass the engine-owned watcher to enable development shader reloads; when
    /// non-null it must outlive the RenderContext.
    [[nodiscard]] static std::expected<std::unique_ptr<RenderContext>, ErrorCode>
        Create(Window& window, const RenderConfig& cfg, FileSystemWatcher* fileSystemWatcher = nullptr) noexcept;

    [[nodiscard]] std::optional<Extent2D> GetFramebufferSize() const;

    // --- Frame Lifecycle (GPU synchronization and presentation only)
    //
    // BeginFrame/EndFrame open and close one frame slot: fences, allocators, the
    // transient descriptor partition, presentation. They run *no* rendering, so a
    // 2D-only client never executes a 3D pass and an empty frame costs nothing.

    /// Begins a frame: std::nullopt when it began, FrameSkipped when there was nothing
    /// to draw into (a minimised window -- nothing is wrong), else an error. Check
    /// `code.Is(FrameResult::DeviceLost)` before rebuilding the device.
    [[nodiscard]] FrameOutcome<FrameSkipped> BeginFrame() noexcept;

    /// Ends a frame: submits and presents every window drawn into. PresentSuboptimal
    /// means a present did not go through as asked -- the renderer already rebuilt its
    /// swapchain and the frame still counts as drawn.
    [[nodiscard]] FrameOutcome<PresentSuboptimal> EndFrame() noexcept;

    void SetResolution(const Extent2D& resolution);

    /// Sub-rectangle of the framebuffer the 3D scene renders into, in pixels, top-left
    /// origin. Applied as a fixed-function viewport and scissor on the screen-space
    /// scene passes, so nothing outside is rasterized; attachment clears still cover
    /// the whole target. Width or height <= 1 restores full-frame rendering. Camera
    /// aspect, GPU culling screen space and picking should all use it (GetViewport).
    using ViewportRect = ZHLN::ViewportRect;

    void                       SetViewport(const ViewportRect& rect) noexcept;
    /// Effective scene viewport: the stored rectangle clamped to the framebuffer, or
    /// the whole framebuffer when none is active.
    [[nodiscard]] ViewportRect GetViewport() const noexcept;
    /// Width / height of GetViewport(), or 1.0 when the viewport is degenerate.
    [[nodiscard]] float        GetViewportAspect() const noexcept;
    /// Identity, presentation path, and optional-feature status as of Create.
    [[nodiscard]] RenderInfo   GetInfo() const noexcept;
    [[nodiscard]] uint32_t     GetFrameIndex() const noexcept;

    // --- High-Level Asset Resolution & GPU Cache API
    [[nodiscard]] std::optional<Mesh>     GetGPUMesh(AssetID id) const noexcept;
    [[nodiscard]] std::optional<Material> GetGPUMaterial(MaterialID id) const noexcept;
    void                                  RegisterGPUMesh(AssetID id, Mesh mesh) noexcept;
    void                                  RegisterGPUMaterial(MaterialID id, Material mat) noexcept;
    void                                  ClearGPUCaches() noexcept;

    // Reuse or create skinned scratch VBO for an entity without leaking handles
    BufferHandle GetOrCreateSkinnedScratchBuffer(uint64_t entityKey, uint32_t vertexCount);
    BufferHandle CreateStorageBuffer(size_t size);
    /// Reuses an owner-scoped particle buffer for one effect subresource. The
    /// render lifecycle reclaims it when owner dies or is explicitly despawned.
    BufferHandle GetOrCreateParticleBuffer(Entity owner, uint32_t subresourceKey, uint32_t maxParticles);
    void         SubmitParticleEmitter(BufferHandle gpuBuffer, uint32_t maxParticles, const ParticleEmitterParams& params);
    void SubmitMeshParticleEmitter(BufferHandle gpuBuffer, uint32_t maxParticles, const MeshParticleEmitterParams& params, AssetID mesh, MaterialID mat);

    // --- Opaque Resource Creation API
    /// Uploads immutable data that shaders reach only through its device
    /// address (VK_BUFFER_USAGE_STORAGE_BUFFER_BIT). Use this, not
    /// CreateVertexBuffer, for BDA-only streams such as the VK_EXT_mesh_shader
    /// meshlet descriptors: they are never bound as vertex or index buffers,
    /// so tagging them VERTEX_BUFFER_BIT misdescribes them to the driver and
    /// to tooling.
    ///
    /// Note that STORAGE_BUFFER_BIT is not what makes a BDA read legal --
    /// SHADER_DEVICE_ADDRESS_BIT is, and CreateGPUBuffer always sets it. This
    /// exists for correct intent, and so the streams can be bound as storage
    /// descriptors later (e.g. a compute pass writing meshlet indirect args).
    auto CreateStorageBuffer(const void* data, size_t size, uint32_t stride = sizeof(uint32_t)) -> BufferHandle;

    auto                                         CreateVertexBuffer(const void* data, size_t size, uint32_t stride = sizeof(VertexPosition)) -> BufferHandle;
    auto                                         CreateIndexBuffer(const void* data, size_t size) -> BufferHandle;
    void                                         DestroyBuffer(BufferHandle handle);
    void                                         UpdateBuffer(BufferHandle handle, const void* data, size_t size) noexcept;
    auto                                         CreateConstantBuffer(size_t size) -> BufferHandle;
    /// Compiles a material from the engine's built-in scene shaders.
    /// Translucent materials (alphaBlend/additiveBlend) use the Forward
    /// variant, everything else the G-buffer variant.
    [[nodiscard]] std::expected<Material, ErrorCode> CreateBasicMaterial(bool doubleSided = false, bool alphaBlend = false, bool additiveBlend = false);
    [[nodiscard]] std::expected<Material, ErrorCode> CreateMaterial(const MaterialDesc& desc);
    [[nodiscard]] std::expected<Material, ErrorCode> CreateDebugLineMaterial();
    [[nodiscard]] std::expected<Material, ErrorCode> CreateDebugSolidMaterial();

    auto CreateSkinnedScratchBuffer(uint32_t vertexCount) -> BufferHandle;

    void                       UploadDebugVertices(const void* posData, size_t posSize, const void* attrData, size_t attrSize, uint32_t vertexCount) noexcept;
    [[nodiscard]] BufferHandle GetDebugMeshBuffer() const noexcept;

    // --- Window Attachments: acquiring and asking
    //
    // A window is a destination, not a mode: the renderer hands out the subresource
    // for the image it acquired this frame and the caller decides what to render into
    // it; EndFrame presents it. Headless windows hand out the offscreen color target,
    // so the same call site works with no window system at all.

    /// Acquires this frame's attachment for a window: creates its destination on the
    /// first frame that draws into it, acquires the swapchain image (or the headless
    /// color target), registers the descriptors it is drawn through, and opens the
    /// destination's command buffer -- the stream every pass aimed at it records into.
    ///
    /// The attachment is optional because "nothing to draw into this frame" is an
    /// answer, not a failure; failures (surface, presenter bring-up, the acquire, a
    /// call outside BeginFrame/EndFrame) arrive in the error slot, so the caller
    /// decides what is worth logging.
    [[nodiscard]] auto AcquireTarget(const Window& window) noexcept -> FrameOutcome<RenderAttachment>;

    /// The attachment this frame already acquired for a window, and nothing else: a
    /// query in the strict sense -- no image acquired, nothing waited on, no command
    /// buffer opened, no state left changed. This is what a pass resolves its own
    /// target against, so what it draws into cannot depend on which window was asked
    /// about last.
    [[nodiscard]] std::optional<RenderAttachment> GetWindowAttachment(const Window& window) noexcept;

    /// Releases the swapchain and present resources of a window about to be destroyed.
    /// Idempotent; an unknown window is a no-op.
    void ReleaseWindow(const Window& window) noexcept;

    // --- Dynamic Render-to-Texture (RTT)
    /// Creates an offscreen texture that can be rendered into and sampled in materials.
    /// The handle addresses it as a RenderAttachment *and* resolves to a bindless slot,
    /// so `CreateMaterial` may bind it.
    [[nodiscard]] auto CreateRenderTexture(uint32_t width, uint32_t height, bool hdr = false) -> std::expected<TextureHandle, ErrorCode>;
    void               DestroyRenderTexture(TextureHandle handle) noexcept;

    // --- Opaque Render Dispatches
    //
    // The only entry points that record 3D/2D work. Their implementations live in
    // src/render/pipelines/: no pipeline header, graph type or pass list crosses this
    // boundary.

    /// Renders the queued scene draws (Draw/DrawCSG/DrawDecal/DrawLine and the particle
    /// emitters) into `view.target`. `settings` is applied on the way in, so it must be
    /// the frame's canonical state.
    void RenderScene(const SceneView& view, const GraphicsSettings& settings) noexcept;
    /// Draws a Clay-geometry payload into `view.target`, preserving its contents -- so
    /// it is safe over a target a scene was just rendered to (HUD overlay).
    void RenderUI(const UIView& view, const UIDrawData& uiData) noexcept;
    /// Records and submits this frame's compute simulations (cluster culling,
    /// volumetric fog, particle updates). Must precede RenderScene when a 3D scene is
    /// drawn; a UI-only frame never pays for it.
    void DispatchCompute(float dt) noexcept;

    void DrawLine(JPH::Vec3Arg start, JPH::Vec3Arg end, JPH::Vec4Arg colorStart, JPH::Vec4Arg colorEnd) noexcept;
    void DrawLine(JPH::Vec3Arg start, JPH::Vec3Arg end, JPH::Vec4Arg color) noexcept {
        DrawLine(start, end, color, color);
    }

    [[nodiscard]] uint32_t GetBindlessIndex(TextureHandle handle) const noexcept;

    [[nodiscard]] auto          CreateTexture(const void* data, uint32_t width, uint32_t height, bool isSRGB = true) -> std::expected<uint32_t, ErrorCode>;
    [[nodiscard]] auto          CreateTextureCube(const void* const* faceData, uint32_t width, uint32_t height) -> std::expected<uint32_t, ErrorCode>;
    [[nodiscard]] TextureHandle RegisterTexture(std::string_view name, uint32_t bindlessIndex, bool isSRGB = true);
    /// Releases the bindless slot behind a registered handle. The record goes
    /// immediately (later GetBindlessIndex calls resolve to the white fallback) and the
    /// slot is recycled once the frames that could still read its descriptor retire, so
    /// streaming loops stop eating the 32768-entry index space. Unknown handles and the
    /// engine's fallbacks are a no-op.
    void UnloadTexture(TextureHandle handle);

    /// Generates a texture through a CPU-side callback filling
    /// `void(uint32_t* pixels, uint32_t width, uint32_t height)`.
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

    /// Records a buffer outside ECS storage; the owner association survives a plain
    /// Registry::Destroy so the render lifecycle can reclaim it later.
    void TrackEntityBuffer(Entity owner, BufferHandle buffer);
    /// Releases every buffer attributed to owner, particle ledgers included;
    /// DespawnEntity uses this for immediate ordered teardown.
    void ReleaseEntityBuffers(Entity owner);
    /// Reclaims tracked buffers whose ECS owner has already died.
    void ReconcileEntityBuffers(EntityAliveQuery alive);
    [[nodiscard]] auto GetTrackedEntityBufferCount() const noexcept -> size_t;

    /// Validation-layer errors observed by the ACTIVE engine (zero when none exists).
    /// Snapshot it around a workload to assert the workload is VUID-clean; observers
    /// that need values to outlive an engine register their own storage with
    /// UseDiagnostics().
    [[nodiscard]] static uint32_t ValidationErrorCount() noexcept;

    /// Device-lost / hang events observed by the ACTIVE engine. See
    /// ValidationErrorCount().
    [[nodiscard]] static uint32_t DeviceLostCount() noexcept;

    /// Registers caller-owned diagnostics storage (both or neither; nullptrs revert to
    /// per-instance counting). Every engine created afterwards increments these atomics
    /// directly, including teardown-time validation events, so deltas across an
    /// engine's whole lifecycle are exact. The storage must outlive those engines, and
    /// registration must precede their creation: the sink is resolved once per
    /// instance.
    static void UseDiagnostics(std::atomic<uint32_t>* validationErrors, std::atomic<uint32_t>* deviceLost) noexcept;

    /// Injects a diagnostic GPU breadcrumb into the active frame's command stream.
    void WriteCheckpoint(std::string_view name) noexcept;

    /// Starts a scoped GPU pipeline-counter capture, live while the returned object is
    /// alive; inactive (converts to false) when the device has no statistics queries.
    /// Statistics queries make drivers serialize counter bookkeeping, so this is a
    /// measurement tool, not always-on telemetry.
    [[nodiscard]] PipelineStatsCapture CapturePipelineStats() noexcept;

    /// Triggers hardware fault diagnostic dumps and unblocks GPU crash handlers.
    void OnDeviceLost() noexcept;

    RenderResult BuildMeshBLAS(Mesh& mesh) noexcept;

    /// Legacy explicit resize for tools/tests: equivalent to a GraphicsSettings delta
    /// on shadows.resolution, the path ApplySettings uses internally.
    [[nodiscard]] std::expected<void, ErrorCode> SetShadowResolution(uint32_t resolution);
    void                                     ProvokeDeviceLost();

    auto          BakeProceduralTexture(uint32_t width, uint32_t height, uint32_t variantIdx, float scale, float randomness) -> std::expected<uint32_t, ErrorCode>;
    TextureHandle CreateProceduralTexture(std::string_view name, uint32_t width, uint32_t height, bool isSRGB, const uint32_t* pixels);

    [[nodiscard]] std::expected<void, ErrorCode> CaptureScreenshotPPM(std::string_view outputPath) noexcept;

    // --- OOP Idiomatic State & Command Submission APIs
    /// Current optical state of the frame's view; RenderScene overwrites the
    /// camera-derived slice (view/proj/invViewProj, camPos) from its SceneView.
    void SetMatrices(const JPH::Mat44& viewProj, const JPH::Mat44& unjitteredViewProj) noexcept;
    /// Frame-uniform state belonging to the *frame*, not one view: sun/sky/probe values,
    /// TAA jitter and previous-frame matrices, the cascade shadow matrix, dt. Kept until
    /// changed, so a view only describes the current optics.
    void SetFrameData(const Camera& cam, const FrameUniforms& uniforms, const JPH::Mat44& shadowProjView, float dt = 0.0166f) noexcept;

    /// Writes view/proj and camPos into the live FrameUniforms slot (no cascade rebuild).
    void BindCamera(const Camera& cam, Extent2D viewSize) noexcept;
    /// Drops the queued draws without rendering them, for callers building a second
    /// view's draw list on the same queue.
    void ClearDrawQueues() noexcept;

    // --- Canonical graphics configuration
    /// Single entry point for graphics configuration: diffs `newSettings` against the
    /// current state and reacts to deltas (resizing cascade shadow targets when
    /// shadows.resolution changes); plain knob changes flow into the next frame's
    /// uniforms, push constants and pipeline-variant selection. Call between BeginFrame
    /// batches -- RenderSystem's ECS sync point does it once per frame.
    void ApplySettings(GraphicsSettings newSettings) noexcept;

    /// Snapshot of the renderer's canonical GraphicsSettings (last applied).
    [[nodiscard]] const GraphicsSettings& GetSettings() const noexcept;

    /// Legacy bridge for tools/tests: overwrites only the post/GI slice. Prefer the ECS
    /// settings components -- RenderSystem re-applies the collected state every frame.
    void SetGISettings(const GISettings& settings) noexcept;
    /// Legacy bridge for tools/tests: overwrites the AA state. The ECS
    /// AASettingsComponent is authoritative and re-applied every frame.
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

    /// False when the device offers no statistics queries and the capture never
    /// started.
    explicit operator bool() const noexcept {
        return _impl != nullptr;
    }

    /// Counter sums over the frames completed since the previous Consume() (or since the
    /// capture started), resetting the accumulator.
    [[nodiscard]] GpuPipelineCounters Consume() noexcept;

  private:
    friend class RenderContext;
    explicit PipelineStatsCapture(RenderContext::Impl* impl) noexcept: _impl(impl) {
    }

    RenderContext::Impl* _impl = nullptr;
};

} // namespace ZHLN
