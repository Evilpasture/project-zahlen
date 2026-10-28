// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PresentationTarget.hpp"
#include <Zahlen/Camera.hpp>
#include <Zahlen/Config.hpp>
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Core/ErrorCode.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Core/Hash.hpp>
#include <Zahlen/Core/Optional.hpp>
#include <Zahlen/Geometry2D.hpp>
#include <Zahlen/GraphicsSettings.hpp>
#include <Zahlen/ParticleEmitterDesc.hpp>
#include <Zahlen/Render/CapturedFrame.hpp>
#include <Zahlen/Render/FrameData.hpp>
#include <Zahlen/Render/FrameResult.hpp>
#include <Zahlen/Render/FrameScope.hpp>
#include <Zahlen/Render/Handles.hpp>
#include <Zahlen/Render/Info.hpp>
#include <Zahlen/Render/PipelineStats.hpp>
#include <Zahlen/Render/PresentTiming.hpp>
#include <Zahlen/Render/RenderContext.hpp>
#include <Zahlen/Render/RenderGraphDump.hpp>
#include <Zahlen/Render/SceneData.hpp>
#include <Zahlen/Render/Types.hpp>
#include <Zahlen/Render/View.hpp>
#include <Zahlen/Vertex.hpp>
#include <Zahlen/gui/UIData.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ZHLN {

namespace {

inline constexpr std::string_view kNullRendererName = "Null";
inline constexpr std::string_view kNullGpuName      = "World-Only Null Device";
inline constexpr std::string_view kNullGraphDump    = "digraph RenderGraph {\n  label=\"ZHLN_BUILD_RENDER=OFF (null renderer)\";\n}\n";

std::atomic<uint64_t> gNextNullRendererToken {1};

} // namespace

struct RenderContext::Impl {
    PresentationTarget&                       primaryTarget;
    uint64_t                                  rendererToken   = gNextNullRendererToken.fetch_add(1, std::memory_order_relaxed);
    Extent2D                                  resolution      {.width = 1280, .height = 720};
    ViewportRect                              viewport        {};
    GraphicsSettings                          settings        {};
    uint32_t                                  frameIndex      = 0;
    uint64_t                                  frameToken      = 1;
    uint64_t                                  nextAcquisition = 1;
    uint64_t                                  nextBufferId    = 1;
    uint64_t                                  nextTextureId   = 4; // Reserve SystemTextures {0..3}.
    uint64_t                                  nextPipelineId  = 1;
    uint32_t                                  morphCursor     = 0;
    bool                                      frameOpen       = false;
    bool                                      destroyed       = false;
    std::unordered_set<uint64_t>              liveBuffers;
    std::unordered_set<uint64_t>              liveTextures {1, 2, 3};
    std::unordered_map<std::string, uint64_t> namedTextures;
    std::unordered_map<AssetID, Mesh>         gpuMeshes;
    std::unordered_map<MaterialID, Material>  gpuMaterials;
    std::unordered_map<uintptr_t, FrameTarget> acquiredTargets;

    Impl(PresentationTarget& target, const RenderConfig& cfg) noexcept:
        primaryTarget(target),
        resolution {.width = cfg.width != 0 ? cfg.width : 1280u, .height = cfg.height != 0 ? cfg.height : 720u} {
        const Extent2D targetExtent = target.GetFramebufferExtent();
        if (targetExtent.width != 0 && targetExtent.height != 0) {
            resolution = targetExtent;
        }
    }

    [[nodiscard]] auto AllocateTexture() -> TextureHandle {
        const uint64_t id = nextTextureId++;
        liveTextures.insert(id);
        return static_cast<TextureHandle>(id);
    }

    [[nodiscard]] auto AllocateNamedTexture(std::string_view name) -> TextureHandle {
        if (!name.empty()) {
            const std::string key {name};
            if (const auto it = namedTextures.find(key); it != namedTextures.end()) {
                return static_cast<TextureHandle>(it->second);
            }
            const TextureHandle handle = AllocateTexture();
            namedTextures.emplace(key, static_cast<uint64_t>(handle));
            return handle;
        }
        return AllocateTexture();
    }
};

