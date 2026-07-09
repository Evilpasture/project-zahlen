// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/ParallelRecorder.hpp
#pragma once
#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/render/Rendering.hpp> before including any other Zahlen render headers."
#endif

namespace ZHLN::Vk {

struct RecordingSlot {
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	uint32_t slotIndex = 0;
};

// Scheduler policy concept (Compile-time duck typing)
template <typename S, typename... Tasks>
concept TaskScheduler = requires(S&& scheduler, Tasks&&... tasks) {
	scheduler.Dispatch(std::forward<Tasks>(tasks)...);
};

template <size_t ConcurrentSlots> class ParallelCommandRecorder {
  public:
	ParallelCommandRecorder() = default;

	ParallelCommandRecorder(const ParallelCommandRecorder&) = delete;
	auto operator=(const ParallelCommandRecorder&) -> ParallelCommandRecorder& = delete;
	ParallelCommandRecorder(ParallelCommandRecorder&&) noexcept = default;
	auto operator=(ParallelCommandRecorder&&) noexcept -> ParallelCommandRecorder& = default;

	[[nodiscard]] auto Init(VkDevice device, uint32_t queueFamily) noexcept
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

	void Reset() noexcept {
		for (auto& pool : _pools) {
			pool.Reset();
		}
	}

	/**
	 * @brief Entry point for static parallel recording.
	 * Enforces slot limits at compile-time and uses zero heap allocations.
	 */
	template <typename SchedulerPolicy, typename... Callables>
	void Record(SchedulerPolicy&& scheduler, Callables&&... callables) {
		static_assert(sizeof...(Callables) <= ConcurrentSlots,
					  "The number of recording tasks exceeds the allocated "
					  "ParallelCommandRecorder slots.");

		RecordImpl(std::forward<SchedulerPolicy>(scheduler),
				   std::make_index_sequence<sizeof...(Callables)>{},
				   std::forward<Callables>(callables)...);
	}

	[[nodiscard]] constexpr auto GetCommandBuffers() const noexcept
		-> std::span<const VkCommandBuffer, ConcurrentSlots> {
		return _cmds;
	}

  private:
	template <typename SchedulerPolicy, size_t... Is, typename... Callables>
	void RecordImpl(SchedulerPolicy&& scheduler, std::index_sequence<Is...> /*unused*/,
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

	VkDevice _device = VK_NULL_HANDLE;
	std::array<CommandPool<QueueType::Graphics>, ConcurrentSlots> _pools;
	std::array<VkCommandBuffer, ConcurrentSlots> _cmds;
};

} // namespace ZHLN::Vk
