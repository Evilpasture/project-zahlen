// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../RenderInternal.hpp"

namespace ZHLN {

auto RenderContext::Impl::SetupUI([[maybe_unused]] GLFWwindow* glfwWindow) -> std::expected<void, ErrorCode> {
    return uiRenderer.Init(*this);
}

} // namespace ZHLN
