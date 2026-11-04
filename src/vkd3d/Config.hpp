// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

namespace ZHLN::VkD3D {

enum class API : std::uint8_t { Vulkan, D3D };

// Selected only by the zahlen_vkd3d target (ZHLN_GPU_ADDRESS_D3D12, PUBLIC in
// src/vkd3d/CMakeLists.txt) at this joint backend boundary; engine and pass
// sources never branch on it.
#if defined(ZHLN_GPU_ADDRESS_D3D12)
constexpr auto APIType = API::D3D;
#else
constexpr auto APIType = API::Vulkan;
#endif

} // namespace ZHLN::VkD3D
