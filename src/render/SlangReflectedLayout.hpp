// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/render/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <slang/slang.h>

namespace ZHLN::Vk {

/**
 * @brief Holds descriptor and pipeline layout handles generated via Slang reflection.
 */
struct SlangReflectedLayout {
    PipelineLayout                     pipelineLayout;
    std::array<DescriptorSetLayout, 4> descriptorSetLayouts;
    uint32_t                           setLayoutCount = 0;

    // Tracks the exact count of each descriptor type needed across all sets
    std::array<uint32_t, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT + 1> descriptorTypeCounts {};

    [[nodiscard]] auto GetSetLayout(uint32_t setIndex = 0) const noexcept -> VkDescriptorSetLayout {
        return setIndex < setLayoutCount ? descriptorSetLayouts[setIndex].Get() : VK_NULL_HANDLE;
    }
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
