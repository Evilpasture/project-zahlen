// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "RenderCore.h"
#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/render/Rendering.hpp> before including any other Zahlen render headers."
#endif

namespace ZHLN::Vk {

/**
 * @brief Configuration for a generic draw batch.
 */
struct DrawBatchConfig {
	VkPipeline pipeline = VK_NULL_HANDLE;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkBuffer vbo = VK_NULL_HANDLE;
	VkBuffer ibo = VK_NULL_HANDLE;
	VkDescriptorSet set = VK_NULL_HANDLE;
	VkShaderStageFlags pushStages = 0;
};

/**
 * @brief A high-performance template that binds common Vulkan state once
 * and executes a stream of draw calls via a user-provided loop.
 *
 * @tparam PushT The Type of the Push Constant struct (use std::monostate if none).
 * @tparam LoopFn A lambda that receives a 'draw(PushT, count, first)' caller.
 */
template <typename PushT = std::monostate, typename LoopFn>
inline void DrawBatch(const VkCommandBuffer cmd, const DrawBatchConfig& cfg, LoopFn&& loop) {
	// 1. Static Bindings (Fixed for the whole batch)
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, cfg.pipeline);

	VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(cmd, 0, 1, &cfg.vbo, &offset);
	vkCmdBindIndexBuffer(cmd, cfg.ibo, 0, VK_INDEX_TYPE_UINT32);

	if (cfg.set != VK_NULL_HANDLE) {
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, cfg.layout, 0, 1, &cfg.set, 0,
								nullptr);
	}

	// 2. Dynamic Recording
	// We provide a 'binder' lambda back to the user to record individual instances
	auto record = [&](const PushT& pc, uint32_t indexCount, uint32_t firstIndex) -> auto {
		if constexpr (!std::is_same_v<PushT, std::monostate>) {
			ZHLN::Vk::Push(cmd, cfg.layout, cfg.pushStages, pc);
		}
		vkCmdDrawIndexed(cmd, indexCount, 1, firstIndex, 0, 0);
	};

	// Forward the loop to ensure the caller's value category is preserved
	std::forward<LoopFn>(loop)(record);
}

/**
 * @brief High-performance, strongly-typed bindless batch drawer.
 * Binds the pipeline and global bindless set once, exposing an optimized draw callback.
 */
template <size_t ColorCount, bool HasDepth, typename LoopFn>
inline void DrawBindlessBatch(const VkCommandBuffer cmd,
							  const TypedPipeline<ColorCount, HasDepth>& pipeline,
							  VkPipelineLayout layout, VkDescriptorSet bindlessSet,
							  VkShaderStageFlags pushStages, LoopFn&& loop) {
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.Get());

	if (bindlessSet != VK_NULL_HANDLE) {
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &bindlessSet, 0,
								nullptr);
	}

	auto draw = [&](uint32_t vertexCount, uint32_t instanceIdx, const auto& pc) {
		Push(cmd, layout, pushStages, pc);
		vkCmdDraw(cmd, vertexCount, 1, 0, instanceIdx);
	};

	std::forward<LoopFn>(loop)(draw);
}

// ============================================================================
// Immediate Command RAII Wrapper (Staging/Initial Uploads)
// ============================================================================

template <QueueType QType, size_t Capacity = 8> class CommandRing {
  public:
	CommandRing() = default;
	~CommandRing() { Cleanup(); }

	// Enforce move-only RAII semantics
	CommandRing(const CommandRing&) = delete;
	CommandRing& operator=(const CommandRing&) = delete;
	CommandRing(CommandRing&&) noexcept = default;
	CommandRing& operator=(CommandRing&&) noexcept = default;

	void Init(VkDevice device, uint32_t queueFamily) noexcept {
		_device = device;
		for (size_t i = 0; i < Capacity; ++i) {
			VkCommandPoolCreateInfo poolInfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
												.pNext = nullptr,
												.flags =
													VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
												.queueFamilyIndex = queueFamily};
			if (vkCreateCommandPool(_device, &poolInfo, nullptr, &_pools[i]) != VK_SUCCESS) {
				continue;
			}

			VkCommandBufferAllocateInfo allocInfo = {
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
				.pNext = nullptr,
				.commandPool = _pools[i],
				.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
				.commandBufferCount = 1};
			vkAllocateCommandBuffers(_device, &allocInfo, &_cmds[i]);

			VkFenceCreateInfo fenceInfo = {
				.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
				.pNext = nullptr,
				// Start signaled so the first Acquire() call passes through without stalling
				.flags = VK_FENCE_CREATE_SIGNALED_BIT};
			vkCreateFence(_device, &fenceInfo, nullptr, &_fences[i]);
		}
	}

	void Cleanup() noexcept {
		if (_device != VK_NULL_HANDLE) {
			for (size_t i = 0; i < Capacity; ++i) {
				if (_fences[i] != VK_NULL_HANDLE) {
					vkDestroyFence(_device, _fences[i], nullptr);
					_fences[i] = VK_NULL_HANDLE;
				}
				if (_pools[i] != VK_NULL_HANDLE) {
					vkDestroyCommandPool(_device, _pools[i], nullptr);
					_pools[i] = VK_NULL_HANDLE;
				}
				_cmds[i] = VK_NULL_HANDLE;
			}
			_device = VK_NULL_HANDLE;
		}
	}

	struct Slot {
		CommandBuffer<QType> cmd;
		VkFence fence;
	};

	[[nodiscard]] auto Acquire() noexcept -> Slot {
		uint32_t slotIdx = _index.fetch_add(1, std::memory_order_relaxed) % Capacity;

		// If the GPU is still processing this slot's last submission, block here
		vkWaitForFences(_device, 1, &_fences[slotIdx], VK_TRUE, UINT64_MAX);
		vkResetFences(_device, 1, &_fences[slotIdx]);

		// Recycle the command pool instantly without any driver reallocation
		vkResetCommandPool(_device, _pools[slotIdx], 0);

		return {CommandBuffer<QType>{_cmds[slotIdx]}, _fences[slotIdx]};
	}

  private:
	VkDevice _device = VK_NULL_HANDLE;
	std::array<VkCommandPool, Capacity> _pools{};
	std::array<VkCommandBuffer, Capacity> _cmds{};
	std::array<VkFence, Capacity> _fences{};
	std::atomic<uint32_t> _index{0};
};

