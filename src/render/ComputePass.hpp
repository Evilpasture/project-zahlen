// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once
#include "PipelineBuilder.hpp"
#include "RenderCore.hpp"

namespace ZHLN::Vk {

struct ComputePass {
	PipelineLayout pipelineLayout;
	Pipeline pipeline;

	[[nodiscard]] bool Build(VkDevice device, VkDescriptorSetLayout descriptorLayout,
							 const ZHLN_ShaderDesc& shader,
							 const VkPushConstantRange* pushConstants = nullptr,
							 uint32_t pushCount = 0) noexcept {
		ZHLN_PipelineLayoutDesc pLayoutDesc = {.set_layouts = &descriptorLayout,
											   .set_layout_count = 1,
											   .push_constants = pushConstants,
											   .push_constant_count = pushCount};
		pipelineLayout = PipelineLayout(device, ZHLN_CreatePipelineLayout(device, &pLayoutDesc));

		pipeline =
			ComputePipelineBuilder().Shader(shader).Layout(pipelineLayout.Get()).Build(device);

		return pipeline.Valid();
	}

	template <GpuTriviallyCopyable T>
	void Dispatch(VkCommandBuffer cmd, VkDescriptorSet set, uint32_t groupCountX,
				  uint32_t groupCountY, uint32_t groupCountZ, const T& pushData) const noexcept {
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.Get());
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout.Get(), 0, 1,
								&set, 0, nullptr);
		Push(cmd, pipelineLayout.Get(), VK_SHADER_STAGE_COMPUTE_BIT, pushData);
		ZHLN_CmdDispatch(cmd, groupCountX, groupCountY, groupCountZ);
	}
};

/**
 * @brief Places a execution and memory barrier synchronizing compute shader writes
 * to be safely read by an indirect rendering command call.
 */
inline void CmdBarrierComputeToIndirect(VkCommandBuffer cmd) noexcept {
	const VkMemoryBarrier barrier = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
		.pNext = nullptr,
		.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT,
	};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
						 VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, 0, 1, &barrier, 0, nullptr, 0,
						 nullptr);
}

} // namespace ZHLN::Vk
