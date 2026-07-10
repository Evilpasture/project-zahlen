// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/ParallelRecorder.hpp
#pragma once
#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/render/Rendering.hpp> before including any other Zahlen render headers."
#endif

namespace ZHLN::Vk {

struct RecordingSlot {
    VkCommandBuffer cmd       = VK_NULL_HANDLE;
    uint32_t        slotIndex = 0;
};

// Scheduler policy concept (Compile-time duck typing)
template <typename S, typename... Tasks>
concept TaskScheduler = requires(S&& scheduler, Tasks&&... tasks) { scheduler.Dispatch(std::forward<Tasks>(tasks)...); };

template <size_t ConcurrentSlots>
class ParallelCommandRecorder {
  public:
    ParallelCommandRecorder() = default;

    ParallelCommandRecorder(const ParallelCommandRecorder&)                        = delete;
    auto operator=(const ParallelCommandRecorder&) -> ParallelCommandRecorder&     = delete;
    ParallelCommandRecorder(ParallelCommandRecorder&&) noexcept                    = default;
    auto operator=(ParallelCommandRecorder&&) noexcept -> ParallelCommandRecorder& = default;

    [[nodiscard]] auto Init(VkDevice device, uint32_t queueFamily) noexcept -> std::expected<void, VkResult>;

    void Reset() noexcept;

    /**
     * @brief Entry point for static parallel recording.
     * Enforces slot limits at compile-time and uses zero heap allocations.
     */
    template <typename SchedulerPolicy, typename... Callables>
    void Record(SchedulerPolicy&& scheduler, Callables&&... callables);

    // Kept in header (3 lines)
    [[nodiscard]] constexpr auto GetCommandBuffers() const noexcept -> std::span<const VkCommandBuffer, ConcurrentSlots> {
        return _cmds;
    }

  private:
    template <typename SchedulerPolicy, size_t... Is, typename... Callables>
    void RecordImpl(SchedulerPolicy&& scheduler, std::index_sequence<Is...> /*unused*/, Callables&&... callables);

    VkDevice                                                      _device = VK_NULL_HANDLE;
    std::array<CommandPool<QueueType::Graphics>, ConcurrentSlots> _pools;
    std::array<VkCommandBuffer, ConcurrentSlots>                  _cmds;
};

} // namespace ZHLN::Vk

#include "ParallelRecorder.inl"
