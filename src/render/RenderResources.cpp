// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/RenderResources.cpp
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
#include <utility>
#include <vector>

// Private Resource Errors (Tier 1)
// Produced only while building materials / resizing shadow targets inside
// this translation unit; no header exposes them, so callers just log the
// type-erased ZHLN::ErrorCode. Declared at file scope (not an anonymous
// namespace) to keep reflected category names stable for both native
// reflection and the AST transpiler fallback.

namespace ZHLN {

enum class BlueNoiseError : uint8_t {
    UnexpectedLayout ZHLN_ANNOTATION(ZHLN::Description<"Blue noise blob is not a whole square of 8-bit RGBA texels"> {}) = 1,
};

} // namespace ZHLN

namespace ZHLN {

// High-Level GPU Asset Registry & Resolution API

auto RenderContext::GetGPUMesh(AssetID id) const noexcept -> std::optional<Mesh> {
    const Mesh* found = _impl->geometry.FindMesh(id);
    if (found != nullptr) {
        return *found;
    }
    return std::nullopt;
}

auto RenderContext::GetGPUMaterial(MaterialID id) const noexcept -> std::optional<Material> {
    const Material* found = _impl->geometry.FindMaterial(id);
    if (found != nullptr) {
        return *found;
    }
    return std::nullopt;
}

void RenderContext::RegisterGPUMesh(AssetID id, Mesh mesh) noexcept { _impl->geometry.RegisterMesh(id, mesh); }

void RenderContext::RegisterGPUMaterial(MaterialID id, Material mat) noexcept { _impl->geometry.RegisterMaterial(id, mat); }

auto RenderContext::GetOrCreateSkinnedScratchBuffer(uint64_t entityKey, uint32_t vertexCount) -> BufferHandle {
    return _impl->geometry.GetOrCreateSkinnedScratchBuffer(entityKey, vertexCount);
}

auto RenderContext::CreateStorageBuffer(size_t size) -> BufferHandle {
    return _impl->geometry.CreateStorageBuffer(size, Vk::BufferUsage::Storage | Vk::BufferUsage::Vertex);
}

auto RenderContext::GetOrCreateParticleBuffer(Entity owner, uint32_t subresourceKey, uint32_t maxParticles) -> BufferHandle {
    if (owner == Entity::Null()) {
        return BufferHandle::Invalid;
    }

    // The key folds the owner and the subresource, and the size comes from the
    // shader's particle struct -- both belong to the caller, so the manager is
    // handed the key, the packed owner and a byte count.
    const uint64_t cacheKey = owner.Pack() ^ static_cast<uint64_t>(subresourceKey);
    return _impl->geometry.GetOrCreateParticleBuffer(cacheKey, owner.Pack(), maxParticles * sizeof(Particle), Vk::BufferUsage::Storage | Vk::BufferUsage::Vertex);
}

void RenderContext::SubmitParticleEmitter(BufferHandle gpuBuffer, uint32_t maxParticles, const ParticleEmitterParams& params) {
    _impl->queues.ParticleEmitters().push_back({.gpuBuffer = gpuBuffer, .maxParticles = maxParticles, .params = params});
}

void RenderContext::SubmitMeshParticleEmitter(
    BufferHandle                     gpuBuffer,
    uint32_t                         maxParticles,
    const MeshParticleEmitterParams& params,
    AssetID                          mesh,
    MaterialID                       mat
) {
    _impl->queues.MeshParticleEmitters().push_back(
        {.gpuBuffer = gpuBuffer, .maxParticles = maxParticles, .params = params, .meshAsset = mesh, .materialAsset = mat}
    );
}

auto RenderContext::GetBindlessIndex(TextureHandle handle) const noexcept -> uint32_t {
    return _impl->textureManager.GetBindlessIndex(handle);
}

void RenderContext::ClearGPUCaches() noexcept {
    // 1. Wait for GPU to finish all in-flight work before destroying any pipelines or buffers
    if (_impl->ctx.Device() != VK_NULL_HANDLE) {
        auto res = Vk::WaitIdle(_impl->ctx.Device());
        if (!res) {
            ZHLN::Log("GPU cache clear aborted due to reason: {}", res.error());
            return;
        }
    }

    // 2. Reclaim the buffers the registered meshes hold. What a cached material
    //    holds instead is pipelines, so that half stays here until the pipeline
    //    registry exists to own it.
    _impl->geometry.ReleaseMeshBuffers();

    // 3. Reclaim all pipeline slots from registered materials (safe now: the
    //    device was idled above).
    _impl->geometry.ForEachMaterial([this](MaterialID, const Material& mat) {
        if (mat.pipeline != PipelineHandle::Invalid) {
            _impl->pipelines.Destroy(mat.pipeline);
        }
        if (mat.prePassPipeline != PipelineHandle::Invalid) {
            _impl->pipelines.Destroy(mat.prePassPipeline);
        }
    });
    _impl->geometry.ClearMaterials();

    _impl->geometry.ReleaseSkinnedScratchBuffers();
    _impl->geometry.ReleaseParticleBuffers();
    _impl->geometry.ReleaseLedgers();

    // The records are the only owner of a texture's bindless index, so the
    // slots go back to the allocator with them. The images are parked until the
    // next frame boundary, which is safe here because the device was idled
    // above. Clearing is the manager's own teardown: it knows which slots are
    // in use, so the caller does not collect the indices and hand them back.
    _impl->textureManager.Clear();

    // 5. Drain the deferred deletion queues
    _impl->deletionQueue.BeginFrame(0);
    _impl->deletionQueue.BeginFrame(1);
}

auto RenderContext::GetTracked2DEmitters() noexcept -> ZHLN::Array<ZHLN::Pair<uint64_t, BufferHandle>>& {
    return _impl->geometry.Emitters2D();
}

auto RenderContext::GetTracked3DEmitters() noexcept -> ZHLN::Array<ZHLN::Pair<uint64_t, BufferHandle>>& {
    return _impl->geometry.Emitters3D();
}

void RenderContext::TrackEntityBuffer(Entity owner, BufferHandle buffer) {
    if (owner != Entity::Null() && buffer != BufferHandle::Invalid) {
        _impl->geometry.TrackEntityBuffer(owner.Pack(), buffer);
    }
}

void RenderContext::ReleaseEntityBuffers(Entity owner) { _impl->geometry.ReleaseOwner(owner.Pack()); }

void RenderContext::ReconcileEntityBuffers(EntityAliveQuery alive) { _impl->geometry.Reconcile(alive); }

auto RenderContext::GetTrackedEntityBufferCount() const noexcept -> size_t {
    return _impl->geometry.EntityBufferCount();
}

void RenderContext::UseDiagnostics(std::atomic<uint32_t>* validationErrors, std::atomic<uint32_t>* deviceLost) noexcept {
    Vk::Instance::UseDiagnostics({validationErrors, deviceLost});
}

uint32_t RenderContext::ValidationErrorCount() noexcept {
    return Vk::Instance::ValidationErrorCount();
}

uint32_t RenderContext::DeviceLostCount() noexcept {
    return Vk::Instance::DeviceLostCount();
}

void RenderContext::WriteCheckpoint(std::string_view name) noexcept {
    // The frame's stream is the destination it is drawing into; a checkpoint
    // written outside a frame's target has no stream to go into, and says so by
    // doing nothing.
    if (const VkCommandBuffer cmd = _impl->FrameCommand(); cmd != VK_NULL_HANDLE) {
        _impl->gpuDiagnostics.WriteCheckpoint(cmd, name);
    }
}

PipelineStatsCapture RenderContext::CapturePipelineStats() noexcept {
    if (!_impl->gpuProfiler.PipelineStatsAvailable()) {
        return {};
    }
    // A fresh capture window: leftovers from an earlier capture must not
    // contaminate this one.
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

// RenderContext Subsystem Implementation

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

// Both read the primary presenter's pacer: the engine paces simulation off
// the display-locked interval and scales fidelity off the present margin.
auto RenderContext::GetPacedDeltaTime() const noexcept -> std::optional<float> {
    return _impl->presenter.GetPacedDeltaTime();
}

auto RenderContext::GetPresentTiming() const noexcept -> PresentTimingMetrics {
    return _impl->presenter.GetPresentTiming();
}

void RenderContext::SetResolution(const Extent2D& res) {
    // With a real window the compositor owns the size: the request is advisory
    // and the recreate re-queries the target's framebuffer extent, which is why this
    // used to ignore its argument entirely. Headless (and TTY) there is nothing
    // to ask -- Window::GetSize just returns what it was told -- so the extent
    // has to be written there or the recreate reproduces the old size and the
    // call is a no-op.
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
    // The impl works in VkViewport (see EffectiveViewport); its values are
    // whole pixels, so narrowing back into the API-neutral rect is exact.
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

auto RenderContext::CreateStorageBuffer(const void* data, size_t size, uint32_t stride) -> BufferHandle {
    return _impl->geometry.CreateStorageBuffer(data, size, stride, Vk::BufferUsage::Storage);
}

auto RenderContext::CreateVertexBuffer(const void* data, size_t size, uint32_t stride) -> BufferHandle {
    return _impl->geometry.CreateVertexBuffer(data, size, stride, Vk::BufferUsage::Vertex);
}

auto RenderContext::CreateIndexBuffer(const void* data, size_t size) -> BufferHandle {
    return _impl->geometry.CreateIndexBuffer(data, size, Vk::BufferUsage::Index);
}

void RenderContext::DestroyBuffer(BufferHandle handle) { _impl->geometry.Destroy(handle); }

void RenderContext::UpdateBuffer(BufferHandle handle, const void* data, size_t size) noexcept { _impl->geometry.Update(handle, data, size); }

// Material pipeline compilation moved to PipelineRegistry::CreateMaterial, which
// owns the table the resulting handle indexes.

namespace {

// The scene-geometry variants, as generated modules: picking a variant picks
// the geometry module AND the fragment module together -- they are compiled
// against one varying set, so pairing across variants mismatches locations --
// plus the mesh-shader twin of that geometry. The vertex pipeline is always
// built; the mesh stages only feed the optional second pipeline.
template <Vk::ShaderProgram Vertex, Vk::ShaderProgram Fragment, Vk::ShaderProgram Mesh>
[[nodiscard]] auto ScenePipelineDesc(bool doubleSided, bool alphaBlend, bool additiveBlend, bool isLineList, bool withMesh) -> PipelineDesc {
    // Two full initializations rather than a field assignment: ZHLN_ShaderDesc
    // carries borrowed bytes, so it is not copy-assignable.
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
        };
    }
    return PipelineDesc {
        .vertexShader  = Vk::CreateShaderDesc<Vertex>(),
        .fragShader    = Vk::CreateShaderDesc<Fragment>(),
        .doubleSided   = doubleSided,
        .alphaBlend    = alphaBlend,
        .additiveBlend = additiveBlend,
        .isLineList    = isLineList,
    };
}

} // namespace

auto RenderContext::CreateBasicMaterial(bool doubleSided, bool alphaBlend, bool additiveBlend) -> std::expected<Material, ErrorCode> {
    // Translucent materials rasterise through PSForward, so they take the
    // Forward modules; the modules themselves carry the pairing invariant.
    const bool               translucent = alphaBlend || additiveBlend;
    const PipelineDesc desc = translucent
        ? ScenePipelineDesc<Shaders::Modules::BasicVSForward, Shaders::Modules::ForwardPS, Shaders::Modules::BasicMeshForward>(
              doubleSided, alphaBlend, additiveBlend, false, true
          )
        : ScenePipelineDesc<Shaders::Modules::BasicVS, Shaders::Modules::BasicPS, Shaders::Modules::BasicMesh>(doubleSided, alphaBlend, additiveBlend, false, true);

    auto mat_res = _impl->pipelines.CreateMaterial(desc);
    if (!mat_res) {
        return std::unexpected(mat_res.error());
    }
    Material mat  = mat_res.value();
    mat.albedoMap = TextureHandle::Invalid;
    return mat;
}

auto RenderContext::CreateMaterial(const MaterialDesc& desc) -> std::expected<Material, ErrorCode> {
    auto basicMat = CreateBasicMaterial(desc.doubleSided, desc.alphaBlend, desc.additiveBlend);
    if (!basicMat) {
        return std::unexpected(basicMat.error());
    }

    Material mat        = *basicMat;
    mat.alphaMode       = (desc.alphaMode != 0) ? desc.alphaMode : basicMat->alphaMode;
    mat.alphaCutoff     = desc.alphaCutoff;
    mat.metallicFactor  = desc.metallic;
    mat.roughnessFactor = desc.roughness;
    mat.albedoMap       = desc.albedoMap;
    mat.normalMap       = desc.normalMap;
    mat.pbrMap          = desc.pbrMap;
    mat.emissiveMap     = desc.emissiveMap;

    std::ranges::copy(desc.baseColor, mat.baseColorFactor);
    std::ranges::copy(desc.emissive, mat.emissiveFactor);

    return mat;
}

void RenderContext::DrawLine(JPH::Vec3Arg start, JPH::Vec3Arg end, JPH::Vec4Arg colorStart, JPH::Vec4Arg colorEnd) noexcept {
    _impl->queues.Lines().push_back({.start = start, .end = end, .colorStart = colorStart, .colorEnd = colorEnd});
}

void RenderContext::Impl::BeginShaderObservation() {
    if constexpr (isDev) {
        if (fileSystemWatcher != nullptr && shaderDirectoryWatch == 0) {
            shaderDirectoryWatch = fileSystemWatcher->WatchDirectory(
                "resources/shaders", [this](const FS::FileWatchEvent& event) { HandleShaderFileEvent(event); }, true, ".slang",
                FS::FileSystemWatcher::kDefaultDebounceMs
            );
        }
    }
}

void RenderContext::Impl::HandleShaderFileEvent(const FS::FileWatchEvent& event) {
    if constexpr (isDev) {
        // The registry owns the matching and the two hazards of iterating a
        // table that rebuilds can mutate; the device-idle wait is the part that
        // touches Vulkan, so it stays here and is handed in to run once, only
        // if something actually needs rebuilding.
        shaderReloads.Dispatch(event.path.lexically_normal().generic_string(), [this] { vkDeviceWaitIdle(ctx.Device()); });
    }
}

auto RenderContext::CreateTexture(const void* data, uint32_t width, uint32_t height, bool isSRGB) -> std::expected<uint32_t, ErrorCode> {
    return _impl->textureManager.Upload2D(data, width, height, Rgba8Format(isSRGB));
}

auto RenderContext::CreateTextureCube(const void* const* faceData, uint32_t width, uint32_t height) -> std::expected<uint32_t, ErrorCode> {
    return _impl->textureManager.UploadCube(faceData, width);
}

auto RenderContext::RegisterTexture(std::string_view name, uint32_t bindlessIndex, bool isSRGB) -> TextureHandle {
    return _impl->textureManager.RegisterUploaded(name, bindlessIndex, Rgba8Format(isSRGB));
}

void RenderContext::UnloadTexture(TextureHandle handle) {
    // The record goes away now, so later GetBindlessIndex calls resolve to the
    // white fallback; the slot itself is only recycled once the frames that
    // could still read its descriptor have retired.
    _impl->textureManager.Unload(handle);
}

namespace {

// The volumetric fog's tileable fBm, packed as 8-bit RGBA in the voxel order
// Vulkan's 3D images expect (x fastest, then y, then z). Pure CPU math: the
// bytes arrive at the uploader as a plain block.
[[nodiscard]] std::vector<uint8_t> Generate3DNoiseData(uint32_t size) {
    const size_t count = static_cast<size_t>(size) * size * size;
    std::vector<uint8_t> pixels(count * 4);
    for (uint32_t z = 0; z < size; ++z) {
        for (uint32_t y = 0; y < size; ++y) {
            for (uint32_t x = 0; x < size; ++x) {
                const float  n   = Math::TileableFbm3(
                    {static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F, static_cast<float>(z) + 0.5F},
                    static_cast<float>(size)
                );
                const auto   v   = static_cast<uint8_t>(std::clamp(n * 255.0F, 0.0F, 255.0F));
                const size_t idx = static_cast<size_t>((z * size + y) * size + x) * 4;
                pixels[idx + 0]  = v;
                pixels[idx + 1]  = v;
                pixels[idx + 2]  = v;
                pixels[idx + 3]  = 255;
            }
        }
    }
    return pixels;
}

} // namespace

auto RenderContext::Impl::InitializeVolumetricNoiseTexture() noexcept -> std::expected<void, ErrorCode> {
    constexpr uint32_t kVolumetricNoiseSize = 64;

    const std::vector<uint8_t> pixels = Generate3DNoiseData(kVolumetricNoiseSize);

    return Vk::TextureUploader(ctx, allocator, stagingRingBuffer, graphicsCmdRing)
        .Upload3D(
            {.data = pixels.data(), .width = kVolumetricNoiseSize, .height = kVolumetricNoiseSize, .depth = kVolumetricNoiseSize,
             .format = VK_FORMAT_R8G8B8A8_UNORM, .debugName = "Volumetric.Noise3D"}
        )
        .transform([&](Vk::TextureResource tex) -> void {
            volumetricNoiseImage    = std::move(tex.image);
            volumetricNoiseView     = std::move(tex.view);
            volumetricNoiseViewInfo = tex.viewInfo;
        });
}

auto RenderContext::Impl::InitializeBlueNoiseTexture() -> std::expected<void, ErrorCode> {
    // Single-mip on purpose. A noise texture must never be mipmapped: every
    // level averages neighbouring texels toward the mean, so the high-frequency
    // content the dither depends on is destroyed exactly where a filter would
    // reach for it. The shader always samples LOD 0.
    constexpr VkFormat kFormat = VK_FORMAT_R8G8B8A8_UNORM;

    // The tile arrives already decoded: configure/cook_blue_noise.py turns the
    // PNG into raw 8-bit RGBA at build time, so this is a memcpy of a block
    // whose layout the renderer asked for rather than an image decode. The
    // renderer cannot pull it through the VFS -- it is uploaded inside
    // RenderContext::Create, and the Kernel builds its AssetManager and mounts
    // data/base.pak only after that returns.
    //
    // Square by definition (it tiles), so the extent comes off the byte count
    // and the count is what validates the blob: anything that is not a whole
    // square of RGBA texels is a cook that disagrees with this reader.
    const size_t   bytes = Resource::blue_noise_rgba.size();
    const size_t   side  = static_cast<size_t>(std::sqrt(static_cast<double>(bytes / 4)));
    if (bytes % 4 != 0 || side * side * 4 != bytes || side == 0 || side > std::numeric_limits<uint32_t>::max()) [[unlikely]] {
        ZHLN::Log("[BlueNoise] Expected a whole square of 8-bit RGBA texels, got {} bytes.", bytes);
        return std::unexpected(ErrorCode {BlueNoiseError::UnexpectedLayout});
    }

    const uint32_t w = static_cast<uint32_t>(side);
    const uint32_t h = static_cast<uint32_t>(side);

    auto imageRes = Vk::ImageBuilder {}.Texture2D(w, h, kFormat, Vk::ImageUsage::TransferDst | Vk::ImageUsage::Sampled, 1).Build(allocator.Get());
    if (!imageRes) {
        return std::unexpected(imageRes.error());
    }

    auto staging = stagingRingBuffer.Allocate(bytes);
    if (staging.mappedData == nullptr) {
        return std::unexpected(Vk::StagingError::MemoryMappingFailed);
    }
    std::memcpy(staging.mappedData, Resource::blue_noise_rgba.data(), bytes);

    Vk::Image image = std::move(*imageRes);

    Vk::ExecuteImmediate(ctx, graphicsCmdRing, stagingRingBuffer, [&](VkCommandBuffer cmd) {
        Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(cmd, image.Handle());

        const VkBufferImageCopy2 region = {
            .sType             = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
            .pNext             = nullptr,
            .bufferOffset      = staging.offset,
            .bufferRowLength   = 0,
            .bufferImageHeight = 0,
            .imageSubresource  = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1},
            .imageOffset       = {0, 0, 0},
            .imageExtent       = {w, h, 1},
        };
        Vk::CopyBufferToImage<1>(cmd, staging.buffer, image.Handle(), {region});
        Vk::TransitionLayout<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, image.Handle());
    });

    auto viewRes = Vk::CreateView<kFormat>(ctx.Device(), image.Handle(), VK_IMAGE_ASPECT_COLOR_BIT, 1);
    if (!viewRes) {
        return std::unexpected(viewRes.error());
    }
    Vk::ImageView view = std::move(*viewRes);

    Vk::Debug::SetImageName(ctx, image.Handle(), "BlueNoise.LDR_RGBA_0");

    // NEAREST + REPEAT. Repeat lets the shader scroll the tile by adding an
    // offset in UV space and letting the sampler wrap, so there is no integer
    // mask in the hot path and the texture need not be power-of-two.
    auto samplerBuilder = Vk::SamplerBuilder {}.Nearest().Repeat().LodRange(0.0F, 0.0F);
    auto samplerRes     = samplerBuilder.Build(ctx.Device());
    if (!samplerRes) {
        return std::unexpected(samplerRes.error());
    }

    blueNoiseSampler     = std::move(*samplerRes);
    blueNoiseSamplerInfo = samplerBuilder.Info();
    blueNoiseWidth       = w;
    blueNoiseHeight      = h;
    blueNoiseViewInfo    = Vk::MakeViewCreateInfo2D(image.Handle(), kFormat, 1, VK_IMAGE_ASPECT_COLOR_BIT);

    auto blueNoiseIdx = textureManager.Adopt(std::move(image), std::move(view), kFormat, 1, false);
    if (!blueNoiseIdx) {
        return std::unexpected(blueNoiseIdx.error());
    }
    blueNoiseTexIdx = *blueNoiseIdx;

    ZHLN::Log("[BlueNoise] Blue noise tile bound as bindless texture {} ({}x{}, single mip).", blueNoiseTexIdx, w, h);
    return {};
}

