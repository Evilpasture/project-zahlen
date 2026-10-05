// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "RenderInternal.hpp"
#include <ShaderBindings.hpp>
#include <Zahlen/Core/Reflection/Structs.hpp>
#include "Resources.hpp"
#include <Zahlen/Error.hpp>
#include <cstdint>

namespace ZHLN {

auto RenderContext::Impl::BuildProceduralBakePipeline() -> std::expected<void, ErrorCode> {
    if (!proceduralBakeDescLayout.Build(
            ctx.Device(), Vk::CreateShaderDesc<Shaders::Modules::ProceduralBakeCS>(), VK_SHADER_STAGE_COMPUTE_BIT
        )) {
        return std::unexpected(Vk::PipelineBuilderError::PipelineCreationFailed);
    }

    const auto source = Vk::MakeStageSource<Shaders::Modules::ProceduralBakeCS>();
    const auto loaded = LoadShaderData(source);
    const ZHLN_ShaderDesc shaderDesc = Vk::CreateShaderDesc(loaded.Code(), source.entryPoint);

    struct BakeSpec {
        int bakeType = 0;
    };

    Vk::Specialization<BakeSpec> bakeSpec;
    Reflect::ForEachFieldInfo<BakeSpec>(bakeSpec);

    const std::array variants  = {BakeSpec {.bakeType = 0}, BakeSpec {.bakeType = 1}, BakeSpec {.bakeType = 2}};
    const auto       specInfos = bakeSpec.Infos(variants);

    auto build_res = proceduralBakePass.BuildHeapVariants(
        ctx.Device(), shaderDesc, specInfos, bakeHeapBindings.GetInfo(), bakeHeapBindings.indexPushOffset, pipelineCache.Get()
    );
    if (!build_res) {
        return std::unexpected(build_res.error());
    }

    ZHLN::Log(
        "[Shader] GPU Procedural Bake Compute Pipeline initialized with specialization "
        "variants."
    );
    return {};
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

auto RenderContext::Impl::BakeProceduralTexture(uint32_t width, uint32_t height, uint32_t variantIdx, float scale, float randomness, float distortion)
    -> std::expected<TextureHandle, ErrorCode> {
    auto* const device = ctx.Device();

    return Vk::ImageBuilder {}
        .Texture2D(width, height, VK_FORMAT_R8G8B8A8_UNORM, Vk::ImageUsage::Storage | Vk::ImageUsage::Sampled, 1)
        .Build(allocator)
        .and_then([&, device, width, height, variantIdx, scale, randomness, distortion](auto&& gpuImage) -> std::expected<TextureHandle, ErrorCode> {
            defer _([&] { allocator.DestroyImage(gpuImage); });
            auto view_res = Vk::ImageView::Create<VK_FORMAT_R8G8B8A8_UNORM>(device, gpuImage.Handle(), VK_IMAGE_ASPECT_COLOR_BIT, 1);
            if (!view_res) {
                return std::unexpected(view_res.error());
            }
            auto writeView = std::move(*view_res);

            heapManager.BeginImmediate();
            const Vk::HeapBlockBase block = heapManager.WriteHeapParameters<Shaders::Bake>(
                ctx, bakeHeapBindings, Vk::Slot<"outTexture">(writeView)
            );

            Vk::ExecuteImmediate(ctx, graphicsCmdRing, [&](VkCommandBuffer cmd) -> auto {
                heapManager.BindHeaps(cmd);

                Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL>(cmd, gpuImage.Handle());

                proceduralBakePass.BindVariant(cmd, variantIdx);
                Vk::PushHeapData<Shaders::Modules::ProceduralBakeCS>(
                    cmd, BakePush {.width = width, .height = height, .scale = scale, .randomness = randomness, .distortion = distortion}
                );
                Vk::PushHeapIndex(cmd, bakeHeapBindings.indexPushOffset, block.slot);
                proceduralBakePass.DispatchThreads(cmd, width, height, 1);

                Vk::TransitionLayout<VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, gpuImage.Handle());
            });

            return textureManager.AdoptTexture(std::forward<decltype(gpuImage)>(gpuImage), std::move(writeView), width, height);
        });
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

}
