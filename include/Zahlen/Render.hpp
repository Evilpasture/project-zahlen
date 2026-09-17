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

// ============================================================================
// Renderer Capability Errors
// Backend-neutral errors produced by the renderer's optional-feature paths
// (e.g. ray-tracing BLAS builds). These deliberately model renderer-level
// capabilities rather than any graphics API, so content/asset code can branch
// on "this GPU lacks the optional feature" without knowing about Vulkan.
// ============================================================================
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

enum class RenderFrameResult : uint8_t { Success = 1, Suboptimal, OutOfDate, DeviceLost, Error };

/// How finished frames reach a display, chosen once at device creation.
/// Kept distinct from "headless" so a windowed session with no window-system
/// integration (macOS has no native Vulkan WSI) does not masquerade as a
/// CI run: headless stays true "no window, no presenter", and macOS
/// windowed sessions present through the host-GL blit instead.
enum class PresentationMode : uint8_t {
    /// Standard Vulkan WSI: VkSurfaceKHR + VkSwapchainKHR.
    NativeSwapchain,
    /// Offscreen Vulkan render target copied out and blitted through the
    /// HostBlit plugin's own OpenGL window (macOS).
    HostBlit,
    /// No window and no presenter at all: CI / servers / --headless.
    OffscreenOnly,
};

/// Physical-device class. Mirrors the graphics API's device-type enum
/// (Vulkan VkPhysicalDeviceType, etc.) without naming any backend.
enum class PhysicalDeviceType : uint8_t {
    Other         = 0,
    IntegratedGPU = 1,
    DiscreteGPU   = 2,
    VirtualGPU    = 3,
    CPU           = 4,
};

/// Snapshot of renderer identity and optional-feature status. `rendererName`
/// and `gpuName` remain valid for the lifetime of the RenderContext that
/// produced the snapshot.
struct RenderInfo {
    std::string_view   rendererName         = {};
    std::string_view   gpuName              = {};
    PhysicalDeviceType deviceType           = PhysicalDeviceType::Other;
    PresentationMode   presentationMode     = PresentationMode::OffscreenOnly;
    bool               meshShadingSupported = false;
    bool               meshShadingActive    = false;
    bool               rayTracingSupported  = false;
};

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

