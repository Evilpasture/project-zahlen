// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/render/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <array>
#include <cstdint>
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
