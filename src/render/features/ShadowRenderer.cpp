// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "features/ShadowRenderer.hpp"
#include "RenderInternal.hpp"
#include <ShaderBindings.hpp>
#include <Zahlen/Log.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace ZHLN {

auto ShadowRenderer::InitResources(RenderContext::Impl& impl) -> std::expected<void, ErrorCode> {
    return CreatePerFrame(
               impl.allocator, sizeof(VkDrawIndirectCommand) * RenderContext::Impl::kGpuCullingMaxInstances * 8, Vk::BufferUsage::Indirect,
               Vk::MemoryUsage::CPUToGPU
    )
        .transform([this](auto&& buffers) -> void { _indirectCommands = std::forward<decltype(buffers)>(buffers); });
}

auto ShadowRenderer::CascadePipeline() const noexcept -> VkPipeline {
    return _cascadePipeline.Get();
}

auto ShadowRenderer::CascadeLayout() const noexcept -> VkPipelineLayout {
    return _cascadeLayout;
}

auto ShadowRenderer::CascadeMeshPipeline() const noexcept -> VkPipeline {
    return _cascadeMeshPipeline.Get();
}

auto ShadowRenderer::PunctualPipeline() const noexcept -> VkPipeline {
    return _punctualPipeline.Get();
}

auto ShadowRenderer::PunctualLayout() const noexcept -> VkPipelineLayout {
    return _punctualLayout;
}

auto ShadowRenderer::IndirectCommands(uint32_t frameIndex) noexcept -> Vk::Buffer& {
    return _indirectCommands[frameIndex];
}

auto ShadowRenderer::IndirectCommands(uint32_t frameIndex) const noexcept -> const Vk::Buffer& {
    return _indirectCommands[frameIndex];
}

auto ShadowRenderer::CompileCascadePipelines(RenderContext::Impl& impl, VkDevice device, const ZHLN_ShaderDesc& vert, const ZHLN_ShaderDesc& frag)
    -> std::expected<void, ErrorCode> {
    _cascadeLayout = impl.emptyPipelineLayout;

    return Vk::ShaderStagesView::Create(vert, frag)
        .transform_error([](auto err) -> ErrorCode { return err; })
        .and_then([this, &impl, device](auto&& shaders) -> std::expected<void, ErrorCode> {
            return Vk::PipelineBuilder {}
                .Shaders(shaders)
                .Layout(impl.emptyPipelineLayout)
                .HeapMappings(&impl.sceneHeapMappings.info, &impl.sceneHeapMappings.info)
                .DepthOnly()
                .DepthFormat(VK_FORMAT_D32_SFLOAT)
                .ViewMask(kCascadeViewMask)
                .CullNone()
                .Cache(impl.pipelineCache.Get())
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

            auto pipeline = Vk::PipelineBuilder {}
                                .Shaders(*shaders)
                                .Layout(impl.emptyPipelineLayout)
                                .HeapMappings(&impl.sceneHeapMappings.info, &impl.sceneHeapMappings.info)
                                .DepthOnly()
                                .DepthFormat(VK_FORMAT_D32_SFLOAT)
                                .ViewMask(kCascadeViewMask)
                                .CullNone()
                                .Cache(impl.pipelineCache.Get())
                                .Build(device);
            if (!pipeline) {
                ZHLN::Log("[ShadowRenderer] Shadow mesh pipeline creation failed; cascades keep the vertex pipeline.");
                return {};
            }
            _cascadeMeshPipeline = std::move(*pipeline);
            return {};
        });
}

