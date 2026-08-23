// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/render/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ZHLN::Vk {

class ShaderStages;

struct SlangReflectedBinding {
    uint32_t                 binding         = 0;
    VkDescriptorType         descriptorType  = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    uint32_t                 descriptorCount = 1;
    VkShaderStageFlags       stageFlags      = 0;
    VkDescriptorBindingFlags bindingFlags    = 0;
};

struct SlangReflectedSet {
    std::vector<SlangReflectedBinding> bindings;
};

struct ReflectedStageInput;

inline constexpr uint32_t kHeapFrameAddressCount = 6;

/**
 * Byte offsets in the vkCmdPushDataEXT blob, reflected from
 * DescriptorHeapPushData in descriptor_heap_layout.slang.
 *
 * Keeping every frame address offset (rather than assuming a packed array)
 * makes the host obey whatever padding the active Slang SPIR-V layout rules
 * select.
 */
struct HeapPushDataLayout {
    std::array<uint32_t, kHeapFrameAddressCount> frameAddressOffsets {};
    uint32_t                                     heapIndexOffset = 0;
    uint32_t                                     requiredSize    = 0;
};

/// Uses Slang's target-specific type-layout reflection to obtain the push-data
/// offsets shared by descriptor-heap mappings and vkCmdPushDataEXT calls.
[[nodiscard]] auto ReflectHeapPushDataLayout() noexcept -> std::optional<HeapPushDataLayout>;

/**
 * @brief Binding structure reflected from compiled SPIR-V.
 *
 * In the descriptor-heap model this carries NO descriptor set layouts, pools,
 * or pipeline layouts: the binding structure (set/binding/type/count/stages)
 * is what the engine bakes into its VkDescriptorSetAndBindingMappingEXT
 * tables (see HeapBindings.hpp).
 */
struct SlangReflectedLayout {
    std::array<SlangReflectedSet, 4> reflectedSets {};

    /// True when `binding` is present in the given reflected set.
    [[nodiscard]] auto HasBinding(uint32_t setIndex, uint32_t binding) const noexcept -> bool {
        if (setIndex >= 4) {
            return false;
        }
        for (const auto& b: reflectedSets[setIndex].bindings) {
            if (b.binding == binding) {
                return true;
            }
        }
        return false;
    }

    bool Build(VkDevice device, const ShaderStages& shaders) noexcept;

    /// Reflects a single stage (commonly a compute shader described by a raw SPV blob).
    bool Build(VkDevice device, const ZHLN_ShaderDesc& shader, VkShaderStageFlagBits stage) noexcept;

    /// Reflects the union of an arbitrary set of stages (e.g. the bindless scene registry).
    bool Build(VkDevice device, std::span<const ReflectedStageInput> stages) noexcept;
};

} // namespace ZHLN::Vk
