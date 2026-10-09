// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <Zahlen/Camera.hpp>
#include <Zahlen/Config.hpp>
#include <Zahlen/Core/Optional.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Core/ErrorCode.hpp>
#include <Zahlen/Geometry2D.hpp>
#include <Zahlen/GraphicsSettings.hpp>
#include <Zahlen/ParticleEmitterDesc.hpp>
#include <Zahlen/Render/FrameResult.hpp>
#include <Zahlen/Render/FrameData.hpp>
#include <Zahlen/Render/Info.hpp>
#include <Zahlen/Render/PipelineStats.hpp>
#include <Zahlen/Render/PresentTiming.hpp>
#include <Zahlen/Render/Types.hpp>
#include <Zahlen/Render/View.hpp>
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Vertex.hpp>
#include <Zahlen/gui/UIData.hpp>
#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace ZHLN {


struct Camera;
namespace FS {
class FileSystemWatcher;
}
using FileSystemWatcher = FS::FileSystemWatcher;
class PipelineStatsCapture;
class PresentationTarget;

struct EnvironmentRadianceDesc {
    // Original top-down HDR is retained for the visible sky. When supplied,
    // the conditioned lighting map drives BOTH SH and specular prefiltering.
    std::span<const float> rgba {};
    std::span<const float> lightingRgba {};
    // Exact smooth irradiance from asset preparation; empty = GPU SH bake.
    std::span<const std::array<float, 3>> diffuseSH {};
    Extent2D               extent {};
    uint64_t               contentHash  = 0;
    bool                   renderSkybox = false;
};


// One immediate-mode quad, in scene terms: where it is, how big it is, how it is
// turned, what it is coloured, and which way it faces. Everything the renderer's
// particle vertex shader needs, and nothing about how it is stored.
struct BillboardQuad {
    JPH::Vec3         position {};                    // world-space centre
    float             size     = 1.0f;                // world units, the quad's edge
    float             rotation = 0.0f;                // radians, in the quad's own plane
    JPH::Vec4         color {1.0f, 1.0f, 1.0f, 1.0f}; // straight RGBA, multiplied with the texture
    ParticleAlignment facing = ParticleAlignment::CameraBillboard;
    JPH::Vec3         velocity {}; // facing == VelocityStretched: the axis it stretches along
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

    // Explicit teardown: drains the device, saves the pipeline cache, and
    // releases destinations, diagnostics and pending staging work. The
    // destructor calls this; hosts may also call it to release GPU resources
    // before the RenderContext itself goes away.
    void Destroy() noexcept;

    RenderContext(const RenderContext&)                    = delete;
    auto operator=(const RenderContext&) -> RenderContext& = delete;

    [[nodiscard]] static std::expected<std::unique_ptr<RenderContext>, ErrorCode>
        Create(PresentationTarget& target, const RenderConfig& cfg, ZHLN::Optional<FileSystemWatcher&> fileSystemWatcher = std::nullopt) noexcept;

    [[nodiscard]] std::optional<Extent2D> GetFramebufferSize() const;


    [[nodiscard]] FrameOutcome<FrameSkipped> BeginFrame() noexcept;

    [[nodiscard]] FrameOutcome<PresentSuboptimal> EndFrame() noexcept;

    void SetResolution(const Extent2D& resolution);

    using ViewportRect = ZHLN::ViewportRect;

    void SetViewport(const ViewportRect& rect) noexcept;
    [[nodiscard]] ViewportRect GetViewport() const noexcept;
    [[nodiscard]] float GetViewportAspect() const noexcept;
    [[nodiscard]] RenderInfo GetInfo() const noexcept;
    [[nodiscard]] uint32_t   GetFrameIndex() const noexcept;

    [[nodiscard]] std::optional<float> GetPacedDeltaTime() const noexcept;
    [[nodiscard]] PresentTimingMetrics GetPresentTiming() const noexcept;

