// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/engine/RenderResources.cpp
#include "RenderInternal.hpp"
#include "Instance.hpp"
#include "Resources.hpp"
#include "Zahlen/Types.hpp"
#include <Zahlen/Core/ControlFlow.hpp>
#include <stb_image.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

// ============================================================================
// Private Resource Errors (Tier 1)
// Produced only while building materials / resizing shadow targets inside
// this translation unit; no header exposes them, so callers just log the
// type-erased ZHLN::Error. Declared at file scope (not an anonymous
// namespace) to keep reflected category names stable for both native
// reflection and the AST transpiler fallback.
// ============================================================================

namespace ZHLN {

enum class MaterialCreationError : uint8_t {
    ShaderCompilationFailed ZHLN_ANNOTATION(ZHLN::Description<"Material shader compilation failed">{}) = 1,
    PipelineLayoutCreationFailed ZHLN_ANNOTATION(ZHLN::Description<"Material pipeline layout creation failed">{}),
    PipelineCreationFailed ZHLN_ANNOTATION(ZHLN::Description<"Material pipeline creation failed">{}),
};

enum class BlueNoiseError : uint8_t {
    DecodeFailed ZHLN_ANNOTATION(ZHLN::Description<"Blue noise PNG decode failed">{}) = 1,
};

enum class ShadowResolutionError : uint8_t {
    DeviceLost ZHLN_ANNOTATION(ZHLN::Description<"Device lost while resizing shadow map">{}) = 1,
    RecreationFailed ZHLN_ANNOTATION(ZHLN::Description<"Shadow map recreation failed">{}),
};

} // namespace ZHLN

