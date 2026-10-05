// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "GpuPack.hpp"
#include "RenderInternal.hpp"
#include "Resources.hpp"
#include <ShaderBindings.hpp>
#include "Zahlen/Core/AssetID.hpp"
#include "Zahlen/Geometry2D.hpp"
#include "Zahlen/GraphicsSettings.hpp"
#include "Zahlen/Render/Handles.hpp"
#include "Zahlen/Render/PresentTiming.hpp"
#include "Zahlen/Render/Types.hpp"
#include "Zahlen/Vertex.hpp"
#include <Zahlen/Core/Reflection/Annotations.hpp>
#include <Zahlen/Core/Reflection/Class.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Core/Ranges.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>


namespace ZHLN {

enum class BlueNoiseError : uint8_t {
    UnexpectedLayout ZHLN_ANNOTATION(ZHLN::Description<"Blue noise blob is not a whole square of 8-bit RGBA texels"> {}) = 1,
};

enum class TextureDataError : uint8_t {
    InvalidPixels ZHLN_ANNOTATION(ZHLN::Description<"RGBA pixels do not match the texture's nonzero extent"> {}) = 1,
};

}

namespace ZHLN {


auto RenderContext::GetGPUMesh(AssetID id) const noexcept -> std::optional<Mesh> {
    if (const auto found = _impl->geometry.FindMesh(id)) {
        return *found;
    }
    return std::nullopt;
}

auto RenderContext::GetGPUMaterial(MaterialID id) const noexcept -> std::optional<Material> {
    if (const auto found = _impl->geometry.FindMaterial(id)) {
        return *found;
    }
    return std::nullopt;
}

void RenderContext::RegisterGPUMesh(AssetID id, Mesh mesh) noexcept { _impl->geometry.RegisterMesh(id, mesh); }

void RenderContext::UnregisterGPUMesh(AssetID id) noexcept { _impl->geometry.UnregisterMesh(id); }

void RenderContext::DestroyMesh(const Mesh& mesh) noexcept {
    // Mesh is a view; callers must unregister all aliases before releasing
    // shared buffers. DestroyBuffer ignores invalid/already-retired handles.
    const std::array buffers = {mesh.posBuffer,   mesh.tangentFrameBuffer, mesh.surfaceBuffer,       mesh.skinBuffer,
                                mesh.indexBuffer, mesh.meshletBuffer,      mesh.meshletVertexBuffer, mesh.meshletTriBuffer};
    for (const BufferHandle handle: buffers) {
        DestroyBuffer(handle);
    }
}

void RenderContext::RegisterGPUMaterial(MaterialID id, Material mat) noexcept { _impl->geometry.RegisterMaterial(id, mat); }

void RenderContext::UnregisterGPUMaterial(MaterialID id) noexcept {
    if (const auto mat = _impl->geometry.FindMaterial(id)) {
        if (mat->pipeline != PipelineHandle::Invalid) {
            _impl->pipelines.Destroy(mat->pipeline);
        }
        if (mat->prePassPipeline != PipelineHandle::Invalid) {
            _impl->pipelines.Destroy(mat->prePassPipeline);
        }
        _impl->geometry.UnregisterMaterial(id);
    }
}

auto RenderContext::CreateParticleBuffer(uint32_t maxParticles) -> BufferHandle {
    return CreateStorageBuffer(static_cast<size_t>(maxParticles) * sizeof(Particle));
}

auto RenderContext::CreateMeshParticleBuffer(uint32_t maxParticles) -> BufferHandle {
    return CreateStorageBuffer(static_cast<size_t>(maxParticles) * sizeof(Particle3D));
}

auto RenderContext::CreateStorageBuffer(size_t size) -> BufferHandle {
    return _impl->geometry.CreateBuffer({.allocationSize = size}, Vk::BufferUsage::Storage | Vk::BufferUsage::Vertex).value_or(BufferHandle::Invalid);
}

void RenderContext::SubmitParticleEmitter(
    BufferHandle gpuBuffer, uint32_t maxParticles, const ParticleEmitterDesc& desc, TextureHandle texture, bool additive
) {
    // The manager already answers a default for an unregistered handle
    // (kFallbackWhiteTextureIndex), so there is nothing to test here: a host that
    // names no texture gets the white slot the sampler expects.
    const uint32_t textureIndex = _impl->textureManager.GetBindlessIndex(texture);

    _impl->queues.ParticleEmitters().push_back(
        {.gpuBuffer    = gpuBuffer,
         .maxParticles = maxParticles,
         .params       = GpuPack::PackParticleEmitter(desc, textureIndex, additive ? 1u : 0u)}
    );
}

void RenderContext::SubmitMeshParticleEmitter(
    BufferHandle gpuBuffer, uint32_t maxParticles, const MeshParticleEmitterDesc& desc, AssetID mesh, MaterialID mat
) {
    _impl->queues.MeshParticleEmitters().push_back(
        {.gpuBuffer = gpuBuffer, .maxParticles = maxParticles, .params = GpuPack::PackMeshParticleEmitter(desc), .meshAsset = mesh, .materialAsset = mat}
    );
}

void RenderContext::DrawBillboards(TextureHandle texture, std::span<const BillboardQuad> billboards, bool additive) {
    if (billboards.empty()) {
        return;
    }

    // The frame's batch index: it keeps two submissions of one key in one frame from
    // sharing a slot, and so a buffer. It restarts at the frame boundary (BeginFrame,
    // next to frameSerial) -- not from the emitter queue's state: that queue is shared
    // with the particle submits, so its emptiness says nothing about frames, and reading
    // it as a frame signal leaves the serial counting up forever in any scene that has
    // an emitter in it (a new slot, and a new buffer, every frame).
    const uint32_t serial = _impl->billboardSerial++;

    const uint32_t textureIndex = _impl->textureManager.GetBindlessIndex(texture); // Invalid -> the white slot
    const uint32_t blendMode    = additive ? 1u : 0u;

    // One draw per facing: the pass takes the alignment as a push constant, so a call
    // that mixes camera-facing and ground-facing quads becomes two draws of one texture.
    for (uint32_t alignment = 0; alignment < 3; ++alignment) {
        _impl->billboardStaging.clear();
        for (const BillboardQuad& quad: billboards) {
            if (static_cast<uint32_t>(quad.facing) != alignment) {
                continue;
            }
            Particle packed {};
            JPH::Vec4(quad.position, 1.0f).StoreFloat4(&packed.position);
            JPH::Vec4(quad.facing == ParticleAlignment::VelocityStretched ? quad.velocity : JPH::Vec3::sZero(), 0.0f)
                .StoreFloat4(&packed.velocity);
            quad.color.StoreFloat4(&packed.color);
            // The render vertex shader reads a quad's size and rotation out of params.z
            // and params.w. This is the one place that fact is written down for billboards.
            JPH::Vec4(0.0f, 0.0f, quad.size, quad.rotation).StoreFloat4(&packed.params);
            _impl->billboardStaging.push_back(packed);
        }

        const uint32_t count = static_cast<uint32_t>(_impl->billboardStaging.size());
        if (count == 0) {
            continue;
        }

        Impl::BillboardSlot* group = nullptr;
        for (auto& candidate: _impl->billboardSlots) {
            if (candidate.textureIndex == textureIndex && candidate.blendMode == blendMode && candidate.alignment == alignment &&
                candidate.serial == serial) {
                group = &candidate;
                break;
            }
        }
        if (group == nullptr) {
            _impl->billboardSlots.push_back(
                {.textureIndex = textureIndex, .blendMode = blendMode, .alignment = alignment, .serial = serial}
            );
            group = &_impl->billboardSlots.back();
        }

        if (group->capacity < count) {
            if (group->buffer != BufferHandle::Invalid) {
                DestroyBuffer(group->buffer);
            }
            group->buffer   = CreateParticleBuffer(count);
            group->capacity = count;
        }
        if (group->buffer == BufferHandle::Invalid) {
            continue;
        }

        UpdateBuffer(group->buffer, std::as_bytes(std::span {_impl->billboardStaging}));
        _impl->queues.ParticleEmitters().push_back(
            {.gpuBuffer    = group->buffer,
             .maxParticles = count,
             // The pass takes the alignment as a push constant (uint); the lane's own
             // type is the enum, so the crossings between the two are explicit.
             .params       = {.textureIndex = textureIndex, .alignment = static_cast<ParticleAlignment>(alignment), .blendMode = blendMode},
             .simulate     = false}
        );
    }
}

auto RenderContext::GetBindlessIndex(TextureHandle handle) const noexcept -> uint32_t {
    return _impl->textureManager.GetBindlessIndex(handle);
}

void RenderContext::ClearGPUCaches() noexcept {
    if (_impl->ctx.Device() != VK_NULL_HANDLE) {
        auto res = Vk::WaitIdle(_impl->ctx.Device());
        if (!res) {
            ZHLN::Log("GPU cache clear aborted due to reason: {}", res.error());
            return;
        }
    }

    _impl->geometry.ClearMeshes();

    _impl->pipelines.RetireAll(); // Also retires materials that were never registered by asset ID.
    _impl->geometry.ClearMaterials();

    for (const auto& entry: _impl->renderTextures) {
        _impl->textureManager.ReleaseSlot(entry.second.bindlessIndex);
    }
    _impl->renderTextures.clear();
    _impl->textureManager.Clear();
    _impl->textureManager.RetireAll(); // WaitIdle above covers all pending texture slots.

    _impl->deletionQueue.Drain();
}

void RenderContext::UseDiagnostics(std::atomic<uint32_t>& validationErrors, std::atomic<uint32_t>& deviceLost) noexcept {
    Vk::Instance::UseDiagnostics({&validationErrors, &deviceLost});
}

uint32_t RenderContext::ValidationErrorCount() noexcept {
    return Vk::Instance::ValidationErrorCount();
}

uint32_t RenderContext::DeviceLostCount() noexcept {
    return Vk::Instance::DeviceLostCount();
}

void RenderContext::WriteCheckpoint(std::string_view name) noexcept {
    if (const VkCommandBuffer cmd = _impl->FrameCommand(); cmd != VK_NULL_HANDLE) {
        _impl->gpuDiagnostics.WriteCheckpoint(cmd, name);
    }
}

PipelineStatsCapture RenderContext::CapturePipelineStats() noexcept {
    if (!_impl->gpuProfiler.PipelineStatsAvailable()) {
        return {};
    }
    _impl->pendingPipelineCounters = {};
    _impl->gpuProfiler.SetPipelineStatsEnabled(true);
    return PipelineStatsCapture {_impl.get()};
}

PipelineStatsCapture::PipelineStatsCapture(PipelineStatsCapture&& other) noexcept: _impl(std::exchange(other._impl, nullptr)) {
}

auto PipelineStatsCapture::operator=(PipelineStatsCapture&& other) noexcept -> PipelineStatsCapture& {
    if (this != &other) {
        if (_impl != nullptr) {
            _impl->gpuProfiler.SetPipelineStatsEnabled(false);
        }
        _impl = std::exchange(other._impl, nullptr);
    }
    return *this;
}

PipelineStatsCapture::~PipelineStatsCapture() noexcept {
    if (_impl != nullptr) {
        _impl->gpuProfiler.SetPipelineStatsEnabled(false);
    }
}

GpuPipelineCounters PipelineStatsCapture::Consume() noexcept {
    if (_impl == nullptr) {
        return {};
    }
    return std::exchange(_impl->pendingPipelineCounters, GpuPipelineCounters {});
}

void RenderContext::OnDeviceLost() noexcept {
    _impl->gpuDiagnostics.OnDeviceLost();
}


auto RenderContext::GetInfo() const noexcept -> RenderInfo {
    const auto& props = _impl->ctx.PhysicalInfo().properties.properties;
    PhysicalDeviceType deviceType = PhysicalDeviceType::Other;
    switch (props.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
            deviceType = PhysicalDeviceType::IntegratedGPU;
            break;
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
            deviceType = PhysicalDeviceType::DiscreteGPU;
            break;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
            deviceType = PhysicalDeviceType::VirtualGPU;
            break;
        case VK_PHYSICAL_DEVICE_TYPE_CPU:
            deviceType = PhysicalDeviceType::CPU;
            break;
        default:
            break;
    }
    return RenderInfo {
        .rendererName         = _impl->appName,
        .gpuName              = props.deviceName,
        .deviceType           = deviceType,
        .presentationMode     = _impl->presentationMode,
        .pacingPolicy         = _impl->presenter.GetPresentTiming().policy,
        .meshShadingSupported = _impl->ctx.MeshShadersSupported(),
        .meshShadingActive    = _impl->MeshShadingActive(),
        .rayTracingSupported  = _impl->ctx.RayTracingSupported(),
    };
}

auto RenderContext::GetFrameIndex() const noexcept -> uint32_t {
    return _impl->presenter.frameIndex;
}

auto RenderContext::GetPacedDeltaTime() const noexcept -> std::optional<float> {
    return _impl->presenter.GetPacedDeltaTime();
}

auto RenderContext::GetPresentTiming() const noexcept -> PresentTimingMetrics {
    return _impl->presenter.GetPresentTiming();
}

void RenderContext::SetResolution(const Extent2D& res) {
    if (res.width > 0 && res.height > 0 && _impl->presentationTarget.IsHeadless()) {
        _impl->presentationTarget.SetFramebufferExtent(res.width, res.height);
    }
    _impl->frameState.resized = true;
}

void RenderContext::SetViewport(const ViewportRect& rect) noexcept {
    _impl->viewportX = rect.x;
    _impl->viewportY = rect.y;
    _impl->viewportW = rect.width;
    _impl->viewportH = rect.height;
}

auto RenderContext::GetViewport() const noexcept -> ViewportRect {
    const VkViewport vp = _impl->EffectiveViewport();
    return ViewportRect {
        .x      = static_cast<uint32_t>(vp.x),
        .y      = static_cast<uint32_t>(vp.y),
        .width  = static_cast<uint32_t>(vp.width),
        .height = static_cast<uint32_t>(vp.height),
    };
}

auto RenderContext::GetViewportAspect() const noexcept -> float {
    const ViewportRect vp = GetViewport();
    if (vp.height == 0) {
        return 1.0f;
    }
    return static_cast<float>(vp.width) / static_cast<float>(vp.height);
}

auto RenderContext::CreateStorageBuffer(std::span<const std::byte> bytes, uint32_t stride) -> BufferHandle {
    return _impl->geometry.CreateBuffer({.bytes = bytes, .stride = stride}, Vk::BufferUsage::Storage).value_or(BufferHandle::Invalid);
}

auto RenderContext::CreateVertexBuffer(std::span<const std::byte> bytes, uint32_t stride) -> BufferHandle {
    return _impl->geometry.CreateBuffer({.bytes = bytes, .stride = stride}, Vk::BufferUsage::Vertex).value_or(BufferHandle::Invalid);
}

auto RenderContext::CreateIndexBuffer(std::span<const uint32_t> indices) -> BufferHandle {
    // An index buffer's element size is the type of its indices, so the caller does
    // not state it -- the stride is the pointer's own.
    return _impl->geometry.CreateBuffer({.bytes = std::as_bytes(indices), .stride = static_cast<uint32_t>(sizeof(uint32_t))}, Vk::BufferUsage::Index)
        .value_or(BufferHandle::Invalid);
}

void RenderContext::DestroyBuffer(BufferHandle handle) { _impl->geometry.Destroy(handle); }

void RenderContext::UpdateBuffer(BufferHandle handle, std::span<const std::byte> bytes) noexcept {
    _impl->geometry.Update(handle, bytes.data(), bytes.size());
}


namespace {

template <Vk::ShaderProgram Vertex, Vk::ShaderProgram Fragment, Vk::ShaderProgram Mesh>
[[nodiscard]] auto ScenePipelineDesc(bool doubleSided, bool alphaBlend, bool additiveBlend, bool isLineList, bool withMesh, bool depthWrite) -> PipelineDesc {
    if (withMesh) {
        return PipelineDesc {
            .vertexShader  = Vk::CreateShaderDesc<Vertex>(),
            .fragShader    = Vk::CreateShaderDesc<Fragment>(),
            .taskShader    = Vk::CreateShaderDesc<Shaders::Modules::BasicTask>(),
            .meshShader    = Vk::CreateShaderDesc<Mesh>(),
            .doubleSided   = doubleSided,
            .alphaBlend    = alphaBlend,
            .additiveBlend = additiveBlend,
            .isLineList    = isLineList,
            .depthWrite    = depthWrite,
        };
    }
    return PipelineDesc {
        .vertexShader  = Vk::CreateShaderDesc<Vertex>(),
        .fragShader    = Vk::CreateShaderDesc<Fragment>(),
        .doubleSided   = doubleSided,
        .alphaBlend    = alphaBlend,
        .additiveBlend = additiveBlend,
        .isLineList    = isLineList,
        .depthWrite    = depthWrite,
    };
}

}

auto RenderContext::CreateBasicMaterial(bool doubleSided, bool alphaBlend, bool additiveBlend, bool depthWrite) -> std::expected<Material, ErrorCode> {
    const bool               translucent = alphaBlend || additiveBlend;
    const PipelineDesc desc = translucent
        ? ScenePipelineDesc<Shaders::Modules::BasicVSForward, Shaders::Modules::ForwardPS, Shaders::Modules::BasicMeshForward>(
              doubleSided, alphaBlend, additiveBlend, false, true, depthWrite
          )
        : ScenePipelineDesc<Shaders::Modules::BasicVS, Shaders::Modules::BasicPS, Shaders::Modules::BasicMesh>(
              doubleSided, alphaBlend, additiveBlend, false, true, depthWrite
          );

    auto mat_res = _impl->pipelines.CreateMaterial(desc);
    if (!mat_res) {
        return std::unexpected(mat_res.error());
    }
    Material mat  = mat_res.value();
    mat.albedoMap = TextureHandle::Invalid;
    return mat;
}

auto RenderContext::CreateMaterial(const MaterialDesc& desc) -> std::expected<Material, ErrorCode> {
    // When two shading models are authored together, unlit wins. Keep its
    // coverage mode independent of the forward-only optical transmission path.
    const bool transmission = !desc.unlit && desc.transmissionFactor > 0.0f;
    const bool forward      = desc.alphaBlend || desc.additiveBlend || desc.alphaMode == 2 || transmission;
    auto basicMat = CreateBasicMaterial(desc.doubleSided, forward && !desc.additiveBlend, desc.additiveBlend, transmission);
    if (!basicMat) {
        return std::unexpected(basicMat.error());
    }

    Material mat        = *basicMat;
    mat.unlit           = desc.unlit;
    // Transmission chooses a forward pipeline, not an alpha-as-coverage mode.
    // Preserve MASK (or OPAQUE) so the forward shader can apply the glTF mask.
    mat.alphaMode       = transmission ? desc.alphaMode : ((desc.alphaMode != 0) ? desc.alphaMode : basicMat->alphaMode);
    mat.alphaCutoff     = desc.alphaCutoff;
    mat.metallicFactor  = desc.metallic;
    mat.roughnessFactor = desc.roughness;
    mat.albedoMap       = desc.albedoMap;
    mat.normalMap       = desc.normalMap;
    mat.pbrMap          = desc.pbrMap;
    mat.emissiveMap     = desc.emissiveMap;
    mat.transmissionFactor = desc.transmissionFactor;
    mat.transmissionMap    = desc.transmissionMap;
    mat.iridescenceFactor  = desc.iridescenceFactor;
    mat.filmThicknessNm    = desc.filmThicknessNm;
    mat.filmThicknessMinNm = desc.filmThicknessMinNm;
    mat.volumeThicknessM   = desc.volumeThicknessM;
    mat.ior                = desc.ior;
    mat.normalScale        = desc.normalScale;
    mat.filmThicknessMap   = desc.filmThicknessMap;
    mat.iridescenceMap     = desc.iridescenceMap;
    mat.volumeThicknessMap = desc.volumeThicknessMap;
    mat.clearcoatFactor          = desc.clearcoatFactor;
    mat.clearcoatRoughnessFactor = desc.clearcoatRoughnessFactor;
    mat.clearcoatNormalScale     = desc.clearcoatNormalScale;
    mat.clearcoatMap             = desc.clearcoatMap;
    mat.clearcoatRoughnessMap    = desc.clearcoatRoughnessMap;
    mat.clearcoatNormalMap       = desc.clearcoatNormalMap;
    mat.anisotropyStrength       = desc.anisotropyStrength;
    mat.anisotropyRotation       = desc.anisotropyRotation;
    mat.anisotropyMap            = desc.anisotropyMap;
    mat.sheenColorFactor        = desc.sheenColorFactor;
    mat.sheenRoughnessFactor    = desc.sheenRoughnessFactor;
    mat.sheenColorMap           = desc.sheenColorMap;
    mat.sheenRoughnessMap       = desc.sheenRoughnessMap;
    mat.occlusionMap            = desc.occlusionMap;
    mat.occlusionStrength       = desc.occlusionStrength;
    mat.textureSamplers          = desc.textureSamplers;
    mat.textureTransforms        = desc.textureTransforms;

    mat.baseColorFactor = desc.baseColor;
    mat.emissiveFactor  = desc.emissive;

    return mat;
}

void RenderContext::DrawLine(JPH::Vec3Arg start, JPH::Vec3Arg end, JPH::Vec4Arg colorStart, JPH::Vec4Arg colorEnd) noexcept {
    _impl->queues.Lines().push_back({.start = start, .end = end, .colorStart = colorStart, .colorEnd = colorEnd});
}

void RenderContext::Impl::BeginShaderObservation() {
    if constexpr (isDev) {
        if (fileSystemWatcher && shaderDirectoryWatch == 0) {
            shaderDirectoryWatch = fileSystemWatcher->WatchDirectory(
                "resources/shaders", [this](const FS::FileWatchEvent& event) { HandleShaderFileEvent(event); }, true, ".slang",
                FS::FileSystemWatcher::kDefaultDebounceMs
            );
        }
    }
}

void RenderContext::Impl::HandleShaderFileEvent(const FS::FileWatchEvent& event) {
    if constexpr (isDev) {
        shaderReloads.Dispatch(event.path.lexically_normal().generic_string(), [this] { vkDeviceWaitIdle(ctx.Device()); });
    }
}

auto RenderContext::CreateTexture(std::span<const std::byte> rgba, Extent2D extent, bool isSRGB) -> std::expected<TextureHandle, ErrorCode> {
    const uint64_t pixels = static_cast<uint64_t>(extent.width) * extent.height;
    if (pixels == 0 || pixels > std::numeric_limits<size_t>::max() / 4 || rgba.size() != static_cast<size_t>(pixels) * 4) {
        return std::unexpected(TextureDataError::InvalidPixels);
    }
    return _impl->textureManager.UploadUnnamed(rgba.data(), extent.width, extent.height, Rgba8Format(isSRGB));
}

auto RenderContext::CreateTexture(std::string_view name, std::span<const std::byte> rgba, Extent2D extent, bool isSRGB)
    -> std::expected<TextureHandle, ErrorCode> {
    const uint64_t pixels = static_cast<uint64_t>(extent.width) * extent.height;
    if (pixels == 0 || pixels > std::numeric_limits<size_t>::max() / 4 || rgba.size() != static_cast<size_t>(pixels) * 4) {
        return std::unexpected(TextureDataError::InvalidPixels);
    }
    return _impl->textureManager.Upload(name, rgba.data(), extent.width, extent.height, Rgba8Format(isSRGB));
}

auto RenderContext::CreateTextureCube(std::array<std::span<const std::byte>, 6> faces, uint32_t faceSize) -> std::expected<TextureHandle, ErrorCode> {
    if (faceSize == 0) {
        return std::unexpected(TextureDataError::InvalidPixels);
    }
    const uint64_t pixelsPerFace = static_cast<uint64_t>(faceSize) * faceSize;
    if (pixelsPerFace > std::numeric_limits<size_t>::max() / 4) {
        return std::unexpected(TextureDataError::InvalidPixels);
    }
    const size_t bytesPerFace = static_cast<size_t>(pixelsPerFace) * 4;
    std::array<const void*, 6> faceData {};
    for (size_t i = 0; i < faces.size(); ++i) {
        if (faces[i].size() != bytesPerFace) {
            return std::unexpected(TextureDataError::InvalidPixels);
        }
        faceData[i] = faces[i].data();
    }
    return _impl->textureManager.UploadCube(faceData.data(), faceSize);
}

void RenderContext::UnloadTexture(TextureHandle handle) {
    _impl->textureManager.Unload(handle);
}

auto RenderContext::Impl::InitializeBlueNoiseTexture() -> std::expected<void, ErrorCode> {
    constexpr VkFormat kFormat = VK_FORMAT_R8G8B8A8_UNORM;

    const size_t   bytes = Resource::blue_noise_rgba.size();
    const size_t   side  = static_cast<size_t>(std::sqrt(static_cast<double>(bytes / 4)));
    if (bytes % 4 != 0 || side * side * 4 != bytes || side == 0 || side > std::numeric_limits<uint32_t>::max()) [[unlikely]] {
        ZHLN::Log("[BlueNoise] Expected a whole square of 8-bit RGBA texels, got {} bytes.", bytes);
        return std::unexpected(ErrorCode {BlueNoiseError::UnexpectedLayout});
    }

    const uint32_t w = static_cast<uint32_t>(side);
    const uint32_t h = static_cast<uint32_t>(side);

    auto imageRes = Vk::ImageBuilder {}.Texture2D(w, h, kFormat, Vk::ImageUsage::TransferDst | Vk::ImageUsage::Sampled, 1).Build(allocator);
    if (!imageRes) {
        return std::unexpected(imageRes.error());
    }

    Vk::Image image = std::move(*imageRes);
    defer _([&] { allocator.DestroyImage(image); });
    auto staging = stagingRingBuffer.Allocate(bytes);
    if (staging.mappedData == nullptr) {
        return std::unexpected(Vk::StagingError::MemoryMappingFailed);
    }
    std::memcpy(staging.mappedData, Resource::blue_noise_rgba.data(), bytes);

    Vk::ExecuteImmediate(ctx, graphicsCmdRing, stagingRingBuffer, [&](VkCommandBuffer cmd) {
        Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(cmd, image.Handle());

        const VkBufferImageCopy2 region = {
            .sType             = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
            .pNext             = nullptr,
            .bufferOffset      = staging.slice.offset,
            .bufferRowLength   = 0,
            .bufferImageHeight = 0,
            .imageSubresource  = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1},
            .imageOffset       = {0, 0, 0},
            .imageExtent       = {w, h, 1},
        };
        Vk::CopyBufferToImage<1>(cmd, staging.slice.buffer, image.Handle(), {region});
        Vk::TransitionLayout<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, image.Handle());
    });

    auto viewRes = Vk::ImageView::Create<kFormat>(ctx.Device(), image.Handle(), VK_IMAGE_ASPECT_COLOR_BIT, 1);
    if (!viewRes) {
        return std::unexpected(viewRes.error());
    }
    Vk::ImageView view = std::move(*viewRes);

    Vk::Debug::SetImageName(ctx, image.Handle(), "BlueNoise.LDR_RGBA_0");

    auto samplerBuilder = Vk::SamplerBuilder {}.Nearest().Repeat().LodRange(0.0F, 0.0F);
    auto samplerRes     = samplerBuilder.Build(ctx.Device());
    if (!samplerRes) {
        return std::unexpected(samplerRes.error());
    }

    blueNoiseSampler     = std::move(*samplerRes);
    blueNoiseSamplerInfo = samplerBuilder.Info();
    auto blueNoiseIdx = textureManager.Adopt(std::move(image), std::move(view));
    if (!blueNoiseIdx) {
        return std::unexpected(blueNoiseIdx.error());
    }
    blueNoiseTexIdx = *blueNoiseIdx;

    ZHLN::Log("[BlueNoise] Blue noise tile bound as bindless texture {} ({}x{}, single mip).", blueNoiseTexIdx, w, h);
    return {};
}