    [[nodiscard]] std::optional<Mesh>     GetGPUMesh(AssetID id) const noexcept;
    [[nodiscard]] std::optional<Material> GetGPUMaterial(MaterialID id) const noexcept;
    // Non-owning lookup: registration never transfers buffer ownership.
    // Unregister before releasing the buffers; a cache clear only drops lookups.
    void                                  RegisterGPUMesh(AssetID id, Mesh mesh) noexcept;
    void                                  UnregisterGPUMesh(AssetID id) noexcept;
    // Release all buffers referenced by one mesh (but not its registration).
    void                                  DestroyMesh(const Mesh& mesh) noexcept;
    void                                  RegisterGPUMaterial(MaterialID id, Material mat) noexcept;
    // Retire a registered material's pipelines when the scene no longer uses it.
    void                                  UnregisterGPUMaterial(MaterialID id) noexcept;
    // Drops mesh lookups (not their caller-owned buffers), materials, and textures.
    void                                  ClearGPUCaches() noexcept;

    BufferHandle CreateStorageBuffer(size_t size);

    // Descriptions, not GPU layouts: the renderer allocates and retains each
    // emitter's simulation storage, keyed by a stable, per-emitter identity
    // (normally Entity::Pack()). Keep it unique within this context and stable
    // across submissions. Storage is evicted after 120 idle frames; use a new
    // identity for a logically new emitter.
    void SubmitParticleEmitter(
        uint64_t emitterId, uint32_t maxParticles, const ParticleEmitterDesc& desc, TextureHandle texture, bool additive = false
    );
    void SubmitMeshParticleEmitter(
        uint64_t emitterId, uint32_t maxParticles, const MeshParticleEmitterDesc& desc, AssetID mesh, MaterialID mat
    );

    // Immediate-mode quads: the host describes billboards in world space for this frame,
    // and the renderer packs, buffers and draws them. Nothing is retained, so a host
    // keeps no BufferHandle, resolves no bindless slot and knows no stride -- which is
    // what a gameplay effect wants when it has positions and colours and no business
    // with a storage layout.
    void DrawBillboards(TextureHandle texture, std::span<const BillboardQuad> billboards, bool additive = false);

    // Raw byte streams declare their element stride; typed spans derive it.
    [[nodiscard]] auto CreateStorageBuffer(std::span<const std::byte> bytes, uint32_t stride) -> BufferHandle;
    [[nodiscard]] auto CreateVertexBuffer(std::span<const std::byte> bytes, uint32_t stride) -> BufferHandle;
    [[nodiscard]] auto CreateIndexBuffer(std::span<const uint32_t> indices) -> BufferHandle;
    void DestroyBuffer(BufferHandle handle);
    void UpdateBuffer(BufferHandle handle, std::span<const std::byte> bytes) noexcept;

    // Accept both dynamic spans and fixed-extent spans deduced from std::array.
    template <typename T, size_t SpanExtent> requires std::is_trivially_copyable_v<T>
    [[nodiscard]] auto CreateStorageBuffer(std::span<T, SpanExtent> elements) -> BufferHandle {
        return CreateStorageBuffer(std::as_bytes(elements), static_cast<uint32_t>(sizeof(T)));
    }
    template <typename T, size_t SpanExtent> requires std::is_trivially_copyable_v<T>
    [[nodiscard]] auto CreateVertexBuffer(std::span<T, SpanExtent> vertices) -> BufferHandle {
        return CreateVertexBuffer(std::as_bytes(vertices), static_cast<uint32_t>(sizeof(T)));
    }
    template <typename T, size_t SpanExtent> requires std::is_trivially_copyable_v<T>
    void UpdateBuffer(BufferHandle handle, std::span<T, SpanExtent> elements) noexcept {
        UpdateBuffer(handle, std::as_bytes(elements));
    }
    auto CreateConstantBuffer(size_t size) -> BufferHandle;
    [[nodiscard]] std::expected<Material, ErrorCode> CreateBasicMaterial(bool doubleSided = false, bool alphaBlend = false, bool additiveBlend = false, bool depthWrite = false);
    [[nodiscard]] std::expected<Material, ErrorCode> CreateMaterial(const MaterialDesc& desc);

