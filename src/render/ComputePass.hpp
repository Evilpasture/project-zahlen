// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/ComputePass.hpp
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

	// --- Stateful Bind and Push Helpers ---

	void Bind(VkCommandBuffer cmd) const noexcept {
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.Get());
	}

	void BindSet(VkCommandBuffer cmd, VkDescriptorSet set, uint32_t firstSet = 0) const noexcept {
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout.Get(), firstSet,
								1, &set, 0, nullptr);
	}

	template <GpuTriviallyCopyable T>
	void PushConstants(VkCommandBuffer cmd, const T& pushData) const noexcept {
		Push(cmd, pipelineLayout.Get(), VK_SHADER_STAGE_COMPUTE_BIT, pushData);
	}

	void Dispatch(VkCommandBuffer cmd, uint32_t x, uint32_t y, uint32_t z) const noexcept {
		ZHLN_CmdDispatch(cmd, x, y, z);
	}

	// --- High-Level Convenience Dispatches ---

	// Dispatch with no Descriptor Set (e.g. BDA only, like Skinning)
	template <GpuTriviallyCopyable T>
	void Dispatch(VkCommandBuffer cmd, uint32_t x, uint32_t y, uint32_t z,
				  const T& pushData) const noexcept {
		Bind(cmd);
		PushConstants(cmd, pushData);
		Dispatch(cmd, x, y, z);
	}

	// Dispatch with Descriptor Set, but no Push Constants (like Clustering)
	void Dispatch(VkCommandBuffer cmd, VkDescriptorSet set, uint32_t x, uint32_t y,
				  uint32_t z) const noexcept {
		Bind(cmd);
		BindSet(cmd, set);
		Dispatch(cmd, x, y, z);
	}

	// Dispatch with both Descriptor Set & Push Constants
	template <GpuTriviallyCopyable T>
	void Dispatch(VkCommandBuffer cmd, VkDescriptorSet set, uint32_t x, uint32_t y, uint32_t z,
				  const T& pushData) const noexcept {
		Bind(cmd);
		BindSet(cmd, set);
		PushConstants(cmd, pushData);
		Dispatch(cmd, x, y, z);
	}
};
// NOLINTNEXTLINE(performance-enum-size)
enum class BarrierStage : VkPipelineStageFlags2 {
	None = 0,
	Compute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
	Fragment = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
	Vertex = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
	Indirect = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
	Transfer = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
	Host = VK_PIPELINE_STAGE_2_HOST_BIT
};

// NOLINTNEXTLINE(performance-enum-size)
enum class BarrierAccess : VkAccessFlags2 {
	None = 0,
	ShaderRead = VK_ACCESS_2_SHADER_READ_BIT,
	ShaderWrite = VK_ACCESS_2_SHADER_WRITE_BIT,
	IndirectRead = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
	TransferRead = VK_ACCESS_2_TRANSFER_READ_BIT,
	TransferWrite = VK_ACCESS_2_TRANSFER_WRITE_BIT,
	HostRead = VK_ACCESS_2_HOST_READ_BIT,
	HostWrite = VK_ACCESS_2_HOST_WRITE_BIT,
	ColorRead = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT,
	ColorWrite = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
	DepthRead = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
	DepthWrite = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
};

// Enable bitwise OR operations on the scoped enums
[[nodiscard]] constexpr auto operator|(BarrierStage a, BarrierStage b) noexcept -> BarrierStage {
	return static_cast<BarrierStage>(static_cast<std::underlying_type_t<BarrierStage>>(a) |
									 static_cast<std::underlying_type_t<BarrierStage>>(b));
}

[[nodiscard]] constexpr auto operator|(BarrierAccess a, BarrierAccess b) noexcept -> BarrierAccess {
	return static_cast<BarrierAccess>(static_cast<std::underlying_type_t<BarrierAccess>>(a) |
									  static_cast<std::underlying_type_t<BarrierAccess>>(b));
}

struct BarrierBuilder {
	VkPipelineStageFlags2 src_stage = 0;
	VkAccessFlags2 src_access = 0;

	constexpr BarrierBuilder() noexcept = default;

	constexpr auto From(BarrierStage stage, BarrierAccess access) noexcept -> BarrierBuilder& {
		src_stage |= static_cast<VkPipelineStageFlags2>(stage);
		src_access |= static_cast<VkAccessFlags2>(access);
		return *this;
	}

	void To(VkCommandBuffer cmd, BarrierStage dst_stage, BarrierAccess dst_access) const noexcept {
		const ZHLN_MemoryBarrierDesc desc = {.src_stage = src_stage,
											 .src_access = src_access,
											 .dst_stage =
												 static_cast<VkPipelineStageFlags2>(dst_stage),
											 .dst_access = static_cast<VkAccessFlags2>(dst_access)};
		ZHLN_CmdMemoryBarrier(cmd, &desc);
	}
};

} // namespace ZHLN::Vk