// GPU buffer allocation lives in GeometryManager::CreateBuffer, which also
// makes the one usage-flag decision left: the ray-tracing build-input bit,
// gated on the device predicate ctx.RayTracingSupported().

auto RenderContext::CreateSkinnedScratchBuffer(uint32_t vertexCount) -> BufferHandle {
    return _impl->geometry.CreateSkinnedScratchBuffer(vertexCount);
}

void RenderContext::Impl::BuildOrUpdateSkinnedBLAS(VkCommandBuffer cmd, const DrawCommand& drawCmd, NativeMesh* scratchMesh) const {
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

    if (scratchMesh->blas == VK_NULL_HANDLE) {
        auto blasBufOpt = Vk::Buffer::Create(
            allocator.Get(), sizes.acceleration_structure_size,
            Vk::BufferUsage::AccelerationStructureStorage | Vk::BufferUsage::ShaderDeviceAddress, Vk::MemoryUsage::GPUOnly
        );
        if (!blasBufOpt) {
            return;
        }
        // The mesh's device stamp arrived at adoption; the destructor retires
        // this BLAS off it.
        scratchMesh->blasBuffer = std::move(*blasBufOpt);
        scratchMesh->blas =
            Vk::CreateAccelerationStructure(ctx.Device(), scratchMesh->blasBuffer.Handle(), sizes.acceleration_structure_size, ZHLN_AS_TYPE_BOTTOM_LEVEL);
        scratchMesh->blasAddress = Vk::GetAccelerationStructureAddress(ctx.Device(), scratchMesh->blas);
    }

    auto scratchBufOpt = Vk::Buffer::Create(
        allocator.Get(), sizes.build_scratch_size, Vk::BufferUsage::Storage | Vk::BufferUsage::ShaderDeviceAddress, Vk::MemoryUsage::GPUOnly
    );
    if (!scratchBufOpt) {
        return;
    }
    Vk::Buffer      scratchBuf     = std::move(*scratchBufOpt);
    VkDeviceAddress scratchAddress = ctx.BufferAddress(scratchBuf.Handle());

    // Record the build command directly onto the active graphics queue command buffer
    Vk::BuildBLAS(cmd, geom, scratchMesh->blas, scratchAddress, primitiveCount);
}

