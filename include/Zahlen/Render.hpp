// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "Common.h"
#include <Zahlen/Config.hpp>
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/Pair.hpp>
#include <Zahlen/Core/Reflection.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Types.hpp>
#include <Zahlen/Window.hpp>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>

namespace ZHLN {

// ============================================================================
// Renderer Capability Errors
// Backend-neutral errors produced by the renderer's optional-feature paths
// (e.g. ray-tracing BLAS builds). These deliberately model renderer-level
// capabilities rather than any graphics API, so content/asset code can branch
// on "this GPU lacks the optional feature" without knowing about Vulkan.
// ============================================================================
enum class RenderFeatureError : uint8_t {
    FeatureNotSupported[[= ZHLN::Reflect::Description("The requested render feature is not supported on this device")]] = 1,
};

namespace Shadows {
inline constexpr float NearClip   = 0.1f;
inline constexpr float BaseOffset = 150.0f;
inline constexpr float BaseDepth  = 300.0f;
inline constexpr float FarOffset  = 500.0f;
inline constexpr float FarDepth   = 1000.0f;
} // namespace Shadows

enum class RenderFrameResult : uint8_t { Success = 1, Suboptimal, OutOfDate, DeviceLost, Error };

using RenderResult = std::expected<void, Error>;

struct PipelineDesc {
    const void* vertexShaderData = nullptr;
    size_t      vertexShaderSize = 0;
    const void* fragShaderData   = nullptr;
    size_t      fragShaderSize   = 0;

    // VK_EXT_mesh_shader: optional task/mesh stages. When both the device
    // supports mesh shading and `meshShaderData` is set, the material gets a
    // SECOND pipeline built from task+mesh+fragment. The vertex pipeline is
    // always built as well, so the renderer can fall back per draw call
    // (skinned meshes, meshes without meshlet streams, unsupported devices).
    const void* taskShaderData = nullptr;
    size_t      taskShaderSize = 0;
    const void* meshShaderData = nullptr;
    size_t      meshShaderSize = 0;
    bool        doubleSided    = false;
    bool        alphaBlend     = false;
    bool        additiveBlend  = false; // ADDED: Support for emissive particles
    bool        isLineList     = false;
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

struct Camera;

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

    [[nodiscard]] static std::expected<std::unique_ptr<RenderContext>, Error> Create(Window& window, const RenderConfig& cfg) noexcept;

    void CheckShaderReload() noexcept;

    [[nodiscard]] std::optional<Extent2D> GetFramebufferSize() const;

    [[nodiscard]] RenderResult BeginFrame() noexcept;
    [[nodiscard]] RenderResult EndFrame() noexcept;
    void                       BeginImGuiFrame() noexcept;
    void                       SetResolution(const Extent2D& resolution);
    [[nodiscard]] const char*  GetRendererName() const;
    [[nodiscard]] const char*  GetGPUName() const;
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
    BufferHandle GetOrCreateParticleBuffer(uint64_t entityKey, uint32_t maxParticles);
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
    [[nodiscard]] std::expected<Material, Error> CreateMaterial(const PipelineDesc& desc);
    [[nodiscard]] std::expected<Material, Error> CreateDebugLineMaterial();
    [[nodiscard]] std::expected<Material, Error> CreateDebugSolidMaterial();

    auto CreateSkinnedScratchBuffer(uint32_t vertexCount) -> BufferHandle;

    void                       UploadDebugVertices(const void* posData, size_t posSize, const void* attrData, size_t attrSize, uint32_t vertexCount) noexcept;
    [[nodiscard]] BufferHandle GetDebugMeshBuffer() const noexcept;

    void SubmitUI(
        const UIBatch*          batches,
        uint32_t                batchCount,
        const VertexPosition*   positions,
        const VertexAttributes* attributes,
        uint32_t                vertexCount
    ) noexcept;

    void DrawLine(JPH::Vec3Arg start, JPH::Vec3Arg end, JPH::Vec4Arg colorStart, JPH::Vec4Arg colorEnd) noexcept;
    void DrawLine(JPH::Vec3Arg start, JPH::Vec3Arg end, JPH::Vec4Arg color) noexcept {
        DrawLine(start, end, color, color);
    }

    [[nodiscard]] uint32_t GetBindlessIndex(TextureHandle handle) const noexcept;

