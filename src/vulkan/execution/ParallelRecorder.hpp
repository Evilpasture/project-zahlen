// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "RecordingBatch.hpp"

namespace ZHLN::Vk {

struct RecordingSlot {
    VkCommandBuffer cmd       = VK_NULL_HANDLE;
    uint32_t        slotIndex = 0;
};

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

    [[nodiscard]] auto Init(VkDevice device, uint32_t queueFamily) noexcept -> std::expected<void, ErrorCode>;

    [[nodiscard]] auto Reset() noexcept -> std::expected<void, ErrorCode>;

    /// Everything a slot must be told before it records: rendering inheritance,
    /// heap inheritance and the frame rows. Held by value, so the recorder owns
    /// what it will use -- the state is set on one frame and used on the next.
    void SetContext(SecondaryPassContext context) noexcept {
        _context = context;
    }

    // One item per callable, one entry per slot in the batch: the slots that
    // came back executable, and the ones that did not, each with its error.
    template <typename SchedulerPolicy, typename... Callables>
    [[nodiscard]] auto Record(SchedulerPolicy&& scheduler, Callables&&... callables) -> RecordingBatch;

    [[nodiscard]] constexpr auto GetCommandBuffers() const noexcept -> std::span<const VkCommandBuffer, ConcurrentSlots> {
        return _cmds;
    }

    [[nodiscard]] static constexpr auto Slots() noexcept -> size_t {
        return ConcurrentSlots;
    }

  private:
    template <typename SchedulerPolicy, size_t... Is, typename... Callables>
    [[nodiscard]] auto RecordImpl(SchedulerPolicy&& scheduler, std::index_sequence<Is...>, Callables&&... callables) -> RecordingBatch;

    VkDevice                                                      _device = VK_NULL_HANDLE;
    std::array<CommandPool<QueueType::Graphics>, ConcurrentSlots> _pools;
    std::array<VkCommandBuffer, ConcurrentSlots>                  _cmds;

    SecondaryPassContext _context {};
    uint32_t             _nextGroup = 0;
};

}

#include "ParallelRecorder.inl"
