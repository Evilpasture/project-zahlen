// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "PBR.hpp"
#include "RenderInternal.hpp"
#include <StagingContext.hpp>
#include <Zahlen/Log.hpp>
#include <array>
#include <cstddef>

namespace ZHLN::Vk {

class IBLProcessor {
  public:
    static std::expected<IBLPayload, VkResult> Bake(RenderContext::Impl& impl, StagingContext& staging) {
        IBLPayload payload;

        // Pass 1: BRDF LUT Generation
        ZHLN::Log("[IBL] Generating 2D BRDF Look-Up Table...");
        std::vector<uint32_t> lutData = ZHLN::PBR::GenerateBRDFLUT(512, 512);
        VkImageCreateInfo     lutInfo = {
            .sType                 = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext                 = nullptr,
            .flags                 = 0,
            .imageType             = VK_IMAGE_TYPE_2D,
            .format                = VK_FORMAT_R8G8B8A8_UNORM,
            .extent                = {.width = 512, .height = 512, .depth = 1},
            .mipLevels             = 1,
            .arrayLayers           = 1,
            .samples               = VK_SAMPLE_COUNT_1_BIT,
            .tiling                = VK_IMAGE_TILING_OPTIMAL,
            .usage                 = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            .sharingMode           = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices   = nullptr,
            .initialLayout         = VK_IMAGE_LAYOUT_UNDEFINED
        };
        payload.brdfLutImage = Image::Create(impl.allocator.Get(), lutInfo, VMA_MEMORY_USAGE_GPU_ONLY);

        auto upload_res = staging.UploadImage2D(payload.brdfLutImage.Handle(), 512, 512, 1, lutData.data(), static_cast<size_t>(512 * 512 * 4));
        if (!upload_res) {
            return std::unexpected(upload_res.error());
        }

        payload.brdfLutView = CreateView<VK_FORMAT_R8G8B8A8_UNORM>(impl.ctx.Device(), payload.brdfLutImage.Handle());

        // Pass 2: Spherical Harmonics Generation
        ZHLN::Log("[IBL] Generating Diffuse Spherical Harmonics...");
        payload.shCoeffs = ZHLN::PBR::GenerateDiffuseSH();

        // Pass 3: Specular Pre-filtered Cubemap Generation
        ZHLN::Log("[IBL] Generating Specular Pre-filtered Cubemap Mips...");
        constexpr uint32_t kBaseSize  = 256;
        constexpr uint32_t kMipLevels = 6;

        VkImageCreateInfo specInfo = lutInfo;
        specInfo.flags             = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        specInfo.extent            = {.width = kBaseSize, .height = kBaseSize, .depth = 1};
        specInfo.mipLevels         = kMipLevels;
        specInfo.arrayLayers       = 6;
        payload.prefilteredImage   = Image::Create(impl.allocator.Get(), specInfo, VMA_MEMORY_USAGE_GPU_ONLY);

        size_t totalBytes = 0;
        for (uint32_t m = 0; m < kMipLevels; ++m) {
            uint32_t s = kBaseSize >> m;
            totalBytes += (static_cast<size_t>(s * s * 4 * 6));
        }

        return Buffer::Create(impl.allocator.Get(), totalBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY).transform([&](auto&& stagingBuf) {
            auto   mapped        = stagingBuf.Map();
            char*  writePtr      = static_cast<char*>(mapped.data);
            size_t currentOffset = 0;

            for (uint32_t mip = 0; mip < kMipLevels; ++mip) {
                uint32_t mipSize   = kBaseSize >> mip;
                float    roughness = static_cast<float>(mip) / static_cast<float>(kMipLevels - 1);
                auto     mipData   = ZHLN::PBR::GenerateSpecularMip(mipSize, roughness);
                auto     faceBytes = static_cast<size_t>(mipSize) * mipSize * 4;
                for (int face = 0; face < 6; ++face) {
                    std::memcpy(writePtr + currentOffset + (face * faceBytes), mipData[face].data(), faceBytes);
                }
                currentOffset += (faceBytes * 6);
            }

            staging.UploadPrefilteredCubeMap(payload.prefilteredImage.Handle(), stagingBuf.Handle(), kBaseSize, kMipLevels);
            staging.AddBuffer(std::forward<decltype(stagingBuf)>(stagingBuf));

            payload.prefilteredView = CreateViewCube<VK_FORMAT_R8G8B8A8_UNORM>(impl.ctx.Device(), payload.prefilteredImage.Handle(), kMipLevels);
            return std::move(payload); // <-- Explicitly move to resolve the deleted copy constructor error
        });
    }
};

} // namespace ZHLN::Vk