RenderContext::RenderContext(PrivateToken, std::unique_ptr<Impl> impl) noexcept: _impl(std::move(impl)) {
}

RenderContext::~RenderContext() {
    Destroy();
}

void RenderContext::Destroy() noexcept {
    if (!_impl || _impl->destroyed) {
        return;
    }
    _impl->destroyed = true;
    _impl->acquiredTargets.clear();
    _impl->gpuMeshes.clear();
    _impl->gpuMaterials.clear();
    _impl->namedTextures.clear();
    _impl->liveTextures.clear();
    _impl->liveBuffers.clear();
}

auto RenderContext::GetPipelineBinaryData() const noexcept -> std::vector<uint8_t> {
    return {};
}

auto RenderContext::Create(
    PresentationTarget& target, const RenderConfig& cfg, ZHLN::Optional<FileSystemWatcher&> /*fileSystemWatcher*/
) noexcept -> std::expected<std::unique_ptr<RenderContext>, ErrorCode> {
    auto impl = std::make_unique<Impl>(target, cfg);
    return std::make_unique<RenderContext>(PrivateToken {}, std::move(impl));
}

auto RenderContext::GetFramebufferSize() const -> std::optional<Extent2D> {
    const Extent2D ext = _impl->primaryTarget.GetFramebufferExtent();
    if (ext.width != 0 && ext.height != 0) {
        return ext;
    }
    if (_impl->primaryTarget.IsHeadless() && _impl->resolution.width != 0 && _impl->resolution.height != 0) {
        return _impl->resolution;
    }
    return std::nullopt;
}

auto RenderContext::BeginFrame() noexcept -> FrameOutcome<FrameScope> {
    if (_impl->destroyed) {
        return std::optional<FrameScope> {};
    }
    const auto fbSize = GetFramebufferSize();
    if (!fbSize.has_value() || fbSize->width == 0 || fbSize->height == 0) {
        return std::optional<FrameScope> {};
    }
    _impl->resolution  = *fbSize;
    _impl->frameOpen   = true;
    _impl->morphCursor = 0;
    return std::optional<FrameScope> {FrameScope {*this}};
}

void RenderContext::SetResolution(const Extent2D& resolution) {
    if (resolution.width != 0 && resolution.height != 0) {
        _impl->resolution = resolution;
    }
}

void RenderContext::SetViewport(const ViewportRect& rect) noexcept {
    _impl->viewport = rect;
}

auto RenderContext::GetViewport() const noexcept -> ViewportRect {
    if (_impl->viewport.width != 0 && _impl->viewport.height != 0) {
        return _impl->viewport;
    }
    return ViewportRect {
        .x      = 0,
        .y      = 0,
        .width  = _impl->resolution.width,
        .height = _impl->resolution.height,
    };
}

auto RenderContext::GetViewportAspect() const noexcept -> float {
    const ViewportRect vp = GetViewport();
    if (vp.height == 0) {
        return 1.0f;
    }
    return static_cast<float>(vp.width) / static_cast<float>(vp.height);
}

auto RenderContext::GetInfo() const noexcept -> RenderInfo {
    return RenderInfo {
        .rendererName         = kNullRendererName,
        .gpuName              = kNullGpuName,
        .deviceType           = PhysicalDeviceType::CPU,
        .presentationMode     = PresentationMode::OffscreenOnly,
        .pacingPolicy         = PacingPolicy::LegacyVBlank,
        .meshShadingSupported = false,
        .meshShadingActive    = false,
        .rayTracingSupported  = false,
    };
}

auto RenderContext::GetFrameIndex() const noexcept -> uint32_t {
    return _impl->frameIndex;
}

auto RenderContext::GetPacedDeltaTime() const noexcept -> std::optional<float> {
    return std::nullopt;
}

auto RenderContext::GetPresentTiming() const noexcept -> PresentTimingMetrics {
    return {};
}

auto RenderContext::GetGPUMesh(AssetID id) const noexcept -> std::optional<Mesh> {
    if (const auto it = _impl->gpuMeshes.find(id); it != _impl->gpuMeshes.end()) {
        return it->second;
    }
    return std::nullopt;
}

