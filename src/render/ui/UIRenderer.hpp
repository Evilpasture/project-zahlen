// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Rendering.hpp"
#include "FormatTags.hpp"
#include <Zahlen/Error.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/GUI/UIData.hpp>
#include <cstdint>
#include <expected>
#include <memory>
#include <utility>

namespace ZHLN::Vk {
class CommandEncoder;
}

namespace ZHLN {

// The UI's supported target formats are types, so the list is a TypeList and
// joins the graph's own vocabulary. UIVariants derives the value-space pipelines
// from it; this declaration is the single source of truth for both.
using SupportedUITargetFormats = Vk::TypeList<
    Vk::Format::B8G8R8A8_SRGB, Vk::Format::B8G8R8A8_UNorm, Vk::Format::R8G8B8A8_SRGB, Vk::Format::R8G8B8A8_UNorm,
    Vk::Format::R16G16B16A16_SFloat>;

template <Vk::Format::IsFormat Format>
using UIColorPass = Vk::DynamicPass<1, false, Vk::AttachmentFormats<Vk::Format::Undefined, Format>>;

class UIRenderer {
  public:
    UIRenderer();
    ~UIRenderer();

    UIRenderer(UIRenderer&&) noexcept;
    auto operator=(UIRenderer&&) noexcept -> UIRenderer&;
    UIRenderer(const UIRenderer&)                    = delete;
    auto operator=(const UIRenderer&) -> UIRenderer& = delete;

    auto Init(RenderContext::Impl& ctx) -> std::expected<void, ErrorCode>;
    void DestroyBuffers(Vk::Allocator& allocator) noexcept;

    void BeginFrame() noexcept;

    [[nodiscard]] auto SupportsFormat(VkFormat colorFormat) const noexcept -> bool;

    template <Vk::Format::IsFormat Format>
    void Record(const UIColorPass<Format>& pass, Vk::CommandEncoder& encoder, uint32_t width, uint32_t height, uint32_t frameIndex,
                UIDrawData uiData) noexcept;

    void RecordFallback(Vk::CommandEncoder& encoder, uint32_t width, uint32_t height, uint32_t frameIndex, VkFormat colorFormat,
                        UIDrawData uiData) noexcept;

  private:
    template <typename DrawBatch>
    void RecordBatches(Vk::CommandEncoder& encoder, uint32_t width, uint32_t height, uint32_t frameIndex, UIDrawData uiData,
                       DrawBatch&& drawBatch) noexcept;

    struct Impl;
    std::unique_ptr<Impl> _impl;
};

}