    auto CreateSkinnedScratchBuffer(uint32_t vertexCount) -> BufferHandle;

    [[nodiscard]] uint32_t     UploadDebugVertices(std::span<const VertexPosition> positions, std::span<const VertexSurface> surfaces) noexcept;
    [[nodiscard]] BufferHandle GetDebugMeshBuffer() const noexcept;


    [[nodiscard]] auto AcquireTarget(const PresentationTarget& target) noexcept -> FrameOutcome<FrameTarget>;

    [[nodiscard]] std::optional<FrameTarget> GetAcquiredTarget(const PresentationTarget& target) noexcept;

    void ReleaseTarget(const PresentationTarget& target) noexcept;

    [[nodiscard]] auto CreateRenderTexture(uint32_t width, uint32_t height, bool hdr = false) -> std::expected<RenderTextureHandle, ErrorCode>;
    void               DestroyRenderTexture(RenderTextureHandle handle) noexcept;


    [[nodiscard]] auto RenderScene(const SceneView& view, const GraphicsSettings& settings) noexcept -> FrameOutcome<FrameSkipped>;
    [[nodiscard]] auto RenderUI(const UIView& view, const UIDrawData& uiData) noexcept -> FrameOutcome<FrameSkipped>;
    [[nodiscard]] auto DispatchSimulations(float dt) noexcept -> RenderResult;

    void DrawLine(JPH::Vec3Arg start, JPH::Vec3Arg end, JPH::Vec4Arg colorStart, JPH::Vec4Arg colorEnd) noexcept;
    void DrawLine(JPH::Vec3Arg start, JPH::Vec3Arg end, JPH::Vec4Arg color) noexcept {
        DrawLine(start, end, color, color);
    }

    // Only draw submission needs the GPU descriptor index; resource creation
    // and lifetime use TextureHandle throughout.
    [[nodiscard]] uint32_t GetBindlessIndex(TextureHandle handle) const noexcept;

    // Unnamed uploads each get a fresh handle; release it with UnloadTexture.
    [[nodiscard]] auto CreateTexture(std::span<const std::byte> rgba, Extent2D extent, bool isSRGB = true) -> std::expected<TextureHandle, ErrorCode>;
    // A name gives repeat uploads a stable identity and deduplicates identical pixels.
    [[nodiscard]] auto CreateTexture(std::string_view name, std::span<const std::byte> rgba, Extent2D extent, bool isSRGB = true)
        -> std::expected<TextureHandle, ErrorCode>;
    [[nodiscard]] auto CreateTextureCube(std::array<std::span<const std::byte>, 6> faces, uint32_t faceSize) -> std::expected<TextureHandle, ErrorCode>;
    template <typename T, size_t SpanExtent> requires std::is_trivially_copyable_v<T>
    [[nodiscard]] auto CreateTexture(std::span<T, SpanExtent> pixels, Extent2D extent, bool isSRGB = true)
        -> std::expected<TextureHandle, ErrorCode> {
        return CreateTexture(std::span<const std::byte> {std::as_bytes(pixels)}, extent, isSRGB);
    }
    template <typename T, size_t SpanExtent> requires std::is_trivially_copyable_v<T>
    [[nodiscard]] auto CreateTexture(std::string_view name, std::span<T, SpanExtent> pixels, Extent2D extent, bool isSRGB = true)
        -> std::expected<TextureHandle, ErrorCode> {
        return CreateTexture(name, std::span<const std::byte> {std::as_bytes(pixels)}, extent, isSRGB);
    }
    void UnloadTexture(TextureHandle handle);

