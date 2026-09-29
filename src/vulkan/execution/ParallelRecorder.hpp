// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

namespace ZHLN::Vk {

struct RecordingSlot {
    VkCommandBuffer cmd       = VK_NULL_HANDLE;
    uint32_t        slotIndex = 0;
};

template <typename S, typename... Tasks>
concept TaskScheduler = requires(S&& scheduler, Tasks&&... tasks) { scheduler.Dispatch(std::forward<Tasks>(tasks)...); };

template <size_t ConcurrentSlots, size_t MaxFrameAddresses = 8>
class ParallelCommandRecorder {
  public:
    ParallelCommandRecorder() = default;

    ParallelCommandRecorder(const ParallelCommandRecorder&)                        = delete;
    auto operator=(const ParallelCommandRecorder&) -> ParallelCommandRecorder&     = delete;
    ParallelCommandRecorder(ParallelCommandRecorder&&) noexcept                    = default;
    auto operator=(ParallelCommandRecorder&&) noexcept -> ParallelCommandRecorder& = default;

    [[nodiscard]] auto Init(VkDevice device, uint32_t queueFamily) noexcept -> std::expected<void, ErrorCode>;

    void Reset() noexcept;

    void SetHeapState(
        const VkBindHeapInfoEXT*         samplerHeapBindInfo,
        const VkBindHeapInfoEXT*         resourceHeapBindInfo,
        std::span<const uint32_t>        frameAddressOffsets,
        std::span<const VkDeviceAddress> frameAddresses
    ) noexcept {
        _samplerHeapBindInfo  = samplerHeapBindInfo;
        _resourceHeapBindInfo = resourceHeapBindInfo;
        _frameAddressCount    = static_cast<uint32_t>(
            std::min({frameAddresses.size(), frameAddressOffsets.size(), _frameAddresses.size()})
        );
        for (uint32_t i = 0; i < _frameAddressCount; ++i) {
            _frameAddressOffsets[i] = frameAddressOffsets[i];
            _frameAddresses[i]      = frameAddresses[i];
        }
    }

    template <typename SchedulerPolicy, typename... Callables>
    [[nodiscard]] auto Record(SchedulerPolicy&& scheduler, Callables&&... callables) -> std::expected<void, ErrorCode>;

    [[nodiscard]] constexpr auto GetCommandBuffers() const noexcept -> std::span<const VkCommandBuffer, ConcurrentSlots> {
        return _cmds;
    }

    [[nodiscard]] static constexpr auto Slots() noexcept -> size_t {
        return ConcurrentSlots;
    }

  private:
    template <typename SchedulerPolicy, size_t... Is, typename... Callables>
    [[nodiscard]] auto RecordImpl(SchedulerPolicy&& scheduler, std::index_sequence<Is...> , Callables&&... callables)
        -> std::expected<void, ErrorCode>;

    VkDevice                                                      _device = VK_NULL_HANDLE;
    std::array<CommandPool<QueueType::Graphics>, ConcurrentSlots> _pools;
    std::array<VkCommandBuffer, ConcurrentSlots>                  _cmds;

    const VkBindHeapInfoEXT*                            _samplerHeapBindInfo  = nullptr;
    const VkBindHeapInfoEXT*                            _resourceHeapBindInfo = nullptr;
    std::array<uint32_t, MaxFrameAddresses>        _frameAddressOffsets {};
    std::array<VkDeviceAddress, MaxFrameAddresses> _frameAddresses {};
    uint32_t                                            _frameAddressCount = 0;
};

}

#include "ParallelRecorder.inl"
