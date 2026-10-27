// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "GpuPack.hpp"
#include "RenderInternal.hpp"
#include "Resources.hpp"
#include "Zahlen/Core/AssetID.hpp"
#include "Zahlen/Geometry2D.hpp"
#include "Zahlen/GraphicsSettings.hpp"
#include "Zahlen/Render/Handles.hpp"
#include "Zahlen/Render/Types.hpp"
#include "Zahlen/Vertex.hpp"
#include "passes/forward/ForwardPass.hpp"
#include "passes/gbuffer/GBufferBasePass.hpp"
#include <PipelineFormats.hpp>
#include <ShaderBindings.hpp>
#include <Zahlen/Core/Math.hpp>
#include <Zahlen/Core/Ranges.hpp>
#include <Zahlen/Core/Reflection/Annotations.hpp>
#include <Zahlen/Core/Reflection/Class.hpp>
#include <Zahlen/Math3D.hpp>
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

} // namespace ZHLN

namespace ZHLN {

namespace {
inline constexpr uint64_t kEmitterStorageIdleFrameLimit = 120;
}

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

void RenderContext::RegisterGPUMesh(AssetID id, Mesh mesh) noexcept {
    _impl->geometry.RegisterMesh(id, mesh);
}

void RenderContext::UnregisterGPUMesh(AssetID id) noexcept {
    _impl->geometry.UnregisterMesh(id);
}

void RenderContext::DestroyMesh(const Mesh& mesh) noexcept {
    for (const BufferHandle handle:
         {mesh.posBuffer, mesh.tangentFrameBuffer, mesh.surfaceBuffer, mesh.skinBuffer, mesh.indexBuffer, mesh.meshletBuffer, mesh.meshletVertexBuffer,
          mesh.meshletTriBuffer}) {
        DestroyBuffer(handle);
    }
}

void RenderContext::RegisterGPUMaterial(MaterialID id, Material mat) noexcept {
    _impl->geometry.RegisterMaterial(id, mat);
}

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

auto RenderContext::Impl::EnsureEmitterStorage(EmitterStorageMap& emitters, const uint64_t emitterId, const uint32_t maxParticles, const size_t particleStride)
    -> BufferHandle {
    if (maxParticles == 0) {
        if (auto storage = emitters.Find(emitterId)) {
            if (storage->buffer != BufferHandle::Invalid) {
                geometry.Destroy(storage->buffer);
            }
            emitters.Erase(emitterId);
        }
        return BufferHandle::Invalid;
    }
    if (particleStride == 0 || static_cast<size_t>(maxParticles) > std::numeric_limits<size_t>::max() / particleStride) {
        ZHLN::LogWarning("[Render] Emitter {} requested an overflowing particle buffer (count={}, stride={}).", emitterId, maxParticles, particleStride);
        return BufferHandle::Invalid;
    }

    if (!emitters.Find(emitterId)) {
        emitters.Insert(emitterId, EmitterStorage {});
    }
    auto storage = emitters.Find(emitterId);
    if (!storage) {
        return BufferHandle::Invalid;
    }

    storage->lastSeenFrame = frameSerial;
    if (storage->buffer != BufferHandle::Invalid && storage->capacity == maxParticles) {
        storage->allocationWarningLogged = false;
        return storage->buffer;
    }

    const size_t allocationSize = static_cast<size_t>(maxParticles) * particleStride;
    auto         created        = geometry.CreateBuffer({.allocationSize = allocationSize}, Vk::BufferUsage::Storage | Vk::BufferUsage::Vertex);
    if (!created || *created == BufferHandle::Invalid) {
        if (!storage->allocationWarningLogged) {
            if (!created) {
                ZHLN::LogWarning("[Render] Could not allocate particle storage for emitter {} ({} particles): {}", emitterId, maxParticles, created.error());
            } else {
                ZHLN::LogWarning(
                    "[Render] Could not allocate particle storage for emitter {} ({} particles): geometry buffer pool is full.", emitterId, maxParticles
                );
            }
            storage->allocationWarningLogged = true;
        }
        return BufferHandle::Invalid;
    }

    if (storage->buffer != BufferHandle::Invalid) {
        geometry.Destroy(storage->buffer);
    }
    storage->buffer                  = *created;
    storage->capacity                = maxParticles;
    storage->allocationWarningLogged = false;
    return storage->buffer;
}

void RenderContext::Impl::EvictInactiveEmitters() noexcept {
    const auto evict = [this](EmitterStorageMap& emitters) {
        std::vector<uint64_t> idleEmitters;
        emitters.ForEach([this, &idleEmitters](const uint64_t& emitterId, const EmitterStorage& storage) {
            if (frameSerial >= storage.lastSeenFrame && frameSerial - storage.lastSeenFrame >= kEmitterStorageIdleFrameLimit) {
                idleEmitters.push_back(emitterId);
            }
        });

        for (const uint64_t emitterId: idleEmitters) {
            if (auto storage = emitters.Find(emitterId)) {
                if (storage->buffer != BufferHandle::Invalid) {
                    geometry.Destroy(storage->buffer);
                }
                emitters.Erase(emitterId);
            }
        }
    };
    evict(particleEmitters);
    evict(meshParticleEmitters);
}

void RenderContext::Impl::ClearEmitterBuffers() noexcept {
    const auto destroy = [this](EmitterStorageMap& emitters) {
        emitters.ForEach([this](const uint64_t&, EmitterStorage& storage) {
            if (storage.buffer != BufferHandle::Invalid) {
                geometry.Destroy(storage.buffer);
            }
        });
        emitters.Clear();
    };
    destroy(particleEmitters);
    destroy(meshParticleEmitters);
    for (auto& slot: billboardSlots) {
        for (uint32_t f = 0; f < Vk::kFramesInFlight; ++f) {
            if (slot.buffers[f] != BufferHandle::Invalid) {
                geometry.Destroy(slot.buffers[f]);
            }
            slot.buffers[f]    = BufferHandle::Invalid;
            slot.capacities[f] = 0;
        }
    }
}

auto RenderContext::CreateBuffer(const BufferDesc& desc) -> BufferHandle {
    if (desc.usage == BufferUsage::SkinScratch) {
        const uint32_t vertexCount = desc.stride == 0 ? 0U : static_cast<uint32_t>(desc.size / desc.stride);
        return _impl->geometry.CreateSkinnedScratchBuffer(vertexCount);
    }

    Vk::BufferUsage vkUsage = Vk::BufferUsage::None;
    switch (desc.usage) {
        case BufferUsage::Vertex:
            vkUsage = Vk::BufferUsage::Vertex;
            break;
        case BufferUsage::Index:
            vkUsage = Vk::BufferUsage::Index;
            break;
        case BufferUsage::Storage:
            vkUsage = desc.data.empty() ? (Vk::BufferUsage::Storage | Vk::BufferUsage::Vertex) : Vk::BufferUsage::Storage;
            break;
        case BufferUsage::Uniform:
            vkUsage = Vk::BufferUsage::Uniform;
            break;
        case BufferUsage::SkinScratch:
            break;
    }

    GeometryManager::BufferSource source {
        .bytes          = desc.data,
        .allocationSize = desc.data.empty() ? desc.size : 0,
        .stride         = desc.stride,
    };
    return _impl->geometry.CreateBuffer(source, vkUsage).value_or(BufferHandle::Invalid);
}

auto RenderContext::CreateMeshletBuffer(std::span<const MeshletDesc> meshlets) -> BufferHandle {
    if (meshlets.empty()) {
        return BufferHandle::Invalid;
    }
    std::vector<GPUMeshlet> packed(meshlets.size());
    for (size_t i = 0; i < meshlets.size(); ++i) {
        packed[i] = GpuPack::PackMeshlet(meshlets[i]);
    }
    return CreateBuffer<BufferUsage::Storage>(std::span<const GPUMeshlet> {packed});
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

void RenderContext::SubmitParticleEmitter(
    const uint64_t             emitterId,
    const uint32_t             maxParticles,
    const ParticleEmitterDesc& desc,
    const TextureHandle        texture,
    const bool                 additive
) {
    const BufferHandle gpuBuffer = _impl->EnsureEmitterStorage(_impl->particleEmitters, emitterId, maxParticles, sizeof(Particle));
    if (gpuBuffer == BufferHandle::Invalid) {
        return;
    }

    const uint32_t textureIndex = _impl->textureManager.GetBindlessIndex(texture);
    _impl->queues.ParticleEmitters().push_back(
        {.gpuBuffer = gpuBuffer, .maxParticles = maxParticles, .params = GpuPack::PackParticleEmitter(desc, textureIndex, additive ? 1u : 0u)}
    );
}

void RenderContext::SubmitMeshParticleEmitter(
    const uint64_t                 emitterId,
    const uint32_t                 maxParticles,
    const MeshParticleEmitterDesc& desc,
    const AssetID                  mesh,
    const MaterialID               mat
) {
    const BufferHandle gpuBuffer = _impl->EnsureEmitterStorage(_impl->meshParticleEmitters, emitterId, maxParticles, sizeof(Particle3D));
    if (gpuBuffer == BufferHandle::Invalid) {
        return;
    }

    _impl->queues.MeshParticleEmitters().push_back(
        {.gpuBuffer = gpuBuffer, .maxParticles = maxParticles, .params = GpuPack::PackMeshParticleEmitter(desc), .meshAsset = mesh, .materialAsset = mat}
    );
}

void RenderContext::DrawBillboards(TextureHandle texture, std::span<const BillboardQuad> billboards, bool additive) {
    if (billboards.empty()) {
        return;
    }

    const uint32_t serial = _impl->billboardSerial++;

    const uint32_t textureIndex = _impl->textureManager.GetBindlessIndex(texture);
    const uint32_t blendMode    = additive ? 1u : 0u;

    for (uint32_t alignment = 0; alignment < 3; ++alignment) {
        _impl->billboardStaging.clear();
        for (const BillboardQuad& quad: billboards) {
            if (static_cast<uint32_t>(quad.facing) != alignment) {
                continue;
            }
            Particle packed {};
            JPH::Vec4(quad.position, 1.0f).StoreFloat4(&packed.position);
            JPH::Vec4(quad.facing == ParticleAlignment::VelocityStretched ? quad.velocity : JPH::Vec3::sZero(), 0.0f).StoreFloat4(&packed.velocity);
            quad.color.StoreFloat4(&packed.color);

            JPH::Vec4(0.0f, 0.0f, quad.size, quad.rotation).StoreFloat4(&packed.params);
            _impl->billboardStaging.push_back(packed);
        }

        const auto count = static_cast<uint32_t>(_impl->billboardStaging.size());
        if (count == 0) {
            continue;
        }

        Impl::BillboardSlot* group = nullptr;
        for (auto& candidate: _impl->billboardSlots) {
            if (candidate.textureIndex == textureIndex && candidate.blendMode == blendMode && candidate.alignment == alignment && candidate.serial == serial) {
                group = &candidate;
                break;
            }
        }
        if (group == nullptr) {
            _impl->billboardSlots.push_back({.textureIndex = textureIndex, .blendMode = blendMode, .alignment = alignment, .serial = serial});
            group = &_impl->billboardSlots.back();
        }

        const uint32_t frameSlot = Vk::FrameSlot(_impl->presenter.frameIndex);
        BufferHandle&  slotBuf   = group->buffers[frameSlot];
        uint32_t&      slotCap   = group->capacities[frameSlot];

        if (slotCap < count) {
            if (slotBuf != BufferHandle::Invalid) {
                DestroyBuffer(slotBuf);
            }
            const auto created = _impl->geometry.CreateUninitializedBuffer(
                static_cast<size_t>(count) * sizeof(Particle), Vk::BufferUsage::Storage | Vk::BufferUsage::Vertex, count
            );
            slotBuf = created.value_or(BufferHandle::Invalid);
            slotCap = slotBuf != BufferHandle::Invalid ? count : 0;
        }
        if (slotBuf == BufferHandle::Invalid) {
            continue;
        }

        if (auto updated = UpdateBuffer(slotBuf, std::as_bytes(std::span {_impl->billboardStaging})); !updated) [[unlikely]] {
            ZHLN::Log("Particle buffer update failed.");
            continue;
        }
        _impl->queues.ParticleEmitters().push_back(
            {.gpuBuffer    = slotBuf,
             .maxParticles = count,
             .params       = {.textureIndex = textureIndex, .alignment = static_cast<Slang::ParticleAlignment>(alignment), .blendMode = blendMode},
             .simulate     = false}
        );
    }
}

void RenderContext::ClearGPUCaches() noexcept {
    if (_impl->ctx.Device() != VK_NULL_HANDLE) {
        auto res = Vk::WaitIdle(_impl->ctx.Device());
        if (!res) {
            ZHLN::Log("GPU cache clear aborted due to reason: {}", res.error());
            return;
        }
    }

    _impl->geometry.DiscardPendingUpdates();
    _impl->geometry.ClearMeshes();

    _impl->pipelines.RetireAll();
    _impl->geometry.ClearMaterials();

    for (auto& entry: _impl->renderTextures) {
        _impl->textureManager.Unload(entry.first);
        _impl->deletionQueue.EnqueueAttachmentView(_impl->ctx.Device(), std::move(entry.second.attachmentView));
    }
    _impl->renderTextures.clear();
    _impl->textureManager.Clear();
    _impl->textureManager.RetireAll();

    _impl->deletionQueue.Drain();
}

uint32_t RenderContext::GetValidationErrorCount() const noexcept {
    return _impl->ctx.Instance().ValidationErrorCount();
}

uint32_t RenderContext::GetDeviceLostCount() const noexcept {
    return _impl->ctx.Instance().DeviceLostCount();
}

void RenderContext::WriteCheckpoint(std::string_view name) noexcept {
    if (auto* const cmd = _impl->FrameCommand(); cmd != VK_NULL_HANDLE) {
        _impl->diagnostics.gpuDiagnostics.WriteCheckpoint(cmd, name);
    }
}

PipelineStatsCapture RenderContext::CapturePipelineStats() noexcept {
    if (!_impl->diagnostics.gpuProfiler.PipelineStatsAvailable()) {
        return {};
    }
    _impl->diagnostics.pendingPipelineCounters = {};
    _impl->diagnostics.gpuProfiler.SetPipelineStatsEnabled(true);
    return PipelineStatsCapture {_impl.get()};
}

PipelineStatsCapture::PipelineStatsCapture(PipelineStatsCapture&& other) noexcept: _impl(std::exchange(other._impl, nullptr)) {
}

auto PipelineStatsCapture::operator=(PipelineStatsCapture&& other) noexcept -> PipelineStatsCapture& {
    if (this != &other) {
        if (_impl != nullptr) {
            _impl->diagnostics.gpuProfiler.SetPipelineStatsEnabled(false);
        }
        _impl = std::exchange(other._impl, nullptr);
    }
    return *this;
}

PipelineStatsCapture::~PipelineStatsCapture() noexcept {
    if (_impl != nullptr) {
        _impl->diagnostics.gpuProfiler.SetPipelineStatsEnabled(false);
    }
}

GpuPipelineCounters PipelineStatsCapture::Consume() noexcept {
    if (_impl == nullptr) {
        return {};
    }
    return std::exchange(_impl->diagnostics.pendingPipelineCounters, GpuPipelineCounters {});
}

auto RenderContext::GetInfo() const noexcept -> RenderInfo {
    const auto&        props      = _impl->ctx.PhysicalInfo().properties.properties;
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

void RenderContext::DestroyBuffer(BufferHandle handle) {
    _impl->geometry.Destroy(handle);
}

auto RenderContext::UpdateBuffer(BufferHandle handle, std::span<const std::byte> bytes) noexcept -> std::expected<void, ErrorCode> {
    return _impl->geometry.Update(handle, bytes.data(), bytes.size());
}

static_assert(ActiveGBuffer::count == 4, "The compact deferred path owns SceneColor, Velocity, NormRough and the BakedMask ownership field only.");
static_assert(ActiveGBuffer::array[3] == VK_FORMAT_R8_UINT, "The baked-light ownership field is one R8 lane of bits, not a weight.");
static_assert(Vk::PassAttachmentFormats<Passes::GBufferBasePass>::depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT);
static_assert(Vk::PassAttachmentFormats<Passes::ForwardPass>::color_count == 1);
static_assert(Vk::PassAttachmentFormats<Passes::ForwardPass>::color_formats[0] == VK_FORMAT_R16G16B16A16_SFLOAT);
static_assert(Vk::PassAttachmentFormats<Passes::ForwardPass>::depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT);

auto RenderContext::CreateMaterial(const MaterialDesc& desc) -> std::expected<Material, ErrorCode> {
    const bool transmission  = !desc.unlit && desc.transmissionFactor > 0.0f;
    const bool hasClearcoat  = !desc.unlit && (desc.clearcoatFactor > 1.0f / 255.0f || desc.clearcoatMap != TextureHandle::Invalid ||
                                               desc.clearcoatRoughnessMap != TextureHandle::Invalid || desc.clearcoatNormalMap != TextureHandle::Invalid);
    const bool hasAnisotropy = !desc.unlit && (desc.anisotropyStrength > 1.0f / 255.0f || desc.anisotropyMap != TextureHandle::Invalid);
    const bool hasSheen      = !desc.unlit &&
                               (desc.sheenColorFactor.x > 1.0f / 255.0f || desc.sheenColorFactor.y > 1.0f / 255.0f || desc.sheenColorFactor.z > 1.0f / 255.0f ||
                                desc.sheenColorMap != TextureHandle::Invalid || desc.sheenRoughnessMap != TextureHandle::Invalid);
    // These paths previously serialized their data through the discarded
    // attachments. Keep their full material evaluation in ForwardPass instead.
    // Emission is deliberately not a forward-only feature: lit emitters retain
    // the deferred normal/AO/velocity path and get their radiance composited
    // additively into LightingGI before the reflection pass.
    const bool forwardOnly = desc.unlit || hasClearcoat || hasAnisotropy || hasSheen || desc.specularEnabled;
    const bool forward     = desc.alphaBlend || desc.additiveBlend || desc.alphaMode == 2 || transmission || forwardOnly;
    const bool alphaBlend  = !desc.additiveBlend && (desc.alphaBlend || desc.alphaMode == 2 || transmission);
    const bool depthWrite  = transmission || (forwardOnly && !alphaBlend && !desc.additiveBlend);

    Vk::MaterialFlags flags = Vk::MaterialFlags::None;
    if (desc.doubleSided) {
        flags |= Vk::MaterialFlags::DoubleSided;
    }
    if (desc.additiveBlend) {
        flags |= Vk::MaterialFlags::AdditiveBlend;
    } else if (alphaBlend) {
        flags |= Vk::MaterialFlags::TranslucentBlend;
    }
    if (depthWrite) {
        flags |= Vk::MaterialFlags::DepthWrite;
    }

    std::expected<Material, ErrorCode> mat_res =
        forward ?
            _impl->pipelines.CreateMaterial<
                MaterialPipelineFamily::Forward, Vk::GraphicsShaderModules<Shaders::Modules::BasicVSForward, Shaders::Modules::ForwardPS>,
                Vk::GraphicsShaderModules<Shaders::Modules::BasicTask, Shaders::Modules::BasicMeshForward, Shaders::Modules::ForwardPS>, Passes::ForwardPass>(
                flags
            ) :
            _impl->pipelines.CreateMaterial<
                MaterialPipelineFamily::Deferred, Vk::GraphicsShaderModules<Shaders::Modules::BasicVS, Shaders::Modules::BasicPS>,
                Vk::GraphicsShaderModules<Shaders::Modules::BasicTask, Shaders::Modules::BasicMesh, Shaders::Modules::BasicPS>, Passes::GBufferBasePass>(flags);
    if (!mat_res) {
        return std::unexpected(mat_res.error());
    }
    auto basicMat = *mat_res;

    return Material {
        .pipeline                       = basicMat.pipeline,
        .prePassPipeline                = basicMat.prePassPipeline,
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
        .alphaMode                      = transmission ? desc.alphaMode : ((desc.alphaMode != 0) ? desc.alphaMode : basicMat.alphaMode),
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

void RenderDiagnostics::BeginShaderObservation() {
    if constexpr (isDev) {
        if (fileSystemWatcher && shaderDirectoryWatch == 0) {
            shaderDirectoryWatch = fileSystemWatcher->WatchDirectory(
                "resources/shaders", [this](const FS::FileWatchEvent& event) { HandleShaderFileEvent(event); }, true, ".slang",
                FS::FileSystemWatcher::kDefaultDebounceMs
            );
        }
    }
}

void RenderDiagnostics::HandleShaderFileEvent(const FS::FileWatchEvent& event) {
    if constexpr (isDev) {
        shaderReloads.Dispatch(event.path.lexically_normal().generic_string(), [this] { vkDeviceWaitIdle(ctx.Device()); });
    }
}

void RenderDiagnostics::StopShaderObservation() noexcept {
    if constexpr (isDev) {
        if (fileSystemWatcher && shaderDirectoryWatch != 0) {
            static_cast<void>(fileSystemWatcher->Unwatch(shaderDirectoryWatch));
        }
    }
}

auto RenderContext::CreateTexture(std::span<const std::byte> rgba, Extent2D extent, bool isSRGB) -> std::expected<TextureHandle, ErrorCode> {
    const uint64_t pixels = static_cast<uint64_t>(extent.width) * extent.height;
    if (pixels == 0 || pixels > std::numeric_limits<size_t>::max() / 4 || rgba.size() != static_cast<size_t>(pixels) * 4) {
        return std::unexpected(TextureDataError::InvalidPixels);
    }
    return _impl->textureManager.UploadUnnamed(rgba.data(), extent.width, extent.height, Rgba8Format(isSRGB));
}

auto RenderContext::CreateTexture(std::string_view name, std::span<const std::byte> rgba, Extent2D extent, bool isSRGB, bool generateMips)
    -> std::expected<TextureHandle, ErrorCode> {
    const uint64_t pixels = static_cast<uint64_t>(extent.width) * extent.height;
    if (pixels == 0 || pixels > std::numeric_limits<size_t>::max() / 4 || rgba.size() != static_cast<size_t>(pixels) * 4) {
        return std::unexpected(TextureDataError::InvalidPixels);
    }
    return _impl->textureManager.Upload(name, rgba.data(), extent.width, extent.height, Rgba8Format(isSRGB), generateMips);
}

auto RenderContext::CreateTextureHDR(std::string_view name, std::span<const std::byte> rgba16f, Extent2D extent, bool generateMips)
    -> std::expected<TextureHandle, ErrorCode> {
    const uint64_t pixels = static_cast<uint64_t>(extent.width) * extent.height;
    if (pixels == 0 || pixels > std::numeric_limits<size_t>::max() / 8 || rgba16f.size() != static_cast<size_t>(pixels) * 8u) {
        return std::unexpected(TextureDataError::InvalidPixels);
    }
    return _impl->textureManager.Upload(name, rgba16f.data(), extent.width, extent.height, VK_FORMAT_R16G16B16A16_SFLOAT, generateMips);
}

auto RenderContext::CreateTextureCube(std::array<std::span<const std::byte>, 6> faces, uint32_t faceSize) -> std::expected<TextureHandle, ErrorCode> {
    if (faceSize == 0) {
        return std::unexpected(TextureDataError::InvalidPixels);
    }
    const uint64_t pixelsPerFace = static_cast<uint64_t>(faceSize) * faceSize;
    if (pixelsPerFace > std::numeric_limits<size_t>::max() / 4) {
        return std::unexpected(TextureDataError::InvalidPixels);
    }
    const size_t               bytesPerFace = static_cast<size_t>(pixelsPerFace) * 4;
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

    const size_t bytes = Resource::blue_noise_rgba.size();
    const auto   side  = static_cast<size_t>(std::sqrt(static_cast<double>(bytes) / 4));
    if (bytes % 4 != 0 || side * side * 4 != bytes || side == 0 || side > std::numeric_limits<uint32_t>::max()) [[unlikely]] {
        ZHLN::Log("[BlueNoise] Expected a whole square of 8-bit RGBA texels, got {} bytes.", bytes);
        return std::unexpected(ErrorCode {BlueNoiseError::UnexpectedLayout});
    }

    const auto w = static_cast<uint32_t>(side);
    const auto h = static_cast<uint32_t>(side);

    const auto imageConfig = Vk::ImageConfig::Texture2D({w, h}, kFormat, Vk::ImageUsage::TransferDst | Vk::ImageUsage::Sampled);
    auto       imageRes    = Vk::Image::Create(allocator, imageConfig);
    if (!imageRes) {
        return std::unexpected(imageRes.error());
    }

    Vk::Image           image = std::move(*imageRes);
    defer               _([&] { allocator.DestroyImage(image); });
    const Vk::ImageView view    = image.View();
    auto                staging = stagingRingBuffer.Allocate(bytes);
    if (staging.mappedData == nullptr) {
        return std::unexpected(Vk::StagingError::MemoryMappingFailed);
    }
    std::memcpy(staging.mappedData, Resource::blue_noise_rgba.data(), bytes);

    if (auto immediate = Vk::ExecuteImmediate(
            ctx, graphicsCmdRing, stagingRingBuffer,
            [&](VkCommandBuffer cmd) {
                view.DiscardTo<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(cmd);

                const VkBufferImageCopy2 region = {
                    .sType             = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
                    .pNext             = nullptr,
                    .bufferOffset      = staging.slice.offset,
                    .bufferRowLength   = 0,
                    .bufferImageHeight = 0,
                    .imageSubresource  = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1},
                    .imageOffset       = {},
                    .imageExtent       = {w, h, 1},
                };
                Vk::CopyBufferToImage(cmd, staging.slice.buffer, image.Handle(), region);
                view.Transition<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd);
            }
        );
        !immediate) [[unlikely]] {
        return std::unexpected(immediate.error());
    }

    Vk::Debug::SetImageName(ctx, image.Handle(), "BlueNoise.LDR_RGBA_0");

    blueNoiseSampler  = Vk::Sampler::NearestRepeat().WithoutMips();
    auto blueNoiseIdx = textureManager.Adopt(std::move(image), view);
    if (!blueNoiseIdx) {
        return std::unexpected(blueNoiseIdx.error());
    }
    blueNoiseTexIdx = *blueNoiseIdx;

    ZHLN::Log("[BlueNoise] Blue noise tile bound as bindless texture {} ({}x{}, single mip).", blueNoiseTexIdx, w, h);
    return {};
}

auto RebuildSkinnedBLAS(
    VkCommandBuffer    cmd,
    Vk::Context&       ctx,
    Vk::Allocator&     allocator,
    Vk::DeletionQueue& deletionQueue,
    const DrawCommand& drawCmd,
    NativeMesh&        scratchMesh
) -> std::expected<Vk::DeviceAddress<>, ErrorCode> {
    if (!ctx.RayTracingSupported()) {
        return std::unexpected(RenderFeatureError::FeatureNotSupported);
    }
    if (cmd == VK_NULL_HANDLE || drawCmd.skinPosAddress == nullptr || scratchMesh.vertexCount == 0) {
        return std::unexpected(RenderFeatureError::UnresolvedMeshHandle);
    }

    const auto geometry = Vk::GeometryView::Triangles<VertexPosition>(scratchMesh.vboAddress, scratchMesh.vertexCount)
                              .MaybeIndexed(drawCmd.instanceData.iboAddress, drawCmd.instanceData.indexCount);

    const Vk::AccelerationStructureSizes sizes = Vk::GetBLASSizes(ctx.Device(), geometry);

    const bool creatingBlas = !scratchMesh.blas;
    if (creatingBlas) {
        auto blasBufOpt = Vk::Buffer::Create(
            allocator, sizes.acceleration_structure_size, Vk::BufferUsage::AccelerationStructureStorage | Vk::BufferUsage::ShaderDeviceAddress,
            Vk::MemoryUsage::GPUOnly
        );
        if (!blasBufOpt) {
            return std::unexpected(blasBufOpt.error());
        }
        scratchMesh.blasBuffer = std::move(*blasBufOpt);
        auto blas              = Vk::CreateAccelerationStructure(
            ctx.Device(), scratchMesh.blasBuffer.Handle(), sizes.acceleration_structure_size, Vk::AccelerationStructureType::BottomLevel
        );
        if (!blas) {
            allocator.DestroyBuffer(scratchMesh.blasBuffer);
            return std::unexpected(blas.error());
        }
        scratchMesh.blas = std::move(*blas);
        if (!scratchMesh.blas.Valid()) {
            allocator.DestroyBuffer(scratchMesh.blasBuffer);
            return std::unexpected(Vk::VulkanCallError::VulkanCallFailed);
        }
        scratchMesh.blasAddress = Vk::DeviceAddress<> {Vk::GetAccelerationStructureAddress(ctx.Device(), scratchMesh.blas.Get())};
    }

    auto scratchBufOpt =
        Vk::Buffer::Create(allocator, sizes.build_scratch_size, Vk::BufferUsage::Storage | Vk::BufferUsage::ShaderDeviceAddress, Vk::MemoryUsage::GPUOnly);
    if (!scratchBufOpt) {
        if (creatingBlas) {
            scratchMesh.blas = {};
            allocator.DestroyBuffer(scratchMesh.blasBuffer);
            scratchMesh.blasAddress = nullptr;
        }
        return std::unexpected(scratchBufOpt.error());
    }
    Vk::Buffer scratchBuf = std::move(*scratchBufOpt);
    Vk::BuildBLAS(cmd, geometry, scratchMesh.blas.Get(), Vk::BufferSlice {scratchBuf, ctx.BufferAddress(scratchBuf.Handle())});
    deletionQueue.Enqueue(std::move(scratchBuf));
    return scratchMesh.blasAddress;
}

void RenderContext::DrawDebugTriangles(std::span<const VertexPosition> positions, std::span<const VertexSurface> surfaces, const Material& material) noexcept {
    if (positions.size() != surfaces.size()) {
        ZHLN::Assert(false, "debug vertex positions and surfaces must have the same count");
        return;
    }
    auto* nativeMesh = _impl->geometry.Resolve(_impl->frames.debugMeshHandles[_impl->presenter.frameIndex]);
    if (nativeMesh == nullptr) {
        return;
    }

    constexpr size_t maxPosSize = RenderContext::Impl::kMaxDebugVertices * sizeof(VertexPosition);

    auto mapped = nativeMesh->buffer.Map(_impl->allocator);
    if (!mapped) {
        return;
    }
    char* basePtr = mapped->As<char>();

    const size_t count = std::min(positions.size(), static_cast<size_t>(RenderContext::Impl::kMaxDebugVertices));
    if (count > 0) {
        std::memcpy(basePtr, positions.data(), count * sizeof(VertexPosition));
        std::memcpy(basePtr + maxPosSize, surfaces.data(), count * sizeof(VertexSurface));
    }
    nativeMesh->vertexCount = static_cast<uint32_t>(count);
    if (nativeMesh->vertexCount == 0) {
        return;
    }

    const BufferHandle buffer = _impl->frames.debugMeshHandles[_impl->presenter.frameIndex];
    Mesh               debugMesh {
        .posBuffer     = buffer,
        .surfaceBuffer = buffer,
        .skinBuffer    = BufferHandle::Invalid,
        .indexBuffer   = BufferHandle::Invalid,
        .vertexCount   = nativeMesh->vertexCount,
        .indexCount    = 0
    };
    Draw(material, debugMesh, {.transform = JPH::Mat44::sIdentity(), .prevTransform = JPH::Mat44::sIdentity(), .cullRadius = 10000.0f});
}

void RenderContext::UpdateJointMatrices(uint32_t offset, std::span<const JPH::Mat44> matrices) {
    if (matrices.empty()) {
        return;
    }
    auto& buffer = _impl->frames.CurrentJointBuffer();
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
    const uint32_t offset   = _impl->nextMorphDeltaIndex;
    const size_t   capacity = _impl->morphDeltasBuffer.Size() / sizeof(float);
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
        impl->settings.qualityPreset      = impl->settings.DetectPreset();
    });
}

void RenderContext::Impl::ApplySettings(GraphicsSettings incoming) noexcept {
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
    settings               = incoming;

    if (settings.qualityPreset != previousTier) {
        ZHLN::Log("Graphics quality tier: {} -> {}", previousTier, settings.qualityPreset);
    }
}

void RenderContext::ApplySettings(GraphicsSettings newSettings) noexcept {
    _impl->ApplySettings(newSettings);
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
    if (!impl.ctx.RayTracingSupported()) {
        return std::unexpected(RenderFeatureError::FeatureNotSupported);
    }

    auto* posMesh = impl.geometry.Resolve(mesh.posBuffer);
    if (posMesh == nullptr) {
        return std::unexpected(RenderFeatureError::UnresolvedMeshHandle);
    }
    auto* indexMesh = mesh.indexBuffer != BufferHandle::Invalid ? impl.geometry.Resolve(mesh.indexBuffer) : nullptr;

    const auto geometry = Vk::GeometryView::Triangles<VertexPosition>(posMesh->vboAddress, mesh.vertexCount)
                              .MaybeIndexed((indexMesh != nullptr) ? indexMesh->vboAddress : nullptr, mesh.indexCount);

    const Vk::AccelerationStructureSizes sizes = Vk::GetBLASSizes(impl.ctx.Device(), geometry);

    auto bufferRes = Vk::Buffer::Create(
        impl.allocator, sizes.acceleration_structure_size, Vk::BufferUsage::AccelerationStructureStorage | Vk::BufferUsage::ShaderDeviceAddress,
        Vk::MemoryUsage::GPUOnly
    );
    if (!bufferRes) {
        return std::unexpected(bufferRes.error());
    }
    defer _([&] { impl.allocator.DestroyBuffer(*bufferRes); });
    auto  blasResult =
        Vk::CreateAccelerationStructure(impl.ctx.Device(), bufferRes->Handle(), sizes.acceleration_structure_size, Vk::AccelerationStructureType::BottomLevel);
    if (!blasResult) {
        return std::unexpected(blasResult.error());
    }
    Vk::AccelerationStructure blas = std::move(*blasResult);

    auto scratchRes =
        Vk::Buffer::Create(impl.allocator, sizes.build_scratch_size, Vk::BufferUsage::Storage | Vk::BufferUsage::ShaderDeviceAddress, Vk::MemoryUsage::GPUOnly);
    if (!scratchRes) {
        return std::unexpected(scratchRes.error());
    }
    defer _([&] { impl.allocator.DestroyBuffer(*scratchRes); });

    Vk::CommandPool<Vk::QueueType::Graphics> tempPool(impl.ctx.Device(), impl.ctx.PhysicalInfo().graphicsFamily);
    auto                                     allocated = tempPool.Allocate(1);
    if (!allocated) {
        return std::unexpected(allocated.error());
    }
    auto recording = Vk::CommandRecorder::Begin(tempPool[0]);
    if (!recording) {
        return std::unexpected(recording.error());
    }
    auto* const cmd = recording->Handle();
    impl.geometry.RecordPendingUpdates(cmd);
    Vk::MemoryBarrier::TransferToAccelerationStructureBuild().Record(cmd);
    Vk::BuildBLAS(cmd, geometry, blas.Get(), Vk::BufferSlice {*scratchRes, impl.ctx.BufferAddress(scratchRes->Handle())});
    auto executable = std::move(*recording).End();
    if (!executable) {
        return std::unexpected(executable.error());
    }

    auto submitted = Vk::SubmitAndWait(
        impl.ctx.GraphicsQueue(), std::move(*executable), impl.transferRingBuffer.GetSemaphore(), impl.transferRingBuffer.GetCurrentValue(),
        VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR
    );
    if (!submitted) {
        vkQueueWaitIdle(impl.ctx.GraphicsQueue());
        return std::unexpected(submitted.error());
    }

    impl.deletionQueue.EnqueueAccelerationStructure(impl.ctx.Device(), std::move(posMesh->blas));
    impl.deletionQueue.Enqueue(std::move(posMesh->blasBuffer));
    posMesh->blasAddress = Vk::DeviceAddress<> {Vk::GetAccelerationStructureAddress(impl.ctx.Device(), blas.Get())};
    posMesh->blasBuffer  = std::move(*bufferRes);
    posMesh->blas        = std::move(blas);
    return {};
}

auto RenderContext::BakeProceduralTexture(uint32_t width, uint32_t height, uint32_t variantIdx, float scale, float randomness)
    -> std::expected<TextureHandle, ErrorCode> {
    return _impl->BakeProceduralTexture(width, height, variantIdx, scale, randomness, 0.0f);
}

auto RenderContext::CreateProceduralTexture(std::string_view name, Extent2D extent, std::span<const uint32_t> pixels, bool isSRGB, bool generateMips)
    -> TextureHandle {
    const uint64_t expectedPixels = static_cast<uint64_t>(extent.width) * extent.height;
    if (expectedPixels == 0 || expectedPixels != pixels.size()) {
        ZHLN::Log("[RenderContext] Procedural texture '{}' has an invalid extent or texel count.", name);
        return TextureHandle::Invalid;
    }
    const auto uploaded = CreateTexture(name, std::as_bytes(pixels), extent, isSRGB, generateMips);
    if (!uploaded) {
        ZHLN::Log("[RenderContext] Procedural texture '{}' ({}x{}) failed to upload: {}", name, extent.width, extent.height, uploaded.error());
        return TextureHandle::Invalid;
    }
    return *uploaded;
}

enum class FrameCaptureError : uint8_t {
    DestinationNotRecorded ZHLN_ANNOTATION(ZHLN::Description<"No completed frame was drawn into the presentation target"> {}) = 1,
};

auto RenderContext::CaptureFrame() noexcept -> std::expected<CapturedFrame, ErrorCode> {
    auto* const impl = _impl.get();

    // -------------------------------------------------------------------------
    // Path A: the presentation target, when there is one to read back
    // -------------------------------------------------------------------------
    if (!impl->presenter.swapchain.Valid()) {
        const auto dest = impl->destinations.Find(impl->presentationTarget);
        if (impl->frameOpen || !dest || !dest->acquired || !dest->acquired->drawn) {
            return std::unexpected(FrameCaptureError::DestinationNotRecorded);
        }

        const auto&   frameImage   = *dest->acquired;
        VkImage       source       = frameImage.image.Handle();
        VkExtent2D    extent       = frameImage.image.Extent2D();
        VkImageLayout sourceLayout = Vk::ToVkImageLayout(frameImage.layout);

        const auto imageBytes = static_cast<size_t>(extent.width) * extent.height * 4u;

        auto stagingRes = Vk::Buffer::Create(impl->allocator, imageBytes, Vk::BufferUsage::TransferDst, Vk::MemoryUsage::GPUToCPU);
        if (!stagingRes) {
            return std::unexpected(stagingRes.error());
        }
        auto  stagingBuffer = std::move(*stagingRes);
        defer _([&] { impl->allocator.DestroyBuffer(stagingBuffer); });

        if (auto immediate = Vk::ExecuteImmediate(
                impl->ctx, impl->graphicsCmdRing,
                [&](VkCommandBuffer cmd) -> void {
                    frameImage.image.Transition(
                        cmd,
                        sourceLayout,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                        VK_ACCESS_2_TRANSFER_READ_BIT
                    );

                    Vk::CopyImageToBuffer(cmd, source, stagingBuffer.Handle(), extent);

                    frameImage.image.Transition(
                        cmd,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                        VK_ACCESS_2_TRANSFER_READ_BIT,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT
                    );
                }
            );
            !immediate) [[unlikely]] {
            return std::unexpected(immediate.error());
        }

        auto mapped = stagingBuffer.Map(impl->allocator);
        if (!mapped) {
            return std::unexpected(mapped.error());
        }

        const auto* const rgba   = mapped->As<const uint8_t>();
        const size_t      pixels = static_cast<size_t>(extent.width) * extent.height;

        CapturedFrame result {
            .extent = {extent.width, extent.height},
            .format = PixelFormat::Rgba8,
            .pixels = std::vector<uint8_t>(rgba, rgba + imageBytes),
        };

        uint64_t lumaSum = 0;
        for (size_t i = 0; i < pixels; ++i) {
            const uint8_t r = rgba[(i * 4) + 0];
            const uint8_t g = rgba[(i * 4) + 1];
            const uint8_t b = rgba[(i * 4) + 2];
            if (rgba[(i * 4) + 3] == 0) {
                ++result.metrics.transparentPixels;
            }

            const uint32_t luma = (2126u * static_cast<uint32_t>(r) + 7152u * static_cast<uint32_t>(g) + 722u * static_cast<uint32_t>(b)) / 10000u;
            lumaSum += luma;
            result.metrics.litPixels += luma > 8u ? 1u : 0u;
        }
        result.metrics.meanLuma = pixels == 0 ? 0.0 : static_cast<double>(lumaSum) / static_cast<double>(pixels);
        return result;
    }

    // -------------------------------------------------------------------------
    // Path B: the HDR scene color, tone-mapped for display
    // -------------------------------------------------------------------------
    const auto   extent     = impl->graphResources.hdrSceneColor.extent;
    const size_t imageBytes = static_cast<size_t>(extent.width) * extent.height * sizeof(uint16_t) * 4;

    auto stagingRes = Vk::Buffer::Create(impl->allocator, imageBytes, Vk::BufferUsage::TransferDst, Vk::MemoryUsage::GPUToCPU);
    if (!stagingRes) {
        return std::unexpected(stagingRes.error());
    }
    auto  stagingBuffer = std::move(*stagingRes);
    defer _([&] { impl->allocator.DestroyBuffer(stagingBuffer); });

    if (auto immediate = Vk::ExecuteImmediate(
            impl->ctx, impl->graphicsCmdRing,
            [&](VkCommandBuffer cmd) -> void {
                const auto& hdrTarget = impl->graphResources.hdrSceneColor;
                hdrTarget.Transition<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL>(cmd);
                Vk::CopyImageToBuffer(cmd, hdrTarget.image.Handle(), stagingBuffer.Handle(), extent);
                hdrTarget.Transition<VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd);
            }
        );
        !immediate) [[unlikely]] {
        return std::unexpected(immediate.error());
    }

