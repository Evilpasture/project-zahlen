// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "vulkan/VulkanHeader.hpp" // IWYU pragma: keep

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

#include "vulkan/core/FrameConfig.hpp"
#include "vulkan/core/FrameStorage.hpp"

// clang-format off
#include "vulkan/core/Extensions.hpp"
#include "vulkan/core/Features.hpp"
#include "vulkan/core/DeviceConfigurator.hpp"
#include "vulkan/diagnostics/DebugNames.hpp"
#include "vulkan/pipeline/Vertex.hpp"
#include "vulkan/core/Handles.hpp"
#include "vulkan/core/Context.hpp"
#include "vulkan/execution/CommandRecorder.hpp"
#include "vulkan/execution/StreamCommandRecorder.hpp"
#include "vulkan/execution/RenderQueue.hpp"
#include "vulkan/execution/GpuPayload.hpp"
#include "vulkan/presentation/Swapchain.hpp"
#include "vulkan/execution/FrameSync.hpp"
#include "vulkan/execution/CommandPool.hpp"
#include "vulkan/pipeline/ShaderStages.hpp"
#include "vulkan/presentation/Surface.hpp"
#include "vulkan/memory/ImageView.hpp"
#include "vulkan/memory/ImageSlice.hpp"
#include "vulkan/memory/DeviceAddress.hpp"
#include "vulkan/memory/GeometryView.hpp"
#include "vulkan/memory/BufferSlice.hpp"
#include "vulkan/execution/Barriers.hpp"
#include "vulkan/pipeline/PipelineTypes.hpp"
#include "vulkan/core/RenderCore.hpp"
#include "vulkan/graph/DynamicRendering.hpp"
#include "vulkan/pipeline/Sampler.hpp"
#include "vulkan/pipeline/DescriptorWrites.hpp"
#include "vulkan/pipeline/ReflectedLayout.hpp"
#include "vulkan/pipeline/PushDataLayout.hpp"
#include "vulkan/diagnostics/Raytracing.hpp"
#include "vulkan/execution/SemaphorePool.hpp"
#include "vulkan/memory/Allocator.hpp"
#include "vulkan/memory/TextureResource.hpp"
#include "vulkan/pipeline/DescriptorHeap.hpp"
#include "vulkan/pipeline/Specialization.hpp"
#include "vulkan/pipeline/ShaderProgram.hpp"
#include "vulkan/pipeline/PipelineBinaries.hpp"
#include "vulkan/pipeline/PipelineBuilder.hpp"
#include "vulkan/pipeline/GraphicsPipeline.hpp"
#include "vulkan/pipeline/HeapBindings.hpp"
#include "vulkan/execution/SecondaryPassContext.hpp"
#include "vulkan/pipeline/HeapMappingBuilder.hpp"
#include "vulkan/memory/RenderTarget.hpp"
#include "vulkan/memory/StagingContext.hpp"
#include "vulkan/execution/Commands.hpp"
#include "vulkan/memory/TextureUploader.hpp"
#include "vulkan/presentation/PresentPacer.hpp"
#include "vulkan/presentation/SwapchainPresenter.hpp"
#include "vulkan/execution/RecordingBatch.hpp"
#include "vulkan/execution/ParallelRecorder.hpp"
#include "vulkan/execution/ParallelDraw.hpp"
// clang-format on
// IWYU pragma: end_exports
