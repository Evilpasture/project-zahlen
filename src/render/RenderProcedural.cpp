// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/render/RenderProcedural.cpp
#include "RenderInternal.hpp"
#include <ShaderBindings.hpp>
#include "Resources.hpp"
#include <Zahlen/Error.hpp>
#include <cstdint>

namespace ZHLN {

auto RenderContext::Impl::BuildProceduralBakePipeline() -> std::expected<void, ErrorCode> {
    // Reflect the bake layout out of the compiled shader instead of allocating
    // from a static C++ descriptor-layout typedef.
    if (!proceduralBakeDescLayout.Build(
            ctx.Device(), Vk::CreateShaderDesc<Shaders::Modules::ProceduralBakeCS>(), VK_SHADER_STAGE_COMPUTE_BIT
        )) {
        return std::unexpected(Vk::PipelineBuilderError::PipelineCreationFailed);
    }

    const void*           cs_code = nullptr;
    size_t                cs_size = 0;
    std::vector<uint32_t> disk_cs;

    LoadShaderData(
        MakeStageSource<ShaderStage::Compute, Shaders::Modules::ProceduralBakeCS>(), cs_code, cs_size,
        disk_cs
    );

    ZHLN_ShaderDesc shaderDesc = {.code = Vk::AsSpirV(cs_code), .size = cs_size, .entry_point = "CSMain"};

    // Map specialization indices to driver pipeline branches
    std::array<VkSpecializationMapEntry, 1> specEntries = {{{.constantID = 0, .offset = 0, .size = sizeof(int)}}};

    std::array<int, 3>                  variants = {0, 1, 2}; // 0=Voronoi, 1=Perlin, 2=Wave
    std::array<VkSpecializationInfo, 3> specInfos {};
    for (int i = 0; i < 3; ++i) {
        specInfos[i] = {.mapEntryCount = 1, .pMapEntries = specEntries.data(), .dataSize = sizeof(int), .pData = &variants[i]};
    }

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
    -> std::expected<uint32_t, ErrorCode> {
    auto* const device = ctx.Device();

    return Vk::ImageBuilder {}
        .Texture2D(width, height, VK_FORMAT_R8G8B8A8_UNORM, Vk::ImageUsage::Storage | Vk::ImageUsage::Sampled, 1)
        .Build(allocator.Get())
        .and_then([&, device, width, height, variantIdx, scale, randomness, distortion](auto&& gpuImage) -> std::expected<uint32_t, ErrorCode> {
            auto view_res = Vk::CreateView<VK_FORMAT_R8G8B8A8_UNORM>(device, gpuImage.Handle(), VK_IMAGE_ASPECT_COLOR_BIT, 1);
            if (!view_res) {
                return std::unexpected(view_res.error());
            }
            auto writeView = std::move(*view_res);

            // VK_EXT_descriptor_heap: the bake is out-of-frame, so BeginImmediate
            // rewinds the bake partition and the write hands back the block the
            // dispatch pushes.
            const auto writeViewInfo = Vk::MakeViewCreateInfo2D(gpuImage.Handle(), VK_FORMAT_R8G8B8A8_UNORM, 1, VK_IMAGE_ASPECT_COLOR_BIT);
            heapManager.BeginImmediate();
            const Vk::HeapBlockBase block = heapManager.WriteHeapParameters<Shaders::Bake>(
                ctx, bakeHeapBindings, Vk::Slot<"outTexture">(Vk::ImageWrite {.view = writeView.Get(), .viewInfo = &writeViewInfo})
            );

            // Dispatch the Compute Shader via allocation-free ExecuteImmediate
            Vk::ExecuteImmediate(ctx, graphicsCmdRing, [&](VkCommandBuffer cmd) -> auto {
                heapManager.BindHeaps(cmd);

                Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL>(cmd, gpuImage.Handle());

                proceduralBakePass.BindVariant(cmd, variantIdx);
                Vk::PushHeapData<Shaders::Modules::ProceduralBakeCS>(
                    ctx, cmd, BakePush {.width = width, .height = height, .scale = scale, .randomness = randomness, .distortion = distortion}
                );
                // Slot-independent mapping: the pushed word is the block's base
                // slot, not an ordinal.
                Vk::PushHeapIndex(ctx, cmd, bakeHeapBindings.indexPushOffset, block.slot);
                proceduralBakePass.DispatchThreads(cmd, width, height, 1);

                Vk::TransitionLayout<VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, gpuImage.Handle());
            });

            return AdoptBindlessTexture(std::forward<decltype(gpuImage)>(gpuImage), std::move(writeView), VK_FORMAT_R8G8B8A8_UNORM);
        });
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

} // namespace ZHLN
