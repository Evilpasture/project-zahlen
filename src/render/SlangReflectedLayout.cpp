// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "SlangReflectedLayout.hpp"
#include "ReflectedLayout.hpp"
#include "ShaderStages.hpp"
#include <map>
#include <vector>

namespace ZHLN::Vk {

/**
 * @brief Maps a leaf Slang TypeLayout to the equivalent Vulkan VkDescriptorType.
 */
static auto MapSlangTypeToVk(slang::TypeLayoutReflection* typeLayout) -> VkDescriptorType {
    slang::TypeReflection* type = typeLayout->getType();
    auto                   kind = type->getKind();

    switch (kind) {
        case slang::TypeReflection::Kind::ConstantBuffer:
            return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        case slang::TypeReflection::Kind::SamplerState:
            return VK_DESCRIPTOR_TYPE_SAMPLER;
        case slang::TypeReflection::Kind::ShaderStorageBuffer:
            return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        case slang::TypeReflection::Kind::TextureBuffer:
            return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
        case slang::TypeReflection::Kind::Resource: {
            SlangResourceShape  shape  = type->getResourceShape();
            SlangResourceAccess access = type->getResourceAccess();

            auto base_shape    = static_cast<uint32_t>(shape & SLANG_RESOURCE_BASE_SHAPE_MASK);
            bool is_read_write = (access == SLANG_RESOURCE_ACCESS_READ_WRITE);

            if (base_shape == SLANG_STRUCTURED_BUFFER || base_shape == SLANG_BYTE_ADDRESS_BUFFER) {
                return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            }
            if (base_shape == SLANG_TEXTURE_BUFFER) {
                return is_read_write ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
            }
            return is_read_write ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        }
        default:
            break;
    }
    return VK_DESCRIPTOR_TYPE_MAX_ENUM;
}

/**
 * @brief Recursively walks variables inside ConstantBuffers, ParameterBlocks, and structures.
 */
static void TraverseVariable(
    slang::VariableLayoutReflection*                                      varLayout,
    VkShaderStageFlags                                                    stages,
    std::map<uint32_t, std::map<uint32_t, VkDescriptorSetLayoutBinding>>& sets,
    std::map<uint32_t, std::map<uint32_t, VkDescriptorBindingFlags>>&     bindingFlags,
    std::vector<VkPushConstantRange>&                                     pushConstants
) {
    if (!varLayout) {
        return;
    }

    slang::TypeLayoutReflection* type_layout = varLayout->getTypeLayout();
    if (!type_layout) {
        return;
    }

    slang::TypeReflection* type = type_layout->getType();
    if (!type) {
        return;
    }

    auto kind = type->getKind();

    // 1. Parse Push Constant Blocks
    if (varLayout->getCategory() == slang::ParameterCategory::PushConstantBuffer) {
        pushConstants.push_back(
            {.stageFlags = stages,
             .offset     = static_cast<uint32_t>(varLayout->getOffset(slang::ParameterCategory::PushConstantBuffer)),
             .size       = static_cast<uint32_t>(type_layout->getSize())}
        );
        return;
    }

    // 2. Parse Descriptors/Arrays
    if (kind == slang::TypeReflection::Kind::Array) {
        auto                         element_count  = static_cast<uint32_t>(type->getElementCount());
        slang::TypeLayoutReflection* element_layout = type_layout->getElementTypeLayout();
        VkDescriptorType             desc_type      = MapSlangTypeToVk(element_layout);

        if (desc_type != VK_DESCRIPTOR_TYPE_MAX_ENUM) {
            uint32_t set_idx     = varLayout->getBindingSpace();
            uint32_t binding_idx = varLayout->getBindingIndex();

            sets[set_idx][binding_idx] = {
                .binding            = binding_idx,
                .descriptorType     = desc_type,
                .descriptorCount    = (element_count == 0) ? 4096 : element_count,
                .stageFlags         = stages,
                .pImmutableSamplers = nullptr
            };

            if (element_count == 0 || element_count >= 1024) {
                bindingFlags[set_idx][binding_idx] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
            }
        }
        return;
    }

    // 3. Recurse Parameter Blocks and Constant Buffers
    if (kind == slang::TypeReflection::Kind::ParameterBlock || kind == slang::TypeReflection::Kind::ConstantBuffer) {
        uint32_t set_idx     = varLayout->getBindingSpace();
        uint32_t binding_idx = varLayout->getBindingIndex();

        if (kind == slang::TypeReflection::Kind::ConstantBuffer) {
            sets[set_idx][binding_idx] = {
                .binding            = binding_idx,
                .descriptorType     = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .descriptorCount    = 1,
                .stageFlags         = stages,
                .pImmutableSamplers = nullptr
            };
        }

        slang::TypeLayoutReflection* element_layout = type_layout->getElementVarLayout()->getTypeLayout();
        if (element_layout) {
            auto field_count = element_layout->getFieldCount();
            for (uint32_t i = 0; i < field_count; ++i) {
                TraverseVariable(element_layout->getFieldByIndex(i), stages, sets, bindingFlags, pushConstants);
            }
        }
        return;
    }

    // 4. Resolve leaf bindings
    if (varLayout->getCategory() == slang::ParameterCategory::DescriptorTableSlot) {
        uint32_t         set_idx     = varLayout->getBindingSpace();
        uint32_t         binding_idx = varLayout->getBindingIndex();
        VkDescriptorType desc_type   = MapSlangTypeToVk(type_layout);

        if (desc_type != VK_DESCRIPTOR_TYPE_MAX_ENUM) {
            sets[set_idx][binding_idx] = {
                .binding = binding_idx, .descriptorType = desc_type, .descriptorCount = 1, .stageFlags = stages, .pImmutableSamplers = nullptr
            };
        }
    }
}

auto SlangReflectedLayoutBuilder::Build(VkDevice device, slang::ProgramLayout* programLayout, VkShaderStageFlags stages) noexcept -> SlangReflectedLayout {
    SlangReflectedLayout result;
    if (!programLayout) {
        return result;
    }

    std::map<uint32_t, std::map<uint32_t, VkDescriptorSetLayoutBinding>> merged_sets;
    std::map<uint32_t, std::map<uint32_t, VkDescriptorBindingFlags>>     merged_flags;
    std::vector<VkPushConstantRange>                                     push_ranges;

    auto param_count = programLayout->getParameterCount();
    for (uint32_t i = 0; i < param_count; ++i) {
        slang::VariableLayoutReflection* var_layout = programLayout->getParameterByIndex(i);
        TraverseVariable(var_layout, stages, merged_sets, merged_flags, push_ranges);
    }

    // Allocate Descriptor Set Layouts (Support up to 4 sets)
    uint32_t set_layout_count = 0;
    for (const auto& [set_idx, bindings_map]: merged_sets) {
        if (set_layout_count >= 4) {
            break;
        }

        std::vector<VkDescriptorSetLayoutBinding> bindings;
        std::vector<VkDescriptorBindingFlags>     binding_flags;
        bool                                      has_bindless_flags = false;

        for (const auto& [binding_idx, binding]: bindings_map) {
            bindings.push_back(binding);
            result.descriptorTypeCounts[binding.descriptorType] += binding.descriptorCount;

            VkDescriptorBindingFlags flags = 0;
            auto                     it    = merged_flags[set_idx].find(binding_idx);
            if (it != merged_flags[set_idx].end()) {
                flags = it->second;
            }
            binding_flags.push_back(flags);
            if (flags != 0) {
                has_bindless_flags = true;
            }

            result.reflectedSets[set_idx].bindings.push_back(
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
            .flags        = has_bindless_flags ? static_cast<VkDescriptorSetLayoutCreateFlags>(VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT) : 0U,
            .bindingCount = static_cast<uint32_t>(bindings.size()),
            .pBindings    = bindings.data()
        };

        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        vkCreateDescriptorSetLayout(device, &create_info, nullptr, &layout);

        result.descriptorSetLayouts[set_layout_count] = DescriptorSetLayout(device, layout);
        set_layout_count++;
    }
    result.setLayoutCount = set_layout_count;

    // Collect push constants
    std::vector<VkDescriptorSetLayout> raw_set_layouts(set_layout_count);
    for (uint32_t i = 0; i < set_layout_count; ++i) {
        raw_set_layouts[i] = result.descriptorSetLayouts[i].Get();
    }

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

auto SlangReflectedLayout::CreatePool(VkDevice device, uint32_t maxSets) const noexcept -> DescriptorPool {
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
        .flags         = (update_after_bind ? static_cast<VkDescriptorPoolCreateFlags>(VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT) : 0U) |
                         VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets       = maxSets,
        .poolSizeCount = static_cast<uint32_t>(pool_sizes.size()),
        .pPoolSizes    = pool_sizes.data(),
    };

    VkDescriptorPool pool = VK_NULL_HANDLE;
    vkCreateDescriptorPool(device, &info, nullptr, &pool);
    return {device, pool};
}

auto SlangReflectedLayout::Allocate(VkDevice device, VkDescriptorPool pool, VkDescriptorSetLayout layout) noexcept -> VkDescriptorSet {
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

bool SlangReflectedLayout::Build(VkDevice device, const ShaderStages& shaders) noexcept {
    UnsafeReflectedLayoutBuilder builder;
    auto                         vert_spv = shaders.GetVertSpv();
    auto                         frag_spv = shaders.GetFragSpv();
    if (!vert_spv.empty()) {
        builder.AddStageUnsafe({.code = vert_spv.data(), .size = vert_spv.size() * 4, .entry_point = {}}, VK_SHADER_STAGE_VERTEX_BIT);
    }
    if (!frag_spv.empty()) {
        builder.AddStageUnsafe({.code = frag_spv.data(), .size = frag_spv.size() * 4, .entry_point = {}}, VK_SHADER_STAGE_FRAGMENT_BIT);
    }
    AdoptUnsafe(builder.BuildUnsafe(device));
    return setLayoutCount > 0;
}

bool SlangReflectedLayout::Build(VkDevice device, const ZHLN_ShaderDesc& shader, VkShaderStageFlagBits stage) noexcept {
    UnsafeReflectedLayoutBuilder builder;
    builder.AddStageUnsafe(shader, stage);
    AdoptUnsafe(builder.BuildUnsafe(device));
    return setLayoutCount > 0;
}

bool SlangReflectedLayout::Build(VkDevice device, std::span<const ReflectedStageInput> stages) noexcept {
    UnsafeReflectedLayoutBuilder builder;
    for (const auto& s: stages) {
        builder.AddStageUnsafe(s.shader, s.stage);
    }
    AdoptUnsafe(builder.BuildUnsafe(device));
    return setLayoutCount > 0;
}

void SlangReflectedLayout::AdoptUnsafe(UnsafeReflectedLayout&& unsafe_res) noexcept {
    this->pipelineLayout       = std::move(unsafe_res.pipelineLayout);
    this->descriptorSetLayouts = std::move(unsafe_res.descriptorSetLayouts);
    this->setLayoutCount       = unsafe_res.setLayoutCount;
    this->descriptorTypeCounts = unsafe_res.descriptorTypeCounts;
    for (size_t s = 0; s < 4; ++s) {
        this->reflectedSets[s].bindings.clear();
        for (const auto& b: unsafe_res.reflectedSets[s].bindings) {
            this->reflectedSets[s].bindings.push_back(
                {.binding         = b.binding,
                 .descriptorType  = b.descriptorType,
                 .descriptorCount = b.descriptorCount,
                 .stageFlags      = b.stageFlags,
                 .bindingFlags    = b.bindingFlags}
            );
        }
    }
}

[[nodiscard]] auto SlangReflectedLayout::CreateLayout(VkDevice /*unused*/) const noexcept -> VkDescriptorSetLayout {
    return GetSetLayout(0);
}
} // namespace ZHLN::Vk