auto RenderContext::CreateSkinnedScratchBuffer(uint32_t vertexCount) -> BufferHandle {
    return _impl->geometry.CreateSkinnedScratchBuffer(vertexCount);
}

void RenderContext::Impl::BuildOrUpdateSkinnedBLAS(VkCommandBuffer cmd, const DrawCommand& drawCmd, NativeMesh* scratchMesh) {
    if (!ctx.RayTracingSupported() || scratchMesh == nullptr || drawCmd.posMesh == nullptr) {
        return;
    }

    ZHLN_BlasGeometryDesc geom = {
        .vertex_data   = scratchMesh->vboAddress,
        .vertex_stride = sizeof(VertexPosition),
        .max_vertex    = scratchMesh->vertexCount > 0 ? scratchMesh->vertexCount - 1 : 0,
        .vertex_format = VK_FORMAT_R32G32B32_SFLOAT,
        .index_data    = drawCmd.instanceData.iboAddress,
        .index_type    = (drawCmd.instanceData.iboAddress != 0) ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_NONE_KHR
    };

    uint32_t primitiveCount = (drawCmd.instanceData.iboAddress != 0) ? drawCmd.instanceData.indexCount / 3 : scratchMesh->vertexCount / 3;

    ZHLN_AccelerationStructureSizes sizes {};
    Vk::GetBLASSizes(ctx.Device(), geom, primitiveCount, sizes);

    const bool creatingBlas = !scratchMesh->blas;
    if (creatingBlas) {
        auto blasBufOpt = Vk::Buffer::Create(
            allocator, sizes.acceleration_structure_size,
            Vk::BufferUsage::AccelerationStructureStorage | Vk::BufferUsage::ShaderDeviceAddress, Vk::MemoryUsage::GPUOnly
        );
        if (!blasBufOpt) {
            return;
        }
        scratchMesh->blasBuffer = std::move(*blasBufOpt);
        scratchMesh->blas       = Vk::AccelerationStructure(
            ctx.Device(),
            Vk::CreateAccelerationStructure(ctx.Device(), scratchMesh->blasBuffer.Handle(), sizes.acceleration_structure_size, ZHLN_AS_TYPE_BOTTOM_LEVEL)
        );
        if (!scratchMesh->blas.Valid()) {
            allocator.DestroyBuffer(scratchMesh->blasBuffer);
            return;
        }
        scratchMesh->blasAddress = Vk::GetAccelerationStructureAddress(ctx.Device(), scratchMesh->blas.Get());
    }

    auto scratchBufOpt = Vk::Buffer::Create(
        allocator, sizes.build_scratch_size, Vk::BufferUsage::Storage | Vk::BufferUsage::ShaderDeviceAddress, Vk::MemoryUsage::GPUOnly
    );
    if (!scratchBufOpt) {
        // No build was recorded: do not leave an uninitialized AS for the
        // next frame to treat as an existing, updateable BLAS.
        if (creatingBlas) {
            scratchMesh->blas = {};
            allocator.DestroyBuffer(scratchMesh->blasBuffer);
            scratchMesh->blasAddress = 0;
        }
        return;
    }
    Vk::Buffer scratchBuf = std::move(*scratchBufOpt);
    Vk::BuildBLAS(cmd, geom, scratchMesh->blas.Get(), Vk::BufferSlice {scratchBuf, ctx.BufferAddress(scratchBuf.Handle())}, primitiveCount);
    deletionQueue.Enqueue(std::move(scratchBuf)); // Build is recorded into the in-flight frame.
}

