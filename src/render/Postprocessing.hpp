// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/Postprocessing.hpp
#pragma once
#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/render/Rendering.hpp> before including any other Zahlen render headers."
#endif

namespace ZHLN::Vk {

template <typename LayoutT> struct PostProcessPass {
	DescriptorSetLayout descLayout;
	DescriptorPool pool;
	ZHLN::DoubleBuffered<VkDescriptorSet> sets;
	PipelineLayout pipelineLayout;
	Pipeline pipeline;
	std::vector<Pipeline> pipelines; // Unified storage for variants

	[[nodiscard]] bool Build(VkDevice device, const ShaderStages& shaders,
							 std::initializer_list<VkFormat> colorFormats,
							 const VkPushConstantRange* pushConstants = nullptr,
							 uint32_t pushCount = 0, bool additive = false) noexcept;

	[[nodiscard]] bool BuildVariants(VkDevice device, const ShaderStages& shaders,
									 std::initializer_list<VkFormat> colorFormats,
									 const VkPushConstantRange* pushConstants, uint32_t pushCount,
									 std::span<const VkSpecializationInfo> specInfos,
									 bool additive = false) noexcept;

	template <typename TargetT, typename PushT, typename... Args>
	auto ExecuteWithTransitions(VkCommandBuffer cmd, VkDevice device, TargetT& targetRenderTarget,
								const PushT& pc, Args&&... inputs)
		-> TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>;

	template <typename TargetT, typename PushT, typename... Args>
	auto ExecuteVariantWithTransitions(VkCommandBuffer cmd, VkDevice device,
									   TargetT& targetRenderTarget, uint32_t variantIdx,
									   const PushT& pc, Args&&... inputs)
		-> TypedImage<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>;

	template <typename... Args> void WriteNext(VkDevice device, Args&&... args) const noexcept;

	template <typename... Args>
	void WriteIndex(VkDevice device, uint32_t idx, Args&&... args) noexcept {
		LayoutT::Write(device, sets[idx], std::forward<Args>(args)...);
	}

	template <GpuTriviallyCopyable T>
	void Execute(VkCommandBuffer cmd, const T& pushData,
				 VkShaderStageFlags stages = VK_SHADER_STAGE_FRAGMENT_BIT) const noexcept;

	template <GpuTriviallyCopyable T>
	void ExecuteVariant(VkCommandBuffer cmd, uint32_t variantIdx, const T& pushData,
						VkShaderStageFlags stages = VK_SHADER_STAGE_FRAGMENT_BIT) const noexcept;

	void Execute(VkCommandBuffer cmd) const noexcept;

	void Flip() noexcept { sets.Flip(); }
};

} // namespace ZHLN::Vk

#include "Postprocessing.inl"
