// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <span>
#include <vector>

namespace ZHLN::Vk {

// The pipeline cache is purely data-driven: the caller hands in bytes it
// loaded from wherever it keeps them and takes bytes back out the same way.
// Nothing in here knows what a file is.

[[nodiscard]] auto LoadPipelineCache(VkDevice device, const VkPhysicalDeviceProperties& props, std::span<const uint8_t> initialData) noexcept
    -> PipelineCache;

[[nodiscard]] auto SavePipelineCache(VkDevice device, VkPipelineCache cache) noexcept -> std::vector<uint8_t>;

}