    [[nodiscard]] auto          CreateTexture(const void* data, uint32_t width, uint32_t height, bool isSRGB = true) -> std::expected<uint32_t, Error>;
    [[nodiscard]] auto          CreateTextureCube(const void* const* faceData, uint32_t width, uint32_t height) -> std::expected<uint32_t, Error>;
    [[nodiscard]] TextureHandle RegisterTexture(std::string_view name, uint32_t bindlessIndex, bool isSRGB = true);

    /**
     * @brief Generates a texture procedurally by invoking a CPU-side callback to populate the pixel buffer.
     * @param callback A callable with signature: void(uint32_t* pixels, uint32_t width, uint32_t height)
     */
    template <typename Func>
    [[nodiscard]] auto CreateTextureProcedural(uint32_t width, uint32_t height, bool isSRGB, Func&& callback) -> std::expected<uint32_t, Error> {
        std::vector<uint32_t> pixels(static_cast<size_t>(width * height));
        callback(pixels.data(), width, height);
        return CreateTexture(pixels.data(), width, height, isSRGB);
    }

    void     UpdateJointMatrices(uint32_t offset, const JPH::Mat44* matrices, uint32_t count);
    uint32_t AllocateMorphDeltas(uint32_t count, const float* deltas);

    ZHLN::Array<ZHLN::Pair<uint64_t, BufferHandle>>& GetTracked2DEmitters() noexcept;
    ZHLN::Array<ZHLN::Pair<uint64_t, BufferHandle>>& GetTracked3DEmitters() noexcept;

    // --- VK_EXT_mesh_shader ---
    /// True when the device exposes mesh shading with limits sufficient for the
    /// engine's meshlet budget (independent of whether it is currently in use).
    [[nodiscard]] bool MeshShadingSupported() const noexcept;
    /// True when scene geometry is actually being drawn through task/mesh
    /// shaders this frame (supported AND not disabled).
    [[nodiscard]] bool MeshShadingActive() const noexcept;
    /// Runtime override of ZHLN_NO_MESH_SHADING. Call between frames only;
    /// both pipelines are always built, so this only changes which is bound.
    void SetMeshShadingEnabled(bool enabled) noexcept;

    /// True when the device exposes acceleration structures (BLAS/TLAS) and the
    /// engine's raytracing context initialised. The RTR reflection and
    /// ray-traced shadow paths are only active when this is true AND
    /// PostProcessSettingsComponent::enableRTR is set; callers use this to skip
    /// RTR-only verification on devices without support (e.g. lavapipe).
    [[nodiscard]] bool RayTracingSupported() const noexcept;

    /// Validation-layer errors seen so far (0 when validation is off). Snapshot
    /// it around a workload to assert that the workload is VUID-clean.
    [[nodiscard]] static uint32_t ValidationErrorCount() noexcept;

    [[nodiscard]] static uint32_t DeviceLostCount() noexcept;

    /// Injects a diagnostic GPU breadcrumb into the active frame's command stream.
    void WriteCheckpoint(std::string_view name) noexcept;

    /// Triggers hardware fault diagnostic dumps and unblocks GPU crash handlers.
    void OnDeviceLost() noexcept;

    RenderResult BuildMeshBLAS(Mesh& mesh) noexcept;

    /// Legacy explicit resize, kept for tools/tests. Equivalent to applying a
    /// GraphicsSettings delta on shadows.resolution — the reactive path
    /// RenderContext::ApplySettings uses internally.
    [[nodiscard]] std::expected<void, Error> SetShadowResolution(uint32_t resolution);
    void                                     ProvokeDeviceLost();

    auto          BakeProceduralTexture(uint32_t width, uint32_t height, uint32_t variantIdx, float scale, float randomness) -> std::expected<uint32_t, Error>;
    TextureHandle CreateProceduralTexture(std::string_view name, uint32_t width, uint32_t height, bool isSRGB, const uint32_t* pixels);

    [[nodiscard]] std::expected<void, Error> CaptureScreenshotPPM(std::string_view outputPath) noexcept;

    // --- OOP Idiomatic State & Command Submission APIs ---
    void SetMatrices(const JPH::Mat44& viewProj, const JPH::Mat44& unjitteredViewProj) noexcept;
    void SetFrameData(const Camera& cam, const FrameUniforms& uniforms, const JPH::Mat44& shadowProjView, float dt = 0.0166f) noexcept;

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

} // namespace ZHLN
