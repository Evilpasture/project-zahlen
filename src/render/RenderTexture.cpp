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

    auto imageRes = Vk::ImageBuilder {}.Texture2D(width, height, format, usage, 1).Build(allocator.Get());
    if (!imageRes) {
        return std::unexpected(imageRes.error());
    }
    auto image = std::move(*imageRes);

    auto viewRes = Vk::ImageView::Create(ctx.Device(), image.Handle(), format, VK_IMAGE_ASPECT_COLOR_BIT, 1);
    if (!viewRes) {
        return std::unexpected(viewRes.error());
    }
    auto view = std::move(*viewRes);

    auto bindless = textureManager.Adopt(std::move(image), std::move(view));
    if (!bindless) {
        return std::unexpected(bindless.error());
    }

    const auto handle = static_cast<RenderTextureHandle>(nextRenderTextureId.fetch_add(1, std::memory_order_relaxed));
    renderTextures.emplace(handle, RenderTexture {
        .image = textureManager.Slice(*bindless, {width, height}),
        .bindlessIndex = *bindless,
    });
    return handle;
}

void RenderContext::Impl::DestroyRenderTexture(RenderTextureHandle handle) noexcept {
    const auto it = renderTextures.find(handle);
    if (it == renderTextures.end()) {
        return;
    }
    const uint32_t bindlessIndex = it->second.bindlessIndex;
    renderTextures.erase(it);
    if (bindlessIndex > kFallbackNormalTextureIndex) {
        textureManager.ReleaseSlot(bindlessIndex);
    }
}

}
