// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/vulkan/memory/TextureUploader.hpp

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

namespace ZHLN::Vk {

// Texture Upload Descriptors

/// Host pixels for a 2D texture upload. Four bytes per texel: everything that
/// goes through this path is 8-bit RGBA (UNORM or SRGB).
struct Upload2DDesc {
    const void*      data;
    uint32_t         width;
    uint32_t         height;
    VkFormat         format;
    bool             generateMips = true;
    std::string_view debugName    = {};
};

/// Host voxels for a 3D volume upload, in Vulkan's 3D-image order (x fastest,
/// then y, then z). Four bytes per voxel (8-bit RGBA).
struct Upload3DDesc {
    const void*      data;
    uint32_t         width;
    uint32_t         height;
    uint32_t         depth;
    VkFormat         format;
    std::string_view debugName = {};
};

/// The six faces of a cubemap in +X, -X, +Y, -Y, +Z, -Z order, four bytes per
/// texel (8-bit RGBA). Faces are square: `size` x `size`.
struct UploadCubeDesc {
    std::span<const void* const, 6> faceData;
    uint32_t                        size;
    VkFormat                        format;
    std::string_view                debugName = {};
};

// TextureUploader

/**
 * @brief Full-lifecycle texture upload: the staging allocation, the memory
 *        transfer, mip generation, view creation and debug labelling in one
 *        place.
 *
 * The caller only decides what to upload; the uploader decides how. Every
 * Upload* call copies the payload through the staging ring, records the
 * transfer, and blocks until the ring's timeline semaphore has retired it, so
 * the returned TextureResource's image is already in
 * SHADER_READ_ONLY_OPTIMAL (every mip, every layer) and can be adopted into
 * the bindless arrays or stored as-is without any further synchronization.
 */
class TextureUploader {
  public:
    TextureUploader(
        const Context&                        ctx,
        Allocator&                            allocator,
        StagingRingBuffer&                    staging,
        CommandRing<QueueType::Graphics, 8>&  cmdRing
    ) noexcept
        : _ctx(ctx), _allocator(allocator), _staging(staging), _cmdRing(cmdRing) {}

    // 1. Upload Standard 2D Texture (with optional automatic mip generation)
    [[nodiscard]] auto Upload2D(const Upload2DDesc& desc) const noexcept -> std::expected<TextureResource, ErrorCode> {
        const size_t     byteSize = static_cast<size_t>(desc.width) * desc.height * 4; // 8-bit RGBA: four bytes per texel
        const uint32_t   mips     = desc.generateMips ? GetMipLevels(desc.width, desc.height) : 1;
        const ImageUsage usage    = ImageUsage::Sampled | ImageUsage::TransferDst | (desc.generateMips ? ImageUsage::TransferSrc : ImageUsage::None);

        auto imgRes = ImageBuilder {}
                          .Texture2D(desc.width, desc.height, desc.format, usage, mips)
                          .Build(_allocator.Get());
        if (!imgRes) return std::unexpected(imgRes.error());

        auto stagingAlloc = _staging.Allocate(byteSize);
        if (stagingAlloc.mappedData == nullptr) return std::unexpected(StagingError::MemoryMappingFailed);
        std::memcpy(stagingAlloc.mappedData, desc.data, byteSize);

        ExecuteImmediate(_ctx, _cmdRing, _staging, [&](VkCommandBuffer cmd) {
            TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(
                cmd, imgRes->Handle(), VK_IMAGE_ASPECT_COLOR_BIT, 0, mips
            );

            const VkBufferImageCopy2 region = {
                .sType             = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
                .pNext             = nullptr,
                .bufferOffset      = stagingAlloc.offset,
                .bufferRowLength   = 0,
                .bufferImageHeight = 0,
                .imageSubresource  = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1},
                .imageOffset       = {0, 0, 0},
                .imageExtent       = {desc.width, desc.height, 1},
            };
            CopyBufferToImage<1>(cmd, stagingAlloc.buffer, imgRes->Handle(), {region});

            if (desc.generateMips && mips > 1) {
                // Transitions level 0 into the chain itself and leaves every
                // level in SHADER_READ_ONLY_OPTIMAL when it finishes.
                GenerateMipmaps(cmd, imgRes->Handle(), desc.width, desc.height);
            } else {
                TransitionLayout<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
                    cmd, imgRes->Handle(), VK_IMAGE_ASPECT_COLOR_BIT, 0, 1
                );
            }
        });

        auto viewInfo = MakeViewCreateInfo2D(imgRes->Handle(), desc.format, mips, VK_IMAGE_ASPECT_COLOR_BIT);
        auto viewRes  = CreateView(_ctx.Device(), viewInfo);
        if (!viewRes) return std::unexpected(viewRes.error());

        if (!desc.debugName.empty()) {
            Debug::SetImageName(_ctx, imgRes->Handle(), desc.debugName);
        }

