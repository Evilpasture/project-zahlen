// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/vulkan/Rendering.hpp

#pragma once

#define ZHLN_RENDERING_HPP_INCLUDED

#include "RenderingPCH.h" // IWYU pragma: keep

// Standard Library Includes (Ordered)
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


// Core C Implementation Declarations
#include "core/RenderCore.h"

// Zahlen Graphics Module (Topologically Sorted)
//
// What this umbrella is: the RHI's own surface -- context, device, queues,
// command recording, images, samplers, descriptor writing, pipelines, shader
// programs and the heap they dispatch through. What it is not: the subsystems a
// handful of translation units use. The render graph, compute passes,
// post-processing, the GPU profiler and the GPU diagnostics are all reachable
// by dropping them in here -- they were, until the include list grew to the
// point where an edit to any one of them recompiled the engine. A translation
// unit that needs one includes it, and then an edit to it rebuilds its users
// instead of everything.
//
// What stayed stayed for a reason: a header in this list uses it. Descriptor
// writes speak TypedImage and TransitionLayout, the parallel recorders speak
// NullInheritanceInfo and the heap push writers, the presentation context owns
// GBufferLayout, and the descriptor heap holds a Buffer by value. Those are
// dependencies of the facade, not conveniences -- taking them out would only
// move the include into the header that already needs it.
//
// The ordering matters: this is a topological sort.
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
#include "core/RenderCore.hpp"
#include "graph/DynamicRendering.hpp" // TypedImage, TransitionLayout, NullInheritanceInfo: what the writes and the recorders speak
#include "pipeline/DescriptorWrites.hpp"
#include "pipeline/ReflectedLayout.hpp"
#include "pipeline/PushDataLayout.hpp" // the push-data ABI constants (the reader lives in SpirvLayout.hpp)
#include "diagnostics/Raytracing.hpp"
#include "execution/SemaphorePool.hpp"
#include "memory/Allocator.hpp"     // Before DescriptorHeap.hpp: it holds Buffer members
#include "memory/TextureResource.hpp" // the uploaded image+view bundle, built from the Image and ImageView handles above
#include "pipeline/DescriptorHeap.hpp"
#include "pipeline/Specialization.hpp" // the map entries a variant's constants are described by
#include "pipeline/ShaderProgram.hpp"
#include "pipeline/PipelineBuilder.hpp"
#include "pipeline/PipelineCache.hpp"
#include "pipeline/HeapBindings.hpp" // the heap push writers the parallel recorders dispatch through
#include "pipeline/SamplerBuilder.hpp"
#include "pipeline/HeapMappingBuilder.hpp"
#include "memory/RenderTarget.hpp" // GBufferLayout and the attachment set the presentation context owns
#include "memory/StagingContext.hpp"
#include "execution/Commands.hpp"
#include "memory/TextureUploader.hpp" // full-lifecycle 2D / 3D / cubemap uploads through the staging ring and the command ring
#include "presentation/SwapchainPresenter.hpp" // surface -> swapchain -> acquired image, and the present it ends with
#include "execution/ParallelRecorder.hpp"
#include "execution/ParallelDraw.hpp"
// clang-format on
// IWYU pragma: end_exports
