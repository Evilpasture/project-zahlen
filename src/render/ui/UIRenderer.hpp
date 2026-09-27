// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Error.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/gui/UIData.hpp>
#include <cstdint>
#include <expected>
#include <memory>

namespace ZHLN::Vk {
class CommandEncoder;
}

namespace ZHLN {

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

    void Record(Vk::CommandEncoder& encoder, uint32_t width, uint32_t height, uint32_t frameIndex, const UIDrawData& uiData) noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

}