/// GPU pipeline counters summed over every profiled pass of the captured
/// frames (hardware VK_QUERY_TYPE_PIPELINE_STATISTICS; see
/// RenderContext::CapturePipelineStats). Counters the device does not
/// support stay 0.
///
/// The ratios this exists to measure:
///   * Clipping: 1 - clipperPrimitivesOut / clipperInvocations.
///   * Meshlet culling: meshInvocations is the number of mesh workgroups the
///     GPU executed after task-level culling; compare it against the count of
///     meshlets the scene issued to get the cull rate.
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

    /// Pass the engine-owned watcher to enable development shader reloads. The
    /// optional pointer keeps direct RenderContext users source-compatible and,
    /// when non-null, must outlive the RenderContext.
    [[nodiscard]] static std::expected<std::unique_ptr<RenderContext>, ErrorCode>
        Create(Window& window, const RenderConfig& cfg, FileSystemWatcher* fileSystemWatcher = nullptr) noexcept;

    [[nodiscard]] std::optional<Extent2D> GetFramebufferSize() const;

    // --- Frame Lifecycle (GPU synchronization and presentation only) ---
    //
    // BeginFrame/EndFrame open and close one frame slot: fences, allocators,
    // the transient descriptor partition, and presentation. They deliberately
    // run *no* rendering: a 2D-only client (the UI editor) never executes a
    // single 3D pass, and a frame that renders nothing costs nothing.
    [[nodiscard]] RenderResult BeginFrame() noexcept;
    [[nodiscard]] RenderResult EndFrame() noexcept;
    void                       SetResolution(const Extent2D& resolution);

    /// Sub-rectangle of the framebuffer the 3D scene renders into, in pixels
    /// (top-left origin, like window coordinates). Applied as a fixed-function
    /// viewport and scissor on the screen-space scene passes: nothing outside
    /// the rectangle is rasterized. Attachment clears still cover the whole
    /// target, so excluded regions stay clean. Width or height <= 1 restores
    /// full-frame rendering; rectangles are clamped to the framebuffer.
    /// The camera aspect, GPU culling screen space, and picking should all use
    /// this rectangle -- see GetViewport.
    using ViewportRect = ZHLN::ViewportRect;

    void                       SetViewport(const ViewportRect& rect) noexcept;
    /// Effective scene viewport: the stored rectangle clamped to the
    /// framebuffer, or {0, 0, framebuffer} when none is active.
    [[nodiscard]] ViewportRect GetViewport() const noexcept;
    /// Width / height of GetViewport(), or 1.0 when the viewport is degenerate.
    [[nodiscard]] float        GetViewportAspect() const noexcept;
    /// Identity, presentation path, and optional-feature status as of Create.
    [[nodiscard]] RenderInfo   GetInfo() const noexcept;
    [[nodiscard]] uint32_t     GetFrameIndex() const noexcept;

    // --- High-Level Asset Resolution & GPU Cache API ---
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

    // --- Opaque Resource Creation API ---
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

    // --- Window Attachment Vending ------------------------------------------
    //
    // A window is a destination, not a mode: the renderer hands out the
    // subresource for the swapchain image it acquired for this frame, and the
    // caller decides what to render into it (a 3D scene, 2D UI, or both). The
    // window is acquired on first vending each frame and presented by
    // EndFrame. Headless windows vend the offscreen color target instead, so
    // the same call site works with no window system at all.
    [[nodiscard]] RenderAttachment GetWindowAttachment(const Window& window) noexcept;

    /// Releases the swapchain and present resources of a window the caller is
    /// about to destroy. Idempotent; an unknown window is a no-op.
    void ReleaseWindow(const Window& window) noexcept;

    // --- Dynamic Render-to-Texture (RTT) ------------------------------------
    /// Creates an offscreen texture that can be rendered into and sampled in
    /// materials. The returned handle addresses it as a RenderAttachment
    /// *and* resolves to a bindless slot, so `CreateMaterial` may bind it.
    [[nodiscard]] auto CreateRenderTexture(uint32_t width, uint32_t height, bool hdr = false) -> std::expected<TextureHandle, ErrorCode>;
    void               DestroyRenderTexture(TextureHandle handle) noexcept;

    // --- Opaque Render Dispatches -------------------------------------------
    //
    // These are the only entry points that record 3D/2D work. Their
    // implementations live in src/render/pipelines/ and are never visible to
    // callers: no pipeline header, no graph type, no pass list crosses this
    // boundary.
    //
    /// Renders the queued scene draws (Draw/DrawCSG/DrawDecal/DrawLine and the
    /// particle emitters) into `view.target` with the given optics. `settings`
    /// is applied on the way in, so it must be the frame's canonical state.
    void RenderScene(const SceneView& view, const GraphicsSettings& settings) noexcept;
    /// Draws a Clay-geometry payload into `view.target`. Safe to call over the
    /// same target a scene was just rendered to (HUD overlay): the target's
    /// contents are preserved.
    void RenderUI(const UIView& view, const UIDrawData& uiData) noexcept;
    /// Records and submits the compute simulations (cluster culling, volumetric
    /// fog, particle updates) for this frame. Must be called before RenderScene
    /// when a 3D scene is drawn; a frame that only draws UI never pays for it.
    void DispatchCompute(float dt) noexcept;

    void DrawLine(JPH::Vec3Arg start, JPH::Vec3Arg end, JPH::Vec4Arg colorStart, JPH::Vec4Arg colorEnd) noexcept;
    void DrawLine(JPH::Vec3Arg start, JPH::Vec3Arg end, JPH::Vec4Arg color) noexcept {
        DrawLine(start, end, color, color);
    }

    [[nodiscard]] uint32_t GetBindlessIndex(TextureHandle handle) const noexcept;

    [[nodiscard]] auto          CreateTexture(const void* data, uint32_t width, uint32_t height, bool isSRGB = true) -> std::expected<uint32_t, ErrorCode>;
    [[nodiscard]] auto          CreateTextureCube(const void* const* faceData, uint32_t width, uint32_t height) -> std::expected<uint32_t, ErrorCode>;
    [[nodiscard]] TextureHandle RegisterTexture(std::string_view name, uint32_t bindlessIndex, bool isSRGB = true);
    /// Releases the bindless slot behind a handle registered through
    /// RegisterTexture or CreateProceduralTexture. The record is dropped
    /// immediately -- later GetBindlessIndex calls resolve to the white
    /// fallback -- and the slot is recycled once the frames that could still
    /// read its descriptor have retired, so unload and streaming loops stop
    /// eating the 32768-entry index space. Unknown handles, and the engine's
    /// black/white/normal fallbacks, are a no-op.
    void UnloadTexture(TextureHandle handle);

    /**
     * @brief Generates a texture procedurally by invoking a CPU-side callback to populate the pixel buffer.
     * @param callback A callable with signature: void(uint32_t* pixels, uint32_t width, uint32_t height)
     */
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

    /// Records a buffer outside ECS storage. The owner association survives a
    /// plain Registry::Destroy so the render lifecycle can reclaim it later.
    void TrackEntityBuffer(Entity owner, BufferHandle buffer);
    /// Releases every buffer currently attributed to owner, including particle
    /// ledgers. DespawnEntity uses this for immediate ordered teardown.
    void ReleaseEntityBuffers(Entity owner);
    /// Reclaims tracked buffers whose ECS owner has already died.
    void ReconcileEntityBuffers(EntityAliveQuery alive);
    [[nodiscard]] auto GetTrackedEntityBufferCount() const noexcept -> size_t;

    /// Validation-layer errors observed by the ACTIVE engine (live view:
    /// zero when no engine exists). Snapshot it around a workload to assert
    /// that the workload is VUID-clean. Observers that need values to
    /// OUTLIVE an engine (test frameworks bracketing whole engine
    /// lifecycles) register their own storage with UseDiagnostics() and read
    /// that instead.
    [[nodiscard]] static uint32_t ValidationErrorCount() noexcept;

    /// Device-lost / hang events observed by the ACTIVE engine (live view:
    /// zero when no engine exists). See ValidationErrorCount().
    [[nodiscard]] static uint32_t DeviceLostCount() noexcept;

    /// Registers caller-owned diagnostics storage (both or neither; pass
    /// nullptrs to revert to per-instance counting). Every engine created
    /// afterwards increments these atomics directly -- including
    /// teardown-time validation events fired during instance destruction --
    /// so deltas taken across an engine's full lifecycle are exact with no
    /// post-mortem state in the library. The storage must outlive every
    /// engine created after registration, and registration must happen
    /// before engine creation (the sink is resolved once per instance, not
    /// synchronised against concurrent engine creation).
    static void UseDiagnostics(std::atomic<uint32_t>* validationErrors, std::atomic<uint32_t>* deviceLost) noexcept;

    /// Injects a diagnostic GPU breadcrumb into the active frame's command stream.
    void WriteCheckpoint(std::string_view name) noexcept;

    /// Starts a scoped GPU pipeline-counter capture (hardware pipeline
    /// statistics queries around the profiled render passes). The capture is
    /// live while the returned object is alive; its destructor stops the
    /// capture. Returns an inactive capture (converts to false) when the
    /// device does not support statistics queries.
    ///
    /// Statistics queries make drivers serialize counter bookkeeping, so this
    /// is a measurement tool, not always-on telemetry -- nothing is recorded
    /// while no capture object exists.
    [[nodiscard]] PipelineStatsCapture CapturePipelineStats() noexcept;

    /// Triggers hardware fault diagnostic dumps and unblocks GPU crash handlers.
    void OnDeviceLost() noexcept;

    RenderResult BuildMeshBLAS(Mesh& mesh) noexcept;

    /// Legacy explicit resize, kept for tools/tests. Equivalent to applying a
    /// GraphicsSettings delta on shadows.resolution — the reactive path
    /// RenderContext::ApplySettings uses internally.
    [[nodiscard]] std::expected<void, ErrorCode> SetShadowResolution(uint32_t resolution);
    void                                     ProvokeDeviceLost();

    auto          BakeProceduralTexture(uint32_t width, uint32_t height, uint32_t variantIdx, float scale, float randomness) -> std::expected<uint32_t, ErrorCode>;
    TextureHandle CreateProceduralTexture(std::string_view name, uint32_t width, uint32_t height, bool isSRGB, const uint32_t* pixels);

    [[nodiscard]] std::expected<void, ErrorCode> CaptureScreenshotPPM(std::string_view outputPath) noexcept;

    // --- OOP Idiomatic State & Command Submission APIs ---
    /// Current optical state of the frame's view. RenderScene overwrites the
    /// camera-derived slice (view/proj/invViewProj, camPos) from its SceneView.
    void SetMatrices(const JPH::Mat44& viewProj, const JPH::Mat44& unjitteredViewProj) noexcept;
    /// Frame-uniform state that belongs to the *frame*, not to one view:
    /// sun/sky/probe values, TAA jitter and previous-frame matrices, the
    /// cascade shadow matrix, and the frame delta time. Kept until changed, so
    /// a view only has to describe the current optics.
    void SetFrameData(const Camera& cam, const FrameUniforms& uniforms, const JPH::Mat44& shadowProjView, float dt = 0.0166f) noexcept;

    /// Writes view/proj and camPos into the live FrameUniforms slot (no cascade rebuild).
    void BindCamera(const Camera& cam, Extent2D viewSize) noexcept;
    /// Drops the queued draws without rendering them. Used by callers that
    /// build a second view's draw list on top of the same queue.
    void ClearDrawQueues() noexcept;

    // --- Canonical graphics configuration ---------------------------------
    /// Single entry point for graphics configuration. Diffs `newSettings`
    /// against the current state and reacts to deltas (e.g. resizing the
    /// cascade shadow targets when shadows.resolution changes); plain knob
    /// changes simply flow into the next frame's uniforms, push constants and
    /// pipeline-variant selection. Call between BeginFrame batches — the ECS
    /// sync point in RenderSystem does this once per frame.
    void ApplySettings(GraphicsSettings newSettings) noexcept;

    /// Snapshot of the renderer's canonical GraphicsSettings (last applied).
    [[nodiscard]] const GraphicsSettings& GetSettings() const noexcept;

    /// Legacy bridge kept for tools/tests: overwrites only the post/GI slice.
    /// Prefer mutating the ECS settings components (the editing surface) —
    /// RenderSystem re-applies the collected state every frame.
    void SetGISettings(const GISettings& settings) noexcept;
    /// Legacy bridge kept for tools/tests: overwrites the AA state. The ECS
    /// AASettingsComponent is authoritative and is re-applied every frame.
    void SetAAState(const AAState& state);
    void SetLights(const Light* lights, uint32_t count) noexcept;
    void Draw(const Material& material, const Mesh& mesh, const DrawParams& params) noexcept;
    void DrawCSG(const Material& eyeMaterial, const Mesh& eyeMesh, const CSGDrawParams& params) noexcept;
    void DrawDecal(const DecalParams& params) noexcept;

  private:
    std::unique_ptr<Impl> _impl;
};