auto RenderContext::GetGPUMaterial(MaterialID id) const noexcept -> std::optional<Material> {
    if (const auto it = _impl->gpuMaterials.find(id); it != _impl->gpuMaterials.end()) {
        return it->second;
    }
    return std::nullopt;
}

void RenderContext::RegisterGPUMesh(AssetID id, Mesh mesh) noexcept {
    if (id != InvalidAssetID) {
        _impl->gpuMeshes.insert_or_assign(id, mesh);
    }
}

void RenderContext::UnregisterGPUMesh(AssetID id) noexcept {
    _impl->gpuMeshes.erase(id);
}

void RenderContext::DestroyMesh(const Mesh& mesh) noexcept {
    DestroyBuffer(mesh.posBuffer);
    DestroyBuffer(mesh.tangentFrameBuffer);
    DestroyBuffer(mesh.surfaceBuffer);
    DestroyBuffer(mesh.skinBuffer);
    DestroyBuffer(mesh.indexBuffer);
    DestroyBuffer(mesh.meshletBuffer);
    DestroyBuffer(mesh.meshletVertexBuffer);
    DestroyBuffer(mesh.meshletTriBuffer);
}

void RenderContext::RegisterGPUMaterial(MaterialID id, Material mat) noexcept {
    if (id != InvalidMaterialID) {
        _impl->gpuMaterials.insert_or_assign(id, mat);
    }
}

void RenderContext::UnregisterGPUMaterial(MaterialID id) noexcept {
    _impl->gpuMaterials.erase(id);
}

void RenderContext::ClearGPUCaches() noexcept {
    _impl->gpuMeshes.clear();
    _impl->gpuMaterials.clear();
    _impl->namedTextures.clear();
    _impl->liveTextures = {1, 2, 3};
}

auto RenderContext::CreateBuffer(const BufferDesc& desc) -> BufferHandle {
    const size_t byteSize = !desc.data.empty() ? desc.data.size_bytes() : desc.size;
    if (byteSize == 0) {
        return BufferHandle::Invalid;
    }
    const uint64_t id = _impl->nextBufferId++;
    _impl->liveBuffers.insert(id);
    return static_cast<BufferHandle>(id);
}

auto RenderContext::CreateMeshletBuffer(std::span<const MeshletDesc> meshlets) -> BufferHandle {
    if (meshlets.empty()) {
        return BufferHandle::Invalid;
    }
    const uint64_t id = _impl->nextBufferId++;
    _impl->liveBuffers.insert(id);
    return static_cast<BufferHandle>(id);
}

auto RenderContext::UploadMeshlets(const MeshletBuildResult& built) -> MeshletStreams {
    if (built.Empty() || built.vertices.empty() || built.triangles.empty()) {
        return {};
    }

    const BufferHandle meshlets  = CreateMeshletBuffer(built.meshlets);
    const BufferHandle vertices  = CreateBuffer<BufferUsage::Storage>(std::span {built.vertices});
    const BufferHandle triangles = CreateBuffer<BufferUsage::Storage>(std::span {built.triangles});
    if (meshlets == BufferHandle::Invalid || vertices == BufferHandle::Invalid || triangles == BufferHandle::Invalid) {
        DestroyBuffer(meshlets);
        DestroyBuffer(vertices);
        DestroyBuffer(triangles);
        return {};
    }

    return MeshletStreams {
        .meshlets  = meshlets,
        .vertices  = vertices,
        .triangles = triangles,
        .count     = static_cast<uint32_t>(built.meshlets.size()),
    };
}

void RenderContext::DestroyBuffer(BufferHandle handle) {
    if (handle != BufferHandle::Invalid) {
        _impl->liveBuffers.erase(static_cast<uint64_t>(handle));
    }
}

auto RenderContext::UpdateBuffer(BufferHandle handle, std::span<const std::byte> /*bytes*/) noexcept -> std::expected<void, ErrorCode> {
    if (handle == BufferHandle::Invalid || !_impl->liveBuffers.contains(static_cast<uint64_t>(handle))) {
        return std::unexpected(RenderFeatureError::UnresolvedMeshHandle);
    }
    return {};
}