void RenderContext::UploadDebugVertices(const void* posData, size_t posSize, const void* attrData, size_t attrSize, uint32_t vertexCount) noexcept {
    auto* nativeMesh = _impl->geometry.Resolve(_impl->frames.debugMeshHandles[_impl->presenter.frameIndex]).value_or(nullptr);
    if (nativeMesh == nullptr) {
        return;
    }

    size_t maxPosSize  = RenderContext::Impl::kMaxDebugVertices * sizeof(VertexPosition);
    size_t maxAttrSize = RenderContext::Impl::kMaxDebugVertices * sizeof(VertexAttributes);

    auto  mapped  = nativeMesh->buffer.Map();
    char* basePtr = static_cast<char*>(mapped.data);

    std::memcpy(basePtr, posData, std::min(posSize, maxPosSize));
    std::memcpy(basePtr + maxPosSize, attrData, std::min(attrSize, maxAttrSize));

    nativeMesh->vertexCount = std::min(vertexCount, RenderContext::Impl::kMaxDebugVertices);
}

auto RenderContext::GetDebugMeshBuffer() const noexcept -> BufferHandle {
    return _impl->frames.debugMeshHandles[_impl->presenter.frameIndex];
}

void RenderContext::UpdateJointMatrices(uint32_t offset, const JPH::Mat44* matrices, uint32_t count) {
    if (count == 0) {
        return;
    }
    auto  mappedRegion = _impl->frames.jointBuffers[_impl->presenter.frameIndex].Map();
    auto* gpuJoints    = std::bit_cast<JPH::Mat44*>(mappedRegion.data);

    std::memcpy(gpuJoints + offset, matrices, count * sizeof(JPH::Mat44));
}

