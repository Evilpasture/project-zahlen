// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "RenderInternal.hpp"
#include "Resources.hpp"
#include <ShaderBindings.hpp>
#include <Zahlen/Core/Reflection/Structs.hpp>
#include <Zahlen/Error.hpp>
#include <cstdint>
#include <span>

namespace ZHLN {

auto RenderContext::Impl::BuildProceduralBakePipeline() -> std::expected<void, ErrorCode> {
    if (!proceduralBakeDescLayout.Build(ctx.Device(), Vk::CreateShaderDesc<Shaders::Modules::ProceduralBakeCS>(), VK_SHADER_STAGE_COMPUTE_BIT)) {
        return std::unexpected(Vk::PipelineBuilderError::PipelineCreationFailed);
    }

    const auto           source     = Vk::MakeStageSource<Shaders::Modules::ProceduralBakeCS>();
    const auto           loaded     = LoadShaderData(source);
    const Vk::ShaderDesc shaderDesc = Vk::CreateShaderDesc(loaded.Code(), source.entryPoint);

    struct BakeSpec {
        int bakeType = 0;
    };

    Vk::Specialization<BakeSpec> bakeSpec;
    Reflect::ForEachFieldInfo<BakeSpec>(bakeSpec);

    const std::array variants  = {BakeSpec {.bakeType = 0}, BakeSpec {.bakeType = 1}, BakeSpec {.bakeType = 2}};
    const auto       specInfos = bakeSpec.Infos(std::span {variants});

    Vk::HeapMappingArena mappingArena {heapManager};
    auto build_res = compute.proceduralBake.BuildHeapVariants(
        ctx.Device(), shaderDesc, specInfos, mappingArena.Map(bakeHeapBindings.mappings), bakeHeapBindings.indexPushOffset, &pipelineBinaries
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
    return Vk::Image::Create(
               allocator, Vk::ImageConfig::Texture2D({width, height}, VK_FORMAT_R8G8B8A8_UNORM, Vk::ImageUsage::Storage | Vk::ImageUsage::Sampled)
    )
        .and_then([&, width, height, variantIdx, scale, randomness, distortion](auto&& gpuImage) -> std::expected<TextureHandle, ErrorCode> {
            defer                       _([&] { allocator.DestroyImage(gpuImage); });
            const Vk::ImageView writeView = gpuImage.View();

            auto                  heapScope = heapManager.CreateImmediateScope();
            const Vk::HeapBlockBase block     = heapScope.WriteParameters<Shaders::Bake>(bakeHeapBindings, Vk::Slot<"outTexture">(writeView));

            if (auto immediate = Vk::ExecuteImmediate(ctx, graphicsCmdRing, [&](VkCommandBuffer cmd) -> auto {
                heapScope.Bind(cmd);

                writeView.DiscardTo<VK_IMAGE_LAYOUT_GENERAL>(cmd);

                compute.proceduralBake.BindVariant(cmd, variantIdx);
                Vk::PushHeapData<Shaders::Modules::ProceduralBakeCS>(
                    cmd, BakePush {.width = width, .height = height, .scale = scale, .randomness = randomness, .distortion = distortion}
                );
                Vk::PushHeapIndex(cmd, bakeHeapBindings.indexPushOffset, block.slot);
                compute.proceduralBake.DispatchThreads(cmd, width, height, 1);

                writeView.Transition<VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd);
            }); !immediate) [[unlikely]] {
                return std::unexpected(immediate.error());
            }

            return textureManager.AdoptTexture(std::forward<decltype(gpuImage)>(gpuImage), writeView, width, height);
        });
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

} // namespace ZHLN
