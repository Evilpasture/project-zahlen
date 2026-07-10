// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#include "RenderInternal.hpp"
#include "Resources.hpp"
#include <cstdint>
namespace ZHLN {

void RenderContext::Impl::BuildProceduralBakePipeline() {
    Vk::AllocateSingleBufferedSet<BakeLayout>(ctx.Device(), proceduralBakeDescLayout, proceduralBakeDescPool, proceduralBakeSet);

    VkPushConstantRange push = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset     = 0,
        .size       = sizeof(BakePush),
    };

    const void*           cs_code = nullptr;
    size_t                cs_size = 0;
    std::vector<uint32_t> disk_cs;

    LoadShaderData({.path = SHADER_PROCEDURAL_BAKE_CS_PATH, .fallback = Resource::procedural_bake_comp, .entryPoint = "CSMain"}, cs_code, cs_size, disk_cs);

    ZHLN_ShaderDesc shaderDesc = {.code = Vk::AsSpirV(cs_code), .size = cs_size, .entry_point = "CSMain"};

    // Map specialization indices to driver pipeline branches
    std::array<VkSpecializationMapEntry, 1> specEntries = {{{.constantID = 0, .offset = 0, .size = sizeof(int)}}};

    std::array<int, 3>                  variants = {0, 1, 2}; // 0=Voronoi, 1=Perlin, 2=Wave
    std::array<VkSpecializationInfo, 3> specInfos {};
    for (int i = 0; i < 3; ++i) {
        specInfos[i] = {.mapEntryCount = 1, .pMapEntries = specEntries.data(), .dataSize = sizeof(int), .pData = &variants[i]};
    }

    ZHLN::PanicIf(
        !proceduralBakePass.BuildVariants(ctx.Device(), proceduralBakeDescLayout.Get(), shaderDesc, specInfos, &push, 1),
        "[Shader] Failed to build specialized Procedural Bake Compute variants!"
    );
    ZHLN::Log(
        "[Shader] GPU Procedural Bake Compute Pipeline initialized with specialization "
        "variants."
    );
}

uint32_t RenderContext::Impl::BakeProceduralTexture(uint32_t width, uint32_t height, uint32_t variantIdx, float scale, float randomness, float distortion) {
    auto* const device = ctx.Device();

    // 1. Create a texture with STORAGE and SAMPLED usage
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

    auto gpuImage  = Vk::Image::Create(allocator.Get(), imgInfo, VMA_MEMORY_USAGE_GPU_ONLY);
    auto writeView = Vk::CreateView<VK_FORMAT_R8G8B8A8_UNORM>(device, gpuImage.Handle(), VK_IMAGE_ASPECT_COLOR_BIT, 1);

    // Write to compute descriptor set
    BakeLayout::Write(device, proceduralBakeSet, Vk::ImageWrite {.view = writeView.Get()});

    // 2. Dispatch the Compute Shader via allocation-free ExecuteImmediate
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

    // 3. Register our generated view into the Bindless Set
    uint32_t index = nextTextureIndex++;
    Vk::UpdateBindlessTextureSlot(device, index, writeView.Get(), bindlessSets, 0);

    textureImages.push_back(std::move(gpuImage));
    textureViews.push_back(std::move(writeView));

    return index;
}

} // namespace ZHLN
