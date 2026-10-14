// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "RenderInternal.hpp"
#include <cstdint>
#include <utility>

namespace ZHLN {

auto RenderContext::Impl::CreateRenderTexture(uint32_t width, uint32_t height, bool hdr) noexcept -> std::expected<TextureHandle, ErrorCode> {
    if (width == 0 || height == 0 || ctx.Device() == VK_NULL_HANDLE) {
        return std::unexpected(Vk::DescriptorHeapError::AllocationFailed);
    }

    const VkFormat format = hdr ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
    const Vk::ImageUsage usage = Vk::ImageUsage::ColorAttachment | Vk::ImageUsage::Sampled | Vk::ImageUsage::TransferSrc;

    const auto imageConfig = Vk::ImageConfig::Texture2D({width, height}, format, usage);
    auto imageRes = Vk::Image::Create(allocator, imageConfig);
    if (!imageRes) {
        return std::unexpected(imageRes.error());
    }
    auto image = std::move(*imageRes);
    defer _([&] { allocator.DestroyImage(image); });
    const VkImage imageHandle = image.Handle();

    auto viewRes = image.CreateAttachmentView(ctx.Device());
    if (!viewRes) {
        return std::unexpected(viewRes.error());
    }
    auto attachmentView = std::move(*viewRes);

    const VkImageViewCreateInfo descriptorInfo = image.ViewInfo();
    auto texture = textureManager.AdoptTexture(std::move(image), descriptorInfo, width, height);
    if (!texture) {
        return std::unexpected(texture.error());
    }

    auto& entry = renderTextures.emplace(*texture, RenderTexture {}).first->second;
    entry.attachmentView = std::move(attachmentView);
    entry.image = Vk::ImageSlice {imageHandle, entry.attachmentView, VkExtent2D {width, height}, format};
    return *texture;
}

void RenderContext::Impl::DestroyRenderTexture(TextureHandle handle) noexcept {
    const auto it = renderTextures.find(handle);
    if (it == renderTextures.end()) {
        return;
    }

    deletionQueue.EnqueueAttachmentView(ctx.Device(), std::move(it->second.attachmentView));
    renderTextures.erase(it);
    textureManager.Unload(handle);
}

}