// Simple RAII wrapper.
class CommandBufferGuard {
  public:
	CommandBufferGuard(VkCommandBuffer cmdBuffer) : cmd(cmdBuffer) { ZHLN_BeginCommandBuffer(cmd); }

	~CommandBufferGuard() { ZHLN_EndCommandBuffer(cmd); }

	[[nodiscard]] VkCommandBuffer get() const { return cmd; }

	CommandBufferGuard(const CommandBufferGuard&) = delete;
	CommandBufferGuard(CommandBufferGuard&&) = delete;
	CommandBufferGuard& operator=(const CommandBufferGuard&) = delete;
	CommandBufferGuard& operator=(CommandBufferGuard&&) = delete;

  private:
	VkCommandBuffer cmd{};
};

/**
 * @brief Recycles a command buffer from the ring, records operations,
 *        and submits it. Stalls the CPU only if blockCPU is true.
 */
template <QueueType QType = QueueType::Graphics, size_t Capacity = 8, typename RecordFn>
void ExecuteImmediate(const Context& ctx, CommandRing<QType, Capacity>& ring, RecordFn&& record,
					  bool blockCPU = true) {
	// 1. Recycle command buffer and fence from the ring (O(1) / Allocation-Free)
	auto [cmd, fence] = ring.Acquire();
	{
		CommandBufferGuard guard(cmd);
		std::forward<RecordFn>(record)(cmd);
	}

	VkCommandBufferSubmitInfo subInfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
										 .pNext = nullptr,
										 .commandBuffer = cmd,
										 .deviceMask = 0};

	VkSubmitInfo2 submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
							.pNext = nullptr,
							.flags = 0,
							.waitSemaphoreInfoCount = 0,
							.pWaitSemaphoreInfos = nullptr,
							.commandBufferInfoCount = 1,
							.pCommandBufferInfos = &subInfo,
							.signalSemaphoreInfoCount = 0,
							.pSignalSemaphoreInfos = nullptr};

	VkQueue queue = ResolveQueue<QType>(ctx);
	vkQueueSubmit2(queue, 1, &submit, fence);

	// 2. Synchronization Strategy
	if (blockCPU) {
		// Hard stall: waits immediately (e.g. for synchronous debug hooks)
		vkWaitForFences(ctx.Device(), 1, &fence, VK_TRUE, UINT64_MAX);
	}
	// If blockCPU is false, we return immediately! The ring buffer design
	// ensures we won't overwrite this command buffer until it is safe to do so.
}

/**
 * @brief Recycles a command buffer from the ring, records operations,
 *        submits via StagingRingBuffer (properly stamping its timeline value),
 *        and blocks the CPU until the staging transfer completes.
 */
template <QueueType QType = QueueType::Graphics, size_t Capacity = 8, typename RecordFn>
void ExecuteImmediate(const Context& ctx, CommandRing<QType, Capacity>& ring,
					  StagingRingBuffer& ringBuffer, RecordFn&& record) {
	// 1. Recycle command buffer and fence from the ring (O(1) / Allocation-Free)
	auto [cmd, fence] = ring.Acquire();
	{
		CommandBufferGuard guard(cmd);
		std::forward<RecordFn>(record)(cmd);
	}

	// 2. Submit via StagingRingBuffer to stamp active allocations and signal timeline
	uint64_t submitVal = ringBuffer.Submit(cmd);

	// 3. Synchronously wait on the timeline semaphore to retire the staging memory
	VkSemaphore semaphore = ringBuffer.GetSemaphore();
	VkSemaphoreWaitInfo waitInfo = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
									.pNext = nullptr,
									.flags = 0,
									.semaphoreCount = 1,
									.pSemaphores = &semaphore,
									.pValues = &submitVal};
	vkWaitSemaphores(ctx.Device(), &waitInfo, UINT64_MAX);

	// 4. Signal the associated fence so that CommandRing's next Acquire doesn't stall
	vkQueueSubmit2(ResolveQueue<QType>(ctx), 0, nullptr, fence);
}

} // namespace ZHLN::Vk
