// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ReflectedLayout.hpp"
#include "ShaderStages.hpp"
#include <map>
#include <vector>

namespace ZHLN::Vk {

void UnsafeReflectedLayoutBuilder::AddStageUnsafe(const ZHLN_ShaderDesc& desc, VkShaderStageFlags stage) noexcept {
    if ((desc.code != nullptr) && desc.size > 0 && _stageCount < 5) {
        _stages[_stageCount++] = {.code = desc.code, .size = desc.size, .stage = stage};
    }
}

auto UnsafeReflectedLayoutBuilder::BuildUnsafe(VkDevice device) noexcept -> UnsafeReflectedLayout {
    UnsafeReflectedLayout result;

    // Sorted map: SetIndex -> Map: BindingIndex -> VkDescriptorSetLayoutBinding
    std::map<uint32_t, std::map<uint32_t, VkDescriptorSetLayoutBinding>> merged_sets;
    std::map<uint32_t, std::map<uint32_t, VkDescriptorBindingFlags>>     merged_flags;
    std::map<uint32_t, VkPushConstantRange>                              merged_push_constants;

    for (uint32_t i = 0; i < _stageCount; ++i) {
        const auto&            stage = _stages[i];
        SpvReflectShaderModule module;
        SpvReflectResult       reflect_result = spvReflectCreateShaderModule(stage.size, stage.code, &module);
        if (reflect_result != SPV_REFLECT_RESULT_SUCCESS) {
            continue;
        }

        uint32_t set_count = 0;
        spvReflectEnumerateDescriptorSets(&module, &set_count, nullptr);
        std::vector<SpvReflectDescriptorSet*> sets(set_count);
        spvReflectEnumerateDescriptorSets(&module, &set_count, sets.data());

        for (const auto* reflected_set: sets) {
            uint32_t set_idx = reflected_set->set;

            for (uint32_t b = 0; b < reflected_set->binding_count; ++b) {
                const auto* reflected_binding = reflected_set->bindings[b];
                uint32_t    binding_idx       = reflected_binding->binding;

                auto& binding           = merged_sets[set_idx][binding_idx];
                binding.binding         = binding_idx;
                binding.descriptorType  = static_cast<VkDescriptorType>(reflected_binding->descriptor_type);

                // SPIRV-Reflect reports runtime-sized arrays (e.g. Slang's
                // `Texture2D globalTextures[]` inside the GlobalSceneRegistry
                // ParameterBlock) with count == 0. Treat them like any other
                // bindless pool: size them to the engine's bindless capacity.
                const bool is_runtime_array = (reflected_binding->count == 0);
                const bool is_bindless_pool = is_runtime_array || (reflected_binding->count >= 1024);
                binding.descriptorCount     = is_bindless_pool ? 4096 : reflected_binding->count;
                binding.stageFlags |= stage.stage;
                binding.pImmutableSamplers = nullptr;

                // ACCUMULATE EXACT TYPE COUNT FOR THE POOL
                result.descriptorTypeCounts[binding.descriptorType] += binding.descriptorCount;

                if (is_bindless_pool) {
                    merged_flags[set_idx][binding_idx] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
                }
            }
        }

        uint32_t push_count = 0;
        spvReflectEnumeratePushConstantBlocks(&module, &push_count, nullptr);
        std::vector<SpvReflectBlockVariable*> push_blocks(push_count);
        spvReflectEnumeratePushConstantBlocks(&module, &push_count, push_blocks.data());

        for (const auto* block: push_blocks) {
            uint32_t offset = block->offset;
            auto&    range  = merged_push_constants[offset];
            range.offset    = offset;
            range.size      = std::max(range.size, block->size);
            range.stageFlags |= stage.stage;
        }

        spvReflectDestroyShaderModule(&module);
    }

    // Create Descriptor Set Layouts (Capped at 4)
    uint32_t set_layout_count = 0;
    for (const auto& [setIdx, bindingsMap]: merged_sets) {
        if (set_layout_count >= 4) {
            break;
        }

        std::vector<VkDescriptorSetLayoutBinding> bindings;
        std::vector<VkDescriptorBindingFlags>     binding_flags;
        bool                                      has_bindless_flags = false;

        for (const auto& [bindingIdx, binding]: bindingsMap) {
            bindings.push_back(binding);

            VkDescriptorBindingFlags flags = merged_flags[setIdx][bindingIdx];
            binding_flags.push_back(flags);
            if (flags != 0) {
                has_bindless_flags = true;
            }

            result.reflectedSets[setIdx].bindings.push_back(
                {.binding         = binding.binding,
                 .descriptorType  = binding.descriptorType,
                 .descriptorCount = binding.descriptorCount,
                 .stageFlags      = binding.stageFlags,
                 .bindingFlags    = flags}
            );
        }

        const VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info = {
            .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
            .pNext         = nullptr,
            .bindingCount  = static_cast<uint32_t>(binding_flags.size()),
            .pBindingFlags = binding_flags.data()
        };

        const VkDescriptorSetLayoutCreateInfo create_info = {
            .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext        = has_bindless_flags ? &flags_info : nullptr,
            .flags        = has_bindless_flags ? (VkDescriptorSetLayoutCreateFlags) VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT : 0U,
            .bindingCount = static_cast<uint32_t>(bindings.size()),
            .pBindings    = bindings.data()
        };

        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        vkCreateDescriptorSetLayout(device, &create_info, nullptr, &layout);

        result.descriptorSetLayouts[set_layout_count] = DescriptorSetLayout(device, layout);
        set_layout_count++;
    }
    result.setLayoutCount = set_layout_count;

    // Flatten Push Constant Ranges
    std::vector<VkPushConstantRange> push_ranges;
    push_ranges.reserve(merged_push_constants.size());
    for (const auto& [offset, range]: merged_push_constants) {
        push_ranges.push_back(range);
    }

    // Extract raw layouts
    std::vector<VkDescriptorSetLayout> raw_set_layouts(set_layout_count);
    for (uint32_t i = 0; i < set_layout_count; ++i) {
        raw_set_layouts[i] = result.descriptorSetLayouts[i].Get();
    }

    // Create Pipeline Layout
    const VkPipelineLayoutCreateInfo pipeline_layout_info = {
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext                  = nullptr,
        .flags                  = 0,
        .setLayoutCount         = set_layout_count,
        .pSetLayouts            = raw_set_layouts.data(),
        .pushConstantRangeCount = static_cast<uint32_t>(push_ranges.size()),
        .pPushConstantRanges    = push_ranges.data()
    };

    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr, &pipeline_layout);
    result.pipelineLayout = PipelineLayout(device, pipeline_layout);

