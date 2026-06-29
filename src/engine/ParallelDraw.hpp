// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderInternal.hpp"

#include <Jolt/Core/Array.h>
#include <Jolt/Jolt.h>
#include <span>
#include <threading/TaskSystem.hpp>

namespace ZHLN::Vk {

struct SecondaryInheritance {
	std::span<const VkFormat> colorFormats;
	VkFormat depthFormat = VK_FORMAT_UNDEFINED;
};

/**
 * @brief Internal engine-level helper to parallelize secondary command buffer recording
 * and synchronize the drawing pipeline across fiber-scheduler tasks.
 */
template <typename RecordFn>
inline void ParallelDrawDispatch(VkCommandBuffer primaryCmd,
								 const SecondaryInheritance& inheritDesc, VkExtent2D extent,
								 uint32_t drawCount, uint32_t chunkSize, uint32_t frameIndex,
								 std::span<WorkerCmdContext> workerCmds, RecordFn&& recordFn) {
	uint32_t numChunks = (drawCount + chunkSize - 1) / chunkSize;
	JPH::Array<VkCommandBuffer> secondaries(numChunks);

	const VkCommandBufferInheritanceRenderingInfo inherit = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO,
		.pNext = nullptr,
		.flags = 0,
		.viewMask = 0,
		.colorAttachmentCount = static_cast<uint32_t>(inheritDesc.colorFormats.size()),
		.pColorAttachmentFormats = inheritDesc.colorFormats.data(),
		.depthAttachmentFormat = inheritDesc.depthFormat, // Fixed typo
		.stencilAttachmentFormat = VK_FORMAT_UNDEFINED,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};

	const VkCommandBufferInheritanceInfo pInherit = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,
		.pNext = &inherit,
		.renderPass = VK_NULL_HANDLE,
		.subpass = 0,
		.framebuffer = VK_NULL_HANDLE,
		.occlusionQueryEnable = VK_FALSE,
		.queryFlags = 0,
		.pipelineStatistics = 0};

	TaskSystem::ParallelFor(
		drawCount, chunkSize, [&](uint32_t start, uint32_t end, uint32_t chunkIdx) -> void {
			uint32_t wIdx = TaskSystem::GetWorkerIndex();
			if (wIdx >= workerCmds.size()) {
				wIdx = (uint32_t)(workerCmds.size() - 1);
			}

			uint32_t localCmdIdx =
				workerCmds[wIdx].cmdCount[frameIndex].fetch_add(1, std::memory_order_relaxed);
			VkCommandBuffer sec_cmd = workerCmds[wIdx].pools[frameIndex][localCmdIdx];

			const VkCommandBufferBeginInfo beginInfo = {
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
				.pNext = nullptr,
				.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT |
						 VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
				.pInheritanceInfo = &pInherit};
			vkBeginCommandBuffer(sec_cmd, &beginInfo);

			// Standard, un-flipped viewport
			const VkViewport viewport = {.x = 0.0f,
										 .y = 0.0f,
										 .width = (float)extent.width,
										 .height = (float)extent.height,
										 .minDepth = 0.0f,
										 .maxDepth = 1.0f};
			const VkRect2D scissor = {.offset = {.x = 0, .y = 0}, .extent = extent};
			vkCmdSetViewport(sec_cmd, 0, 1, &viewport);
			vkCmdSetScissor(sec_cmd, 0, 1, &scissor);

			for (uint32_t i = start; i < end; ++i) {
				recordFn(sec_cmd, i);
			}

			ZHLN_EndCommandBuffer(sec_cmd);
			secondaries[chunkIdx] = sec_cmd;
		});

	Vk::ExecuteCommands(primaryCmd, secondaries);
}

} // namespace ZHLN::Vk