auto RenderContext::CreateMaterial(const MaterialDesc& desc) -> std::expected<Material, ErrorCode> {
    const auto pipeline = static_cast<PipelineHandle>(_impl->nextPipelineId++);
    const auto prePass  = (desc.alphaBlend || desc.transmissionFactor > 0.0f)
        ? static_cast<PipelineHandle>(_impl->nextPipelineId++)
        : PipelineHandle::Invalid;

    return Material {
        .pipeline                       = pipeline,
        .prePassPipeline                = prePass,
        .albedoMap                      = desc.albedoMap,
        .normalMap                      = desc.normalMap,
        .pbrMap                         = desc.pbrMap,
        .emissiveMap                    = desc.emissiveMap,
        .bakedIrradianceMap             = desc.bakedIrradianceMap,
        .bakedIrradianceScale           = std::max(desc.bakedIrradianceScale, 0.0f),
        .baseColorFactor                = desc.baseColor,
        .emissiveFactor                 = desc.emissive,
        .metallicFactor                 = desc.metallic,
        .roughnessFactor                = desc.roughness,
        .alphaCutoff                    = desc.alphaCutoff,
        .alphaMode                      = desc.alphaMode,
        .doubleSided                    = desc.doubleSided,
        .unlit                          = desc.unlit,
        .transmissionFactor             = desc.transmissionFactor,
        .transmissionMap                = desc.transmissionMap,
        .iridescenceFactor              = desc.iridescenceFactor,
        .filmThicknessNm                = desc.filmThicknessNm,
        .filmThicknessMinNm             = desc.filmThicknessMinNm,
        .volumeThicknessM               = desc.volumeThicknessM,
        .ior                            = desc.ior,
        .normalScale                    = desc.normalScale,
        .filmThicknessMap               = desc.filmThicknessMap,
        .iridescenceMap                 = desc.iridescenceMap,
        .volumeThicknessMap             = desc.volumeThicknessMap,
        .clearcoatFactor                = desc.clearcoatFactor,
        .clearcoatRoughnessFactor       = desc.clearcoatRoughnessFactor,
        .clearcoatNormalScale           = desc.clearcoatNormalScale,
        .clearcoatMap                   = desc.clearcoatMap,
        .clearcoatRoughnessMap          = desc.clearcoatRoughnessMap,
        .clearcoatNormalMap             = desc.clearcoatNormalMap,
        .anisotropyStrength             = desc.anisotropyStrength,
        .anisotropyRotation             = desc.anisotropyRotation,
        .anisotropyMap                  = desc.anisotropyMap,
        .sheenColorFactor               = desc.sheenColorFactor,
        .sheenRoughnessFactor           = desc.sheenRoughnessFactor,
        .sheenColorMap                  = desc.sheenColorMap,
        .sheenRoughnessMap              = desc.sheenRoughnessMap,
        .occlusionMap                   = desc.occlusionMap,
        .occlusionStrength              = desc.occlusionStrength,
        .specularEnabled                = desc.specularEnabled,
        .environmentReflectionEnabled   = desc.environmentReflectionEnabled,
        .environmentReflectionAlphaMask = desc.environmentReflectionAlphaMask,
        .environmentReflectionFactor    = desc.environmentReflectionFactor,
        .specularFactor                 = desc.specularFactor,
        .specularColorFactor            = desc.specularColorFactor,
        .specularMap                    = desc.specularMap,
        .specularTexMode                = desc.specularTexMode,
        .textureSamplers                = desc.textureSamplers,
        .textureTransforms              = desc.textureTransforms,
    };
}

auto RenderContext::AcquireTarget(const PresentationTarget& target) noexcept -> FrameOutcome<FrameTarget> {
    if (_impl->destroyed || target.WasClosed()) {
        return std::optional<FrameTarget> {};
    }
    const Extent2D ext = target.GetFramebufferExtent();
    if (!target.IsHeadless() && (ext.width == 0 || ext.height == 0)) {
        return std::optional<FrameTarget> {};
    }
    const auto key = reinterpret_cast<uintptr_t>(&target);
    if (const auto it = _impl->acquiredTargets.find(key); it != _impl->acquiredTargets.end()) {
        return std::optional<FrameTarget> {it->second};
    }
    const FrameTarget acquired {_impl->rendererToken, _impl->frameToken, static_cast<uint64_t>(key), _impl->nextAcquisition++};
    _impl->acquiredTargets.emplace(key, acquired);
    return std::optional<FrameTarget> {acquired};
}

