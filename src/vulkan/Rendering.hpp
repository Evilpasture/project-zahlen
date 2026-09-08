// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/vulkan/Rendering.hpp

#pragma once

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
#include <atomic>


// ============================================================================
// Core C Implementation Declarations
// ============================================================================
#include "RenderCore.h"

// ============================================================================
// Zahlen Graphics Module (Topologically Sorted)
// ============================================================================
// clang-format off
#include "Extensions.hpp"
#include "Features.hpp"
#include "GPUDiagnostics.hpp"
#include "DebugNames.hpp"
#include "Vertex.hpp"
#include "Handles.hpp"
#include "Context.hpp"
#include "RenderQueue.hpp"   
#include "Swapchain.hpp"
#include "FrameSync.hpp"
#include "CommandPool.hpp"
#include "ShaderStages.hpp"
#include "Surface.hpp"
#include "ImageView.hpp"
#include "RenderCore.hpp"    
#include "DynamicRendering.hpp"
#include "DescriptorWrites.hpp"
#include "ReflectedLayout.hpp"
#include "SlangTypeLayout.hpp"
#include "SlangReflectedLayout.hpp"
#include "Raytracing.hpp"
#include "SemaphorePool.hpp"
#include "Allocator.hpp"     // Before DescriptorHeap.hpp: it holds Buffer members
#include "DescriptorHeap.hpp"
#include "HeapBindings.hpp"
#include "PipelineBuilder.hpp"
#include "RenderTarget.hpp"
#include "SamplerBuilder.hpp"
#include "StagingContext.hpp"
#include "Commands.hpp"
#include "ComputePass.hpp"
#include "Postprocessing.hpp"
#include "GpuProfiler.hpp"
#include "PresentationContext.hpp"
#include "ParallelRecorder.hpp"
#include "ParallelDraw.hpp"
#include "RenderGraph.hpp"
// clang-format on
// IWYU pragma: end_exports
