// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The render subsystem's log channel. See vulkan/VulkanLog.hpp for the pattern:
// the name belongs to the subsystem that emits it.

#include <Zahlen/Log.hpp>

namespace ZHLN {

inline constexpr LogChannel kRenderLog {"Render"};

} // namespace ZHLN