auto RenderContext::GetAcquiredTarget(const PresentationTarget& target) noexcept -> std::optional<FrameTarget> {
    const auto key = reinterpret_cast<uintptr_t>(&target);
    if (const auto it = _impl->acquiredTargets.find(key); it != _impl->acquiredTargets.end()) {
        return it->second;
    }
    return std::nullopt;
}

void RenderContext::ReleaseTarget(const PresentationTarget& target) noexcept {
    _impl->acquiredTargets.erase(reinterpret_cast<uintptr_t>(&target));
}

auto RenderContext::CreateRenderTexture(uint32_t width, uint32_t height, bool /*hdr*/) -> std::expected<TextureHandle, ErrorCode> {
    if (width == 0 || height == 0) {
        return TextureHandle::Invalid;
    }
    return _impl->AllocateTexture();
}

void RenderContext::DestroyRenderTexture(TextureHandle handle) noexcept {
    if (static_cast<uint64_t>(handle) > 3) {
        _impl->liveTextures.erase(static_cast<uint64_t>(handle));
    }
}

auto RenderContext::CreateTexture(std::span<const std::byte> rgba, Extent2D extent, bool /*isSRGB*/) -> std::expected<TextureHandle, ErrorCode> {
    if (rgba.empty() || extent.width == 0 || extent.height == 0) {
        return TextureHandle::Invalid;
    }
    return _impl->AllocateTexture();
}

auto RenderContext::CreateTexture(
    std::string_view name, std::span<const std::byte> rgba, Extent2D extent, bool /*isSRGB*/, bool /*generateMips*/
) -> std::expected<TextureHandle, ErrorCode> {
    if (rgba.empty() || extent.width == 0 || extent.height == 0) {
        return TextureHandle::Invalid;
    }
    return _impl->AllocateNamedTexture(name);
}

auto RenderContext::CreateTextureCube(std::array<std::span<const std::byte>, 6> faces, uint32_t faceSize) -> std::expected<TextureHandle, ErrorCode> {
    if (faceSize == 0 || faces[0].empty()) {
        return TextureHandle::Invalid;
    }
    return _impl->AllocateTexture();
}

auto RenderContext::CreateTextureHDR(
    std::string_view name, std::span<const std::byte> rgba16f, Extent2D extent, bool /*generateMips*/
) -> std::expected<TextureHandle, ErrorCode> {
    if (rgba16f.empty() || extent.width == 0 || extent.height == 0) {
        return TextureHandle::Invalid;
    }
    return _impl->AllocateNamedTexture(name);
}

void RenderContext::UnloadTexture(TextureHandle handle) {
    if (static_cast<uint64_t>(handle) > 3) {
        _impl->liveTextures.erase(static_cast<uint64_t>(handle));
    }
}

void RenderContext::UpdateJointMatrices(uint32_t /*offset*/, std::span<const JPH::Mat44> /*matrices*/) {
}

auto RenderContext::AllocateMorphDeltas(std::span<const float> deltas) -> uint32_t {
    const uint32_t base = _impl->morphCursor;
    _impl->morphCursor += static_cast<uint32_t>(deltas.size() / 4);
    return base;
}

void RenderContext::WriteCheckpoint(std::string_view /*name*/) noexcept {
}

auto RenderContext::CapturePipelineStats() noexcept -> PipelineStatsCapture {
    return PipelineStatsCapture {_impl.get()};
}

auto RenderContext::BuildMeshBLAS(Mesh& mesh) noexcept -> RenderResult {
    if (mesh.posBuffer == BufferHandle::Invalid || !_impl->liveBuffers.contains(static_cast<uint64_t>(mesh.posBuffer))) {
        return std::unexpected(RenderFeatureError::UnresolvedMeshHandle);
    }
    return std::unexpected(RenderFeatureError::FeatureNotSupported);
}

