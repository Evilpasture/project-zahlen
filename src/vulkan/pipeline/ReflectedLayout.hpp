// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ZHLN::Vk {

class ShaderStagesView;

struct ReflectedBinding {
    uint32_t                 binding         = 0;
    VkDescriptorType         descriptorType  = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    uint32_t                 descriptorCount = 1;
    VkShaderStageFlags       stageFlags      = 0;
    VkDescriptorBindingFlags bindingFlags    = 0;
    std::string              name;
};

struct ReflectedSet {
    std::vector<ReflectedBinding> bindings;
};

struct ReflectedStageInput {
    ZHLN_ShaderDesc       shader;
    VkShaderStageFlagBits stage;
};

struct ReflectedLayout {
    std::array<ReflectedSet, 4> sets {};

    [[nodiscard]] auto HasBinding(uint32_t setIndex, uint32_t binding) const noexcept -> bool {
        if (setIndex >= 4) {
            return false;
        }
        for (const auto& b: sets[setIndex].bindings) {
            if (b.binding == binding) {
                return true;
            }
        }
        return false;
    }

    bool Build(VkDevice device, ShaderStagesView shaders) noexcept;

    bool Build(VkDevice device, const ZHLN_ShaderDesc& shader, VkShaderStageFlagBits stage) noexcept;

    bool Build(VkDevice device, std::span<const ReflectedStageInput> stages) noexcept;
};

[[nodiscard]] auto ReflectComputeThreadGroupSize(const ZHLN_ShaderDesc& shader) noexcept -> std::optional<std::array<uint32_t, 3>>;

[[nodiscard]] auto ReflectComputeDispatchSize(const ZHLN_ShaderDesc& shader) noexcept -> std::optional<std::array<uint32_t, 3>>;

[[nodiscard]] auto ReflectSpecializationConstantU32(const ZHLN_ShaderDesc& shader, uint32_t constantId) noexcept -> std::optional<uint32_t>;
[[nodiscard]] auto ReflectSpecializationConstantF32(const ZHLN_ShaderDesc& shader, uint32_t constantId) noexcept -> std::optional<float>;

class ReflectedLayoutBuilder {
  public:
    ReflectedLayoutBuilder() noexcept = default;

    ReflectedLayoutBuilder(ReflectedLayoutBuilder&&)                 = delete;
    ReflectedLayoutBuilder& operator=(ReflectedLayoutBuilder&&)      = delete;
    ReflectedLayoutBuilder(const ReflectedLayoutBuilder&)            = delete;
    ReflectedLayoutBuilder& operator=(const ReflectedLayoutBuilder&) = delete;
    ~ReflectedLayoutBuilder() noexcept                               = default;

    void AddStageUnsafe(const ZHLN_ShaderDesc& desc, VkShaderStageFlags stage) noexcept;

    [[nodiscard]] auto BuildUnsafe(std::array<ReflectedSet, 4>& out) noexcept -> bool;

  private:
    struct StageData {
        const uint32_t*    code  = nullptr;
        size_t             size  = 0;
        VkShaderStageFlags stage = 0;
    };
    std::array<StageData, 8> _stages {};
    uint32_t                 _stageCount = 0;
};

} // namespace ZHLN::Vk
