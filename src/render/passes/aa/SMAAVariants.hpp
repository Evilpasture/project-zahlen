// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "vulkan/VulkanHeader.hpp"

#include "render/RenderTargetTags.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ZHLN::Passes {

// Payload for the SMAA weight pass's Vulkan specialization constants.
//
// Field declaration order IS VkSpecializationMapEntry::constantID order:
// Vk::Specialization<T> assigns IDs from ForEachFieldInfo's field order, so
// maxSearchSteps -> constantID 0 and maxSearchStepsDiag -> constantID 1. That
// must match [[vk::constant_id(0/1)]] in resources/shaders/SMAA.slang.
//
// This struct is also the tier table's element type: one specialization value
// set per precompiled VkPipeline, so the build and the draw share one source of
// truth and cannot drift apart.
struct SmaaWeightSpec {
    int maxSearchSteps     = 16;
    int maxSearchStepsDiag = 8;
};

static_assert(std::is_standard_layout_v<SmaaWeightSpec>, "Vk::Specialization maps fields by ABI offset");
static_assert(sizeof(SmaaWeightSpec) == 2 * sizeof(int), "two 32-bit specialization constants");
static_assert(offsetof(SmaaWeightSpec, maxSearchSteps) == 0, "SpecId 0 offset");
static_assert(offsetof(SmaaWeightSpec, maxSearchStepsDiag) == sizeof(int), "SpecId 1 offset");

// Precompiled pipeline variants, in the exact order BuildHeapVariants() stores
// them in FullscreenPass::pipelines. The values are SMAA's HIGH -> ULTRA search
// budgets; 32 / 16 is the official ULTRA preset and stays inside the baked
// 160x560 area LUT and 64x16 search LUT (Area() feeds sqrt(distance) into the
// area LUT, so a 32-step search still lands well within its 16-texel axis).
inline constexpr std::array<SmaaWeightSpec, 3> kSmaaWeightVariants = {{
    SmaaWeightSpec {.maxSearchSteps = 16, .maxSearchStepsDiag = 8},  // 0: 1080p and below
    SmaaWeightSpec {.maxSearchSteps = 24, .maxSearchStepsDiag = 12}, // 1: 1440p
    SmaaWeightSpec {.maxSearchSteps = 32, .maxSearchStepsDiag = 16}, // 2: 2160p / 4K and above
}};

// Resolution tier -> pipeline variant index. A pure constexpr comparison: the
// draw does no shader compilation, no pipeline creation and no heap work beyond
// the descriptor write it already did, so a window resize cannot stutter.
[[nodiscard]] constexpr auto SmaaWeightVariantForExtent(VkExtent2D extent) noexcept -> uint32_t {
    if (extent.height >= 2160) {
        return 2;
    }
    if (extent.height >= 1440) {
        return 1;
    }
    return 0;
}

static_assert(SmaaWeightVariantForExtent({1920, 1080}) == 0);
static_assert(SmaaWeightVariantForExtent({2560, 1439}) == 0);
static_assert(SmaaWeightVariantForExtent({2560, 1440}) == 1);
static_assert(SmaaWeightVariantForExtent({3840, 2159}) == 1);
static_assert(SmaaWeightVariantForExtent({3840, 2160}) == 2);
static_assert(SmaaWeightVariantForExtent({7680, 4320}) == 2);
static_assert(kSmaaWeightVariants.size() == 3);

} // namespace ZHLN::Passes