auto RenderContext::SetShadowResolution(uint32_t resolution) -> std::expected<void, ErrorCode> {
    _impl->settings.shadows.resolution = resolution;
    return {};
}

void RenderContext::ProvokeDeviceLost() {
    ZHLN::LogError("[DeviceLost] Simulated null-renderer device lost");
}

auto RenderContext::BakeProceduralTexture(
    uint32_t width, uint32_t height, uint32_t /*variantIdx*/, float /*scale*/, float /*randomness*/
) -> std::expected<TextureHandle, ErrorCode> {
    if (width == 0 || height == 0) {
        return TextureHandle::Invalid;
    }
    return _impl->AllocateTexture();
}

auto RenderContext::CreateProceduralTexture(
    std::string_view name, Extent2D extent, std::span<const uint32_t> pixels, bool /*isSRGB*/, bool /*generateMips*/
) -> TextureHandle {
    if (pixels.empty() || extent.width == 0 || extent.height == 0) {
        return TextureHandle::Invalid;
    }
    return _impl->AllocateNamedTexture(name);
}

auto RenderContext::CaptureFrame() noexcept -> std::expected<CapturedFrame, ErrorCode> {
    const Extent2D ext        = _impl->resolution;
    const size_t   pixelCount = static_cast<size_t>(ext.width) * static_cast<size_t>(ext.height);
    return CapturedFrame {
        .extent  = ext,
        .format  = PixelFormat::Rgba8,
        .pixels  = std::vector<uint8_t>(pixelCount * 4u, 0u),
        .metrics = FrameCaptureMetrics {
            .meanLuma          = 0.0,
            .litPixels         = 0,
            .transparentPixels = pixelCount,
        },
    };
}

auto RenderContext::SetEnvironmentRadiance(const EnvironmentRadianceDesc& /*desc*/) noexcept -> std::expected<void, ErrorCode> {
    return {};
}

void RenderContext::ClearDrawQueues() noexcept {
}

void RenderContext::ApplySettings(GraphicsSettings newSettings) noexcept {
    _impl->settings = newSettings;
}

auto RenderContext::GetSettings() const noexcept -> const GraphicsSettings& {
    return _impl->settings;
}

void RenderContext::SetGISettings(const GISettings& settings) noexcept {
    _impl->settings.post = settings;
}

void RenderContext::SetAAState(const AAState& state) {
    _impl->settings.antiAliasing = state;
}

void RenderContext::Draw(const Material& /*material*/, const Mesh& /*mesh*/, const DrawParams& /*params*/) noexcept {
}

void RenderContext::DrawCSG(const Material& /*eyeMaterial*/, const Mesh& /*eyeMesh*/, const CSGDrawParams& /*params*/) noexcept {
}

void RenderContext::DrawDecal(const DecalParams& /*params*/) noexcept {
}

void RenderContext::SubmitParticleEmitter(
    uint64_t /*emitterId*/, uint32_t /*maxParticles*/, const ParticleEmitterDesc& /*desc*/, TextureHandle /*texture*/, bool /*additive*/
) {
}

void RenderContext::SubmitMeshParticleEmitter(
    uint64_t /*emitterId*/, uint32_t /*maxParticles*/, const MeshParticleEmitterDesc& /*desc*/, AssetID /*mesh*/, MaterialID /*mat*/
) {
}

void RenderContext::DrawBillboards(TextureHandle /*texture*/, std::span<const BillboardQuad> /*billboards*/, bool /*additive*/) {
}

auto RenderContext::DispatchSimulations(float /*dt*/) noexcept -> RenderResult {
    return {};
}

void RenderContext::SetFrameData(
    const Camera& /*cam*/, const FrameData& /*frame*/, const JPH::Mat44& /*shadowProjView*/, std::span<const LightDesc> /*lights*/, float /*dt*/
) noexcept {
}

void RenderContext::ApplySimulationData(const SceneData& /*scene*/) noexcept {
}

void RenderContext::ApplyDrawData(const SceneData& /*scene*/) noexcept {
}