uint32_t RenderContext::UploadDebugVertices(std::span<const VertexPosition> positions, std::span<const VertexSurface> surfaces) noexcept {
    if (positions.size() != surfaces.size()) {
        ZHLN::Assert(false, "debug vertex positions and surfaces must have the same count");
        return 0;
    }
    auto* nativeMesh = _impl->geometry.Resolve(_impl->frames.debugMeshHandles[_impl->presenter.frameIndex]);
    if (nativeMesh == nullptr) {
        return 0;
    }

    constexpr size_t maxPosSize = RenderContext::Impl::kMaxDebugVertices * sizeof(VertexPosition);

    auto mapped = nativeMesh->buffer.Map(_impl->allocator);
    if (!mapped) return 0;
    char* basePtr = mapped->As<char>();

    const size_t count = std::min(positions.size(), static_cast<size_t>(RenderContext::Impl::kMaxDebugVertices));
    if (count > 0) {
        std::memcpy(basePtr, positions.data(), count * sizeof(VertexPosition));
        std::memcpy(basePtr + maxPosSize, surfaces.data(), count * sizeof(VertexSurface));
    }
    nativeMesh->vertexCount = static_cast<uint32_t>(count);
    return nativeMesh->vertexCount;
}

auto RenderContext::GetDebugMeshBuffer() const noexcept -> BufferHandle {
    return _impl->frames.debugMeshHandles[_impl->presenter.frameIndex];
}

