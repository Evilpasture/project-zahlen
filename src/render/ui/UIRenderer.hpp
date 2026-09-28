// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Rendering.hpp"
#include <Zahlen/Error.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/gui/UIData.hpp>
#include <cstdint>
#include <expected>
#include <memory>
#include <utility>

namespace ZHLN::Vk {
class CommandEncoder;
}

namespace ZHLN {

// Explicitly enumerate the UI formats we can lift from a runtime destination
// into a checked DynamicPass. BGRA UNORM is a valid native swapchain fallback.
using SupportedUITargetFormats = std::integer_sequence<
    VkFormat, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM,
    VK_FORMAT_R16G16B16A16_SFLOAT>;

template <VkFormat Format>
using UIColorPass = Vk::DynamicPass<1, false, Vk::AttachmentFormats<VK_FORMAT_UNDEFINED, Format>>;

class UIRenderer {
  public:
    UIRenderer();
    ~UIRenderer();

    UIRenderer(UIRenderer&&) noexcept;
    auto operator=(UIRenderer&&) noexcept -> UIRenderer&;
    UIRenderer(const UIRenderer&)                    = delete;
    auto operator=(const UIRenderer&) -> UIRenderer& = delete;

    auto Init(RenderContext::Impl& ctx) -> std::expected<void, ErrorCode>;

    void BeginFrame() noexcept;

    [[nodiscard]] auto SupportsFormat(VkFormat colorFormat) const noexcept -> bool;

    template <VkFormat Format>
    void Record(const UIColorPass<Format>& pass, Vk::CommandEncoder& encoder, uint32_t width, uint32_t height, uint32_t frameIndex,
                const UIDrawData& uiData) noexcept;

    // Only for an uncommon presentation format not in SupportedUITargetFormats.
    // The runtime format check in SupportsFormat precedes this path.
    void RecordFallback(Vk::CommandEncoder& encoder, uint32_t width, uint32_t height, uint32_t frameIndex, VkFormat colorFormat,
                        const UIDrawData& uiData) noexcept;

  private:
    template <typename DrawBatch>
    void RecordBatches(Vk::CommandEncoder& encoder, uint32_t width, uint32_t height, uint32_t frameIndex, const UIDrawData& uiData,
                       DrawBatch&& drawBatch) noexcept;

    struct Impl;
    std::unique_ptr<Impl> _impl;
};

}
