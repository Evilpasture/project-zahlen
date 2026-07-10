// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/ParallelRecorder.inl
#pragma once

#include "ParallelRecorder.hpp"

namespace ZHLN::Vk {

template <size_t ConcurrentSlots>
auto ParallelCommandRecorder<ConcurrentSlots>::Init(VkDevice device, uint32_t queueFamily) noexcept
	-> std::expected<void, VkResult> {
	_device = device;
	for (size_t i = 0; i < ConcurrentSlots; ++i) {
		_pools[i] = CommandPool(_device, queueFamily);
		if (!_pools[i].Valid()) {
			return std::unexpected(VK_ERROR_INITIALIZATION_FAILED);
		}
		if (!_pools[i].AllocateSecondary(1)) {
			return std::unexpected(VK_ERROR_OUT_OF_DEVICE_MEMORY);
		}
		_cmds[i] = _pools[i][0];
	}
	return {};
}

template <size_t ConcurrentSlots> void ParallelCommandRecorder<ConcurrentSlots>::Reset() noexcept {
	for (auto& pool : _pools) {
		pool.Reset();
	}
}

template <size_t ConcurrentSlots>
template <typename SchedulerPolicy, typename... Callables>
void ParallelCommandRecorder<ConcurrentSlots>::Record(SchedulerPolicy&& scheduler,
													  Callables&&... callables) {
	static_assert(sizeof...(Callables) <= ConcurrentSlots,
				  "The number of recording tasks exceeds the allocated "
				  "ParallelCommandRecorder slots.");

	RecordImpl(std::forward<SchedulerPolicy>(scheduler),
			   std::make_index_sequence<sizeof...(Callables)>{},
			   std::forward<Callables>(callables)...);
}

template <size_t ConcurrentSlots>
template <typename SchedulerPolicy, size_t... Is, typename... Callables>
void ParallelCommandRecorder<ConcurrentSlots>::RecordImpl(SchedulerPolicy&& scheduler,
														  std::index_sequence<Is...> /*unused*/,
														  Callables&&... callables) {
	// Create a temporary reference tuple of the callables. Zero runtime cost.
	auto taskTuple = std::forward_as_tuple(std::forward<Callables>(callables)...);

	// Expand the lambda pack and dispatch them to the scheduler at compile-time.
	// Each lambda bakes the constant 'Is' directly into its generated class structure.
	std::forward<SchedulerPolicy>(scheduler).Dispatch([this, &taskTuple]() {
		RecordingSlot slot{.cmd = _cmds[Is], .slotIndex = static_cast<uint32_t>(Is)};
		const VkCommandBufferBeginInfo beginInfo = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.pNext = nullptr,
			.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, // Do not set CONTINUE_BIT
																  // since they begin their own
																  // render passes
			.pInheritanceInfo = &NullInheritanceInfo};
		vkBeginCommandBuffer(slot.cmd, &beginInfo);
		std::get<Is>(taskTuple)(slot);
		vkEndCommandBuffer(slot.cmd);
	}...);
}

} // namespace ZHLN::Vk
