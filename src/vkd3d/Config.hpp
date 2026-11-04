// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

namespace ZHLN::VkD3D {

enum class API : std::uint8_t { Vulkan, D3D };

constexpr auto APIType = API::Vulkan;

} // namespace ZHLN::VkD3D
