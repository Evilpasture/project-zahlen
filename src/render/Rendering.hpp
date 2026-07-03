// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/Rendering.hpp

#pragma once

#define ZHLN_RENDERING_HPP_INCLUDED

// ============================================================================
// External APIs & Library Config
// ============================================================================
#include <spirv_reflect.h>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

// ============================================================================
// Standard Library Includes (Ordered)
// ============================================================================
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
#ifdef __cpp_reflection
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