    return result;
}

auto UnsafeReflectedLayout::CreatePool(VkDevice device, uint32_t maxSets) const noexcept -> DescriptorPool {
    std::vector<VkDescriptorPoolSize> pool_sizes;
    bool                              update_after_bind = false;

    for (const auto& [type, count]: descriptorTypeCounts) {
        if (count > 0) {
            pool_sizes.push_back({.type = type, .descriptorCount = count * maxSets});
        }
    }
    if (pool_sizes.empty()) {
        pool_sizes.push_back({.type = VK_DESCRIPTOR_TYPE_SAMPLER, .descriptorCount = maxSets});
    }

    for (const auto& set: reflectedSets) {
        for (const auto& b: set.bindings) {
            if ((b.bindingFlags & VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT) != 0) {
                update_after_bind = true;
            }
        }
    }

    const VkDescriptorPoolCreateInfo info = {
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .pNext         = nullptr,
        .flags         = (update_after_bind ? VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT : 0U) | VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets       = maxSets,
        .poolSizeCount = static_cast<uint32_t>(pool_sizes.size()),
        .pPoolSizes    = pool_sizes.data(),
    };

    VkDescriptorPool pool = VK_NULL_HANDLE;
    vkCreateDescriptorPool(device, &info, nullptr, &pool);
    return {device, pool};
}

auto UnsafeReflectedLayout::Allocate(VkDevice device, VkDescriptorPool pool, VkDescriptorSetLayout layout) noexcept -> VkDescriptorSet {
    const VkDescriptorSetAllocateInfo info = {
        .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .pNext              = nullptr,
        .descriptorPool     = pool,
        .descriptorSetCount = 1,
        .pSetLayouts        = &layout,
    };
    VkDescriptorSet set = VK_NULL_HANDLE;
    vkAllocateDescriptorSets(device, &info, &set);
    return set;
}

bool UnsafeReflectedLayout::Build(VkDevice device, const ShaderStages& shaders) noexcept {
    UnsafeReflectedLayoutBuilder builder;
    auto                         vert_spv = shaders.GetVertSpv();
    auto                         frag_spv = shaders.GetFragSpv();
    if (!vert_spv.empty()) {
        builder.AddStageUnsafe({.code = vert_spv.data(), .size = vert_spv.size() * 4, .entry_point = {}}, VK_SHADER_STAGE_VERTEX_BIT);
    }
    if (!frag_spv.empty()) {
        builder.AddStageUnsafe({.code = frag_spv.data(), .size = frag_spv.size() * 4, .entry_point = {}}, VK_SHADER_STAGE_FRAGMENT_BIT);
    }
    *this = builder.BuildUnsafe(device);
    return setLayoutCount > 0;
}

[[nodiscard]] auto UnsafeReflectedLayout::CreateLayout(VkDevice /*unused*/) const noexcept -> VkDescriptorSetLayout {
    return GetSetLayout(0);
}
} // namespace ZHLN::Vk