auto RenderContext::AllocateMorphDeltas(uint32_t count, const float* deltas) -> uint32_t {
    uint32_t offset = _impl->nextMorphDeltaIndex;

    auto   mappedRegion = _impl->morphDeltasBuffer.Map();
    float* gpuDeltas    = std::bit_cast<float*>(mappedRegion.data) + (static_cast<size_t>(offset * 4));

    std::memcpy(gpuDeltas, deltas, count * sizeof(float) * 4);

    _impl->nextMorphDeltaIndex += count;
    return offset;
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

// Resizes the GPU cascade shadow targets. On success the canonical settings'
// shadows.resolution is updated by the caller (ApplySettings / the public
// SetShadowResolution bridge).
// Shadow-target reallocation moved to TargetManager::ResizeShadows, which owns
// the cascade map pair and the views carved out of it.

auto RenderContext::SetShadowResolution(uint32_t resolution) -> std::expected<void, ErrorCode> {
    auto* impl = _impl.get();

    return impl->targets.ResizeShadows(resolution).transform([&]() -> void {
        impl->settings.shadows.resolution = resolution;
        // Keep the informational preset tier honest after an out-of-band change.
        impl->settings.qualityPreset = impl->settings.DetectPreset();
    });
}

void RenderContext::Impl::ApplySettings(GraphicsSettings&& incoming) noexcept {
    // --- Delta detection
    // Reactive consequences key off specific fields; plain knob changes
    // simply become part of the canonical state consumed by the next frame.
    const QualityLevel previousTier = settings.qualityPreset;

    if (incoming.shadows.resolution != settings.shadows.resolution) {
        if (targets.ResizeShadows(incoming.shadows.resolution)) {
            settings.shadows.resolution = incoming.shadows.resolution;
        } else {
            // Keep the GPU-consistent resolution so uniforms and samplers
            // match the actual targets; the rejected value is dropped.
            ZHLN::Log(
                "WARN: failed to resize shadow targets to {}x{}; keeping {}x{}", incoming.shadows.resolution, incoming.shadows.resolution,
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
    auto* impl = _impl.get();

    struct BuildContext {
        NativeMesh*                     posMesh;
        NativeMesh*                     indexMesh;
        ZHLN_BlasGeometryDesc           geom;
        uint32_t                        primitiveCount;
        ZHLN_AccelerationStructureSizes sizes;
        Vk::Buffer                      blasBuffer;
        VkAccelerationStructureKHR      blas;
        Vk::Buffer                      scratch;
    };

    return std::expected<void, ErrorCode>()
        .and_then([&]() -> std::expected<BuildContext, ErrorCode> {
            if (!impl->ctx.RayTracingSupported()) {
                return std::unexpected(RenderFeatureError::FeatureNotSupported);
            }
            return impl->geometry.Resolve(mesh.posBuffer)
                .transform_error([](auto err) -> ErrorCode { return err; })
                .and_then([&](auto* pos) -> std::expected<BuildContext, ErrorCode> {
                    auto* index = (mesh.indexBuffer != BufferHandle::Invalid) ? impl->geometry.Resolve(mesh.indexBuffer).value_or(nullptr) : nullptr;
                    return BuildContext {
                        .posMesh = pos, .indexMesh = index, .geom = {}, .primitiveCount = {}, .sizes = {}, .blasBuffer = {}, .blas = nullptr, .scratch = {}
                    };
                });
        })
        .and_then([&](BuildContext b) -> std::expected<BuildContext, ErrorCode> {
            b.geom = {
                .vertex_data   = b.posMesh->vboAddress,
                .vertex_stride = sizeof(VertexPosition),
                .max_vertex    = mesh.vertexCount > 0 ? mesh.vertexCount - 1 : 0,
                .vertex_format = VK_FORMAT_R32G32B32_SFLOAT,
                .index_data    = (b.indexMesh != nullptr) ? b.indexMesh->vboAddress : 0,
                .index_type    = (b.indexMesh != nullptr) ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_NONE_KHR
            };
            b.primitiveCount = (b.indexMesh != nullptr) ? mesh.indexCount / 3 : mesh.vertexCount / 3;

            Vk::GetBLASSizes(impl->ctx.Device(), b.geom, b.primitiveCount, b.sizes);

            return Vk::Buffer::Create(
                       impl->allocator.Get(), b.sizes.acceleration_structure_size,
                       Vk::BufferUsage::AccelerationStructureStorage | Vk::BufferUsage::ShaderDeviceAddress, Vk::MemoryUsage::GPUOnly
            )
                .transform([b = std::move(b)](auto&& buffer) mutable -> auto {
                    b.blasBuffer = std::forward<decltype(buffer)>(buffer);
                    return std::move(b);
                });
        })
        .and_then([&](BuildContext b) -> std::expected<BuildContext, ErrorCode> {
            b.blas = Vk::CreateAccelerationStructure(impl->ctx.Device(), b.blasBuffer.Handle(), b.sizes.acceleration_structure_size, ZHLN_AS_TYPE_BOTTOM_LEVEL);
            if (b.blas == VK_NULL_HANDLE) {
                return std::unexpected(Vk::VulkanCallError::VulkanCallFailed);
            }

            return Vk::Buffer::Create(
                       impl->allocator.Get(), b.sizes.build_scratch_size, Vk::BufferUsage::Storage | Vk::BufferUsage::ShaderDeviceAddress,
                       Vk::MemoryUsage::GPUOnly
            )
                .transform([b = std::move(b)](auto&& buffer) mutable -> auto {
                    b.scratch = std::forward<decltype(buffer)>(buffer);
                    return std::move(b);
                });
        })
        .and_then([&](BuildContext b) -> std::expected<void, ErrorCode> {
            Vk::CommandPool<Vk::QueueType::Graphics> tempPool(impl->ctx.Device(), impl->ctx.PhysicalInfo().graphics_family);
            auto                                     alloc_res = tempPool.Allocate(1);
            if (!alloc_res) [[unlikely]] {
                return std::unexpected(alloc_res.error());
            }

            VkCommandBuffer tempCmd = tempPool[0];
            {
                Vk::CommandBufferGuard guard(tempCmd);

                Vk::MemoryBarrier(
                    tempCmd, Vk::BarrierStage::Copy, Vk::BarrierAccess::TransferWrite, Vk::BarrierStage::AccelerationStructureBuild,
                    Vk::BarrierAccess::AccelerationStructureRead
                );
                Vk::BuildBLAS(tempCmd, b.geom, b.blas, Vk::GetBufferAddress(impl->ctx.Device(), b.scratch.Handle()), b.primitiveCount);
            }

            return Vk::SubmitAndWait(
                       impl->ctx.GraphicsQueue(), tempCmd, impl->transferRingBuffer.GetSemaphore(), impl->transferRingBuffer.GetCurrentValue(),
                       VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR
            )
                .transform_error([](auto err) -> ErrorCode { return err; })
                .transform([&]() -> void {
                    // The mesh's device stamp arrived at adoption; ~NativeMesh
                    // retires the BLAS off it.
                    b.posMesh->blasBuffer  = std::move(b.blasBuffer);
                    b.posMesh->blas        = b.blas;
                    b.posMesh->blasAddress = Vk::GetAccelerationStructureAddress(impl->ctx.Device(), b.blas);
                });
        });
}


auto RenderContext::BakeProceduralTexture(uint32_t width, uint32_t height, uint32_t variantIdx, float scale, float randomness)
    -> std::expected<uint32_t, ErrorCode> {
    return _impl->BakeProceduralTexture(width, height, variantIdx, scale, randomness, 0.0f);
}

auto RenderContext::CreateProceduralTexture(std::string_view name, uint32_t width, uint32_t height, bool isSRGB, const uint32_t* pixels) -> TextureHandle {
    // Pixels arrive already generated: the renderer uploads them and names the
    // slot, and the caller stays the owner of the source.
    const auto uploaded = _impl->textureManager.Upload(name, pixels, width, height, Rgba8Format(isSRGB));
    if (!uploaded) {
        ZHLN::Log("[RenderContext] Procedural texture '{}' ({}x{}) failed to upload: {}", name, width, height, uploaded.error());
        return TextureHandle::Invalid;
    }
    return *uploaded;
}

enum class ScreenshotError : uint8_t {
    FileOpenFailed ZHLN_ANNOTATION(ZHLN::Description<"Failed to open screenshot output file for writing"> {}) = 1,
    ReadbackFailed ZHLN_ANNOTATION(ZHLN::Description<"GPU readback buffer mapping failed"> {}),
    DestinationNotRecorded
        ZHLN_ANNOTATION(ZHLN::Description<"The frame's destination was never drawn into; the image holds the background fill, not a frame"> {}),
};

auto RenderContext::CaptureScreenshotPPM(std::string_view outputPath) noexcept -> std::expected<void, ErrorCode> {
    auto* const impl = _impl.get();

    if (!impl->presenter.swapchain.Valid()) {
        // Capture the frame, not "whatever the primary presenter's offscreen
        // target happens to be". Those are the same image until a destination
        // rebuild, and different ones after: the frame writes the record it
        // vended, and copying the other image reads a target nothing has drawn
        // into since it was created -- a black capture with no other symptom.
        VkImage       source       = impl->presenter.headlessColorTarget.image.Handle();
        VkExtent2D    extent       = impl->presenter.headlessColorTarget.extent;
        VkImageLayout sourceLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        if (auto* dest = impl->destinations.Find(impl->presentationTarget); dest != nullptr && dest->imageIndex < dest->recordHandles.size()) {
            const DestinationRegistry::Handle handle = dest->recordHandles[dest->imageIndex];
            if (handle.Valid() && handle.Index() < impl->destinations.Records().size()) {
                const DestinationRegistry::Record& record = impl->destinations.Records()[handle.Index()];

                // What the frame put in this image, in the frame vocabulary:
                // gone, nothing yet, or written. What a capture must not do is
                // read an image whose contents nothing established, and the
                // fallback fill -- defined pixels, no frame -- is not something
                // to hand back as one either, so both are refused by name.
                const auto receipt = record.GetRenderedContent();
                if (!receipt) {
                    ZHLN::Log("[Test Capture] Destination 0x{:016X} has no image to capture: {}; capture refused.", record.handle.Raw(), receipt.error());
                    return std::unexpected(ScreenshotError::DestinationNotRecorded);
                }
                if (!receipt->has_value()) {
                    ZHLN::Log(
                        "[Test Capture] Destination 0x{:016X} was not written this frame (its contents are undefined); capture refused.",
                        record.handle.Raw()
                    );
                    return std::unexpected(ScreenshotError::DestinationNotRecorded);
                }
                if (!(*receipt)->Drawn()) {
                    // EndFrame fills a vended-but-unwritten destination with the
                    // background colour. Reading it back hands the caller a
                    // black frame that no lighting metric can tell from "no
                    // light reached the scene", so refuse the capture instead
                    // and name the actual cause.
                    ZHLN::Log(
                        "[Test Capture] Destination 0x{:016X} was never drawn into this frame (filled with the background colour); capture refused.",
                        record.handle.Raw()
                    );
                    return std::unexpected(ScreenshotError::DestinationNotRecorded);
                }

                // A pass drew it: the image is the frame's, and the receipt
                // having refused every case where it is not is why this needs
                // no validity check of its own.
                if (record.image.handle != source) {
                    ZHLN::Log(
                        "[Test Capture] Frame destination 0x{:016X} is not the presentation's offscreen target 0x{:016X}; capturing the destination.",
                        reinterpret_cast<uint64_t>(record.image.handle), reinterpret_cast<uint64_t>(source)
                    );
                }
                source = record.image.handle;
                extent = record.image.Extent2D();
                // The frame's own bookkeeping, not a guessed layout: a barrier
                // whose oldLayout lies about the contents is allowed to discard
                // them, and saying "colour attachment" about an image nothing
                // wrote is exactly such a lie.
                sourceLayout = Vk::ToVkImageLayout(record.trackedLayout);
            }
        }

        const auto imageBytes = static_cast<size_t>(extent.width) * extent.height * 4u;

        auto stagingRes = Vk::Buffer::Create(impl->allocator.Get(), imageBytes, Vk::BufferUsage::TransferDst, Vk::MemoryUsage::GPUToCPU);
        if (!stagingRes) {
            return std::unexpected(stagingRes.error());
        }
        auto stagingBuffer = std::move(*stagingRes);

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

        auto mapped = stagingBuffer.Map();
        if (mapped.data == nullptr) {
            return std::unexpected(ScreenshotError::ReadbackFailed);
        }

        std::ofstream ofs(std::string(outputPath), std::ios::binary);
        if (!ofs.is_open()) {
            return std::unexpected(ScreenshotError::FileOpenFailed);
        }

        ofs << "P6\n" << extent.width << " " << extent.height << "\n255\n";

        const auto*  rgba   = mapped.As<const uint8_t>();
        const size_t pixels = static_cast<size_t>(extent.width) * extent.height;
        uint64_t     lumaSum = 0;
        uint64_t     lit     = 0;
        // Per-channel detail, because a frame's luma alone cannot tell "no
        // light reached the scene" from "one hue never survived shading": the
        // suite's chroma gates classify pixels by channel ratios above an
        // 8-bit floor of 45, so the floor count and the channel maxima are the
        // numbers that say which of the two happened.
        std::array<uint64_t, 3> channelSum {};
        std::array<uint64_t, 3> aboveFloor {};
        std::array<uint8_t, 3>  channelMax {};
        for (size_t i = 0; i < pixels; ++i) {
            const uint8_t r = rgba[i * 4 + 0];
            const uint8_t g = rgba[i * 4 + 1];
            const uint8_t b = rgba[i * 4 + 2];
            ofs.put(static_cast<char>(r));
            ofs.put(static_cast<char>(g));
            ofs.put(static_cast<char>(b));

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
        ofs.close();

        // Say what the capture holds, not only where it went. A capture that
        // read the wrong image and a capture of a frame nothing drew into are
        // the same "black frame" downstream, and the readback is the only place
        // where the difference is still visible.
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

    // 1. Allocate host-visible readback buffer via engine Allocator
    auto stagingRes = Vk::Buffer::Create(impl->allocator.Get(), imageBytes, Vk::BufferUsage::TransferDst, Vk::MemoryUsage::GPUToCPU);
    if (!stagingRes) {
        return std::unexpected(stagingRes.error());
    }
    auto stagingBuffer = std::move(*stagingRes);

    // 2. Record and submit transfer from internal hdrSceneColor
    Vk::ExecuteImmediate(impl->ctx, impl->graphicsCmdRing, [&](VkCommandBuffer cmd) -> void {
        auto* const targetImg = impl->graphResources.hdrSceneColor.image.Handle();

        Vk::TransitionLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL>(cmd, targetImg);
        Vk::CopyImageToBuffer(cmd, targetImg, stagingBuffer.Handle(), extent);
        Vk::TransitionLayout<VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, targetImg);
    });

    // 3. Map memory with typed pointer accessor
    auto mapped = stagingBuffer.Map();
    if (mapped.data == nullptr) {
        return std::unexpected(ScreenshotError::ReadbackFailed);
    }
    const auto* const halfFloats = mapped.As<const uint16_t>();

    // 4. Output image file
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

} // namespace ZHLN
