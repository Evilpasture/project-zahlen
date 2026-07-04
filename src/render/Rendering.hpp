// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/Rendering.hpp

#pragma once

#if defined(ZHLN_LEGACY_VULKAN_1_1)
#include "legacy/DynamicPass.hpp"
#include "legacy/PassCache.hpp"
// Alias modern namespace target to the legacy Vulkan 1.1 implementation
namespace ZHLN::Vk {
static constexpr auto isLegacy = true;

using namespace Vk11;
} // namespace ZHLN::Vk
#else
// Default: Alias modern namespace target directly to standard Vulkan 1.3
namespace ZHLN::Vk {
static constexpr auto isLegacy = false;
}

#endif

#define ZHLN_RENDERING_HPP_INCLUDED

#include "RenderingPCH.h" // IWYU pragma: keep

// ============================================================================
// Standard Library Includes (Ordered)
// ============================================================================
// clang-format off
// IWYU pragma: begin_exports
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <memory>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
#if defined(__cpp_reflection) && !defined(__clang__)
#include <meta>
#endif

// ============================================================================
// Core C Implementation Declarations
// ============================================================================
#include "RenderCore.h"

// ============================================================================
// Zahlen Graphics Module (Topologically Sorted)
// ============================================================================
// clang-format off
#include "Utils.hpp"
#include "Features.hpp"
#include "Vertex.hpp"
#include "RenderQueue.hpp"   
#include "RenderCore.hpp"    
#include "Allocator.hpp"     
#include "PipelineBuilder.hpp"
#include "RenderTarget.hpp"
#include "SamplerBuilder.hpp"
#include "StagingContext.hpp"
#include "Commands.hpp"
#include "ComputePass.hpp"
#include "Postprocessing.hpp"
#include "DescriptorLayout.hpp"
#include "GpuProfiler.hpp"
#include "PresentationContext.hpp"
#include "Texture.hpp"
#include "TextureUtils.hpp"
// clang-format on
// IWYU pragma: end_exports
