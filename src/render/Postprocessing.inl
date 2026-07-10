// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/Postprocessing.inl
#pragma once

#include "Postprocessing.hpp"

namespace ZHLN::Vk {

template <typename LayoutT>
bool PostProcessPass<LayoutT>::Build(VkDevice device, const ShaderStages& shaders,
									 std::initializer_list<VkFormat> colorFormats,
									 const VkPushConstantRange* pushConstants, uint32_t pushCount,
									 bool additive) noexcept {
	descLayout = LayoutT::CreateLayout(device);
	pool = LayoutT::CreatePool(device, 2);

	sets[0] = LayoutT::Allocate(device, pool.Get(), descLayout.Get());
	sets[1] = LayoutT::Allocate(device, pool.Get(), descLayout.Get());

	VkDescriptorSetLayout rawLayout = descLayout.Get();
	const ZHLN_PipelineLayoutDesc pLayoutDesc = {.set_layouts = &rawLayout,
												 .set_layout_count = 1,
												 .push_constants = pushConstants,
												 .push_constant_count = pushCount};
	pipelineLayout = PipelineLayout(device, ZHLN_CreatePipelineLayout(device, &pLayoutDesc));

	auto builder = PipelineBuilder{}
					   .Shaders(shaders)
					   .Layout(pipelineLayout.Get())
					   .ColorFormats(colorFormats)
					   .NoDepth()
					   .CullNone();

	if (additive) {
		builder.AdditiveBlend();
	}

	auto p_res = builder.Build(device);
	if (!p_res) {
		return false;
	}
	pipeline = std::move(*p_res);
	return true;
}

template <typename LayoutT>
bool PostProcessPass<LayoutT>::BuildVariants(VkDevice device, const ShaderStages& shaders,
											 std::initializer_list<VkFormat> colorFormats,
											 const VkPushConstantRange* pushConstants,
											 uint32_t pushCount,
											 std::span<const VkSpecializationInfo> specInfos,
											 bool additive) noexcept {
	descLayout = LayoutT::CreateLayout(device);
	pool = LayoutT::CreatePool(device, 2);

	sets[0] = LayoutT::Allocate(device, pool.Get(), descLayout.Get());
	sets[1] = LayoutT::Allocate(device, pool.Get(), descLayout.Get());

	VkDescriptorSetLayout rawLayout = descLayout.Get();
	const ZHLN_PipelineLayoutDesc pLayoutDesc = {.set_layouts = &rawLayout,
												 .set_layout_count = 1,
												 .push_constants = pushConstants,
												 .push_constant_count = pushCount};
	pipelineLayout = PipelineLayout(device, ZHLN_CreatePipelineLayout(device, &pLayoutDesc));

	pipelines.clear();
	pipelines.reserve(specInfos.size());

	for (const auto& spec : specInfos) {
		auto builder = PipelineBuilder{}
						   .Shaders(shaders)
						   .Layout(pipelineLayout.Get())
						   .ColorFormats(colorFormats)
						   .Specialization(&spec)
						   .NoDepth()
						   .CullNone();
		if (additive) {
			builder.AdditiveBlend();
		}

		auto p_res = builder.Build(device);
		if (!p_res) {
			return false;
		}
		pipelines.push_back(std::move(*p_res));
	}

	return !pipelines.empty();
}

template <typename LayoutT>
template <typename TargetT, typename PushT, typename... Args>
auto PostProcessPass<LayoutT>::ExecuteWithTransitions(VkCommandBuffer cmd, VkDevice device,
													  TargetT& targetRenderTarget, const PushT& pc,
													  Args&&... inputs)
	-> TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> {

	auto target_u = AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(targetRenderTarget);
	auto target_att = IssueBarrier<Vk::ShaderReadState, Vk::ColorAttachmentState>(cmd, target_u);

	WriteNext(device, std::forward<Args>(inputs)...);

	DynamicPass(targetRenderTarget.extent)
		.AddColor(target_att, VK_ATTACHMENT_LOAD_OP_DONT_CARE)
		.Execute(cmd, [&]() { Execute(cmd, pc); });

	return IssueBarrier<Vk::ColorAttachmentState, Vk::ShaderReadState>(cmd, target_att);
}

template <typename LayoutT>
template <typename TargetT, typename PushT, typename... Args>
auto PostProcessPass<LayoutT>::ExecuteVariantWithTransitions(VkCommandBuffer cmd, VkDevice device,
															 TargetT& targetRenderTarget,
															 uint32_t variantIdx, const PushT& pc,
															 Args&&... inputs)
	-> TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL> {

	auto target_u = AssumeLayout<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(targetRenderTarget);
	auto target_att = IssueBarrier<Vk::ShaderReadState, Vk::ColorAttachmentState>(cmd, target_u);

	WriteNext(device, std::forward<Args>(inputs)...);

	DynamicPass(targetRenderTarget.extent)
		.AddColor(target_att, VK_ATTACHMENT_LOAD_OP_DONT_CARE)
		.Execute(cmd, [&]() { ExecuteVariant(cmd, variantIdx, pc); });

	return IssueBarrier<Vk::ColorAttachmentState, Vk::ShaderReadState>(cmd, target_att);
}

template <typename LayoutT>
template <typename... Args>
void PostProcessPass<LayoutT>::WriteNext(VkDevice device, Args&&... args) const noexcept {
	if constexpr (sizeof...(Args) == 1) {
		using FirstT = std::decay_t<std::tuple_element_t<0, std::tuple<Args...>>>;

		if constexpr (requires { std::declval<FirstT>().AsTuple(); }) {
			std::apply(
				[&](auto&&... unpacked) {
					LayoutT::Write(device, sets.Next(),
								   std::forward<decltype(unpacked)>(unpacked)...);
				},
				std::get<0>(std::forward_as_tuple(args...)).AsTuple());
		} else {
			LayoutT::Write(device, sets.Next(), std::forward<Args>(args)...);
		}
	} else {
		LayoutT::Write(device, sets.Next(), std::forward<Args>(args)...);
	}
}

template <typename LayoutT>
template <GpuTriviallyCopyable T>
void PostProcessPass<LayoutT>::Execute(VkCommandBuffer cmd, const T& pushData,
									   VkShaderStageFlags stages) const noexcept {
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.Get());
	VkDescriptorSet set = sets.Next();
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout.Get(), 0, 1, &set,
							0, nullptr);
	Push(cmd, pipelineLayout.Get(), stages, pushData);
	vkCmdDraw(cmd, 3, 1, 0, 0);
}

template <typename LayoutT>
template <GpuTriviallyCopyable T>
void PostProcessPass<LayoutT>::ExecuteVariant(VkCommandBuffer cmd, uint32_t variantIdx,
											  const T& pushData,
											  VkShaderStageFlags stages) const noexcept {
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[variantIdx].Get());
	VkDescriptorSet set = sets.Next();
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout.Get(), 0, 1, &set,
							0, nullptr);
	Push(cmd, pipelineLayout.Get(), stages, pushData);
	vkCmdDraw(cmd, 3, 1, 0, 0);
}

template <typename LayoutT>
void PostProcessPass<LayoutT>::Execute(VkCommandBuffer cmd) const noexcept {
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.Get());
	VkDescriptorSet set = sets.Next();
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout.Get(), 0, 1, &set,
							0, nullptr);
	vkCmdDraw(cmd, 3, 1, 0, 0);
}

} // namespace ZHLN::Vk
