// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "../RenderInternal.hpp"
#include <Zahlen/Error.hpp>
#include <Zahlen/UIRenderer.hpp>
#include <expected>

namespace ZHLN {

struct UIRendererAccess {
    static auto Init(UIRenderer& ui, RenderContext::Impl& ctx) -> std::expected<void, Error>;
    static void Record(UIRenderer& ui, Vk::CommandEncoder& encoder, uint32_t width, uint32_t height, uint32_t frameIndex) noexcept;
};

} // namespace ZHLN