auto ShadowRenderer::CompilePunctualPipeline(RenderContext::Impl& impl, VkDevice device, const ZHLN_ShaderDesc& vert, const ZHLN_ShaderDesc& frag)
    -> std::expected<void, ErrorCode> {
    _punctualLayout = impl.emptyPipelineLayout;
    return Vk::ShaderStagesView::Create(vert, frag)
        .transform_error([](auto err) -> ErrorCode { return err; })
        .and_then([this, &impl, device](auto&& shaders) -> std::expected<void, ErrorCode> {
            return Vk::PipelineBuilder {}
                .Shaders(shaders)
                .Layout(impl.emptyPipelineLayout)
                .HeapMappings(&impl.sceneHeapMappings.info, &impl.sceneHeapMappings.info)
                .DepthOnly()
                .DepthFormat(VK_FORMAT_D32_SFLOAT)
                .ViewMask(kCubemapFaceMask)
                .CullNone()
                .Cache(impl.pipelineCache.Get())
                .Build(device)
                .transform_error([](auto) -> ErrorCode { return Vk::PipelineBuilderError::PipelineCreationFailed; })
                .transform([this](auto&& pipeline) -> auto { _punctualPipeline = std::forward<decltype(pipeline)>(pipeline); });
        });
}

auto ShadowRenderer::ComputeCascadeLightSpaceMatrix(
    const Camera&     cam,
    const JPH::Mat44& lightView,
    const JPH::Vec3&  sunDir,
    float             nearDist,
    float             farDist,
    float             aspect,
    float             tanHalfFov,
    uint32_t          shadowResolution
) noexcept -> JPH::Mat44 {
    const float hNear = 2.0f * tanHalfFov * nearDist;
    const float wNear = hNear * aspect;
    const float hFar  = 2.0f * tanHalfFov * farDist;
    const float wFar  = hFar * aspect;

    std::array<JPH::Vec3, 8> corners = {
        {{-wNear * 0.5f, hNear * 0.5f, -nearDist},
         {wNear * 0.5f, hNear * 0.5f, -nearDist},
         {wNear * 0.5f, -hNear * 0.5f, -nearDist},
         {-wNear * 0.5f, -hNear * 0.5f, -nearDist},
         {-wFar * 0.5f, hFar * 0.5f, -farDist},
         {wFar * 0.5f, hFar * 0.5f, -farDist},
         {wFar * 0.5f, -hFar * 0.5f, -farDist},
         {-wFar * 0.5f, -hFar * 0.5f, -farDist}}
    };

    const JPH::Mat44 invCamView = cam.GetViewMatrix().Inversed();
    for (auto& corner: corners) {
        corner = invCamView * corner;
    }

    JPH::Vec3 nearCenter = JPH::Vec3::sZero();
    JPH::Vec3 farCenter  = JPH::Vec3::sZero();
    for (int i = 0; i < 4; ++i) {
        nearCenter += corners[static_cast<size_t>(i)];
        farCenter += corners[4 + static_cast<size_t>(i)];
    }
    JPH::Vec3 center = (nearCenter + farCenter) * 0.125f;

    float radius = 0.0f;
    for (const auto& corner: corners) {
        radius = std::max(radius, (corner - center).Length());
    }
    radius = std::ceil(radius * 16.0f) / 16.0f;
    const float invTexels = 2.0f / static_cast<float>(shadowResolution);
    radius                = std::ceil(radius / invTexels) * invTexels;

    JPH::Vec3 centerLight   = lightView * center;
    const float texelsPerUnit = static_cast<float>(shadowResolution) / (radius * 2.0f);

    centerLight.SetX(std::floor(centerLight.GetX() * texelsPerUnit) / texelsPerUnit);
    centerLight.SetY(std::floor(centerLight.GetY() * texelsPerUnit) / texelsPerUnit);

    center = lightView.Inversed() * centerLight;

    float offset  = Shadows::BaseOffset;
    float farClip = Shadows::BaseDepth;

    if (farDist > 550.0f) {
        offset  = Shadows::FarOffset;
        farClip = Shadows::FarDepth;
    }

    const JPH::Vec3  cascadeLightPos  = center + sunDir * offset;
    const JPH::Mat44 cascadeLightView = Math::CreateLookAt(cascadeLightPos, center, JPH::Vec3::sAxisY());
    const JPH::Mat44 cascadeLightProj = Math::CreateOrtho(-radius, radius, -radius, radius, Shadows::NearClip, farClip);

    return cascadeLightProj * cascadeLightView;
}

}
