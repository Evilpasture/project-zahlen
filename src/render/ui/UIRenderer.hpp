// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Rendering.hpp"
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

using SupportedUITargetFormats = std::integer_sequence<
    Vk::Format, Vk::Format::B8G8R8A8Srgb, Vk::Format::B8G8R8A8Unorm, Vk::Format::R8G8B8A8Srgb, Vk::Format::R8G8B8A8Unorm,
    Vk::Format::R16G16B16A16Sfloat>;

template <Vk::Format Format>
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

    [[nodiscard]] auto SupportsFormat(Vk::Format colorFormat) const noexcept -> bool;

    template <Vk::Format Format>
    void Record(const UIColorPass<Format>& pass, Vk::CommandEncoder& encoder, uint32_t width, uint32_t height, uint32_t frameIndex,
                UIDrawData uiData) noexcept;

    void RecordFallback(Vk::CommandEncoder& encoder, uint32_t width, uint32_t height, uint32_t frameIndex, Vk::Format colorFormat,
                        UIDrawData uiData) noexcept;

  private:
    template <typename DrawBatch>
    void RecordBatches(Vk::CommandEncoder& encoder, uint32_t width, uint32_t height, uint32_t frameIndex, UIDrawData uiData,
                       DrawBatch&& drawBatch) noexcept;

    struct Impl;
    std::unique_ptr<Impl> _impl;
};

}
