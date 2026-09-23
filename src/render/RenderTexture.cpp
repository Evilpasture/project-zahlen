// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/RenderTexture.cpp
//
// Render-to-texture: the offscreen targets a caller creates and draws into,
// and releases again.
//
// A render texture is the other kind of destination. It is not a window, has
// no swapchain and is never presented; what makes it worth its own file is that
// it is where the two halves of the RTT API meet -- a `Vk::RenderTarget` built
// through the image builder (the renderer's memory) and a slot in the bindless
// texture array (the heaps'), registered in the same table as a swapchain image
// so `RenderAttachment` addresses them interchangeably.
//
// Deferred release is the part that matters here: a texture may still be
// sampled by an in-flight frame when the caller destroys it, so the bindless
// slot goes back to the deferred path and the record is retired in place --
// which keeps every later record's index stable and rejects the handle rather
// than allowing it to resolve to whatever occupies the slot next.

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

    auto imageRes = Vk::ImageBuilder {}.Texture2D(width, height, format, usage, 1).Build(allocator.Get());
    if (!imageRes) {
        return std::unexpected(imageRes.error());
    }
    auto image = std::move(*imageRes);

    auto viewRes = Vk::CreateView(ctx.Device(), image.Handle(), format, VK_IMAGE_ASPECT_COLOR_BIT, 1);
    if (!viewRes) {
        return std::unexpected(viewRes.error());
    }
    auto view = std::move(*viewRes);

    const VkImage     rawImage = image.Handle();
    const VkImageView rawView  = view.Get();

    // The render texture is published in the bindless texture array, so a
    // material may sample what a previous pass rendered into it. That is the
    // whole point of the RTT API: the handle addresses a subresource *and*
    // resolves to a descriptor.
    auto bindless = textureManager.Adopt(std::move(image), std::move(view), format, 1, false);
    if (!bindless) {
        return std::unexpected(bindless.error());
    }

    const auto handle = destinations.Register(DestinationRegistry::Record {
        .bindlessIndex = *bindless,
        .image         = Vk::MakeSlice(rawImage, rawView, {.width = width, .height = height}, format),
        .presentable   = false,
        .target        = nullptr,
    });

    // `image`/`view` are owned by the bindless arrays from here on; the record
    // only references them. Stamp the record's handle so the caller can address
    // it and release it later.
    return handle.AsTexture();
}

void RenderContext::Impl::DestroyRenderTexture(TextureHandle handle) noexcept {
    const auto decoded = DestinationRegistry::Handle::FromTexture(handle);
    if (!decoded.has_value() || decoded->Index() >= destinations.Records().size()) {
        return;
    }
    DestinationRegistry::Record& record = destinations.Records()[decoded->Index()];
    if (record.serial != decoded->Serial()) {
        return;
    }

    // The texture may still be sampled by an in-flight frame, so hand the
    // bindless slot back to the deferred-release path instead of destroying it
    // here; the texture manager neutralizes the descriptor at the next frame
    // boundary for this parity.
    const uint32_t bindlessIndex = record.bindlessIndex;
    if (bindlessIndex > kFallbackNormalTextureIndex) {
        textureManager.ReleaseSlot(bindlessIndex);
    }
    // Retire the slot rather than erasing it: every later record keeps its
    // index, so handles already handed to callers stay valid -- and stay
    // rejected, because the serial no longer matches.
    record.handle        = {};
    record.serial        = 0;
    record.image         = {};
    record.bindlessIndex = 0;
    record.trackedLayout = Vk::AttachmentLayout::Undefined;
    record.content.reset();
}

} // namespace ZHLN
