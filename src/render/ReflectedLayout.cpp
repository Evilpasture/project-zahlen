// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ReflectedLayout.hpp"
#include <map>
#include <spirv_reflect.h>
#include <vector>

namespace ZHLN::Vk {

void UnsafeReflectedLayoutBuilder::AddStageUnsafe(const ZHLN_ShaderDesc& desc, VkShaderStageFlags stage) noexcept {
    if ((desc.code != nullptr) && desc.size > 0 && _stageCount < _stages.size()) {
        _stages[_stageCount++] = {.code = desc.code, .size = desc.size, .stage = stage};
    }
}

auto UnsafeReflectedLayoutBuilder::BuildUnsafe(std::array<ReflectedSet, 4>& out) noexcept -> bool {
    for (auto& set: out) {
        set.bindings.clear();
    }

    // Sorted map: SetIndex -> BindingIndex -> binding (merged across stages).
    struct MergedBinding {
        VkDescriptorType         type   = VK_DESCRIPTOR_TYPE_MAX_ENUM;
        uint32_t                 count  = 0;
        VkShaderStageFlags       stages = 0;
        VkDescriptorBindingFlags flags  = 0;
    };
    std::map<uint32_t, std::map<uint32_t, MergedBinding>> merged_sets;

    for (uint32_t i = 0; i < _stageCount; ++i) {
        const auto&            stage = _stages[i];
        SpvReflectShaderModule module;
        if (spvReflectCreateShaderModule(stage.size, stage.code, &module) != SPV_REFLECT_RESULT_SUCCESS) {
            continue;
        }

        uint32_t set_count = 0;
        spvReflectEnumerateDescriptorSets(&module, &set_count, nullptr);
        std::vector<SpvReflectDescriptorSet*> sets(set_count);
        spvReflectEnumerateDescriptorSets(&module, &set_count, sets.data());

        for (const auto* reflected_set: sets) {
            for (uint32_t b = 0; b < reflected_set->binding_count; ++b) {
                const auto* rb = reflected_set->bindings[b];

                auto& merged = merged_sets[reflected_set->set][rb->binding];
                merged.type  = static_cast<VkDescriptorType>(rb->descriptor_type);
                // Runtime-sized arrays (Slang `T arr[]` in a ParameterBlock)
                // reflect with count == 0; treat them as bindless pools.
                const bool is_runtime_array = (rb->count == 0);
                const bool is_bindless_pool = is_runtime_array || (rb->count >= 1024);
                merged.count                = is_bindless_pool ? 4096 : rb->count;
                merged.stages |= stage.stage;
                if (is_bindless_pool) {
                    merged.flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
                }
            }
        }

        spvReflectDestroyShaderModule(&module);
    }

    bool any = false;
    for (const auto& [set_idx, bindings]: merged_sets) {
        if (set_idx >= 4) {
            break;
        }
        auto& target = out[set_idx];
        target.bindings.reserve(bindings.size());
        for (const auto& [binding_idx, merged]: bindings) {
            target.bindings.push_back(
                {.binding         = binding_idx,
                 .descriptorType  = merged.type,
                 .descriptorCount = merged.count,
                 .stageFlags      = merged.stages,
                 .bindingFlags    = merged.flags}
            );
            any = true;
        }
    }
    return any;
}

} // namespace ZHLN::Vk
