// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Render/GraphicsSettings.hpp>
#include <string_view>

namespace ZHLN {

auto GetRenderGraphDump(AAMode currentMode) noexcept -> std::string_view;

} // namespace ZHLN
