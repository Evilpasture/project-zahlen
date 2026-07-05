// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/ParallelDraw.hpp
#pragma once
#include <concepts>
#include <span>
#include <utility>
#include <vector>
#include <vulkan/vulkan.h>

namespace ZHLN::Vk {

struct SecondaryInheritance {
	std::span<const VkFormat> colorFormats;
	VkFormat depthFormat = VK_FORMAT_UNDEFINED;
};

namespace detail {
// Archetype callback to test scheduler invocation without using lambdas in unevaluated contexts
struct DummyParallelForCallback {
	void operator()(uint32_t start, uint32_t end, uint32_t chunkIdx) const noexcept {}
};
} // namespace detail

/**
 * @brief Concept enforcing that the scheduler policy provides a valid ParallelFor loop
 * with the correct signature.
 */
template <typename S>
concept ParallelScheduler = requires(S&& s, uint32_t count, uint32_t chunkSize) {
	s.ParallelFor(count, chunkSize, detail::DummyParallelForCallback{});
};

/**
 * @brief Fully decoupled, template-driven parallel command recorder.
 * Enforces the ParallelScheduler concept at compile-time for friendly error reporting.
 */
template <ParallelScheduler SchedulerT, typename CmdProviderFn, typename RecordFn>
inline void ParallelDrawDispatch(VkCommandBuffer primaryCmd,
								 const SecondaryInheritance& inheritDesc, VkExtent2D extent,
								 uint32_t drawCount, uint32_t chunkSize, SchedulerT&& scheduler,
								 CmdProviderFn&& cmdProvider, RecordFn&& recordFn) {

	uint32_t numChunks = (drawCount + chunkSize - 1) / chunkSize;
	if (numChunks == 0) {
		return;
	}

	std::vector<VkCommandBuffer> secondaries(numChunks, VK_NULL_HANDLE);

	const VkCommandBufferInheritanceRenderingInfo inherit = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO,
		.pNext = nullptr,
		.flags = 0,
		.viewMask = 0,
		.colorAttachmentCount = static_cast<uint32_t>(inheritDesc.colorFormats.size()),
		.pColorAttachmentFormats = inheritDesc.colorFormats.data(),
		.depthAttachmentFormat = inheritDesc.depthFormat,
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

	// Parallel execution delegated to the compile-time injected scheduler policy
	std::forward<SchedulerT>(scheduler).ParallelFor(
		drawCount, chunkSize, [&](uint32_t start, uint32_t end, uint32_t chunkIdx) noexcept {
			// Fetch the task-local command buffer via the callback lambda
			VkCommandBuffer sec_cmd = std::forward<CmdProviderFn>(cmdProvider)(chunkIdx);

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

			vkEndCommandBuffer(sec_cmd);
			secondaries[chunkIdx] = sec_cmd;
		});

	if (!secondaries.empty()) {
		vkCmdExecuteCommands(primaryCmd, static_cast<uint32_t>(secondaries.size()),
							 secondaries.data());
	}
}

} // namespace ZHLN::Vk