    auto mapped = stagingBuffer.Map(impl->allocator);
    if (!mapped) {
        return std::unexpected(mapped.error());
    }

    const auto* const halfFloats   = mapped->As<const uint16_t>();
    const bool        isFullBright = impl->currentUniforms.fullBright != 0;
    const size_t      pixels       = static_cast<size_t>(extent.width) * extent.height;

    CapturedFrame result {
        .extent = {extent.width, extent.height},
        .format = PixelFormat::Rgb8,
        .pixels = std::vector<uint8_t>(pixels * 3),
    };

    // The display transform belongs to the renderer, not to the encoder: whether
    // the tone curve applies is a renderer uniform. What the caller receives is
    // already display-referred, so every consumer sees the same image. The curve
    // itself lives in CapturedFrame.hpp so a caller re-encoding a raw frame
    // cannot land on a different one.
    // NaN (a half-float readback can hold one) has no ordering, so it is
    // answered before the clamp rather than cast through it.
    const auto quantize = [](float v) noexcept -> uint8_t {
        if (!(v > 0.0f)) {
            return 0;
        }
        return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f);
    };

    uint64_t lumaSum = 0;
    for (size_t i = 0; i < pixels; ++i) {
        float r = Math::HalfToFloat(halfFloats[(i * 4) + 0]);
        float g = Math::HalfToFloat(halfFloats[(i * 4) + 1]);
        float b = Math::HalfToFloat(halfFloats[(i * 4) + 2]);

        if (!isFullBright) {
            r = Math::AcesFilm(r * kCaptureExposure);
            g = Math::AcesFilm(g * kCaptureExposure);
            b = Math::AcesFilm(b * kCaptureExposure);
        }

        const uint8_t red   = quantize(r);
        const uint8_t green = quantize(g);
        const uint8_t blue  = quantize(b);

        result.pixels[(i * 3) + 0] = red;
        result.pixels[(i * 3) + 1] = green;
        result.pixels[(i * 3) + 2] = blue;

        const uint32_t luma = (2126u * static_cast<uint32_t>(red) + 7152u * static_cast<uint32_t>(green) + 722u * static_cast<uint32_t>(blue)) / 10000u;
        lumaSum += luma;
        result.metrics.litPixels += luma > 8u ? 1u : 0u;
    }
    result.metrics.meanLuma = pixels == 0 ? 0.0 : static_cast<double>(lumaSum) / static_cast<double>(pixels);
    return result;
}

void RenderContext::ProvokeDeviceLost() {
    if (auto provoked = _impl->ProvokeDeviceLostInternal(); !provoked) [[unlikely]] {
        ZHLN::Log("Failed to submit the device-loss dispatch.");
    }
}

} // namespace ZHLN