void RenderContext::UpdateJointMatrices(uint32_t offset, std::span<const JPH::Mat44> matrices) {
    if (matrices.empty()) {
        return;
    }
    auto& buffer = _impl->frames.jointBuffers[_impl->presenter.frameIndex];
    if (offset > buffer.Size() / sizeof(JPH::Mat44) || matrices.size() > buffer.Size() / sizeof(JPH::Mat44) - offset) {
        ZHLN::Assert(false, "joint palette exceeds the current frame's buffer");
        return;
    }
    auto mapped = buffer.Map(_impl->allocator);
    if (!mapped) {
        return;
    }
    std::memcpy(mapped->As<JPH::Mat44>() + offset, matrices.data(), matrices.size_bytes());
}

auto RenderContext::AllocateMorphDeltas(std::span<const float> deltas) -> uint32_t {
    const uint32_t offset = _impl->nextMorphDeltaIndex;
    const size_t capacity = _impl->morphDeltasBuffer.Size() / sizeof(float);
    if (deltas.size() % 4 != 0 || static_cast<size_t>(offset) * 4 > capacity || deltas.size() > capacity - static_cast<size_t>(offset) * 4) {
        ZHLN::Assert(false, "morph deltas must fit in the buffer as complete float4s");
        return offset;
    }
    if (!deltas.empty()) {
        auto mapped = _impl->morphDeltasBuffer.Map(_impl->allocator);
        if (mapped) {
            std::memcpy(mapped->As<float>() + static_cast<size_t>(offset) * 4, deltas.data(), deltas.size_bytes());
        }
    }
    _impl->nextMorphDeltaIndex += static_cast<uint32_t>(deltas.size() / 4);
    return offset;
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif


auto RenderContext::SetShadowResolution(uint32_t resolution) -> std::expected<void, ErrorCode> {
    auto* impl = _impl.get();

    return impl->targets.ResizeShadows(resolution).transform([&]() -> void {
        impl->settings.shadows.resolution = resolution;
        impl->settings.qualityPreset = impl->settings.DetectPreset();
    });
}

void RenderContext::Impl::ApplySettings(GraphicsSettings&& incoming) noexcept {
    const QualityLevel previousTier = settings.qualityPreset;

    if (incoming.shadows.resolution != settings.shadows.resolution) {
        if (targets.ResizeShadows(incoming.shadows.resolution)) {
            settings.shadows.resolution = incoming.shadows.resolution;
        } else {
            ZHLN::LogWarning(
                "failed to resize shadow targets to {}x{}; keeping {}x{}", incoming.shadows.resolution, incoming.shadows.resolution,
                settings.shadows.resolution, settings.shadows.resolution
            );
            incoming.shadows.resolution = settings.shadows.resolution;
        }
    }

    incoming.qualityPreset = incoming.DetectPreset();
    settings               = std::move(incoming);

    if (settings.qualityPreset != previousTier) {
        ZHLN::Log("Graphics quality tier: {} -> {}", previousTier, settings.qualityPreset);
    }
}

void RenderContext::ApplySettings(GraphicsSettings newSettings) noexcept {
    _impl->ApplySettings(std::move(newSettings));
}

const GraphicsSettings& RenderContext::GetSettings() const noexcept {
    return _impl->settings;
}

void RenderContext::SetAAState(const AAState& state) {
    _impl->settings.antiAliasing = state;
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

auto RenderContext::BuildMeshBLAS(Mesh& mesh) noexcept -> RenderResult {
    auto& impl = *_impl;
    if (!impl.ctx.RayTracingSupported()) return std::unexpected(RenderFeatureError::FeatureNotSupported);
    // MeshBuilder and the glTF importer report this specific resolution error.
    auto* posMesh = impl.geometry.Resolve(mesh.posBuffer);
    if (posMesh == nullptr) return std::unexpected(RenderFeatureError::UnresolvedMeshHandle);
    auto* indexMesh = mesh.indexBuffer != BufferHandle::Invalid ? impl.geometry.Resolve(mesh.indexBuffer) : nullptr;

    const ZHLN_BlasGeometryDesc geom {
        .vertex_data = posMesh->vboAddress,
        .vertex_stride = sizeof(VertexPosition),
        .max_vertex = mesh.vertexCount > 0 ? mesh.vertexCount - 1 : 0,
        .vertex_format = VK_FORMAT_R32G32B32_SFLOAT,
        .index_data = indexMesh != nullptr ? indexMesh->vboAddress : 0,
        .index_type = indexMesh != nullptr ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_NONE_KHR,
    };
    const uint32_t primitiveCount = indexMesh != nullptr ? mesh.indexCount / 3 : mesh.vertexCount / 3;
    ZHLN_AccelerationStructureSizes sizes {};
    Vk::GetBLASSizes(impl.ctx.Device(), geom, primitiveCount, sizes);

    auto bufferRes = Vk::Buffer::Create(
        impl.allocator, sizes.acceleration_structure_size,
        Vk::BufferUsage::AccelerationStructureStorage | Vk::BufferUsage::ShaderDeviceAddress, Vk::MemoryUsage::GPUOnly
    );
    if (!bufferRes) return std::unexpected(bufferRes.error());
    defer _([&] { impl.allocator.DestroyBuffer(*bufferRes); });
    Vk::AccelerationStructure blas(
        impl.ctx.Device(), Vk::CreateAccelerationStructure(impl.ctx.Device(), bufferRes->Handle(), sizes.acceleration_structure_size, ZHLN_AS_TYPE_BOTTOM_LEVEL)
    );
    if (!blas.Valid()) return std::unexpected(Vk::VulkanCallError::VulkanCallFailed);

    auto scratchRes = Vk::Buffer::Create(
        impl.allocator, sizes.build_scratch_size, Vk::BufferUsage::Storage | Vk::BufferUsage::ShaderDeviceAddress,
        Vk::MemoryUsage::GPUOnly
    );
    if (!scratchRes) return std::unexpected(scratchRes.error());
    defer _([&] { impl.allocator.DestroyBuffer(*scratchRes); });

    Vk::CommandPool<Vk::QueueType::Graphics> tempPool(impl.ctx.Device(), impl.ctx.PhysicalInfo().graphics_family);
    auto allocated = tempPool.Allocate(1);
    if (!allocated) return std::unexpected(allocated.error());
    auto recording = Vk::CommandRecorder::Begin(tempPool[0]);
    if (!recording) return std::unexpected(recording.error());
    const VkCommandBuffer cmd = recording->Handle();
    Vk::MemoryBarrier(cmd, Vk::BarrierStage::Copy, Vk::BarrierAccess::TransferWrite,
                      Vk::BarrierStage::AccelerationStructureBuild, Vk::BarrierAccess::AccelerationStructureRead);
    Vk::BuildBLAS(cmd, geom, blas.Get(), Vk::BufferSlice {*scratchRes, impl.ctx.BufferAddress(scratchRes->Handle())}, primitiveCount);
    auto executable = std::move(*recording).End();
    if (!executable) return std::unexpected(executable.error());

    auto submitted = Vk::SubmitAndWait(
        impl.ctx.GraphicsQueue(), std::move(*executable), impl.transferRingBuffer.GetSemaphore(), impl.transferRingBuffer.GetCurrentValue(),
        VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR
    );
    if (!submitted) {
        vkQueueWaitIdle(impl.ctx.GraphicsQueue());
        return std::unexpected(submitted.error());
    }

    // The old BLAS may still be referenced by another in-flight frame.
    impl.deletionQueue.EnqueueAccelerationStructure(impl.ctx.Device(), std::move(posMesh->blas));
    impl.deletionQueue.Enqueue(std::move(posMesh->blasBuffer));
    posMesh->blasAddress = Vk::GetAccelerationStructureAddress(impl.ctx.Device(), blas.Get());
    posMesh->blasBuffer = std::move(*bufferRes);
    posMesh->blas = std::move(blas);
    return {};
}


auto RenderContext::BakeProceduralTexture(uint32_t width, uint32_t height, uint32_t variantIdx, float scale, float randomness)
    -> std::expected<TextureHandle, ErrorCode> {
    return _impl->BakeProceduralTexture(width, height, variantIdx, scale, randomness, 0.0f);
}

auto RenderContext::CreateProceduralTexture(std::string_view name, Extent2D extent, std::span<const uint32_t> pixels, bool isSRGB) -> TextureHandle {
    const uint64_t expectedPixels = static_cast<uint64_t>(extent.width) * extent.height;
    if (expectedPixels == 0 || expectedPixels != pixels.size()) {
        ZHLN::Log("[RenderContext] Procedural texture '{}' has an invalid extent or texel count.", name);
        return TextureHandle::Invalid;
    }
    const auto uploaded = CreateTexture(name, std::as_bytes(pixels), extent, isSRGB);
    if (!uploaded) {
        ZHLN::Log("[RenderContext] Procedural texture '{}' ({}x{}) failed to upload: {}", name, extent.width, extent.height, uploaded.error());
        return TextureHandle::Invalid;
    }
    return *uploaded;
}

enum class ScreenshotError : uint8_t {
    FileOpenFailed ZHLN_ANNOTATION(ZHLN::Description<"Failed to open screenshot output file for writing"> {}) = 1,
    DestinationNotRecorded
        ZHLN_ANNOTATION(ZHLN::Description<"No completed frame was drawn into the headless presentation target"> {}),
};

auto RenderContext::CaptureScreenshotPPM(std::string_view outputPath) noexcept -> std::expected<void, ErrorCode> {
    auto* const impl = _impl.get();

    if (!impl->presenter.swapchain.Valid()) {
        const auto dest = impl->destinations.Find(impl->presentationTarget);
        if (impl->frameOpen || !dest || !dest->acquired || !dest->acquired->drawn) {
            // A headless target starts UNDEFINED. If rendering failed before
            // acquisition, treating it as COLOR_ATTACHMENT here makes the
            // readback barrier invalid and writes a black success image.
            ZHLN::Log("[Test Capture] No completed headless frame was drawn; capture refused.");
            return std::unexpected(ScreenshotError::DestinationNotRecorded);
        }
        const auto& frameImage = *dest->acquired;
        VkImage       source       = frameImage.image.Handle();
        VkExtent2D    extent       = frameImage.image.Extent2D();
        VkImageLayout sourceLayout = Vk::ToVkImageLayout(frameImage.layout);

        const auto imageBytes = static_cast<size_t>(extent.width) * extent.height * 4u;

        auto stagingRes = Vk::Buffer::Create(impl->allocator, imageBytes, Vk::BufferUsage::TransferDst, Vk::MemoryUsage::GPUToCPU);
        if (!stagingRes) {
            return std::unexpected(stagingRes.error());
        }
        auto stagingBuffer = std::move(*stagingRes);
        defer _([&] { impl->allocator.DestroyBuffer(stagingBuffer); });

        Vk::ExecuteImmediate(impl->ctx, impl->graphicsCmdRing, [&](VkCommandBuffer cmd) -> void {
            const VkImageMemoryBarrier2 toTransfer = Vk::MakeImageBarrier({
                .image      = source,
                .src_access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                .dst_access = VK_ACCESS_2_TRANSFER_READ_BIT,
                .src_layout = sourceLayout,
                .dst_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .src_stage  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                .dst_stage  = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .aspect     = VK_IMAGE_ASPECT_COLOR_BIT,
                .base_mip   = 0,
                .mip_count  = VK_REMAINING_MIP_LEVELS,
            });
            Vk::PipelineBarrier(cmd, std::span<const VkBufferMemoryBarrier2> {}, std::span<const VkImageMemoryBarrier2> {&toTransfer, 1});

            Vk::CopyImageToBuffer(cmd, source, stagingBuffer.Handle(), extent);

            const VkImageMemoryBarrier2 toFrame = Vk::MakeImageBarrier({
                .image      = source,
                .src_access = VK_ACCESS_2_TRANSFER_READ_BIT,
                .dst_access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                .src_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .dst_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .src_stage  = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                .dst_stage  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                .aspect     = VK_IMAGE_ASPECT_COLOR_BIT,
                .base_mip   = 0,
                .mip_count  = VK_REMAINING_MIP_LEVELS,
            });
            Vk::PipelineBarrier(cmd, std::span<const VkBufferMemoryBarrier2> {}, std::span<const VkImageMemoryBarrier2> {&toFrame, 1});
        });

        auto mapped = stagingBuffer.Map(impl->allocator);
        if (!mapped) {
            return std::unexpected(mapped.error());
        }

        std::ofstream ofs(std::string(outputPath), std::ios::binary);
        if (!ofs.is_open()) {
            return std::unexpected(ScreenshotError::FileOpenFailed);
        }

        const bool writeAlpha = outputPath.ends_with(".pam") || outputPath.ends_with(".PAM");
        if (writeAlpha) {
            ofs << "P7\nWIDTH " << extent.width << "\nHEIGHT " << extent.height << "\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n";
        } else {
            ofs << "P6\n" << extent.width << " " << extent.height << "\n255\n";
        }

        const auto*  rgba   = mapped->As<const uint8_t>();
        const size_t pixels = static_cast<size_t>(extent.width) * extent.height;
        uint64_t     lumaSum = 0;
        uint64_t     lit     = 0;
        uint64_t     transparent = 0;
        std::array<uint64_t, 3> channelSum {};
        std::array<uint64_t, 3> aboveFloor {};
        std::array<uint8_t, 3>  channelMax {};
        for (size_t i = 0; i < pixels; ++i) {
            const uint8_t r = rgba[i * 4 + 0];
            const uint8_t g = rgba[i * 4 + 1];
            const uint8_t b = rgba[i * 4 + 2];
            if (rgba[i * 4 + 3] == 0) {
                ++transparent;
            }
            if (!writeAlpha) {
                ofs.put(static_cast<char>(r));
                ofs.put(static_cast<char>(g));
                ofs.put(static_cast<char>(b));
            }

            const std::array<uint8_t, 3> channels {r, g, b};
            for (size_t c = 0; c < channels.size(); ++c) {
                channelSum[c] += channels[c];
                channelMax[c] = std::max(channelMax[c], channels[c]);
                aboveFloor[c] += channels[c] >= 45u ? 1u : 0u;
            }

            const uint32_t luma = (2126u * static_cast<uint32_t>(r) + 7152u * static_cast<uint32_t>(g) + 722u * static_cast<uint32_t>(b)) / 10000u;
            lumaSum += luma;
            lit += luma > 8u ? 1u : 0u;
        }
        if (writeAlpha) {
            ofs.write(reinterpret_cast<const char*>(rgba), static_cast<std::streamsize>(pixels * 4u));
            ZHLN::Log("[Test Capture] {} of {} pixels have alpha 0 (omit-background).", transparent, pixels);
        }
        ofs.close();

        const double meanLuma = pixels == 0 ? 0.0 : static_cast<double>(lumaSum) / static_cast<double>(pixels);
        const auto   meanOf   = [pixels](uint64_t sum) -> double { return pixels == 0 ? 0.0 : static_cast<double>(sum) / static_cast<double>(pixels); };
        ZHLN::Log(
            "[Test Capture] Rendered frame written to: {} ({}x{} from image 0x{:016X}: mean luma {:.2f}, {} of {} pixels above black; mean RGB ({:.2f},{:.2f},{:.2f}); "
            "max RGB ({},{},{}); channel pixels >=45: {}/{}/{})",
            outputPath, extent.width, extent.height, reinterpret_cast<uint64_t>(source), meanLuma, lit, pixels, meanOf(channelSum[0]), meanOf(channelSum[1]),
            meanOf(channelSum[2]), channelMax[0], channelMax[1], channelMax[2], aboveFloor[0], aboveFloor[1], aboveFloor[2]
        );
        return {};
    }

    const auto extent = impl->graphResources.hdrSceneColor.extent;

    const size_t imageBytes = static_cast<size_t>(extent.width) * extent.height * sizeof(uint16_t) * 4;

    auto stagingRes = Vk::Buffer::Create(impl->allocator, imageBytes, Vk::BufferUsage::TransferDst, Vk::MemoryUsage::GPUToCPU);
    if (!stagingRes) {
        return std::unexpected(stagingRes.error());
    }
    auto stagingBuffer = std::move(*stagingRes);
    defer _([&] { impl->allocator.DestroyBuffer(stagingBuffer); });

    Vk::ExecuteImmediate(impl->ctx, impl->graphicsCmdRing, [&](VkCommandBuffer cmd) -> void {
        auto* const targetImg = impl->graphResources.hdrSceneColor.image.Handle();

        Vk::TransitionLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL>(cmd, targetImg);
        Vk::CopyImageToBuffer(cmd, targetImg, stagingBuffer.Handle(), extent);
        Vk::TransitionLayout<VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, targetImg);
    });

    auto mapped = stagingBuffer.Map(impl->allocator);
    if (!mapped) {
        return std::unexpected(mapped.error());
    }
    const auto* const halfFloats = mapped->As<const uint16_t>();

    std::ofstream ofs(std::string(outputPath), std::ios::binary);
    if (!ofs.is_open()) {
        return std::unexpected(ScreenshotError::FileOpenFailed);
    }

    ofs << "P6\n" << extent.width << " " << extent.height << "\n255\n";

    auto HalfToFloat = [](uint16_t h) noexcept -> float {
        uint32_t sign     = (h >> 15) & 0x00000001;
        uint32_t exponent = (h >> 10) & 0x0000001f;
        uint32_t mantissa = h & 0x000003ff;

        if (exponent == 0) {
            if (mantissa == 0) {
                return sign ? -0.0f : 0.0f;
            }
            return (sign ? -1.0f : 1.0f) * std::ldexp(static_cast<float>(mantissa), -24);
        }
        if (exponent == 31) {
            return sign ? -INFINITY : INFINITY;
        }
        return (sign ? -1.0f : 1.0f) * std::ldexp(static_cast<float>(mantissa | 0x0400), static_cast<int>(exponent) - 15 - 10);
    };

    auto ACESFilm = [](float x) noexcept -> float {
        float a = 2.51f;
        float b = 0.03f;
        float c = 2.43f;
        float d = 0.59f;
        float e = 0.14f;
        return std::clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0f, 1.0f);
    };

    bool isFullBright = (impl->currentUniforms.fullBright != 0);

    ZHLN::Array<uint8_t> rgb(static_cast<size_t>(extent.width) * extent.height * 3);
    for (size_t i = 0; i < static_cast<size_t>(extent.width) * extent.height; ++i) {
        float r = HalfToFloat(halfFloats[i * 4 + 0]);
        float g = HalfToFloat(halfFloats[i * 4 + 1]);
        float b = HalfToFloat(halfFloats[i * 4 + 2]);

        if (!isFullBright) {
            r = ACESFilm(r * 0.015f);
            g = ACESFilm(g * 0.015f);
            b = ACESFilm(b * 0.015f);
        }

        rgb[i * 3 + 0] = static_cast<uint8_t>(std::clamp(r, 0.0f, 1.0f) * 255.0f);
        rgb[i * 3 + 1] = static_cast<uint8_t>(std::clamp(g, 0.0f, 1.0f) * 255.0f);
        rgb[i * 3 + 2] = static_cast<uint8_t>(std::clamp(b, 0.0f, 1.0f) * 255.0f);
    }

    ofs.write(reinterpret_cast<const char*>(rgb.data()), rgb.size());
    ofs.close();

    ZHLN::Log("[Test Capture] Rendered frame written to: {}", outputPath);
    return {};
}

void RenderContext::ProvokeDeviceLost() {
    _impl->ProvokeDeviceLostInternal();
}

}
