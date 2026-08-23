// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ReflectedLayout.hpp"
#include <cstring>
#include <map>
#include <spirv_reflect.h>
#include <vector>

namespace ZHLN::Vk {

namespace {

// Reserved specialization-constant IDs used only as reflected metadata for a
// shader-owned fixed logical dispatch domain.
constexpr std::array<uint32_t, 3> kDispatchSizeConstantIds = {1000, 1001, 1002};

} // namespace

auto ReflectComputeThreadGroupSize(const ZHLN_ShaderDesc& shader) noexcept -> std::optional<std::array<uint32_t, 3>> {
    if (shader.code == nullptr || shader.size == 0) {
        return std::nullopt;
    }

    SpvReflectShaderModule module;
    if (spvReflectCreateShaderModule(shader.size, shader.code, &module) != SPV_REFLECT_RESULT_SUCCESS) {
        return std::nullopt;
    }

    const SpvReflectEntryPoint* entryPoint = nullptr;
    if (shader.entry_point != nullptr && shader.entry_point[0] != '\0') {
        entryPoint = spvReflectGetEntryPoint(&module, shader.entry_point);
    } else {
        // A descriptor with no explicit name is valid only when its module has
        // one compute entry point. Refuse ambiguity rather than reflecting a
        // different kernel's LocalSize by accident.
        for (uint32_t i = 0; i < module.entry_point_count; ++i) {
            const auto& candidate = module.entry_points[i];
            if (candidate.shader_stage != SPV_REFLECT_SHADER_STAGE_COMPUTE_BIT) {
                continue;
            }
            if (entryPoint != nullptr) {
                entryPoint = nullptr;
                break;
            }
            entryPoint = &candidate;
        }
    }

    const auto isConcreteSize = [](uint32_t value) { return value > 0 && value != SPV_REFLECT_EXECUTION_MODE_SPEC_CONSTANT; };

    std::optional<std::array<uint32_t, 3>> result;
    if (entryPoint != nullptr && entryPoint->shader_stage == SPV_REFLECT_SHADER_STAGE_COMPUTE_BIT && isConcreteSize(entryPoint->local_size.x) &&
        isConcreteSize(entryPoint->local_size.y) && isConcreteSize(entryPoint->local_size.z)) {
        result = std::array<uint32_t, 3> {entryPoint->local_size.x, entryPoint->local_size.y, entryPoint->local_size.z};
    }

    spvReflectDestroyShaderModule(&module);
    return result;
}

auto ReflectComputeDispatchSize(const ZHLN_ShaderDesc& shader) noexcept -> std::optional<std::array<uint32_t, 3>> {
    if (shader.code == nullptr || shader.size == 0) {
        return std::nullopt;
    }

    SpvReflectShaderModule module;
    if (spvReflectCreateShaderModule(shader.size, shader.code, &module) != SPV_REFLECT_RESULT_SUCCESS) {
        return std::nullopt;
    }

    std::array<uint32_t, 3> dispatchSize {};
    std::array<bool, 3>     found {};
    for (uint32_t i = 0; i < module.spec_constant_count; ++i) {
        const auto& constant = module.spec_constants[i];
        for (size_t axis = 0; axis < kDispatchSizeConstantIds.size(); ++axis) {
            if (constant.constant_id != kDispatchSizeConstantIds[axis] || constant.default_value == nullptr ||
                constant.default_value_size != sizeof(uint32_t)) {
                continue;
            }
            std::memcpy(&dispatchSize[axis], constant.default_value, sizeof(uint32_t));
            found[axis] = dispatchSize[axis] > 0;
        }
    }

    spvReflectDestroyShaderModule(&module);
    if (!found[0] || !found[1] || !found[2]) {
        return std::nullopt;
    }
    return dispatchSize;
}

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
