// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <Zahlen/Core/Defer.hpp>
#include <cstddef>

namespace ZHLN::Vk {

struct Upload2DDesc {
    const void*      data;
    uint32_t         width;
    uint32_t         height;
    VkFormat         format;
    size_t           texelBytes = 4;
    bool             generateMips = true;
    std::string_view debugName    = {};
};

struct Upload3DDesc {
    const void*      data;
    uint32_t         width;
    uint32_t         height;
    uint32_t         depth;
    VkFormat         format;
    std::string_view debugName = {};
};

struct UploadCubeDesc {
    std::span<const void* const, 6> faceData;
    uint32_t                        size;
    VkFormat                        format;
    std::string_view                debugName = {};
};

class TextureUploader {
  public:
    TextureUploader(
        const Context&                        ctx,
        Allocator&                            allocator,
        StagingRingBuffer&                    staging,
        CommandRing<QueueType::Graphics, 8>&  cmdRing
    ) noexcept
        : _ctx(ctx), _allocator(allocator), _staging(staging), _cmdRing(cmdRing) {}

    [[nodiscard]] auto Upload2D(const Upload2DDesc& desc) const noexcept -> std::expected<TextureResource, ErrorCode> {
        const size_t     byteSize = static_cast<size_t>(desc.width) * desc.height * desc.texelBytes;
        const uint32_t   mips     = desc.generateMips ? GetMipLevels(desc.width, desc.height) : 1;
        const ImageUsage usage    = ImageUsage::Sampled | ImageUsage::TransferDst | (desc.generateMips ? ImageUsage::TransferSrc : ImageUsage::None);

        const ImageConfig imageConfig = ImageConfig::Texture2D({desc.width, desc.height}, desc.format, usage, mips);
        auto imgRes = Image::Create(_allocator, imageConfig);
        if (!imgRes) return std::unexpected(imgRes.error());
        ZHLN::defer _([&] { _allocator.DestroyImage(*imgRes); });

        const ImageView view = imgRes->View(VK_IMAGE_ASPECT_COLOR_BIT, 0, mips);

        if (byteSize > _staging.Capacity()) {
            // Do not turn a legitimate large image into an out-of-bounds memcpy
            // merely because it exceeds the reusable ring. The dedicated path
            // owns its CPU-visible buffer until the synchronous copy completes.
            auto dedicated = StagingContext::Begin(_allocator, _ctx);
            if (!dedicated) {
                return std::unexpected(dedicated.error());
            }
            if (auto copied = dedicated->UploadImage2D(imgRes->Handle(), desc.width, desc.height, mips, desc.data, byteSize); !copied) {
                return std::unexpected(copied.error());
            }
            auto submitted = std::move(*dedicated).ExecuteAsync();
            if (!submitted) {
                return std::unexpected(submitted.error());
            }
            submitted->Wait();
        } else {
            auto stagingAlloc = _staging.Allocate(byteSize);
            if (stagingAlloc.mappedData == nullptr) {
                return std::unexpected(StagingError::StagingSpaceExhausted);
            }
            std::memcpy(stagingAlloc.mappedData, desc.data, byteSize);

            if (auto uploaded = ExecuteImmediate(_ctx, _cmdRing, _staging, [&](VkCommandBuffer cmd) {
                view.DiscardTo<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(cmd);

                const VkBufferImageCopy2 region = {
                    .sType             = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
                    .pNext             = nullptr,
                    .bufferOffset      = stagingAlloc.slice.offset,
                    .bufferRowLength   = 0,
                    .bufferImageHeight = 0,
                    .imageSubresource  = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1},
                    .imageOffset       = {0, 0, 0},
                    .imageExtent       = {desc.width, desc.height, 1},
                };
                CopyBufferToImage(cmd, stagingAlloc.slice.buffer, imgRes->Handle(), region);

                if (desc.generateMips && mips > 1) {
                    GenerateMipmaps(cmd, imgRes->Handle(), desc.width, desc.height);
                } else {
                    view.Mip(0).Transition<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd);
                }
            }); !uploaded) [[unlikely]] {
                return std::unexpected(uploaded.error());
            }
        }

        if (!desc.debugName.empty()) {
            Debug::SetImageName(_ctx, imgRes->Handle(), desc.debugName);
        }