// ============================================================================
// Scoped GPU Pipeline-Counter Capture
// ============================================================================
//
// Created by RenderContext::CapturePipelineStats(). A live capture records
// hardware pipeline statistics around the profiled render passes; the
// destructor stops the capture, so no loose on/off flag can be left behind.
// Counters accumulate over COMPLETED frames -- retrieval lags one frame (a
// frame's counters are pulled at the next frame's begin), so tick one extra
// frame after the measured work before Consume().
//
// Move-only. Must not outlive the RenderContext it was created from.
class ZHLN_API PipelineStatsCapture {
  public:
    PipelineStatsCapture() noexcept = default;
    PipelineStatsCapture(PipelineStatsCapture&& other) noexcept;
    auto operator=(PipelineStatsCapture&& other) noexcept -> PipelineStatsCapture&;
    ~PipelineStatsCapture() noexcept;

    PipelineStatsCapture(const PipelineStatsCapture&)                    = delete;
    auto operator=(const PipelineStatsCapture&) -> PipelineStatsCapture& = delete;

    /// False when the device offers no statistics queries and the capture
    /// never started (explicit: use bool(capture) inside an expectation).
    explicit operator bool() const noexcept {
        return _impl != nullptr;
    }

    /// Counter sums over the frames completed since the previous Consume()
    /// (or since the capture started), resetting the accumulator.
    [[nodiscard]] GpuPipelineCounters Consume() noexcept;

  private:
    friend class RenderContext;
    explicit PipelineStatsCapture(RenderContext::Impl* impl) noexcept: _impl(impl) {
    }

    RenderContext::Impl* _impl = nullptr;
};

} // namespace ZHLN
