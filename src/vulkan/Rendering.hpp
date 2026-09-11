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
#include "core/RenderCore.h"

// ============================================================================
// Zahlen Graphics Module (Topologically Sorted)
// ============================================================================
// clang-format off
#include "core/Extensions.hpp"
#include "core/Features.hpp"
#include "diagnostics/GPUDiagnostics.hpp"
#include "diagnostics/DebugNames.hpp"
#include "pipeline/Vertex.hpp"
#include "core/Handles.hpp"
#include "core/Context.hpp"
#include "execution/RenderQueue.hpp"
#include "presentation/Swapchain.hpp"
#include "execution/FrameSync.hpp"
#include "execution/CommandPool.hpp"
#include "pipeline/ShaderStages.hpp"
#include "presentation/Surface.hpp"
#include "memory/ImageView.hpp"
#include "core/RenderCore.hpp"
#include "graph/DynamicRendering.hpp"
#include "pipeline/DescriptorWrites.hpp"
#include "pipeline/ReflectedLayout.hpp"
#include "pipeline/SlangTypeLayout.hpp"
#include "pipeline/SlangReflectedLayout.hpp"
#include "diagnostics/Raytracing.hpp"
#include "execution/SemaphorePool.hpp"
#include "memory/Allocator.hpp"     // Before DescriptorHeap.hpp: it holds Buffer members
#include "pipeline/DescriptorHeap.hpp"
#include "pipeline/HeapBindings.hpp"
#include "pipeline/PipelineBuilder.hpp"
#include "memory/RenderTarget.hpp"
#include "pipeline/SamplerBuilder.hpp"
#include "memory/StagingContext.hpp"
#include "execution/Commands.hpp"
#include "pipeline/ComputePass.hpp"
#include "pipeline/Postprocessing.hpp"
#include "diagnostics/GpuProfiler.hpp"
#include "presentation/PresentationContext.hpp"
#include "presentation/SwapchainSession.hpp"
#include "execution/ParallelRecorder.hpp"
#include "execution/ParallelDraw.hpp"
#include "graph/RenderGraph.hpp"
// clang-format on
// IWYU pragma: end_exports
