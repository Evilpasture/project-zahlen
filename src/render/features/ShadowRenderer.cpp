// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "render/features/ShadowRenderer.hpp"
#include <Zahlen/Math/Vec3.hpp>
#include "render/RenderInternal.hpp"
#include <ShaderBindings.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math/Matrix.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace ZHLN {

auto ShadowRenderer::InitResources(RenderContext::Impl& impl) -> std::expected<void, ErrorCode> {
    constexpr VkDeviceSize kCommands =
        sizeof(VkDrawIndirectCommand) * (RenderContext::Impl::kGpuCullingMaxInstances + ShadowAtlas::kMaxLights * kAtlasInstancesPerLight);
    return CreatePerFrame(impl.allocator, kCommands, Vk::BufferUsage::Indirect, Vk::MemoryUsage::CPUToGPU)
        .transform([this, &impl](auto&& buffers) -> void {
            DestroyResources(impl.allocator);
            _indirectCommands = std::forward<decltype(buffers)>(buffers);
        });
}

void ShadowRenderer::DestroyResources(Vk::Allocator& allocator) noexcept {
    for (auto& buffer: _indirectCommands) {
        allocator.DestroyBuffer(buffer);
    }
}

auto ShadowRenderer::CascadePipeline() const noexcept -> VkPipeline {
    return _cascadePipeline.Get();
}

auto ShadowRenderer::CascadeMeshPipeline() const noexcept -> VkPipeline {
    return _cascadeMeshPipeline.Get();
}

auto ShadowRenderer::AtlasPipeline() const noexcept -> VkPipeline {
    return _atlasPipeline.Get();
}

auto ShadowRenderer::IndirectCommands(uint32_t frameIndex) noexcept -> Vk::Buffer& {
    return _indirectCommands[frameIndex];
}

auto ShadowRenderer::IndirectCommands(uint32_t frameIndex) const noexcept -> const Vk::Buffer& {
    return _indirectCommands[frameIndex];
}

auto ShadowRenderer::CompileCascadePipelines(RenderContext::Impl& impl, VkDevice device, const Vk::ShaderDesc& vert, const Vk::ShaderDesc& frag)
    -> std::expected<void, ErrorCode> {
    return Vk::ShaderStagesView::Create(vert, frag)
        .transform_error([](auto err) -> ErrorCode { return err; })
        .and_then([this, &impl, device](auto&& shaders) -> std::expected<void, ErrorCode> {
            Vk::HeapMappingArena mappingArena {impl.heapManager};
            const auto*           sceneMappings = mappingArena.Map(impl.sceneHeapMappings);
            return Vk::PipelineBuilder {}
                .Shaders(shaders)
                .HeapMappings(sceneMappings, sceneMappings)
                .DepthOnly()
                .DepthFormat(VK_FORMAT_D32_SFLOAT)
                .ViewMask(kCascadeViewMask)
                .CullNone()
                .Binaries(&impl.pipelineBinaries)
                .Build(device)
                .transform_error([](auto) -> ErrorCode { return Vk::PipelineBuilderError::PipelineCreationFailed; })
                .transform([this](auto&& pipeline) -> auto { _cascadePipeline = std::forward<decltype(pipeline)>(pipeline); });
        })
        .and_then([this, &impl, device]() -> std::expected<void, ErrorCode> {
            const bool multiviewMesh = impl.ctx.HasFeature<VkPhysicalDeviceMeshShaderFeaturesEXT>([](const VkPhysicalDeviceMeshShaderFeaturesEXT& f) -> bool {
                return f.multiviewMeshShader == VK_TRUE;
            });
            if (!impl.ctx.MeshShadersSupported() || !multiviewMesh) {
                return {};
            }

            auto shaders = Vk::ShaderStagesView::CreateMesh<
                Shaders::Modules::BasicTask, Shaders::Modules::BasicMeshShadow, Shaders::Modules::ShadowPS
            >();
            if (!shaders) {
                ZHLN::Log("[ShadowRenderer] Shadow mesh-stage creation failed; cascades keep the vertex pipeline.");
                return {};
            }

            Vk::HeapMappingArena mappingArena {impl.heapManager};
            const auto*           sceneMappings = mappingArena.Map(impl.sceneHeapMappings);
            auto pipeline = Vk::PipelineBuilder {}
                                .Shaders(*shaders)
                                .HeapMappings(sceneMappings, sceneMappings)
                                .DepthOnly()
                                .DepthFormat(VK_FORMAT_D32_SFLOAT)
                                .ViewMask(kCascadeViewMask)
                                .CullNone()
                                .Binaries(&impl.pipelineBinaries)
                                .Build(device);
            if (!pipeline) {
                ZHLN::Log("[ShadowRenderer] Shadow mesh pipeline creation failed; cascades keep the vertex pipeline.");
                return {};
            }
            _cascadeMeshPipeline = std::move(*pipeline);
            return {};
        });
}