    template <typename Func> requires std::invocable<Func&, std::span<uint32_t>>
    [[nodiscard]] auto CreateTextureProcedural(Extent2D extent, bool isSRGB, Func&& callback) -> std::expected<TextureHandle, ErrorCode> {
        std::vector<uint32_t> pixels(static_cast<size_t>(extent.width) * extent.height);
        callback(std::span<uint32_t> {pixels});
        return CreateTexture(std::span {pixels}, extent, isSRGB);
    }

    void UpdateJointMatrices(uint32_t offset, std::span<const JPH::Mat44> matrices);
    // Morph deltas are tightly packed float4s; the count is derived from the span.
    uint32_t AllocateMorphDeltas(std::span<const float> deltas);

    [[nodiscard]] uint32_t GetValidationErrorCount() const noexcept;

    [[nodiscard]] uint32_t GetDeviceLostCount() const noexcept;

    void WriteCheckpoint(std::string_view name) noexcept;

    [[nodiscard]] PipelineStatsCapture CapturePipelineStats() noexcept;

    void OnDeviceLost() noexcept;

    RenderResult BuildMeshBLAS(Mesh& mesh) noexcept;

    [[nodiscard]] std::expected<void, ErrorCode> SetShadowResolution(uint32_t resolution);
    void                                         ProvokeDeviceLost();

    [[nodiscard]] auto BakeProceduralTexture(uint32_t width, uint32_t height, uint32_t variantIdx, float scale, float randomness)
        -> std::expected<TextureHandle, ErrorCode>;
    [[nodiscard]] TextureHandle CreateProceduralTexture(std::string_view name, Extent2D extent, std::span<const uint32_t> pixels, bool isSRGB = true);

    [[nodiscard]] std::expected<void, ErrorCode> CaptureScreenshotPPM(std::string_view outputPath) noexcept;

    [[nodiscard]] std::expected<void, ErrorCode> SetEnvironmentRadiance(const EnvironmentRadianceDesc& desc) noexcept;

    void SetMatrices(const JPH::Mat44& viewProj, const JPH::Mat44& unjitteredViewProj) noexcept;
    // The scene state, and the lights that go with it. Both are the engine's
    // terms (include/Zahlen/Render/FrameData.hpp); the renderer packs them into
    // the shader's structs, so neither call makes a caller name a GPU layout.
    void SetFrameData(const Camera& cam, const FrameData& frame, const JPH::Mat44& shadowProjView, float dt = 0.0166f) noexcept;

    void BindCamera(const Camera& cam, Extent2D viewSize) noexcept;
    void ClearDrawQueues() noexcept;

    void ApplySettings(GraphicsSettings newSettings) noexcept;

    [[nodiscard]] const GraphicsSettings& GetSettings() const noexcept;

    void SetGISettings(const GISettings& settings) noexcept;
    void SetAAState(const AAState& state);
    void SetLights(std::span<const LightDesc> lights) noexcept;
    void Draw(const Material& material, const Mesh& mesh, const DrawParams& params) noexcept;
    void DrawCSG(const Material& eyeMaterial, const Mesh& eyeMesh, const CSGDrawParams& params) noexcept;
    void DrawDecal(const DecalParams& params) noexcept;

  private:
    std::unique_ptr<Impl> _impl;
};

class ZHLN_API PipelineStatsCapture {
  public:
    PipelineStatsCapture() noexcept = default;
    PipelineStatsCapture(PipelineStatsCapture&& other) noexcept;
    auto operator=(PipelineStatsCapture&& other) noexcept -> PipelineStatsCapture&;
    ~PipelineStatsCapture() noexcept;

    PipelineStatsCapture(const PipelineStatsCapture&)                    = delete;
    auto operator=(const PipelineStatsCapture&) -> PipelineStatsCapture& = delete;

    explicit operator bool() const noexcept {
        return _impl != nullptr;
    }

    [[nodiscard]] GpuPipelineCounters Consume() noexcept;

  private:
    friend class RenderContext;
    explicit PipelineStatsCapture(RenderContext::Impl* impl) noexcept: _impl(impl) {
    }

    RenderContext::Impl* _impl = nullptr;
};

}
