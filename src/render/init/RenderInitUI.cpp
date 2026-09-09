// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../ui/UIRendererAccess.hpp"

namespace ZHLN {

auto RenderContext::Impl::SetupUI([[maybe_unused]] GLFWwindow* glfwWindow) -> std::expected<void, Error> {
    return UIRendererAccess::Init(uiRenderer, *this);
}

} // namespace ZHLN
