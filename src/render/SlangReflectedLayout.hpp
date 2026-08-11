// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/render/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <array>
#include <slang/slang.h>
#include <tuple>
#include <utility>
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

/**
 * @brief Holds descriptor and pipeline layout handles generated via Slang reflection.
 */
struct SlangReflectedLayout {
    PipelineLayout                     pipelineLayout;
    std::array<DescriptorSetLayout, 4> descriptorSetLayouts;
    uint32_t                           setLayoutCount = 0;

    // Tracks the exact count of each descriptor type needed across all sets
    std::array<uint32_t, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT + 1> descriptorTypeCounts {};
    std::array<SlangReflectedSet, 4>                              reflectedSets {};

    [[nodiscard]] auto GetSetLayout(uint32_t setIndex = 0) const noexcept -> VkDescriptorSetLayout {
        return setIndex < setLayoutCount ? descriptorSetLayouts[setIndex].Get() : VK_NULL_HANDLE;
    }

    [[nodiscard]] auto CreateLayout(VkDevice device = VK_NULL_HANDLE) const noexcept -> VkDescriptorSetLayout;

    [[nodiscard]] auto CreatePool(VkDevice device, uint32_t maxSets) const noexcept -> DescriptorPool;

    [[nodiscard]] static auto Allocate(VkDevice device, VkDescriptorPool pool, VkDescriptorSetLayout layout) noexcept -> VkDescriptorSet;

    template <typename... Args>
    void Write(VkDevice device, VkDescriptorSet set, Args&&... args) const noexcept {
        const auto                                                        arg_tuple = std::forward_as_tuple(std::forward<Args>(args)...);
        constexpr size_t                                                  k_count   = sizeof...(Args);
        std::array<VkDescriptorImageInfo, k_count>                        image_infos {};
        std::array<VkDescriptorBufferInfo, k_count>                       buffer_infos {};
        std::array<VkWriteDescriptorSetAccelerationStructureKHR, k_count> as_infos {};
        std::array<VkWriteDescriptorSet, k_count>                         writes {};

        [&]<size_t... I>(std::index_sequence<I...>) {
            (detail::WriteReflectedBinding(
                 set, I < reflectedSets[0].bindings.size() ? reflectedSets[0].bindings[I].binding : 0,
                 I < reflectedSets[0].bindings.size() ? reflectedSets[0].bindings[I].descriptorType : VK_DESCRIPTOR_TYPE_MAX_ENUM, std::get<I>(arg_tuple),
                 image_infos[I], buffer_infos[I], as_infos[I], writes[I]
             ),
             ...);
        }(std::make_index_sequence<k_count> {});

        std::array<VkWriteDescriptorSet, k_count> valid_writes {};
        uint32_t                                  valid_count = 0;
        for (uint32_t i = 0; i < k_count; ++i) {
            if (writes[i].descriptorCount > 0) {
                valid_writes[valid_count++] = writes[i];
            }
        }
        if (valid_count > 0) {
            vkUpdateDescriptorSets(device, valid_count, valid_writes.data(), 0, nullptr);
        }
    }

    bool Build(VkDevice device, const ShaderStages& shaders) noexcept;
};

/**
 * @brief Standalone parser to extract Vulkan layout structures from Slang compilation artifacts.
 */
class SlangReflectedLayoutBuilder {
  public:
    SlangReflectedLayoutBuilder() noexcept  = default;
    ~SlangReflectedLayoutBuilder() noexcept = default;

    SlangReflectedLayoutBuilder(const SlangReflectedLayoutBuilder&)                    = delete;
    auto operator=(const SlangReflectedLayoutBuilder&) -> SlangReflectedLayoutBuilder& = delete;
    SlangReflectedLayoutBuilder(SlangReflectedLayoutBuilder&&)                         = delete;
    auto operator=(SlangReflectedLayoutBuilder&&) -> SlangReflectedLayoutBuilder&      = delete;

    /**
     * @brief Parses the provided Slang program layout to generate Vulkan pipeline and descriptor layouts.
     * @param device The active logical VkDevice.
     * @param program_layout Non-owning pointer to the Slang reflection layout interface.
     * @param stages The shader stages associated with these bindings.
     */
    [[nodiscard]] static auto
        Build(VkDevice device, slang::ProgramLayout* programLayout, VkShaderStageFlags stages = VK_SHADER_STAGE_ALL_GRAPHICS) noexcept -> SlangReflectedLayout;
};

} // namespace ZHLN::Vk