auto RenderContext::RenderScene(const SceneView& /*view*/, const GraphicsSettings& /*settings*/) noexcept -> FrameOutcome<FrameSkipped> {
    return std::optional<FrameSkipped> {};
}

auto RenderContext::RenderUI(const UIView& /*view*/, UIDrawData /*uiData*/) noexcept -> FrameOutcome<FrameSkipped> {
    return std::optional<FrameSkipped> {};
}

auto RenderContext::EndFrame() noexcept -> FrameOutcome<PresentSuboptimal> {
    _impl->frameOpen = false;
    _impl->acquiredTargets.clear();
    ++_impl->frameIndex;
    ++_impl->frameToken;
    return std::optional<PresentSuboptimal> {};
}

void RenderContext::AbortFrame() noexcept {
    _impl->frameOpen = false;
    _impl->acquiredTargets.clear();
    ++_impl->frameToken;
}

void RenderContext::DrawDebugTriangles(
    std::span<const VertexPosition> /*positions*/, std::span<const VertexSurface> /*surfaces*/, const Material& /*material*/
) noexcept {
}

FrameScope::FrameScope(RenderContext& rc) noexcept: _rc(&rc) {
}

FrameScope::FrameScope(FrameScope&& other) noexcept: _rc(other._rc), _ended(other._ended) {
    other._rc    = nullptr;
    other._ended = true;
}

auto FrameScope::operator=(FrameScope&& other) noexcept -> FrameScope& {
    if (this == &other) {
        return *this;
    }
    if (_rc != nullptr && !_ended) {
        _rc->AbortFrame();
    }
    _rc          = other._rc;
    _ended       = other._ended;
    other._rc    = nullptr;
    other._ended = true;
    return *this;
}

FrameScope::~FrameScope() {
    Cancel();
}

void FrameScope::Cancel() noexcept {
    if (_rc != nullptr && !_ended) {
        _rc->AbortFrame();
        _ended = true;
    }
}

auto FrameScope::DispatchSimulations(const SceneData& scene, float dt) noexcept -> RenderResult {
    if (_rc == nullptr) {
        return {};
    }
    _rc->ApplySimulationData(scene);
    return _rc->DispatchSimulations(dt);
}

auto FrameScope::RenderScene(const SceneRenderPass& pass, const SceneData& scene) noexcept -> FrameOutcome<FrameSkipped> {
    if (_rc == nullptr) {
        return std::optional<FrameSkipped> {FrameSkipped {}};
    }
    _rc->SetFrameData(pass.camera, pass.frame, pass.shadowProjView, std::span<const LightDesc> {scene.lights.data(), scene.lights.size()}, scene.dt);
    _rc->ApplyDrawData(scene);
    return _rc->RenderScene(pass.view, pass.settings);
}

auto FrameScope::RenderUI(const UIView& view, UIDrawData uiData) noexcept -> FrameOutcome<FrameSkipped> {
    if (_rc == nullptr) {
        return std::optional<FrameSkipped> {FrameSkipped {}};
    }
    return _rc->RenderUI(view, uiData);
}

auto FrameScope::End() && noexcept -> FrameOutcome<PresentSuboptimal> {
    if (_rc == nullptr || _ended) {
        return std::optional<PresentSuboptimal> {};
    }
    _ended = true;
    return _rc->EndFrame();
}

PipelineStatsCapture::PipelineStatsCapture(PipelineStatsCapture&& other) noexcept: _impl(std::exchange(other._impl, nullptr)) {
}

auto PipelineStatsCapture::operator=(PipelineStatsCapture&& other) noexcept -> PipelineStatsCapture& {
    if (this != &other) {
        _impl = std::exchange(other._impl, nullptr);
    }
    return *this;
}

PipelineStatsCapture::~PipelineStatsCapture() noexcept = default;

auto PipelineStatsCapture::Consume() noexcept -> GpuPipelineCounters {
    _impl = nullptr;
    return {};
}

auto GetRenderGraphDump(AAMode /*currentMode*/) noexcept -> std::string_view {
    return kNullGraphDump;
}

} // namespace ZHLN