namespace ZHLN {

// ============================================================================
// High-Level GPU Asset Registry & Resolution API
// ============================================================================

auto RenderContext::GetGPUMesh(AssetID id) const noexcept -> std::optional<Mesh> {
    const Mesh* found = _impl->assetMeshMap.Find(id);
    if (found != nullptr) {
        return *found;
    }
    return std::nullopt;
}

auto RenderContext::GetGPUMaterial(MaterialID id) const noexcept -> std::optional<Material> {
    const Material* found = _impl->assetMaterialMap.Find(id);
    if (found != nullptr) {
        return *found;
    }
    return std::nullopt;
}

void RenderContext::RegisterGPUMesh(AssetID id, Mesh mesh) noexcept {
    _impl->assetMeshMap.Insert(id, mesh);
}

void RenderContext::RegisterGPUMaterial(MaterialID id, Material mat) noexcept {
    _impl->assetMaterialMap.Insert(id, mat);
}

auto RenderContext::GetOrCreateSkinnedScratchBuffer(uint64_t entityKey, uint32_t vertexCount) -> BufferHandle {
    const BufferHandle* existing = _impl->skinnedScratchMap.Find(entityKey);
    if (existing != nullptr && *existing != BufferHandle::Invalid) {
        return *existing;
    }

    BufferHandle handle = CreateSkinnedScratchBuffer(vertexCount);
    if (handle != BufferHandle::Invalid) {
        _impl->skinnedScratchMap.Insert(entityKey, handle);
    }
    return handle;
}

auto RenderContext::CreateStorageBuffer(size_t size) -> BufferHandle {
    auto res = _impl->CreateGPUBuffer(size, nullptr, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    if (res) {
        return _impl->meshPool.Create(std::move(res->first), 0, res->second);
    }
    return BufferHandle::Invalid;
}

auto RenderContext::GetOrCreateParticleBuffer(uint64_t entityKey, uint32_t maxParticles) -> BufferHandle {
    const BufferHandle* existing = _impl->particleBufferMap.Find(entityKey);
    if (existing != nullptr && *existing != BufferHandle::Invalid) {
        return *existing;
    }

    BufferHandle handle = CreateStorageBuffer(maxParticles * sizeof(Particle));
    if (handle != BufferHandle::Invalid) {
        _impl->particleBufferMap.Insert(entityKey, handle);
    }
    return handle;
}

void RenderContext::SubmitParticleEmitter(BufferHandle gpuBuffer, uint32_t maxParticles, const ParticleEmitterParams& params) {
    _impl->queues.particleEmittersQueue.push_back({.gpuBuffer = gpuBuffer, .maxParticles = maxParticles, .params = params});
}

void RenderContext::SubmitMeshParticleEmitter(
    BufferHandle                     gpuBuffer,
    uint32_t                         maxParticles,
    const MeshParticleEmitterParams& params,
    AssetID                          mesh,
    MaterialID                       mat
) {
    _impl->queues.meshParticleQueue.push_back(
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

    // 2. Reclaim all buffer slots from registered meshes
    _impl->assetMeshMap.ForEach([this](AssetID, const Mesh& mesh) {
        if (mesh.posBuffer != BufferHandle::Invalid)
            DestroyBuffer(mesh.posBuffer);
        if (mesh.attrBuffer != BufferHandle::Invalid)
            DestroyBuffer(mesh.attrBuffer);
        if (mesh.skinBuffer != BufferHandle::Invalid)
            DestroyBuffer(mesh.skinBuffer);
        if (mesh.indexBuffer != BufferHandle::Invalid)
            DestroyBuffer(mesh.indexBuffer);
        if (mesh.meshletBuffer != BufferHandle::Invalid)
            DestroyBuffer(mesh.meshletBuffer);
        if (mesh.meshletVertexBuffer != BufferHandle::Invalid)
            DestroyBuffer(mesh.meshletVertexBuffer);
        if (mesh.meshletTriBuffer != BufferHandle::Invalid)
            DestroyBuffer(mesh.meshletTriBuffer);
    });
    _impl->assetMeshMap.Clear();

    // 3. Reclaim all pipeline slots from registered materials (now safe because GPU is idle)
    _impl->assetMaterialMap.ForEach([this](MaterialID, const Material& mat) {
        if (mat.pipeline != PipelineHandle::Invalid) {
            _impl->materialPool.Destroy(mat.pipeline);
        }
        if (mat.prePassPipeline != PipelineHandle::Invalid) {
            _impl->materialPool.Destroy(mat.prePassPipeline);
        }
    });
    _impl->assetMaterialMap.Clear();

    // 4. Reclaim scratch & particle buffers
    _impl->skinnedScratchMap.ForEach([this](uint64_t /*key*/, BufferHandle handle) -> void { DestroyBuffer(handle); });
    _impl->skinnedScratchMap.Clear();
    _impl->particleBufferMap.ForEach([this](uint64_t /*key*/, BufferHandle handle) -> void { DestroyBuffer(handle); });
    _impl->particleBufferMap.Clear();

    for (const auto& pair: _impl->tracked2DEmitters) {
        DestroyBuffer(pair.second);
    }
    _impl->tracked2DEmitters.clear();

    for (const auto& pair: _impl->tracked3DEmitters) {
        DestroyBuffer(pair.second);
    }
    _impl->tracked3DEmitters.clear();

    _impl->textureManager.Clear();

    // 5. Drain the deferred deletion queues
    _impl->deletionQueue.BeginFrame(0);
    _impl->deletionQueue.BeginFrame(1);
}

auto RenderContext::GetTracked2DEmitters() noexcept -> ZHLN::Array<ZHLN::Pair<uint64_t, BufferHandle>>& {
    return _impl->tracked2DEmitters;
}

auto RenderContext::GetTracked3DEmitters() noexcept -> ZHLN::Array<ZHLN::Pair<uint64_t, BufferHandle>>& {
    return _impl->tracked3DEmitters;
}

bool RenderContext::MeshShadingSupported() const noexcept {
    return _impl->ctx.MeshShadersSupported();
}

bool RenderContext::MeshShadingActive() const noexcept {
    return _impl->MeshShadingActive();
}

void RenderContext::SetMeshShadingEnabled(bool enabled) noexcept {
    Diag::SetMeshShadingDisabled(!enabled);
}

bool RenderContext::RayTracingSupported() const noexcept {
    return _impl->rtCtx.Valid();
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
    if (_impl->current_cmd != VK_NULL_HANDLE) {
        _impl->gpuDiagnostics.WriteCheckpoint(_impl->current_cmd, name);
    }
}

void RenderContext::OnDeviceLost() noexcept {
    _impl->gpuDiagnostics.OnDeviceLost();
}

// ============================================================================
// RenderContext Subsystem Implementation
// ============================================================================

auto RenderContext::GetRendererName() const -> const char* {
    return _impl->appName.data();
}

auto RenderContext::GetGPUName() const -> const char* {
    return &_impl->ctx.PhysicalInfo().properties.properties.deviceName[0];
}

auto RenderContext::GetFrameIndex() const noexcept -> uint32_t {
    return _impl->frame_index;
}

void RenderContext::CheckShaderReload() noexcept {
    if constexpr (isDev) {
        _impl->CheckShaderWatchers();
    }
}

void RenderContext::SetResolution(const Extent2D& res) {
    // With a real window the compositor owns the size: the request is advisory
    // and the recreate re-queries glfwGetFramebufferSize, which is why this
    // used to ignore its argument entirely. Headless (and TTY) there is nothing
    // to ask -- Window::GetSize just returns what it was told -- so the extent
    // has to be written there or the recreate reproduces the old size and the
    // call is a no-op.
    if (res.width > 0 && res.height > 0 && _impl->window.IsHeadless()) {
        _impl->window.SetSize(res.width, res.height);
    }
    _impl->resized = true;
}

auto RenderContext::CreateStorageBuffer(const void* data, size_t size, uint32_t stride) -> BufferHandle {
    const uint32_t safeStride = (stride > 0) ? stride : 1u;
    return _impl->CreateGPUBuffer(size, data, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)
        .transform([this, size, safeStride](auto&& pair) -> auto {
            return _impl->meshPool.Create(std::move(pair.first), static_cast<uint32_t>(size / safeStride), pair.second);
        })
        .value_or(BufferHandle::Invalid);
}

auto RenderContext::CreateVertexBuffer(const void* data, size_t size, uint32_t stride) -> BufferHandle {
    return _impl->CreateGPUBuffer(size, data, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)
        .transform([this, size, stride](auto&& pair) -> auto {
            return _impl->meshPool.Create(std::move(pair.first), static_cast<uint32_t>(size / stride), pair.second);
        })
        .value_or(BufferHandle::Invalid);
}

auto RenderContext::CreateIndexBuffer(const void* data, size_t size) -> BufferHandle {
    return _impl->CreateGPUBuffer(size, data, VK_BUFFER_USAGE_INDEX_BUFFER_BIT)
        .transform([this, size](auto&& pair) -> auto {
            return _impl->meshPool.Create(std::move(pair.first), static_cast<uint32_t>(size / sizeof(uint32_t)), pair.second);
        })
        .value_or(BufferHandle::Invalid);
}

void RenderContext::DestroyBuffer(BufferHandle handle) {
    if (handle != BufferHandle::Invalid) {
        // Defer destruction for 2 frames so the GPU finishes reading from the buffer
        Vk::ScopedDeletionQueue guard(_impl->deletionQueue);
        _impl->meshPool.Destroy(handle);
    }
}

void RenderContext::UpdateBuffer(BufferHandle handle, const void* data, size_t size) noexcept {
    if (handle == BufferHandle::Invalid || data == nullptr || size == 0) {
        return;
    }
    auto* nativeMesh = _impl->meshPool.Resolve(handle).value_or(nullptr);
    if (nativeMesh == nullptr) {
        return;
    }

    auto stagingAlloc = _impl->transferRingBuffer.Allocate(size);
    std::memcpy(stagingAlloc.mappedData, data, size);

    Vk::ExecuteImmediate<Vk::QueueType::Transfer>(_impl->ctx, _impl->transferCmdRing, _impl->transferRingBuffer, [&](VkCommandBuffer cmd) -> void {
        Vk::CopyRingBuffer(cmd, stagingAlloc, nativeMesh->buffer, size);
    });
}

namespace {

/// VK_EXT_mesh_shader: builds the task+mesh+fragment twin of a material's
/// graphics pipeline. Returns an invalid pipeline (not an error) whenever mesh
/// shading is unavailable or the material did not provide mesh stages: the
/// vertex pipeline built by CreateMaterial always remains the fallback.
[[nodiscard]] Vk::Pipeline BuildMeshVariant(RenderContext::Impl* impl, const PipelineDesc& desc) noexcept {
    if (!impl->ctx.MeshShadersSupported() || desc.meshShaderData == nullptr || desc.meshShaderSize == 0) {
        return {};
    }

    const ZHLN_ShaderDesc taskDesc = {.code = Vk::AsSpirV(desc.taskShaderData), .size = desc.taskShaderSize, .entry_point = nullptr};
    const ZHLN_ShaderDesc meshDesc = {.code = Vk::AsSpirV(desc.meshShaderData), .size = desc.meshShaderSize, .entry_point = nullptr};
    const ZHLN_ShaderDesc fragDesc = {.code = Vk::AsSpirV(desc.fragShaderData), .size = desc.fragShaderSize, .entry_point = nullptr};

    auto shaders = Vk::ShaderStages::CreateMesh(impl->ctx.Device(), taskDesc, meshDesc, fragDesc);
    if (!shaders) {
        ZHLN::Log("[RenderResources] Mesh-shader stage creation failed ({}); this material keeps the vertex pipeline.", shaders.error().Message());
        return {};
    }

    // Register task & mesh shaders with GPU diagnostics
    impl->gpuDiagnostics.RegisterShader(taskDesc, "TaskMain");
    impl->gpuDiagnostics.RegisterShader(meshDesc, "MeshMain");

    auto builder = Vk::PipelineBuilder {}
                       .Shaders(*shaders)
                       .Layout(impl->emptyPipelineLayout)
                       .HeapMappings(&impl->sceneHeapMappings.info, &impl->sceneHeapMappings.info)
                       .DepthFormat(VK_FORMAT_D32_SFLOAT_S8_UINT);

    if (desc.doubleSided) {
        builder.CullNone();
    } else {
        builder.CullBack();
    }

    if (desc.alphaBlend || desc.additiveBlend) {
        builder.ColorFormats({VK_FORMAT_R16G16B16A16_SFLOAT});
        builder.DepthWrite(false);
        if (desc.additiveBlend) {
            builder.AdditiveBlend();
        } else {
            builder.AlphaBlend();
        }
    } else {
        builder.ColorFormats(ActiveGBuffer::array);
    }

    auto pipeline = builder.Build(impl->ctx.Device());
    if (!pipeline) {
        ZHLN::Log("[RenderResources] Mesh pipeline creation failed ({}); this material keeps the vertex pipeline.", pipeline.error().Message());
        return {};
    }
    return std::move(*pipeline);
}

} // namespace

auto RenderContext::CreateMaterial(const PipelineDesc& desc) -> std::expected<Material, Error> {
    const ZHLN_ShaderDesc v_desc = {.code = Vk::AsSpirV(desc.vertexShaderData), .size = desc.vertexShaderSize, .entry_point = nullptr};
    const ZHLN_ShaderDesc f_desc = {.code = Vk::AsSpirV(desc.fragShaderData), .size = desc.fragShaderSize, .entry_point = nullptr};

    auto* impl = _impl.get();

    return Vk::ShaderStages::Create(impl->ctx.Device(), v_desc, f_desc)
        .transform_error([](auto) -> Error { return MaterialCreationError::ShaderCompilationFailed; })
        .and_then([impl, &desc, v_desc, f_desc](auto&& shaders) -> std::expected<Material, Error> {
            // Register vertex & fragment shaders with GPU diagnostics
            impl->gpuDiagnostics.RegisterShader(v_desc, "VSMain");
            impl->gpuDiagnostics.RegisterShader(f_desc, "PSMain");

            const VkPipelineLayout layout = impl->emptyPipelineLayout;

            auto pipeline = Vk::PipelineBuilder {}
                                .Shaders(shaders)
                                .Layout(layout)
                                .HeapMappings(&impl->sceneHeapMappings.info, &impl->sceneHeapMappings.info)
                                .DepthFormat(VK_FORMAT_D32_SFLOAT_S8_UINT);

            if (desc.doubleSided) {
                pipeline.CullNone();
            } else {
                pipeline.CullBack();
            }

            if (desc.alphaBlend || desc.additiveBlend) {
                pipeline.ColorFormats({VK_FORMAT_R16G16B16A16_SFLOAT});
                pipeline.DepthWrite(false);
                if (desc.additiveBlend) {
                    pipeline.AdditiveBlend();
                } else {
                    pipeline.AlphaBlend();
                }
            } else {
                pipeline.ColorFormats(ActiveGBuffer::array);
            }

            if (desc.isLineList) {
                pipeline.Topology(VK_PRIMITIVE_TOPOLOGY_LINE_LIST);
            }

            return pipeline.Build(impl->ctx.Device())
                .transform_error([](auto) -> Error { return MaterialCreationError::PipelineCreationFailed; })
                .transform([impl, layout, &desc](auto&& compiledPipeline) -> auto {
                    Vk::Pipeline meshPipeline = BuildMeshVariant(impl, desc);

                    return Material {
                        .pipeline  = impl->materialPool.Create(std::forward<decltype(compiledPipeline)>(compiledPipeline), layout, std::move(meshPipeline)),
                        .alphaMode = (desc.alphaBlend || desc.additiveBlend) ? 2u : 0u
                    };
                });
        });
}

auto RenderContext::CreateDebugLineMaterial() -> std::expected<Material, Error> {
    // PSForward => the Forward geometry variant. No mesh stages: a LINE_LIST
    // has no mesh-shader equivalent (mesh pipelines declare their own topology).
    const auto shaders = Resource::GetSceneShaders(Resource::SceneShaderVariant::Forward);
    return CreateMaterial({
        .vertexShaderData = shaders.vertex.data(),
        .vertexShaderSize = shaders.vertex.size(),
        .fragShaderData   = shaders.fragment.data(),
        .fragShaderSize   = shaders.fragment.size(),
        .doubleSided      = true,
        .alphaBlend       = true,
        .isLineList       = true,
    });
}

auto RenderContext::CreateDebugSolidMaterial() -> std::expected<Material, Error> {
    const auto shaders = Resource::GetSceneShaders(Resource::SceneShaderVariant::Forward);
    return CreateMaterial({
        .vertexShaderData = shaders.vertex.data(),
        .vertexShaderSize = shaders.vertex.size(),
        .fragShaderData   = shaders.fragment.data(),
        .fragShaderSize   = shaders.fragment.size(),
        // Designator order must follow PipelineDesc's declaration order: the
        // task/mesh members sit between the fragment stage and the state flags.
        // GCC rejects any other order outright (ISO C++ [dcl.init.aggr]/3.1).
        .taskShaderData = shaders.task.data(),
        .taskShaderSize = shaders.task.size(),
        .meshShaderData = shaders.mesh.data(),
        .meshShaderSize = shaders.mesh.size(),
        .doubleSided    = true,
        .alphaBlend     = true,
    });
}

void RenderContext::DrawLine(JPH::Vec3Arg start, JPH::Vec3Arg end, JPH::Vec4Arg colorStart, JPH::Vec4Arg colorEnd) noexcept {
    _impl->queues.lineQueue.push_back({.start = start, .end = end, .colorStart = colorStart, .colorEnd = colorEnd});
}

void RenderContext::Impl::CheckShaderWatchers() noexcept {
    if constexpr (isDev) {
        bool anyReloaded = false;
        for (auto& watcher: shaderWatchers) {
            if (watcher.watcher.CheckModified()) {
                if (!anyReloaded) {
                    vkDeviceWaitIdle(ctx.Device());
                    anyReloaded = true;
                }
                watcher.reloadCallback();
            }
        }
    }
}

auto RenderContext::CreateTexture(const void* data, uint32_t width, uint32_t height, bool isSRGB) -> std::expected<uint32_t, Error> {
    return _impl->CreateTextureInternal(data, width, height, isSRGB);
}

auto RenderContext::CreateTextureCube(const void* const* faceData, uint32_t width, uint32_t height) -> std::expected<uint32_t, Error> {
    return _impl->CreateTextureCubeInternal(faceData, width, height);
}

auto RenderContext::RegisterTexture(std::string_view name, uint32_t bindlessIndex, bool isSRGB) -> TextureHandle {
    return _impl->textureManager.RegisterUploaded(name, bindlessIndex, isSRGB);
}

namespace {
constexpr uint32_t kVolumetricNoiseSize = 64;

using Noise3 = std::array<float, 3>;

[[nodiscard]] float Fract(float x) {
    return x - std::floor(x);
}

[[nodiscard]] Noise3 Fract(Noise3 p) {
    return {Fract(p[0]), Fract(p[1]), Fract(p[2])};
}

[[nodiscard]] Noise3 Floor(Noise3 p) {
    return {std::floor(p[0]), std::floor(p[1]), std::floor(p[2])};
}

[[nodiscard]] Noise3 Add(Noise3 a, Noise3 b) {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}

[[nodiscard]] Noise3 Mul(Noise3 a, float s) {
    return {a[0] * s, a[1] * s, a[2] * s};
}

[[nodiscard]] Noise3 MulAdd(Noise3 a, float s, Noise3 b) {
    return {a[0] * s + b[0], a[1] * s + b[1], a[2] * s + b[2]};
}

[[nodiscard]] Noise3 Wrap(Noise3 p, Noise3 period) {
    for (uint32_t i = 0; i < 3; ++i) {
        p[i] = std::fmod(std::fmod(p[i], period[i]) + period[i], period[i]);
    }
    return p;
}

[[nodiscard]] float Lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

[[nodiscard]] float TileableHash3(Noise3 p, const Noise3& period) {
    // Match the HLSL hash: p = frac(p * 0.1031); wrap must happen on the
    // integer lattice point BEFORE the multiply so adjacent tile edges map to
    // the same fractional hash cell.
    p = Fract(Mul(Wrap(p, period), 0.1031F));
    const Noise3 shifted {p[1] + 33.33F, p[2] + 33.33F, p[0] + 33.33F};
    p = MulAdd(p, 1.0F, shifted);
    return Fract((p[0] + p[1]) * p[2]);
}

[[nodiscard]] float TileableNoise3(Noise3 p, const Noise3& period) {
    const Noise3 ip = Floor(p);
    const Noise3 fp = Fract(p);
    const Noise3 u  = {fp[0] * fp[0] * (3.0F - 2.0F * fp[0]), fp[1] * fp[1] * (3.0F - 2.0F * fp[1]), fp[2] * fp[2] * (3.0F - 2.0F * fp[2])};

    const auto  at   = [&](float ox, float oy, float oz) { return TileableHash3(Add(ip, {ox, oy, oz}), period); };
    const float n000 = at(0, 0, 0);
    const float n100 = at(1, 0, 0);
    const float n010 = at(0, 1, 0);
    const float n110 = at(1, 1, 0);
    const float n001 = at(0, 0, 1);
    const float n101 = at(1, 0, 1);
    const float n011 = at(0, 1, 1);
    const float n111 = at(1, 1, 1);

    const float r00 = Lerp(n000, n100, u[0]);
    const float r10 = Lerp(n010, n110, u[0]);
    const float r01 = Lerp(n001, n101, u[0]);
    const float r11 = Lerp(n011, n111, u[0]);
    return Lerp(Lerp(r00, r10, u[1]), Lerp(r01, r11, u[1]), u[2]);
}

[[nodiscard]] float TileableFbm3(Noise3 p) {
    // Each octave doubles frequency; its tile period halves accordingly and
    // must still divide the 64-cell texture so the composite is seamless.
    const Noise3 periods[3] = {
        {64.0F, 64.0F, 64.0F},
        {32.0F, 32.0F, 32.0F},
        {16.0F, 16.0F, 16.0F},
    };
    float value = 0.0F;
    float amp   = 0.5F;
    for (uint32_t octave = 0; octave < 3; ++octave) {
        value += amp * TileableNoise3(p, periods[octave]);
        p = Mul(p, 2.0F);
        amp *= 0.5F;
    }
    // Match the original procedural FBM, which summed octave amplitudes
    // without normalizing by their total.
    return value;
}
} // namespace

auto RenderContext::Impl::InitializeVolumetricNoiseTexture() noexcept -> std::expected<void, Error> {
    constexpr VkFormat kFormat = VK_FORMAT_R8G8B8A8_UNORM;
    constexpr uint32_t kCount  = kVolumetricNoiseSize * kVolumetricNoiseSize * kVolumetricNoiseSize;
    const size_t       bytes   = static_cast<size_t>(kCount) * 4;

    std::vector<uint8_t> pixels(bytes);
    for (uint32_t z = 0; z < kVolumetricNoiseSize; ++z) {
        for (uint32_t y = 0; y < kVolumetricNoiseSize; ++y) {
            for (uint32_t x = 0; x < kVolumetricNoiseSize; ++x) {
                const float  n   = TileableFbm3({static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F, static_cast<float>(z) + 0.5F});
                const auto   v   = static_cast<uint8_t>(std::clamp(n * 255.0F, 0.0F, 255.0F));
                const size_t idx = static_cast<size_t>((z * kVolumetricNoiseSize + y) * kVolumetricNoiseSize + x) * 4;
                pixels[idx + 0]  = v;
                pixels[idx + 1]  = v;
                pixels[idx + 2]  = v;
                pixels[idx + 3]  = 255;
            }
        }
    }

    auto imageRes = Vk::ImageBuilder {}
                        .Type(VK_IMAGE_TYPE_3D)
                        .Format(kFormat)
                        .Dimensions(kVolumetricNoiseSize, kVolumetricNoiseSize, kVolumetricNoiseSize)
                        .Usage(VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)
                        .Build(allocator.Get());
    if (!imageRes) {
        return std::unexpected(imageRes.error());
    }
    volumetricNoiseImage = std::move(*imageRes);

    auto viewRes = Vk::CreateView3D<kFormat>(ctx.Device(), volumetricNoiseImage.Handle(), VK_IMAGE_ASPECT_COLOR_BIT, 1);
    if (!viewRes) {
        return std::unexpected(viewRes.error());
    }
    volumetricNoiseView     = std::move(*viewRes);
    volumetricNoiseViewInfo = {
        .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext            = nullptr,
        .flags            = 0,
        .image            = volumetricNoiseImage.Handle(),
        .viewType         = VK_IMAGE_VIEW_TYPE_3D,
        .format           = kFormat,
        .components       = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1},
    };

    auto staging = stagingRingBuffer.Allocate(bytes);
    if (staging.mappedData == nullptr) {
        return std::unexpected(Vk::StagingError::MemoryMappingFailed);
    }
    std::memcpy(staging.mappedData, pixels.data(), bytes);

    Vk::ExecuteImmediate(ctx, graphicsCmdRing, stagingRingBuffer, [&](VkCommandBuffer cmd) {
        Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(cmd, volumetricNoiseImage.Handle());

        const VkBufferImageCopy2 region = {
            .sType             = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
            .pNext             = nullptr,
            .bufferOffset      = staging.offset,
            .bufferRowLength   = 0,
            .bufferImageHeight = 0,
            .imageSubresource  = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1},
            .imageOffset       = {0, 0, 0},
            .imageExtent       = {kVolumetricNoiseSize, kVolumetricNoiseSize, kVolumetricNoiseSize},
        };
        Vk::CopyBufferToImage<1>(cmd, staging.buffer, volumetricNoiseImage.Handle(), {region});
        Vk::TransitionLayout<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, volumetricNoiseImage.Handle());
    });

