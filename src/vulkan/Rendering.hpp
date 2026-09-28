// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#define ZHLN_RENDERING_HPP_INCLUDED

#include "RenderingPCH.h" // IWYU pragma: keep

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


#include "core/RenderCore.h"
#include "core/FrameConfig.hpp"

// clang-format off
#include "core/Extensions.hpp"
#include "core/Features.hpp"
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
#include "memory/ImageSlice.hpp"
#include "memory/BufferSlice.hpp"
#include "core/RenderCore.hpp"
#include "graph/DynamicRendering.hpp"
#include "pipeline/DescriptorWrites.hpp"
#include "pipeline/ReflectedLayout.hpp"
#include "pipeline/PushDataLayout.hpp"
#include "diagnostics/Raytracing.hpp"
#include "execution/SemaphorePool.hpp"
#include "memory/Allocator.hpp"
#include "memory/TextureResource.hpp"
#include "pipeline/DescriptorHeap.hpp"
#include "pipeline/Specialization.hpp"
#include "pipeline/ShaderProgram.hpp"
#include "pipeline/PipelineBuilder.hpp"
#include "pipeline/PipelineCache.hpp"
#include "pipeline/HeapBindings.hpp"
#include "pipeline/SamplerBuilder.hpp"
#include "pipeline/HeapMappingBuilder.hpp"
#include "memory/RenderTarget.hpp"
#include "memory/StagingContext.hpp"
#include "execution/Commands.hpp"
#include "memory/TextureUploader.hpp"
#include "presentation/PresentPacer.hpp"
#include "presentation/SwapchainPresenter.hpp"
#include "execution/ParallelRecorder.hpp"
#include "execution/ParallelDraw.hpp"
// clang-format on
// IWYU pragma: end_exports
