// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/ParallelRecorder.inl
#pragma once

#include "ParallelRecorder.hpp"

namespace ZHLN::Vk {

template <size_t ConcurrentSlots>
auto ParallelCommandRecorder<ConcurrentSlots>::Init(VkDevice device, uint32_t queueFamily) noexcept -> std::expected<void, Error> {
    _device = device;
    for (size_t i = 0; i < ConcurrentSlots; ++i) {
        _pools[i] = CommandPool(_device, queueFamily);
        // AllocateSecondary internally EnsureValid()s the pool, so this both
        // reports PoolNotReady and CommandBufferAllocationFailed as domain
        // errors instead of leaking raw VkResult.
        auto alloc = _pools[i].AllocateSecondary(1);
        if (!alloc) [[unlikely]] {
            return std::unexpected(alloc.error());
        }
        _cmds[i] = _pools[i][0];
    }
    return {};
}

template <size_t ConcurrentSlots>
void ParallelCommandRecorder<ConcurrentSlots>::Reset() noexcept {
    for (auto& pool: _pools) {
        pool.Reset();
    }
}

template <size_t ConcurrentSlots>
template <typename SchedulerPolicy, typename... Callables>
void ParallelCommandRecorder<ConcurrentSlots>::Record(SchedulerPolicy&& scheduler, Callables&&... callables) {
    static_assert(
        sizeof...(Callables) <= ConcurrentSlots, "The number of recording tasks exceeds the allocated "
                                                 "ParallelCommandRecorder slots."
    );

    RecordImpl(std::forward<SchedulerPolicy>(scheduler), std::make_index_sequence<sizeof...(Callables)> {}, std::forward<Callables>(callables)...);
}

template <size_t ConcurrentSlots>
template <typename SchedulerPolicy, size_t... Is, typename... Callables>
void ParallelCommandRecorder<ConcurrentSlots>::RecordImpl(SchedulerPolicy&& scheduler, std::index_sequence<Is...> /*unused*/, Callables&&... callables) {
    auto task_tuple = std::forward_as_tuple(std::forward<Callables>(callables)...);

    // Expand the lambda pack and dispatch them to the scheduler at compile-time.
    // Each lambda bakes the constant 'Is' directly into its generated class structure.
    std::forward<SchedulerPolicy>(scheduler).Dispatch([this, &task_tuple]() {
        RecordingSlot slot {.cmd = _cmds[Is], .slotIndex = static_cast<uint32_t>(Is)};

        // VK_EXT_descriptor_heap: inherit the primary's heap bindings (binding
        // our own would invalidate the primary's heap state after execution).
        VkCommandBufferInheritanceInfo                        inherit_info = NullInheritanceInfo;
        const VkCommandBufferInheritanceDescriptorHeapInfoEXT heap_inherit = {
            .sType                 = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_DESCRIPTOR_HEAP_INFO_EXT,
            .pNext                 = nullptr,
            .pSamplerHeapBindInfo  = _samplerHeapBindInfo,
            .pResourceHeapBindInfo = _resourceHeapBindInfo,
        };
        if (_samplerHeapBindInfo != nullptr || _resourceHeapBindInfo != nullptr) {
            inherit_info.pNext = &heap_inherit;
        }

        const VkCommandBufferBeginInfo begin_info = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .pNext = nullptr,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, // Do not set CONTINUE_BIT
                                                                  // since they begin their own
                                                                  // render passes
            .pInheritanceInfo = &inherit_info
        };
        vkBeginCommandBuffer(slot.cmd, &begin_info);

        // Push data does not carry over from the primary: re-push the
        // per-frame device-address block in every secondary.
        if (_ctx != nullptr && _frameAddressCount > 0) {
            PushHeapFrameAddresses(
                *_ctx, slot.cmd, std::span<const uint32_t> {_frameAddressOffsets.data(), _frameAddressCount},
                std::span<const VkDeviceAddress> {_frameAddresses.data(), _frameAddressCount}
            );
        }

        std::get<Is>(task_tuple)(slot);
        vkEndCommandBuffer(slot.cmd);
    }...);
}

} // namespace ZHLN::Vk
