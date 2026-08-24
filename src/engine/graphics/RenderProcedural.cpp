// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#include "RenderInternal.hpp"
#include "Resources.hpp"
#include <Zahlen/Error.hpp>
#include <cstdint>

namespace ZHLN {

std::expected<void, Error> RenderContext::Impl::BuildProceduralBakePipeline() {
    // Reflect the bake layout out of the compiled shader instead of allocating
    // from a static C++ descriptor-layout typedef.
    if (!proceduralBakeDescLayout.Build(
            ctx.Device(), Vk::CreateShaderDesc(Resource::GetShaderProgram(Resource::ShaderID::ProceduralBakeComp).vertex, "CSMain"), VK_SHADER_STAGE_COMPUTE_BIT
        )) {
        return std::unexpected(RenderInitError::PipelineCreationFailed);
    }
    // bakeHeapBindings is allocated once in InitBakeHeapBindings (slot span
    // covers IBL specular mips too). Re-reflect here so hot-reload still
    // rebuilds the pipeline against the same mapping table.

    const void*           cs_code = nullptr;
    size_t                cs_size = 0;
    std::vector<uint32_t> disk_cs;

    LoadShaderData(
        ComputeStageSource {.path = Resource::Paths::ProceduralBakeCS, .fallback = Resource::procedural_bake_comp, .entryPoint = "CSMain"}, cs_code, cs_size,
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
        ctx.Device(), shaderDesc, specInfos, bakeHeapBindings.GetInfo(), bakeHeapBindings.indexPushOffset
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

std::expected<uint32_t, Error>
    RenderContext::Impl::BakeProceduralTexture(uint32_t width, uint32_t height, uint32_t variantIdx, float scale, float randomness, float distortion) {
    auto* const device = ctx.Device();

    const VkImageCreateInfo imgInfo = {
        .sType                 = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext                 = {},
        .flags                 = {},
        .imageType             = VK_IMAGE_TYPE_2D,
        .format                = VK_FORMAT_R8G8B8A8_UNORM,
        .extent                = {.width = width, .height = height, .depth = 1},
        .mipLevels             = 1,
        .arrayLayers           = 1,
        .samples               = VK_SAMPLE_COUNT_1_BIT,
        .tiling                = VK_IMAGE_TILING_OPTIMAL,
        .usage                 = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode           = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = {},
        .pQueueFamilyIndices   = {},
        .initialLayout         = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    return Vk::Image::Create(allocator.Get(), imgInfo, VMA_MEMORY_USAGE_GPU_ONLY)
        .transform_error([](VkResult res) -> Error { return res; })
        .and_then([&, device, width, height, variantIdx, scale, randomness, distortion](auto&& gpuImage) -> std::expected<uint32_t, Error> {
            auto view_res = Vk::CreateView<VK_FORMAT_R8G8B8A8_UNORM>(device, gpuImage.Handle(), VK_IMAGE_ASPECT_COLOR_BIT, 1);
            if (!view_res) {
                return std::unexpected(Error(view_res.error()));
            }
            auto writeView = std::move(*view_res);

            // VK_EXT_descriptor_heap: write the bake output's storage-image
            // descriptor into the static heap slot.
            const auto writeViewInfo = Vk::MakeViewCreateInfo2D(gpuImage.Handle(), VK_FORMAT_R8G8B8A8_UNORM, 1, VK_IMAGE_ASPECT_COLOR_BIT);
            Vk::WriteHeapBindings(heapManager, ctx, bakeHeapBindings, kBake2DHeapIndex, Vk::ImageWrite {.view = writeView.Get(), .viewInfo = &writeViewInfo});

            // Dispatch the Compute Shader via allocation-free ExecuteImmediate
            Vk::ExecuteImmediate(ctx, graphicsCmdRing, [&](VkCommandBuffer cmd) {
                // VK_EXT_descriptor_heap: this command buffer records a heap
                // pipeline, so the heaps must be bound on it (push data also
                // does not carry over from other command buffers).
                BindHeaps(cmd);

                // Transition Undefined -> General (Safe for Compute storage writes)
                Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL>(cmd, gpuImage.Handle());

                proceduralBakePass.BindVariant(cmd, variantIdx);
                Vk::PushData(
                    ctx, cmd, 0,
                    BakePush {.width = width, .height = height, .scale = scale, .randomness = randomness, .distortion = distortion, .bakeType = variantIdx}
                );
                Vk::PushHeapIndex(ctx, cmd, bakeHeapBindings.indexPushOffset, kBake2DHeapIndex);
                proceduralBakePass.DispatchThreads(cmd, width, height, 1);

                // Transition General -> Shader Read Only (Ready for Bindless fragment reads)
                Vk::TransitionLayout<VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, gpuImage.Handle());
            });

            return AdoptBindlessTexture(std::forward<decltype(gpuImage)>(gpuImage), std::move(writeView), VK_FORMAT_R8G8B8A8_UNORM);
        });
}

} // namespace ZHLN
