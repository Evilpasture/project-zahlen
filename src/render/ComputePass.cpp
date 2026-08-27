// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/ComputePass.cpp
#include "ComputePass.hpp"

namespace ZHLN::Vk {

std::expected<void, ZHLN::Error> ComputePass::BuildHeap(
    VkDevice                                             device,
    const ZHLN_ShaderDesc&                               shader,
    const VkShaderDescriptorSetAndBindingMappingInfoEXT* mapping,
    uint32_t                                             indexPushOffset
) noexcept {
    // VK_EXT_descriptor_heap: heap pipelines require layout == VK_NULL_HANDLE
    // (VUID-VkComputePipelineCreateInfo-flags-11311). Per-dispatch data
    // travels through vkCmdPushDataEXT.
    if (!ReflectDispatchLayout(shader)) {
        return std::unexpected(PipelineBuilderError::PipelineCreationFailed);
    }
    pipelineLayout      = {};
    heapIndexPushOffset = indexPushOffset;

    auto p_res = ComputePipelineBuilder().Shader(shader).Layout(VK_NULL_HANDLE).HeapMappings(mapping).Build(device);
    if (!p_res) {
        return std::unexpected(p_res.error());
    }
    pipeline = std::move(*p_res);
    return {};
}

std::expected<void, ZHLN::Error> ComputePass::BuildHeapVariants(
    VkDevice                                             device,
    const ZHLN_ShaderDesc&                               shader,
    std::span<const VkSpecializationInfo>                specInfos,
    const VkShaderDescriptorSetAndBindingMappingInfoEXT* mapping,
    uint32_t                                             indexPushOffset
) noexcept {
    if (!ReflectDispatchLayout(shader)) {
        return std::unexpected(PipelineBuilderError::PipelineCreationFailed);
    }
    heapIndexPushOffset = indexPushOffset;
    pipelines.clear();
    pipelines.reserve(specInfos.size());

    for (const auto& spec: specInfos) {
        auto p_res = ComputePipelineBuilder().Shader(shader).Layout(VK_NULL_HANDLE).HeapMappings(mapping).Specialization(&spec).Build(device);
        if (!p_res) {
            return std::unexpected(p_res.error());
        }
        pipelines.push_back(std::move(*p_res));
    }

    if (pipelines.empty()) {
        return std::unexpected(PipelineBuilderError::PipelineCreationFailed);
    }
    return {};
}

} // namespace ZHLN::Vk
