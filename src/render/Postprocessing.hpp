// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "PipelineBuilder.hpp"
#include "RenderCore.hpp"

namespace ZHLN::Vk {
// ============================================================================
// Postprocessing helpers
// ============================================================================

/**
 * @brief Generic TMP Post-Processing Pipeline Builder & Executor.
 *
 * @tparam LayoutT A ZHLN::Vk::DescriptorLayout defining the bindings.
 */
template <typename LayoutT> struct PostProcessPass {
	DescriptorSetLayout descLayout;
	DescriptorPool pool;
	ZHLN::DoubleBuffered<VkDescriptorSet> sets;
	PipelineLayout pipelineLayout;
	Pipeline pipeline;

	[[nodiscard]] bool Build(VkDevice device, const ShaderStages& shaders,
							 std::initializer_list<VkFormat> colorFormats,
							 const VkPushConstantRange* pushConstants = nullptr,
							 uint32_t pushCount = 0) noexcept {
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

		pipeline = PipelineBuilder{}
					   .Shaders(shaders)
					   .Layout(pipelineLayout.Get())
					   .ColorFormats(colorFormats)
					   .NoDepth()
					   .CullNone()
					   .Build(device);

		return pipeline.Valid();
	}

	// Variadic template forwards safely to your existing LayoutT::Write
	template <typename... Args> void WriteNext(VkDevice device, Args&&... args) noexcept {
		LayoutT::Write(device, sets.Next(), std::forward<Args>(args)...);
	}

	template <typename... Args>
	void WriteIndex(VkDevice device, uint32_t idx, Args&&... args) noexcept {
		LayoutT::Write(device, sets[idx], std::forward<Args>(args)...);
	}

	// Execution with optional Push Constants
	template <GpuTriviallyCopyable T>
	void Execute(VkCommandBuffer cmd, const T& pushData,
				 VkShaderStageFlags stages = VK_SHADER_STAGE_FRAGMENT_BIT) const noexcept {
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.Get());
		VkDescriptorSet set = sets.Next(); // Execute on the frame we just updated
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout.Get(), 0, 1,
								&set, 0, nullptr);
		Push(cmd, pipelineLayout.Get(), stages, pushData);
		vkCmdDraw(cmd, 3, 1, 0, 0);
	}

	// Parameter-less execution (e.g., standard Blit)
	void Execute(VkCommandBuffer cmd) const noexcept {
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.Get());
		VkDescriptorSet set = sets.Next();
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout.Get(), 0, 1,
								&set, 0, nullptr);
		vkCmdDraw(cmd, 3, 1, 0, 0);
	}

	void Flip() noexcept { sets.Flip(); }
};
} // namespace ZHLN::Vk