auto ShadowRenderer::CompileAtlasPipeline(RenderContext::Impl& impl, VkDevice device, const Vk::ShaderDesc& vert, const Vk::ShaderDesc& frag)
    -> std::expected<void, ErrorCode> {
    return Vk::ShaderStagesView::Create(vert, frag)
        .transform_error([](auto err) -> ErrorCode { return err; })
        .and_then([this, &impl, device](auto&& shaders) -> std::expected<void, ErrorCode> {
            Vk::HeapMappingArena mappingArena {impl.heapManager};
            const auto*           sceneMappings = mappingArena.Map(impl.sceneHeapMappings);
            return Vk::PipelineBuilder {}
                .Shaders(shaders)
                .HeapMappings(sceneMappings, sceneMappings)
                .DepthOnly()
                .DepthFormat(VK_FORMAT_D32_SFLOAT)
                .CullNone()
                .Binaries(&impl.pipelineBinaries)
                .Build(device)
                .transform_error([](auto) -> ErrorCode { return Vk::PipelineBuilderError::PipelineCreationFailed; })
                .transform([this](auto&& pipeline) -> auto { _atlasPipeline = std::forward<decltype(pipeline)>(pipeline); });
        });
}

auto ShadowRenderer::ComputeCascadeLightSpaceMatrix(
    const Camera&     cam,
    const Mat4x4& lightView,
    const FVec3&  sunDir,
    float             nearDist,
    float             farDist,
    float             aspect,
    float             tanHalfFov,
    uint32_t          shadowResolution
) noexcept -> Mat4x4 {
    const float hNear = 2.0f * tanHalfFov * nearDist;
    const float wNear = hNear * aspect;
    const float hFar  = 2.0f * tanHalfFov * farDist;
    const float wFar  = hFar * aspect;

    std::array<FVec3, 8> corners = {
        {{-wNear * 0.5f, hNear * 0.5f, -nearDist},
         {wNear * 0.5f, hNear * 0.5f, -nearDist},
         {wNear * 0.5f, -hNear * 0.5f, -nearDist},
         {-wNear * 0.5f, -hNear * 0.5f, -nearDist},
         {-wFar * 0.5f, hFar * 0.5f, -farDist},
         {wFar * 0.5f, hFar * 0.5f, -farDist},
         {wFar * 0.5f, -hFar * 0.5f, -farDist},
         {-wFar * 0.5f, -hFar * 0.5f, -farDist}}
    };

    const Mat4x4 invCamView = Inverse(cam.GetViewMatrix());
    for (auto& corner: corners) {
        corner = TransformPoint(invCamView, corner);
    }

    FVec3 nearCenter = FVec3::Zero();
    FVec3 farCenter  = FVec3::Zero();
    for (int i = 0; i < 4; ++i) {
        nearCenter += corners[static_cast<size_t>(i)];
        farCenter += corners[4 + static_cast<size_t>(i)];
    }
    FVec3 center = (nearCenter + farCenter) * 0.125f;

    float radius = 0.0f;
    for (const auto& corner: corners) {
        radius = std::max(radius, (corner - center).Length());
    }
    radius = std::ceil(radius * 16.0f) / 16.0f;
    const float invTexels = 2.0f / static_cast<float>(shadowResolution);
    radius                = std::ceil(radius / invTexels) * invTexels;

    FVec3 centerLight   = TransformPoint(lightView, center);
    const float texelsPerUnit = static_cast<float>(shadowResolution) / (radius * 2.0f);

    centerLight.x = std::floor(centerLight.x * texelsPerUnit) / texelsPerUnit;
    centerLight.y = std::floor(centerLight.y * texelsPerUnit) / texelsPerUnit;

    center = TransformPoint(Inverse(lightView), centerLight);

    float offset  = Shadows::BaseOffset;
    float farClip = Shadows::BaseDepth;

    if (farDist > 550.0f) {
        offset  = Shadows::FarOffset;
        farClip = Shadows::FarDepth;
    }

    const FVec3  cascadeLightPos  = center + sunDir * offset;
    const Mat4x4 cascadeLightView = CreateLookAt(cascadeLightPos, center, {0.0f, 1.0f, 0.0f});
    const Mat4x4 cascadeLightProj = CreateOrtho(-radius, radius, -radius, radius, Shadows::NearClip, farClip);

    return cascadeLightProj * cascadeLightView;
}

}
