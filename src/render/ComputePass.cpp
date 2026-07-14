// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/ComputePass.cpp
#include "ComputePass.hpp"

namespace ZHLN::Vk {

std::expected<void, ZHLN::Error> ComputePass::Build(
    VkDevice                   device,
    VkDescriptorSetLayout      descriptorLayout,
    const ZHLN_ShaderDesc&     shader,
    const VkPushConstantRange* pushConstants,
    uint32_t                   pushCount
) noexcept {
    ZHLN_PipelineLayoutDesc p_layout_desc = {
        .set_layouts = &descriptorLayout, .set_layout_count = 1, .push_constants = pushConstants, .push_constant_count = pushCount
    };
    VkPipelineLayout raw_layout = ZHLN_CreatePipelineLayout(device, &p_layout_desc);
    if (raw_layout == VK_NULL_HANDLE) {
        return std::unexpected(RenderInitError::PipelineLayoutCreationFailed);
    }
    pipelineLayout = PipelineLayout(device, raw_layout);

    auto p_res = ComputePipelineBuilder().Shader(shader).Layout(pipelineLayout.Get()).Build(device);
    if (!p_res) {
        return std::unexpected(p_res.error());
    }
    pipeline = std::move(*p_res);
    return {};
}

std::expected<void, ZHLN::Error> ComputePass::BuildVariants(
    VkDevice                              device,
    VkDescriptorSetLayout                 descriptorLayout,
    const ZHLN_ShaderDesc&                shader,
    std::span<const VkSpecializationInfo> specInfos,
    const VkPushConstantRange*            pushConstants,
    uint32_t                              pushCount
) noexcept {
    ZHLN_PipelineLayoutDesc p_layout_desc = {
        .set_layouts = &descriptorLayout, .set_layout_count = 1, .push_constants = pushConstants, .push_constant_count = pushCount
    };
    VkPipelineLayout raw_layout = ZHLN_CreatePipelineLayout(device, &p_layout_desc);
    if (raw_layout == VK_NULL_HANDLE) {
        return std::unexpected(RenderInitError::PipelineLayoutCreationFailed);
    }
    pipelineLayout = PipelineLayout(device, raw_layout);

    pipelines.clear();
    pipelines.reserve(specInfos.size());

    for (const auto& spec: specInfos) {
        auto p_res = ComputePipelineBuilder().Shader(shader).Layout(pipelineLayout.Get()).Specialization(&spec).Build(device);
        if (!p_res) {
            return std::unexpected(p_res.error());
        }
        pipelines.push_back(std::move(*p_res));
    }

    if (pipelines.empty()) {
        return std::unexpected(RenderInitError::PipelineCreationFailed);
    }
    return {};
}

} // namespace ZHLN::Vk
