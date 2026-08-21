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
            ctx.Device(), Vk::CreateShaderDesc(Resource::GetShaderProgram(Resource::ShaderID::ProceduralBakeComp).vertex, "CSMain"),
            VK_SHADER_STAGE_COMPUTE_BIT
        )) {
        ZHLN::Log("[Shader] Failed to reflect procedural bake layout!");
        return std::unexpected(RenderInitError::PipelineCreationFailed);
    }
    proceduralBakeDescPool = proceduralBakeDescLayout.CreatePool(ctx.Device(), 1);
    proceduralBakeSet      = proceduralBakeDescLayout.Allocate(ctx.Device(), proceduralBakeDescPool.Get(), proceduralBakeDescLayout.GetSetLayout());

    VkPushConstantRange push = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset     = 0,
        .size       = sizeof(BakePush),
    };

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

    auto build_res = proceduralBakePass.BuildVariants(ctx.Device(), proceduralBakeDescLayout.GetSetLayout(), shaderDesc, specInfos, &push, 1);
    if (!build_res) {
        ZHLN::Log("[Shader] Failed to build specialized Procedural Bake Compute variants: {}", build_res.error().Message());
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
            auto writeView = Vk::CreateView<VK_FORMAT_R8G8B8A8_UNORM>(device, gpuImage.Handle(), VK_IMAGE_ASPECT_COLOR_BIT, 1);

            // Write to compute descriptor set
            proceduralBakeDescLayout.Write(device, proceduralBakeSet, Vk::ImageWrite {.view = writeView.Get()});

            // Dispatch the Compute Shader via allocation-free ExecuteImmediate
            Vk::ExecuteImmediate(ctx, graphicsCmdRing, [&](VkCommandBuffer cmd) {
                // Transition Undefined -> General (Safe for Compute storage writes)
                Vk::TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL>(cmd, gpuImage.Handle());

                proceduralBakePass.DispatchVariant(
                    cmd, proceduralBakeSet, variantIdx, (width + 15) / 16, (height + 15) / 16, 1,
                    BakePush {.width = width, .height = height, .scale = scale, .randomness = randomness, .distortion = distortion, .bakeType = variantIdx}
                );

                // Transition General -> Shader Read Only (Ready for Bindless fragment reads)
                Vk::TransitionLayout<VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, gpuImage.Handle());
            });

            // Register our generated image into the bindless texture heap region.
            uint32_t index = nextTextureIndex++;
            WriteTextureSlotToHeap(index, gpuImage.Handle(), VK_FORMAT_R8G8B8A8_UNORM, 1, false);

            textureImages.push_back(std::forward<decltype(gpuImage)>(gpuImage));
            textureViews.push_back(std::move(writeView));

            return index;
        });
}

} // namespace ZHLN
