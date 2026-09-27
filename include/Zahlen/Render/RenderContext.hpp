// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

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


struct Camera;
namespace FS {
class FileSystemWatcher;
}
using FileSystemWatcher = FS::FileSystemWatcher;
class PipelineStatsCapture;
class PresentationTarget;

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

    [[nodiscard]] static std::expected<std::unique_ptr<RenderContext>, ErrorCode>
        Create(PresentationTarget& target, const RenderConfig& cfg, FileSystemWatcher* fileSystemWatcher = nullptr) noexcept;

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
    void                                  RegisterGPUMesh(AssetID id, Mesh mesh) noexcept;
    void                                  RegisterGPUMaterial(MaterialID id, Material mat) noexcept;
    void                                  ClearGPUCaches() noexcept;

    BufferHandle GetOrCreateSkinnedScratchBuffer(uint64_t entityKey, uint32_t vertexCount);
    BufferHandle CreateStorageBuffer(size_t size);
    BufferHandle GetOrCreateParticleBuffer(Entity owner, uint32_t subresourceKey, uint32_t maxParticles);
    void         SubmitParticleEmitter(BufferHandle gpuBuffer, uint32_t maxParticles, const ParticleEmitterParams& params);
    void SubmitMeshParticleEmitter(BufferHandle gpuBuffer, uint32_t maxParticles, const MeshParticleEmitterParams& params, AssetID mesh, MaterialID mat);

    auto CreateStorageBuffer(const void* data, size_t size, uint32_t stride = sizeof(uint32_t)) -> BufferHandle;

    auto CreateVertexBuffer(const void* data, size_t size, uint32_t stride = sizeof(VertexPosition)) -> BufferHandle;
    auto CreateIndexBuffer(const void* data, size_t size) -> BufferHandle;
    void DestroyBuffer(BufferHandle handle);
    void UpdateBuffer(BufferHandle handle, const void* data, size_t size) noexcept;
    auto CreateConstantBuffer(size_t size) -> BufferHandle;
    [[nodiscard]] std::expected<Material, ErrorCode> CreateBasicMaterial(bool doubleSided = false, bool alphaBlend = false, bool additiveBlend = false, bool depthWrite = false);
    [[nodiscard]] std::expected<Material, ErrorCode> CreateMaterial(const MaterialDesc& desc);

    auto CreateSkinnedScratchBuffer(uint32_t vertexCount) -> BufferHandle;

    void                       UploadDebugVertices(const void* posData, size_t posSize, const void* attrData, size_t attrSize, uint32_t vertexCount) noexcept;
    [[nodiscard]] BufferHandle GetDebugMeshBuffer() const noexcept;


    [[nodiscard]] auto AcquireTarget(const PresentationTarget& target) noexcept -> FrameOutcome<RenderAttachment>;

    [[nodiscard]] std::optional<RenderAttachment> GetTargetAttachment(const PresentationTarget& target) noexcept;

    void ReleaseTarget(const PresentationTarget& target) noexcept;

    [[nodiscard]] auto CreateRenderTexture(uint32_t width, uint32_t height, bool hdr = false) -> std::expected<TextureHandle, ErrorCode>;
    void               DestroyRenderTexture(TextureHandle handle) noexcept;


    [[nodiscard]] auto RenderScene(const SceneView& view, const GraphicsSettings& settings) noexcept -> FrameOutcome<FrameSkipped>;
    [[nodiscard]] auto RenderUI(const UIView& view, const UIDrawData& uiData) noexcept -> FrameOutcome<FrameSkipped>;
    [[nodiscard]] auto DispatchSimulations(float dt) noexcept -> RenderResult;

    void DrawLine(JPH::Vec3Arg start, JPH::Vec3Arg end, JPH::Vec4Arg colorStart, JPH::Vec4Arg colorEnd) noexcept;
    void DrawLine(JPH::Vec3Arg start, JPH::Vec3Arg end, JPH::Vec4Arg color) noexcept {
        DrawLine(start, end, color, color);
    }

    [[nodiscard]] uint32_t GetBindlessIndex(TextureHandle handle) const noexcept;

    [[nodiscard]] auto          CreateTexture(const void* data, uint32_t width, uint32_t height, bool isSRGB = true) -> std::expected<uint32_t, ErrorCode>;
    [[nodiscard]] auto          CreateTextureCube(const void* const* faceData, uint32_t width, uint32_t height) -> std::expected<uint32_t, ErrorCode>;
    [[nodiscard]] TextureHandle RegisterTexture(std::string_view name, uint32_t bindlessIndex, bool isSRGB = true);
    void UnloadTexture(TextureHandle handle);

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

    void TrackEntityBuffer(Entity owner, BufferHandle buffer);
    void ReleaseEntityBuffers(Entity owner);
    void               ReconcileEntityBuffers(EntityAliveQuery alive);
    [[nodiscard]] auto GetTrackedEntityBufferCount() const noexcept -> size_t;

    [[nodiscard]] static uint32_t ValidationErrorCount() noexcept;

    [[nodiscard]] static uint32_t DeviceLostCount() noexcept;

    static void UseDiagnostics(std::atomic<uint32_t>* validationErrors, std::atomic<uint32_t>* deviceLost) noexcept;

    void WriteCheckpoint(std::string_view name) noexcept;

    [[nodiscard]] PipelineStatsCapture CapturePipelineStats() noexcept;

    void OnDeviceLost() noexcept;

    RenderResult BuildMeshBLAS(Mesh& mesh) noexcept;

    [[nodiscard]] std::expected<void, ErrorCode> SetShadowResolution(uint32_t resolution);
    void                                         ProvokeDeviceLost();

    auto BakeProceduralTexture(uint32_t width, uint32_t height, uint32_t variantIdx, float scale, float randomness) -> std::expected<uint32_t, ErrorCode>;
    TextureHandle CreateProceduralTexture(std::string_view name, uint32_t width, uint32_t height, bool isSRGB, const uint32_t* pixels);

    [[nodiscard]] std::expected<void, ErrorCode> CaptureScreenshotPPM(std::string_view outputPath) noexcept;

    [[nodiscard]] std::expected<void, ErrorCode> SetEnvironmentRadiance(const EnvironmentRadianceDesc& desc) noexcept;

    void SetMatrices(const JPH::Mat44& viewProj, const JPH::Mat44& unjitteredViewProj) noexcept;
    void SetFrameData(const Camera& cam, const FrameUniforms& uniforms, const JPH::Mat44& shadowProjView, float dt = 0.0166f) noexcept;

    void BindCamera(const Camera& cam, Extent2D viewSize) noexcept;
    void ClearDrawQueues() noexcept;

    void ApplySettings(GraphicsSettings newSettings) noexcept;

    [[nodiscard]] const GraphicsSettings& GetSettings() const noexcept;

    void SetGISettings(const GISettings& settings) noexcept;
    void SetAAState(const AAState& state);
    void SetLights(const Light* lights, uint32_t count) noexcept;
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