        return TextureResource {
            .image  = std::move(*imgRes),
            .view   = view,
            .extent = {desc.width, desc.height, 1}
        };
    }

    [[nodiscard]] auto Upload3D(const Upload3DDesc& desc) const noexcept -> std::expected<TextureResource, ErrorCode> {
        const size_t byteSize = static_cast<size_t>(desc.width) * desc.height * desc.depth * 4;

        const ImageConfig imageConfig = ImageConfig::Texture3D(
            {desc.width, desc.height, desc.depth}, desc.format, ImageUsage::Sampled | ImageUsage::TransferDst
        );
        auto imgRes = Image::Create(_allocator, imageConfig);
        if (!imgRes) return std::unexpected(imgRes.error());
        ZHLN::defer _([&] { _allocator.DestroyImage(*imgRes); });

        const ImageView view = imgRes->View();

        auto stagingAlloc = _staging.Allocate(byteSize);
        if (stagingAlloc.mappedData == nullptr) return std::unexpected(StagingError::MemoryMappingFailed);
        std::memcpy(stagingAlloc.mappedData, desc.data, byteSize);

        if (auto uploaded = ExecuteImmediate(_ctx, _cmdRing, _staging, [&](VkCommandBuffer cmd) {
            view.DiscardTo<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(cmd);
            const VkBufferImageCopy2 region = {
                .sType             = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
                .pNext             = nullptr,
                .bufferOffset      = stagingAlloc.slice.offset,
                .bufferRowLength   = 0,
                .bufferImageHeight = 0,
                .imageSubresource  = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1},
                .imageOffset       = {0, 0, 0},
                .imageExtent       = {desc.width, desc.height, desc.depth},
            };
            CopyBufferToImage(cmd, stagingAlloc.slice.buffer, imgRes->Handle(), region);
            view.Transition<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd);
        }); !uploaded) [[unlikely]] {
            return std::unexpected(uploaded.error());
        }

        if (!desc.debugName.empty()) {
            Debug::SetImageName(_ctx, imgRes->Handle(), desc.debugName);
        }

        return TextureResource {
            .image  = std::move(*imgRes),
            .view   = view,
            .extent = {desc.width, desc.height, desc.depth}
        };
    }

    [[nodiscard]] auto UploadCube(const UploadCubeDesc& desc) const noexcept -> std::expected<TextureResource, ErrorCode> {
        const size_t faceBytes  = static_cast<size_t>(desc.size) * desc.size * 4;
        const size_t totalBytes = faceBytes * 6;

        const ImageConfig imageConfig = ImageConfig::Cube(desc.size, desc.format, ImageUsage::Sampled | ImageUsage::TransferDst);
        auto imgRes = Image::Create(_allocator, imageConfig);
        if (!imgRes) return std::unexpected(imgRes.error());
        ZHLN::defer _([&] { _allocator.DestroyImage(*imgRes); });

        const ImageView view = imgRes->View();

        auto stagingAlloc = _staging.Allocate(totalBytes);
        if (stagingAlloc.mappedData == nullptr) return std::unexpected(StagingError::MemoryMappingFailed);

        for (uint32_t i = 0; i < 6; ++i) {
            std::memcpy(static_cast<char*>(stagingAlloc.mappedData) + (i * faceBytes), desc.faceData[i], faceBytes);
        }

        if (auto uploaded = ExecuteImmediate(_ctx, _cmdRing, _staging, [&](VkCommandBuffer cmd) {
            view.DiscardTo<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(cmd);
            auto regions = CreateCopyRegions<6>(stagingAlloc.slice.offset, faceBytes, {.width = desc.size, .height = desc.size, .depth = 1});
            CopyBufferToImage<6>(cmd, stagingAlloc.slice.buffer, imgRes->Handle(), regions);
            view.Transition<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd);
        }); !uploaded) [[unlikely]] {
            return std::unexpected(uploaded.error());
        }

        if (!desc.debugName.empty()) {
            Debug::SetImageName(_ctx, imgRes->Handle(), desc.debugName);
        }

        return TextureResource {
            .image  = std::move(*imgRes),
            .view   = view,
            .extent = {desc.size, desc.size, 1}
        };
    }

  private:
    const Context&                        _ctx;
    Allocator&                            _allocator;
    StagingRingBuffer&                    _staging;
    CommandRing<QueueType::Graphics, 8>&  _cmdRing;
};

}
