// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <string_view>

namespace ZHLN::Vk {


[[nodiscard]] auto LoadPipelineCache(VkDevice device, const VkPhysicalDeviceProperties& props, std::string_view path) noexcept -> PipelineCache;

void SavePipelineCache(VkDevice device, VkPipelineCache cache, std::string_view path) noexcept;

}
