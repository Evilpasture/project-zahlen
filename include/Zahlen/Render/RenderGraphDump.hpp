// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/GraphicsSettings.hpp>
#include <string_view>

namespace ZHLN {

[[nodiscard]] auto GetRenderGraphDump(AAMode currentMode) noexcept -> std::string_view;

} // namespace ZHLN