    Vk::Debug::SetImageName(ctx, volumetricNoiseImage.Handle(), "Volumetric.Noise3D");
    return {};
}

auto RenderContext::Impl::InitializeBlueNoiseTexture() -> std::expected<void, Error> {
    // Single-mip on purpose. A noise texture must never be mipmapped: every
    // level averages neighbouring texels toward the mean, so the high-frequency
    // content the dither depends on is destroyed exactly where a filter would
    // reach for it. The shader always samples LOD 0.
    constexpr VkFormat kFormat = VK_FORMAT_R8G8B8A8_UNORM;

    int width = 0, height = 0, channels = 0;
    unsigned char* pixels = stbi_load_from_memory(
        Resource::blue_noise_png.data(), static_cast<int>(Resource::blue_noise_png.size()), &width, &height, &channels, 4
    );
    if (pixels == nullptr || width <= 0 || height <= 0) {
        if (pixels != nullptr) {
            stbi_image_free(pixels);
        }
        return std::unexpected(Error {BlueNoiseError::DecodeFailed});
    }

    const uint32_t w     = static_cast<uint32_t>(width);
    const uint32_t h     = static_cast<uint32_t>(height);
    const size_t   bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;

    auto imageRes = Vk::ImageBuilder {}
                        .Texture2D(w, h, kFormat, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 1)
                        .Build(allocator.Get());
    if (!imageRes) {
        stbi_image_free(pixels);
        return std::unexpected(imageRes.error());
    }

    auto staging = stagingRingBuffer.Allocate(bytes);
    if (staging.mappedData == nullptr) {
        stbi_image_free(pixels);
        return std::unexpected(Vk::StagingError::MemoryMappingFailed);
    }
    std::memcpy(staging.mappedData, pixels, bytes);
    stbi_image_free(pixels);

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

    auto blueNoiseIdx = AdoptBindlessTexture(std::move(image), std::move(view), kFormat, 1, false);
    if (!blueNoiseIdx) {
        return std::unexpected(blueNoiseIdx.error());
    }
    blueNoiseTexIdx = *blueNoiseIdx;

    ZHLN::Log("[BlueNoise] LDR_RGBA_0 bound as bindless texture {} ({}x{}, single mip).", blueNoiseTexIdx, w, h);
    return {};
}

