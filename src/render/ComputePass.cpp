// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/ComputePass.cpp
#include "ComputePass.hpp"

namespace ZHLN::Vk {

bool ComputePass::Build(VkDevice device, VkDescriptorSetLayout descriptorLayout,
						const ZHLN_ShaderDesc& shader, const VkPushConstantRange* pushConstants,
						uint32_t pushCount) noexcept {
	ZHLN_PipelineLayoutDesc pLayoutDesc = {.set_layouts = &descriptorLayout,
										   .set_layout_count = 1,
										   .push_constants = pushConstants,
										   .push_constant_count = pushCount};
	pipelineLayout = PipelineLayout(device, ZHLN_CreatePipelineLayout(device, &pLayoutDesc));

	auto p_res = ComputePipelineBuilder().Shader(shader).Layout(pipelineLayout.Get()).Build(device);

	if (!p_res) {
		return false;
	}
	pipeline = std::move(*p_res);
	return true;
}

bool ComputePass::BuildVariants(VkDevice device, VkDescriptorSetLayout descriptorLayout,
								const ZHLN_ShaderDesc& shader,
								std::span<const VkSpecializationInfo> specInfos,
								const VkPushConstantRange* pushConstants,
								uint32_t pushCount) noexcept {
	ZHLN_PipelineLayoutDesc pLayoutDesc = {.set_layouts = &descriptorLayout,
										   .set_layout_count = 1,
										   .push_constants = pushConstants,
										   .push_constant_count = pushCount};
	pipelineLayout = PipelineLayout(device, ZHLN_CreatePipelineLayout(device, &pLayoutDesc));

	pipelines.clear();
	pipelines.reserve(specInfos.size());

	for (const auto& spec : specInfos) {
		auto p_res = ComputePipelineBuilder()
						 .Shader(shader)
						 .Layout(pipelineLayout.Get())
						 .Specialization(&spec)
						 .Build(device);

		if (!p_res) {
			return false;
		}
		pipelines.push_back(std::move(*p_res));
	}
	return !pipelines.empty();
}

} // namespace ZHLN::Vk
