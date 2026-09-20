// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/vulkan/pipeline/PipelineCache.hpp

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <string_view>

namespace ZHLN::Vk {

// Driver Pipeline Cache — disk persistence
//
// Every vkCreateGraphicsPipelines / vkCreateComputePipelines call in the engine
// goes through PipelineBuilder / ComputePipelineBuilder, so handing those a
// VkPipelineCache is enough to capture the whole startup. Without one the
// driver recompiles every pipeline from SPIR-V on every single run, which is
// the bulk of the renderer's init time.
//
// A cache blob is only valid for the exact driver build and GPU that wrote it,
// so the header is validated against the live physical device before the bytes
// are handed to vkCreatePipelineCache; a mismatch starts an empty cache instead
// of feeding the driver data it may reject.

/// Loads the cache at `path`, or starts an empty one when the file is missing,
/// unreadable, oversized, or written by a different driver/GPU. The returned
/// handle owns the cache and destroys it with the device.
///
/// A missing or unusable cache is never an error: it just means this run
/// compiles its pipelines the slow way.
[[nodiscard]] auto LoadPipelineCache(VkDevice device, const VkPhysicalDeviceProperties& props, std::string_view path) noexcept -> PipelineCache;

/// Flushes the cache to `path`, writing to a sibling `.tmp` first and renaming
/// over the target so an abort mid-write cannot leave a truncated cache behind.
/// A no-op when the cache is empty or the write fails.
void SavePipelineCache(VkDevice device, VkPipelineCache cache, std::string_view path) noexcept;

} // namespace ZHLN::Vk
