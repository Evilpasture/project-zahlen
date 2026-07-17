#pragma once

#include "ComputePass.hpp"

namespace ZHLN::Vk {

template <typename LayoutT>
inline bool DoubleBufferedComputePass<LayoutT>::Build(
    VkDevice                   device,
    const ZHLN_ShaderDesc&     shader,
    const VkPushConstantRange* pushConstants,
    uint32_t                   pushCount
) noexcept {
    descLayout = LayoutT::CreateLayout(device);
    pool       = LayoutT::CreatePool(device, 2);
    sets[0]    = LayoutT::Allocate(device, pool.Get(), descLayout.Get());
    sets[1]    = LayoutT::Allocate(device, pool.Get(), descLayout.Get());

    VkDescriptorSetLayout         raw_layout    = descLayout.Get();
    const ZHLN_PipelineLayoutDesc p_layout_desc = {
        .set_layouts = &raw_layout, .set_layout_count = 1, .push_constants = pushConstants, .push_constant_count = pushCount
    };
    pipelineLayout = PipelineLayout(device, ZHLN_CreatePipelineLayout(device, &p_layout_desc));

    auto p_res = ComputePipelineBuilder().Shader(shader).Layout(pipelineLayout.Get()).Build(device);
    if (!p_res) {
        return false;
    }
    pipeline = std::move(*p_res);
    return true;
}

template <typename LayoutT>
inline void DoubleBufferedComputePass<LayoutT>::Dispatch(VkCommandBuffer cmd, uint32_t x, uint32_t y, uint32_t z) const noexcept {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.Get());
    VkDescriptorSet set = sets.Next();
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout.Get(), 0, 1, &set, 0, nullptr);
    vkCmdDispatch(cmd, x, y, z);
}

template <typename LayoutT>
template <GpuTriviallyCopyable T>
inline void DoubleBufferedComputePass<LayoutT>::Dispatch(VkCommandBuffer cmd, uint32_t x, uint32_t y, uint32_t z, const T& pushData) const noexcept {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.Get());
    VkDescriptorSet set = sets.Next();
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout.Get(), 0, 1, &set, 0, nullptr);
    Push(cmd, pipelineLayout.Get(), VK_SHADER_STAGE_COMPUTE_BIT, pushData);
    vkCmdDispatch(cmd, x, y, z);
}

} // namespace ZHLN::Vk
