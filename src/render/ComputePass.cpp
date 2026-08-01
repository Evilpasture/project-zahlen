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
    PipelineLayoutBuilder builder(device);
    builder.AddDescriptorSetLayout(descriptorLayout);
    for (uint32_t i = 0; i < pushCount; ++i) {
        builder.AddPushConstant(pushConstants[i].stageFlags, pushConstants[i].size, pushConstants[i].offset);
    }

    auto layout_res = builder.Build();
    if (!layout_res) {
        return std::unexpected(RenderInitError::PipelineLayoutCreationFailed);
    }
    pipelineLayout = std::move(layout_res.value());

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
    PipelineLayoutBuilder builder(device);
    builder.AddDescriptorSetLayout(descriptorLayout);
    for (uint32_t i = 0; i < pushCount; ++i) {
        builder.AddPushConstant(pushConstants[i].stageFlags, pushConstants[i].size, pushConstants[i].offset);
    }

    auto layout_res = builder.Build();
    if (!layout_res) {
        return std::unexpected(RenderInitError::PipelineLayoutCreationFailed);
    }
    pipelineLayout = std::move(layout_res.value());

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
