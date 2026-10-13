// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "RenderInternal.hpp"
#include <cstdint>
#include <utility>

namespace ZHLN {

auto RenderContext::Impl::CreateRenderTexture(uint32_t width, uint32_t height, bool hdr) noexcept -> std::expected<RenderTextureHandle, ErrorCode> {
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

    // One real handle for the attachment role, held by the RenderTexture
    // itself; the bindless descriptor side only gets a create-info.
    auto viewRes = image.CreateAttachmentView(ctx.Device());
    if (!viewRes) {
        return std::unexpected(viewRes.error());
    }
    auto attachmentView = std::move(*viewRes);

    const VkImageViewCreateInfo descriptorInfo = image.ViewInfo();
    auto bindless = textureManager.Adopt(std::move(image), descriptorInfo);
    if (!bindless) {
        return std::unexpected(bindless.error());
    }

    const auto handle = static_cast<RenderTextureHandle>(nextRenderTextureId.fetch_add(1, std::memory_order::relaxed));
    auto& entry = renderTextures.emplace(handle, RenderTexture {.bindlessIndex = *bindless}).first->second;
    entry.attachmentView = std::move(attachmentView);
    entry.image = Vk::ImageSlice {textureManager.Image(*bindless).Handle(), entry.attachmentView, VkExtent2D {width, height}, format};
    return handle;
}

void RenderContext::Impl::DestroyRenderTexture(RenderTextureHandle handle) noexcept {
    const auto it = renderTextures.find(handle);
    if (it == renderTextures.end()) {
        return;
    }
    const uint32_t bindlessIndex = it->second.bindlessIndex;
    // The attachment view may still be referenced by in-flight rendering;
    // retire it with the frame fence like the bindless slot's image.
    deletionQueue.EnqueueAttachmentView(ctx.Device(), std::move(it->second.attachmentView));
    renderTextures.erase(it);
    if (bindlessIndex > kFallbackNormalTextureIndex) {
        textureManager.ReleaseSlot(bindlessIndex);
    }
}

}