void RenderContext::Impl::WriteVolumetricNoiseDescriptor() noexcept {
    if (!volumetricNoiseView.Valid()) {
        return;
    }
    for (uint32_t frame = 0; frame < 2; ++frame) {
        Vk::TextureHandle slot {volumetricFogInjectPass.heapBindings.slotBase[1] + frame};
        heapManager.WriteImage(slot, volumetricNoiseViewInfo, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

auto RenderContext::Impl::CreateTextureInternal(const void* data, uint32_t width, uint32_t height, bool isSRGB) -> std::expected<uint32_t, Error> {
    auto* const  device    = ctx.Device();
    const size_t imageSize = static_cast<size_t>(width) * height * 4;
    uint32_t     mipLevels = std::bit_width(std::max(width, height));

    VkFormat          format = isSRGB ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    VkImageUsageFlags usage  = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    return Vk::ImageBuilder {}
        .Texture2D(width, height, format, usage, mipLevels)
        .Build(allocator.Get())
        .and_then([&, device, width, height, isSRGB, mipLevels, data, imageSize](auto&& gpuImage) -> std::expected<uint32_t, Error> {
            auto stagingAlloc = stagingRingBuffer.Allocate(imageSize);
            std::memcpy(stagingAlloc.mappedData, data, imageSize);

            Vk::ExecuteImmediate(ctx, graphicsCmdRing, stagingRingBuffer, [&](VkCommandBuffer cmd) -> void {
                Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(cmd, gpuImage.Handle());

                Vk::CopyBufferToImage(
                    cmd, {.buffer           = stagingAlloc.buffer,
                          .image            = gpuImage.Handle(),
                          .layout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          .width            = width,
                          .height           = height,
                          .buffer_offset    = stagingAlloc.offset,
                          .mip_level        = 0,
                          .base_array_layer = 0}
                );

                Vk::GenerateMipmaps(cmd, gpuImage.Handle(), width, height);
            });

            auto view_res = isSRGB ? Vk::CreateView<VK_FORMAT_R8G8B8A8_SRGB>(device, gpuImage.Handle(), VK_IMAGE_ASPECT_COLOR_BIT, mipLevels) :
                                     Vk::CreateView<VK_FORMAT_R8G8B8A8_UNORM>(device, gpuImage.Handle(), VK_IMAGE_ASPECT_COLOR_BIT, mipLevels);
            if (!view_res) {
                return std::unexpected(view_res.error());
            }
            auto gpuView = std::move(*view_res);

            const auto index = AdoptBindlessTexture(std::forward<decltype(gpuImage)>(gpuImage), std::move(gpuView), format, mipLevels, false);
            if (index) {
                Vk::Debug::SetImageName(ctx, textureImages.back().Handle(), std::format("BindlessTexture{:03}", *index));
            }
            return index;
        });
}

auto RenderContext::Impl::CreateTextureCubeInternal(const void* const* faceData, uint32_t width, uint32_t height) -> std::expected<uint32_t, Error> {
    auto* const       device   = ctx.Device();
    const size_t      faceSize = static_cast<size_t>(width) * height * 4;
    VkImageUsageFlags usage    = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    return Vk::ImageBuilder {}
        .TextureCube(width, VK_FORMAT_R8G8B8A8_UNORM, usage, 1)
        .Build(allocator.Get())
        .and_then([&, device, width, height, faceData, faceSize](auto&& gpuImage) -> std::expected<uint32_t, Error> {
            auto stagingAlloc = stagingRingBuffer.Allocate(faceSize * 6);
            for (uint32_t i = 0; i < 6; ++i) {
                std::memcpy(static_cast<char*>(stagingAlloc.mappedData) + (i * faceSize), faceData[i], faceSize);
            }

            Vk::ExecuteImmediate(ctx, graphicsCmdRing, stagingRingBuffer, [&](VkCommandBuffer cmd) -> void {
                Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(cmd, gpuImage.Handle());

                auto regions = Vk::CreateCopyRegions<6>(stagingAlloc.offset, faceSize, {.width = width, .height = height, .depth = {}});
                Vk::CopyBufferToImage(cmd, stagingAlloc.buffer, gpuImage.Handle(), regions);

                Vk::TransitionLayout<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, gpuImage.Handle());
            });

            auto cube_view_res = Vk::CreateViewCube<VK_FORMAT_R8G8B8A8_UNORM>(device, gpuImage.Handle(), 1);
            if (!cube_view_res) {
                return std::unexpected(cube_view_res.error());
            }
            auto gpuView = std::move(*cube_view_res);

            const auto index = AdoptBindlessTexture(std::forward<decltype(gpuImage)>(gpuImage), std::move(gpuView), VK_FORMAT_R8G8B8A8_UNORM, 1, true);
            if (index) {
                std::array<char, 32> buf {};
                Vk::Debug::SetImageName(ctx, textureImages.back().Handle(), FormatTo(buf, "BindlessCubeTexture{:03}", *index));
            }
            return index;
        });
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

auto RenderContext::Impl::CreateGPUBuffer(size_t size, const void* data, VkBufferUsageFlags functionalUsage) const
    -> std::expected<std::pair<Vk::Buffer, VkDeviceAddress>, Error> {
    VkBufferUsageFlags usage = functionalUsage | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    if (rtCtx.Valid()) {
        usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    }

    bool diffQueue = ctx.PhysicalInfo().graphics_family != ctx.PhysicalInfo().transfer_family;

    return Vk::Buffer::Create(allocator.Get(), size, usage, VMA_MEMORY_USAGE_GPU_ONLY).transform([&, size, data, diffQueue](auto&& gpu_buf) -> auto {
        auto stagingAlloc = transferRingBuffer.Allocate(size);

        if (data != nullptr) {
            std::memcpy(stagingAlloc.mappedData, data, size);
        } else {
            std::memset(stagingAlloc.mappedData, 0, size);
        }

        Vk::ExecuteImmediate<Vk::QueueType::Transfer>(ctx, transferCmdRing, transferRingBuffer, [&](VkCommandBuffer cmd) -> void {
            Vk::CopyRingBuffer(cmd, stagingAlloc, gpu_buf, size);
            if (diffQueue) {
                auto [release, acquire] = Vk::BufferQueueBarrier::Create(
                    {.buffer           = gpu_buf.Handle(),
                     .size             = size,
                     .src_queue_family = ctx.PhysicalInfo().transfer_family,
                     .dst_queue_family = ctx.PhysicalInfo().graphics_family,
                     .src_stage        = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     .src_access       = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     .dst_stage        = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                     .dst_access       = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT}
                );

                Vk::BufferBarrier(cmd, release);

                ZHLN::Lock(pendingAcquires.mutex, [&] -> void { pendingAcquires.buffers.push_back(acquire); });
            }
        });

        VkDeviceAddress address = Vk::GetBufferAddress(ctx.Device(), gpu_buf.Handle());
        return std::make_pair(std::forward<decltype(gpu_buf)>(gpu_buf), address);
    });
}

auto RenderContext::CreateSkinnedScratchBuffer(uint32_t vertexCount) -> BufferHandle {
    size_t size = (vertexCount * sizeof(VertexPosition)) + (vertexCount * sizeof(VertexAttributes));

    // Add ray tracing input read flag if context is valid
    VkBufferUsageFlags usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (_impl->rtCtx.Valid()) {
        usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    }

    return Vk::Buffer::Create(_impl->allocator.Get(), size, usage, VMA_MEMORY_USAGE_GPU_ONLY)
        .transform([this, vertexCount](auto&& gpu_buf) -> auto {
            VkDeviceAddress address = Vk::GetBufferAddress(_impl->ctx.Device(), gpu_buf.Handle());
            auto            handle  = _impl->meshPool.Create(std::forward<decltype(gpu_buf)>(gpu_buf), vertexCount, address);

            // Register RT Context with the scratch mesh for automatic lifecycle cleanup
            if (_impl->rtCtx.Valid()) {
                if (auto* nativeMesh = _impl->meshPool.Resolve(handle).value_or(nullptr)) {
                    nativeMesh->rtCtx  = &_impl->rtCtx;
                    nativeMesh->device = _impl->ctx.Device();
                }
            }
            return handle;
        })
        .value_or(BufferHandle::Invalid);
}

void RenderContext::Impl::BuildOrUpdateSkinnedBLAS(VkCommandBuffer cmd, const DrawCommand& drawCmd, NativeMesh* scratchMesh) const {
    if (!rtCtx.Valid() || scratchMesh == nullptr || drawCmd.posMesh == nullptr) {
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
    rtCtx.GetBLASSizes(geom, primitiveCount, sizes);

    if (scratchMesh->blas == VK_NULL_HANDLE) {
        auto blasBufOpt = Vk::Buffer::Create(
            allocator.Get(), sizes.acceleration_structure_size,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_GPU_ONLY
        );
        if (!blasBufOpt) {
            return;
        }
        scratchMesh->blasBuffer = std::move(*blasBufOpt);
        scratchMesh->blas = rtCtx.CreateAccelerationStructure(scratchMesh->blasBuffer.Handle(), sizes.acceleration_structure_size, ZHLN_AS_TYPE_BOTTOM_LEVEL);
        scratchMesh->blasAddress = rtCtx.GetAccelerationStructureAddress(scratchMesh->blas);
    }

    auto scratchBufOpt = Vk::Buffer::Create(
        allocator.Get(), sizes.build_scratch_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_GPU_ONLY
    );
    if (!scratchBufOpt) {
        return;
    }
    Vk::Buffer      scratchBuf     = std::move(*scratchBufOpt);
    VkDeviceAddress scratchAddress = ctx.BufferAddress(scratchBuf.Handle());

    // Record the build command directly onto the active graphics queue command buffer
    rtCtx.BuildBLAS(cmd, geom, scratchMesh->blas, scratchAddress, primitiveCount);
}

void RenderContext::UploadDebugVertices(const void* posData, size_t posSize, const void* attrData, size_t attrSize, uint32_t vertexCount) noexcept {
    auto* nativeMesh = _impl->meshPool.Resolve(_impl->frames.debugMeshHandles[_impl->frame_index]).value_or(nullptr);
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
    return _impl->frames.debugMeshHandles[_impl->frame_index];
}

void RenderContext::SubmitUI(
    const UIBatch*          batches,
    uint32_t                batchCount,
    const VertexPosition*   positions,
    const VertexAttributes* attributes,
    uint32_t                vertexCount
) noexcept {
    if (batchCount == 0 || vertexCount == 0 || _impl->current_cmd == VK_NULL_HANDLE) {
        return;
    }

    auto&  vbo         = _impl->frames.uiVbos[_impl->frame_index];
    size_t maxVertices = vbo.Size() / (sizeof(VertexPosition) + sizeof(VertexAttributes));

    uint32_t safeVertexCount = std::min(vertexCount, static_cast<uint32_t>(maxVertices));

    auto  mappedRegion = vbo.Map();
    auto* basePosPtr   = static_cast<VertexPosition*>(mappedRegion.data);
    auto* baseAttrPtr  = reinterpret_cast<VertexAttributes*>(basePosPtr + maxVertices);

    std::memcpy(basePosPtr, positions, safeVertexCount * sizeof(VertexPosition));
    std::memcpy(baseAttrPtr, attributes, safeVertexCount * sizeof(VertexAttributes));

    _impl->queues.uiBatches.reserve(batchCount);
    for (uint32_t i = 0; i < batchCount; ++i) {
        _impl->queues.uiBatches.push_back(batches[i]);
    }
}

void RenderContext::UpdateJointMatrices(uint32_t offset, const JPH::Mat44* matrices, uint32_t count) {
    if (count == 0) {
        return;
    }
    auto  mappedRegion = _impl->frames.jointBuffers[_impl->frame_index].Map();
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
std::expected<void, Error> RenderContext::Impl::ResizeShadowTargets(uint32_t resolution) noexcept {
    auto* device = ctx.Device();

    return Vk::WaitIdle(device).transform_error(
                                   [](auto) -> Error { return ShadowResolutionError::RecreationFailed; }
    ).and_then([&]() -> std::expected<void, Error> {
        auto sm_res = Vk::RenderTarget<VK_FORMAT_D32_SFLOAT>::Create(
            allocator, ctx, {.width = resolution, .height = resolution},
            {.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, .arrayLayers = RenderContext::Impl::NUM_CASCADES}
        );
        if (!sm_res) {
            return std::unexpected(sm_res.error());
        }
        graphResources.shadowMap = std::move(*sm_res);

        auto smp_res = Vk::RenderTarget<VK_FORMAT_D32_SFLOAT>::Create(
            allocator, ctx, {.width = resolution, .height = resolution},
            {.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, .arrayLayers = RenderContext::Impl::NUM_CASCADES}
        );
        if (!smp_res) {
            return std::unexpected(smp_res.error());
        }
        shadowMapPrev = std::move(*smp_res);

        shadowCascadeViews.clear();
        shadowCascadeViews.resize(RenderContext::Impl::NUM_CASCADES);
        shadowCascadeViewsPrev.clear();
        shadowCascadeViewsPrev.resize(RenderContext::Impl::NUM_CASCADES);
        for (uint32_t i = 0; i < RenderContext::Impl::NUM_CASCADES; ++i) {
            auto view_res = Vk::CreateView2DArray<VK_FORMAT_D32_SFLOAT>(ctx.Device(), graphResources.shadowMap.image.Handle(), i, 1);
            if (!view_res) {
                return std::unexpected(view_res.error());
            }
            shadowCascadeViews[i] = std::move(*view_res);

            auto prev_res = Vk::CreateView2DArray<VK_FORMAT_D32_SFLOAT>(ctx.Device(), shadowMapPrev.image.Handle(), i, 1);
            if (!prev_res) {
                return std::unexpected(prev_res.error());
            }
            shadowCascadeViewsPrev[i] = std::move(*prev_res);
        }

        Vk::ExecuteImmediate(ctx, graphicsCmdRing, [&](VkCommandBuffer cmd) -> void {
            Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(
                cmd, graphResources.shadowMap.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT
            );

            Vk::TransitionLayout<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
                cmd, graphResources.shadowMap.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT
            );

            Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL>(
                cmd, shadowMapPrev.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT
            );

            Vk::TransitionLayout<VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
                cmd, shadowMapPrev.image.Handle(), VK_IMAGE_ASPECT_DEPTH_BIT
            );
        });

        ZHLN::Log("Shadow map dynamically resized on the GPU to {}x{}", resolution, resolution);
        return {};
    });
}

auto RenderContext::SetShadowResolution(uint32_t resolution) -> std::expected<void, Error> {
    auto* impl = _impl.get();

    return impl->ResizeShadowTargets(resolution).transform([&]() -> void {
        impl->settings.shadows.resolution = resolution;
        // Keep the informational preset tier honest after an out-of-band change.
        impl->settings.qualityPreset = impl->settings.DetectPreset();
    });
}

void RenderContext::Impl::ApplySettings(GraphicsSettings&& incoming) noexcept {
    // --- Delta detection -----------------------------------------------------
    // Reactive consequences key off specific fields; plain knob changes
    // simply become part of the canonical state consumed by the next frame.
    const QualityLevel previousTier = settings.qualityPreset;

    if (incoming.shadows.resolution != settings.shadows.resolution) {
        if (ResizeShadowTargets(incoming.shadows.resolution)) {
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
        ZHLN::Log("Graphics quality tier: {} -> {}", ToString(previousTier), ToString(settings.qualityPreset));
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

    return std::expected<void, Error>()
        .and_then([&]() -> std::expected<BuildContext, Error> {
            if (!impl->rtCtx.Valid()) {
                return std::unexpected(RenderFeatureError::FeatureNotSupported);
            }
            return impl->meshPool.Resolve(mesh.posBuffer)
                .transform_error([](auto err) -> Error { return err; })
                .and_then([&](auto* pos) -> std::expected<BuildContext, Error> {
                    auto* index = (mesh.indexBuffer != BufferHandle::Invalid) ? impl->meshPool.Resolve(mesh.indexBuffer).value_or(nullptr) : nullptr;
                    return BuildContext {
                        .posMesh = pos, .indexMesh = index, .geom = {}, .primitiveCount = {}, .sizes = {}, .blasBuffer = {}, .blas = nullptr, .scratch = {}
                    };
                });
        })
        .and_then([&](BuildContext b) -> std::expected<BuildContext, Error> {
            b.geom = {
                .vertex_data   = b.posMesh->vboAddress,
                .vertex_stride = sizeof(VertexPosition),
                .max_vertex    = mesh.vertexCount > 0 ? mesh.vertexCount - 1 : 0,
                .vertex_format = VK_FORMAT_R32G32B32_SFLOAT,
                .index_data    = (b.indexMesh != nullptr) ? b.indexMesh->vboAddress : 0,
                .index_type    = (b.indexMesh != nullptr) ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_NONE_KHR
            };
            b.primitiveCount = (b.indexMesh != nullptr) ? mesh.indexCount / 3 : mesh.vertexCount / 3;

            impl->rtCtx.GetBLASSizes(b.geom, b.primitiveCount, b.sizes);

            return Vk::Buffer::Create(
                       impl->allocator.Get(), b.sizes.acceleration_structure_size,
                       VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_GPU_ONLY
            )
                .transform([b = std::move(b)](auto&& buffer) mutable -> auto {
                    b.blasBuffer = std::forward<decltype(buffer)>(buffer);
                    return std::move(b);
                });
        })
        .and_then([&](BuildContext b) -> std::expected<BuildContext, Error> {
            b.blas = impl->rtCtx.CreateAccelerationStructure(b.blasBuffer.Handle(), b.sizes.acceleration_structure_size, ZHLN_AS_TYPE_BOTTOM_LEVEL);
            if (b.blas == VK_NULL_HANDLE) {
                return std::unexpected(Vk::VulkanCallError::VulkanCallFailed);
            }

            return Vk::Buffer::Create(
                       impl->allocator.Get(), b.sizes.build_scratch_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                       VMA_MEMORY_USAGE_GPU_ONLY
            )
                .transform([b = std::move(b)](auto&& buffer) mutable -> auto {
                    b.scratch = std::forward<decltype(buffer)>(buffer);
                    return std::move(b);
                });
        })
        .and_then([&](BuildContext b) -> std::expected<void, Error> {
            Vk::CommandPool<Vk::QueueType::Graphics> tempPool(impl->ctx.Device(), impl->ctx.PhysicalInfo().graphics_family);
            auto                                     alloc_res = tempPool.Allocate(1);
            if (!alloc_res) [[unlikely]] {
                return std::unexpected(alloc_res.error());
            }

            VkCommandBuffer tempCmd = tempPool[0];
            {
                Vk::CommandBufferGuard guard(tempCmd);
                impl->pendingAcquires.Drain(tempCmd);

                Vk::MemoryBarrier(
                    tempCmd, Vk::BarrierStage::Copy, Vk::BarrierAccess::TransferWrite,
                    Vk::BarrierStage::AccelerationStructureBuild, Vk::BarrierAccess::AccelerationStructureRead
                );
                impl->rtCtx.BuildBLAS(tempCmd, b.geom, b.blas, Vk::GetBufferAddress(impl->ctx.Device(), b.scratch.Handle()), b.primitiveCount);
            }

            return Vk::SubmitAndWait(
                       impl->ctx.GraphicsQueue(), tempCmd, impl->transferRingBuffer.GetSemaphore(), impl->transferRingBuffer.GetCurrentValue(),
                       VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR
            )
                .transform_error([](auto err) -> Error { return err; })
                .transform([&]() -> void {
                    b.posMesh->blasBuffer  = std::move(b.blasBuffer);
                    b.posMesh->blas        = b.blas;
                    b.posMesh->blasAddress = impl->rtCtx.GetAccelerationStructureAddress(b.blas);
                    b.posMesh->device      = impl->ctx.Device();
                    b.posMesh->rtCtx       = &impl->rtCtx;
                });
        });
}

void RenderContext::Impl::RegisterShaderWatcher(const char* path, std::function<void()> callback) {
    if constexpr (isDev) {
        shaderWatchers.push_back({.path = path, .watcher = FileWatcher(path), .reloadCallback = std::move(callback)});
    }
}

auto RenderContext::BakeProceduralTexture(uint32_t width, uint32_t height, uint32_t variantIdx, float scale, float randomness)
    -> std::expected<uint32_t, Error> {
    return _impl->BakeProceduralTexture(width, height, variantIdx, scale, randomness, 0.0f);
}

auto RenderContext::CreateProceduralTexture(std::string_view name, uint32_t width, uint32_t height, bool isSRGB, const uint32_t* pixels) -> TextureHandle {
    return _impl->textureManager.CreateProcedural(*this, name, width, height, isSRGB, pixels);
}

enum class ScreenshotError : uint8_t {
    FileOpenFailed ZHLN_ANNOTATION(ZHLN::Description<"Failed to open screenshot output file for writing">{}) = 1,
    ReadbackFailed ZHLN_ANNOTATION(ZHLN::Description<"GPU readback buffer mapping failed">{}),
};

auto RenderContext::CaptureScreenshotPPM(std::string_view outputPath) noexcept -> std::expected<void, Error> {
    auto* const impl = _impl.get();

    if (!impl->presentation.swapchain.Valid()) {
        const auto extent     = impl->presentation.headlessColorTarget.extent;
        const auto imageBytes = static_cast<size_t>(extent.width) * extent.height * 4u;

        auto stagingRes = Vk::Buffer::Create(impl->allocator.Get(), imageBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU);
        if (!stagingRes) {
            return std::unexpected(stagingRes.error());
        }
        auto stagingBuffer = std::move(*stagingRes);

        Vk::ExecuteImmediate(impl->ctx, impl->graphicsCmdRing, [&](VkCommandBuffer cmd) -> void {
            auto* const targetImg = impl->presentation.headlessColorTarget.image.Handle();

            Vk::TransitionLayout<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL>(cmd, targetImg);
            Vk::CopyImageToBuffer(cmd, targetImg, stagingBuffer.Handle(), extent);
            Vk::TransitionLayout<VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(cmd, targetImg);
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

        const auto* rgba = mapped.As<const uint8_t>();
        for (size_t i = 0; i < static_cast<size_t>(extent.width) * extent.height; ++i) {
            ofs.put(static_cast<char>(rgba[i * 4 + 0]));
            ofs.put(static_cast<char>(rgba[i * 4 + 1]));
            ofs.put(static_cast<char>(rgba[i * 4 + 2]));
        }
        ofs.close();

        ZHLN::Log("[Test Capture] Rendered frame written to: {}", outputPath);
        return {};
    }

    const auto extent = impl->graphResources.hdrSceneColor.extent;

    const size_t imageBytes = static_cast<size_t>(extent.width) * extent.height * sizeof(uint16_t) * 4;

    // 1. Allocate host-visible readback buffer via engine Allocator
    auto stagingRes = Vk::Buffer::Create(impl->allocator.Get(), imageBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU);
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

void RenderContext::Impl::RegisterPipeline(const PipelineRegistration& reg) noexcept {
    reg.build();
    if constexpr (isDev) {
        for (const auto* path: reg.watchPaths) {
            RegisterShaderWatcher(path, reg.build);
        }
    }
}

std::expected<void, Error> RenderContext::Impl::ValidateSlangTypeLayouts() noexcept {
    const void*  spirv   = Resource::gpu_abi_comp.data();
    const size_t spirvSz = Resource::gpu_abi_comp.size();

    std::expected<void, Error> result {};
    Reflect::ForEachNestedType<GPUTypes>([&]<typename Group>() {
        Reflect::ForEachNestedType<Group>([&]<typename T>() {
            if (!result) {
                return;
            }
            result = Vk::ReflectTypeLayout(spirv, spirvSz, Reflect::AnnotatedName<T>())
                         .and_then([](const Vk::SlangTypeLayout& layout) -> std::expected<void, Error> {
                             if (layout.size != sizeof(T)) {
                                 return std::unexpected(Vk::SpirvLayoutError::TypeSizeMismatch);
                             }
                             return {};
                         });
        });
    });
    return result.and_then([&]() -> std::expected<void, Error> { return Vk::ReflectHeapPushDataLayout(spirv, spirvSz).transform([](const auto&) {}); });
}

} // namespace ZHLN
