// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
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

} // namespace ZHLN::Vk