        return TextureResource {
            .image     = std::move(*imgRes),
            .view      = std::move(*viewRes),
            .viewInfo  = viewInfo,
            .extent    = {desc.width, desc.height, 1},
            .format    = desc.format,
            .mipLevels = mips,
            .isCube    = false
        };
    }

    // 2. Upload 3D Volumetric Texture
    [[nodiscard]] auto Upload3D(const Upload3DDesc& desc) const noexcept -> std::expected<TextureResource, ErrorCode> {
        const size_t byteSize = static_cast<size_t>(desc.width) * desc.height * desc.depth * 4; // 8-bit RGBA: four bytes per voxel

        auto imgRes = ImageBuilder {}
                          .Type(VK_IMAGE_TYPE_3D)
                          .Format(desc.format)
                          .Dimensions(desc.width, desc.height, desc.depth)
                          .Usage(ImageUsage::Sampled | ImageUsage::TransferDst)
                          .Build(_allocator.Get());
        if (!imgRes) return std::unexpected(imgRes.error());

        auto stagingAlloc = _staging.Allocate(byteSize);
        if (stagingAlloc.mappedData == nullptr) return std::unexpected(StagingError::MemoryMappingFailed);
        std::memcpy(stagingAlloc.mappedData, desc.data, byteSize);

        ExecuteImmediate(_ctx, _cmdRing, _staging, [&](VkCommandBuffer cmd) {
            TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(
                cmd, imgRes->Handle(), VK_IMAGE_ASPECT_COLOR_BIT, 0, 1
            );
            const VkBufferImageCopy2 region = {
                .sType             = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
                .pNext             = nullptr,
                .bufferOffset      = stagingAlloc.offset,
                .bufferRowLength   = 0,
                .bufferImageHeight = 0,
                .imageSubresource  = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1},
                .imageOffset       = {0, 0, 0},
                .imageExtent       = {desc.width, desc.height, desc.depth},
            };
            CopyBufferToImage<1>(cmd, stagingAlloc.buffer, imgRes->Handle(), {region});
            TransitionLayout<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
                cmd, imgRes->Handle(), VK_IMAGE_ASPECT_COLOR_BIT, 0, 1
            );
        });

        auto viewInfo = MakeViewCreateInfo3D(imgRes->Handle(), desc.format, VK_IMAGE_ASPECT_COLOR_BIT, 1);
        auto viewRes  = CreateView(_ctx.Device(), viewInfo);
        if (!viewRes) return std::unexpected(viewRes.error());

        if (!desc.debugName.empty()) {
            Debug::SetImageName(_ctx, imgRes->Handle(), desc.debugName);
        }

        return TextureResource {
            .image     = std::move(*imgRes),
            .view      = std::move(*viewRes),
            .viewInfo  = viewInfo,
            .extent    = {desc.width, desc.height, desc.depth},
            .format    = desc.format,
            .mipLevels = 1,
            .isCube    = false
        };
    }

    // 3. Upload 6-Face Cubemap
    [[nodiscard]] auto UploadCube(const UploadCubeDesc& desc) const noexcept -> std::expected<TextureResource, ErrorCode> {
        const size_t faceBytes  = static_cast<size_t>(desc.size) * desc.size * 4; // 8-bit RGBA: four bytes per texel
        const size_t totalBytes = faceBytes * 6;

        auto imgRes = ImageBuilder {}
                          .TextureCube(desc.size, desc.format, ImageUsage::Sampled | ImageUsage::TransferDst, 1)
                          .Build(_allocator.Get());
        if (!imgRes) return std::unexpected(imgRes.error());

        auto stagingAlloc = _staging.Allocate(totalBytes);
        if (stagingAlloc.mappedData == nullptr) return std::unexpected(StagingError::MemoryMappingFailed);

        for (uint32_t i = 0; i < 6; ++i) {
            std::memcpy(static_cast<char*>(stagingAlloc.mappedData) + (i * faceBytes), desc.faceData[i], faceBytes);
        }

        ExecuteImmediate(_ctx, _cmdRing, _staging, [&](VkCommandBuffer cmd) {
            TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(
                cmd, imgRes->Handle(), VK_IMAGE_ASPECT_COLOR_BIT, 0, 1
            );
            auto regions = CreateCopyRegions<6>(stagingAlloc.offset, faceBytes, {.width = desc.size, .height = desc.size, .depth = 1});
            CopyBufferToImage<6>(cmd, stagingAlloc.buffer, imgRes->Handle(), regions);
            TransitionLayout<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
                cmd, imgRes->Handle(), VK_IMAGE_ASPECT_COLOR_BIT, 0, 1
            );
        });

        auto viewInfo = MakeViewCreateInfoCube(imgRes->Handle(), desc.format, 1);
        auto viewRes  = CreateView(_ctx.Device(), viewInfo);
        if (!viewRes) return std::unexpected(viewRes.error());

        if (!desc.debugName.empty()) {
            Debug::SetImageName(_ctx, imgRes->Handle(), desc.debugName);
        }

        return TextureResource {
            .image     = std::move(*imgRes),
            .view      = std::move(*viewRes),
            .viewInfo  = viewInfo,
            .extent    = {desc.size, desc.size, 1},
            .format    = desc.format,
            .mipLevels = 1,
            .isCube    = true
        };
    }

  private:
    const Context&                        _ctx;
    Allocator&                            _allocator;
    StagingRingBuffer&                    _staging;
    CommandRing<QueueType::Graphics, 8>&  _cmdRing;
};

} // namespace ZHLN::Vk
