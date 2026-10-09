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
#include <Zahlen/Render/SceneData.hpp>
#include <Zahlen/Render/FrameScope.hpp>
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


    // Empty optional = skipped (minimized). A FrameScope = this frame is open.
    [[nodiscard]] FrameOutcome<FrameScope> BeginFrame() noexcept;

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

    [[nodiscard]] auto CreateBuffer(const BufferDesc& desc) -> BufferHandle;

    // Usage is an NTTP so Index can reject non-integer element types at compile time.
    // (A runtime `if (usage == Index) static_assert` would fire for every T.)
    template <BufferUsage Usage, typename T, size_t SpanExtent>
        requires std::is_trivially_copyable_v<T>
    [[nodiscard]] auto CreateBuffer(std::span<T, SpanExtent> elements) -> BufferHandle {
        if constexpr (Usage == BufferUsage::Index) {
            static_assert(ValidIndexType<T>, "Index buffers must use uint16_t or uint32_t elements.");
        }
        return CreateBuffer(BufferDesc {
            .usage  = Usage,
            .data   = std::as_bytes(elements),
            .stride = static_cast<uint32_t>(sizeof(T)),
        });
    }

    template <BufferUsage Usage, typename T = std::byte>
    [[nodiscard]] auto CreateBuffer(size_t elementCount) -> BufferHandle {
        return CreateBuffer(BufferDesc {
            .usage  = Usage,
            .size   = elementCount * sizeof(T),
            .stride = static_cast<uint32_t>(sizeof(T)),
        });
    }

    void DestroyBuffer(BufferHandle handle);
    void UpdateBuffer(BufferHandle handle, std::span<const std::byte> bytes) noexcept;

    template <typename T, size_t SpanExtent>
        requires std::is_trivially_copyable_v<T>
    void UpdateBuffer(BufferHandle handle, std::span<T, SpanExtent> elements) noexcept {
        UpdateBuffer(handle, std::as_bytes(elements));
    }
    [[nodiscard]] std::expected<Material, ErrorCode> CreateMaterial(const MaterialDesc& desc);

    [[nodiscard]] auto AcquireTarget(const PresentationTarget& target) noexcept -> FrameOutcome<FrameTarget>;

    [[nodiscard]] std::optional<FrameTarget> GetAcquiredTarget(const PresentationTarget& target) noexcept;

    void ReleaseTarget(const PresentationTarget& target) noexcept;

    [[nodiscard]] auto CreateRenderTexture(uint32_t width, uint32_t height, bool hdr = false) -> std::expected<RenderTextureHandle, ErrorCode>;
    void               DestroyRenderTexture(RenderTextureHandle handle) noexcept;


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

    RenderResult BuildMeshBLAS(Mesh& mesh) noexcept;

    [[nodiscard]] std::expected<void, ErrorCode> SetShadowResolution(uint32_t resolution);
    void                                         ProvokeDeviceLost();

    [[nodiscard]] auto BakeProceduralTexture(uint32_t width, uint32_t height, uint32_t variantIdx, float scale, float randomness)
        -> std::expected<TextureHandle, ErrorCode>;
    [[nodiscard]] TextureHandle CreateProceduralTexture(std::string_view name, Extent2D extent, std::span<const uint32_t> pixels, bool isSRGB = true);

    [[nodiscard]] std::expected<void, ErrorCode> CaptureScreenshotPPM(std::string_view outputPath) noexcept;

    [[nodiscard]] std::expected<void, ErrorCode> SetEnvironmentRadiance(const EnvironmentRadianceDesc& desc) noexcept;

    void ClearDrawQueues() noexcept;

    void ApplySettings(GraphicsSettings newSettings) noexcept;

    [[nodiscard]] const GraphicsSettings& GetSettings() const noexcept;

    void SetGISettings(const GISettings& settings) noexcept;
    void SetAAState(const AAState& state);
    void Draw(const Material& material, const Mesh& mesh, const DrawParams& params) noexcept;
    void DrawCSG(const Material& eyeMaterial, const Mesh& eyeMesh, const CSGDrawParams& params) noexcept;
    void DrawDecal(const DecalParams& params) noexcept;

  private:
    friend class FrameScope;
    void SubmitParticleEmitter(
        uint64_t emitterId, uint32_t maxParticles, const ParticleEmitterDesc& desc, TextureHandle texture, bool additive
    );
    void SubmitMeshParticleEmitter(
        uint64_t emitterId, uint32_t maxParticles, const MeshParticleEmitterDesc& desc, AssetID mesh, MaterialID mat
    );
    void DrawBillboards(TextureHandle texture, std::span<const BillboardQuad> billboards, bool additive);
    [[nodiscard]] auto DispatchSimulations(float dt) noexcept -> RenderResult;
    void SetFrameData(
        const Camera& cam, const FrameData& frame, const JPH::Mat44& shadowProjView, std::span<const LightDesc> lights, float dt
    ) noexcept;
    void ApplySimulationData(const SceneData& scene) noexcept;
    void ApplyDrawData(const SceneData& scene) noexcept;
    [[nodiscard]] auto RenderScene(const SceneView& view, const GraphicsSettings& settings) noexcept -> FrameOutcome<FrameSkipped>;
    [[nodiscard]] auto RenderUI(const UIView& view, const UIDrawData& uiData) noexcept -> FrameOutcome<FrameSkipped>;
    [[nodiscard]] FrameOutcome<PresentSuboptimal> EndFrame() noexcept;
    void AbortFrame() noexcept;
    void DrawDebugTriangles(std::span<const VertexPosition> positions, std::span<const VertexSurface> surfaces, const Material& material) noexcept;

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
