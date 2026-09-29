// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/vulkan/execution/ParallelRecorder.inl
#pragma once

#include "ParallelRecorder.hpp"

namespace ZHLN::Vk {

template <size_t ConcurrentSlots, size_t MaxFrameAddresses>
auto ParallelCommandRecorder<ConcurrentSlots, MaxFrameAddresses>::Init(VkDevice device, uint32_t queueFamily) noexcept -> std::expected<void, ErrorCode> {
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

template <size_t ConcurrentSlots, size_t MaxFrameAddresses>
void ParallelCommandRecorder<ConcurrentSlots, MaxFrameAddresses>::Reset() noexcept {
    for (auto& pool: _pools) {
        pool.Reset();
    }
}

template <size_t ConcurrentSlots, size_t MaxFrameAddresses>
template <typename SchedulerPolicy, typename... Callables>
auto ParallelCommandRecorder<ConcurrentSlots, MaxFrameAddresses>::Record(SchedulerPolicy&& scheduler, Callables&&... callables)
    -> std::expected<void, ErrorCode> {
    static_assert(
        sizeof...(Callables) <= ConcurrentSlots, "The number of recording tasks exceeds the allocated "
                                                 "ParallelCommandRecorder slots."
    );

    return RecordImpl(std::forward<SchedulerPolicy>(scheduler), std::make_index_sequence<sizeof...(Callables)> {}, std::forward<Callables>(callables)...);
}

template <size_t ConcurrentSlots, size_t MaxFrameAddresses>
template <typename SchedulerPolicy, size_t... Is, typename... Callables>
auto ParallelCommandRecorder<ConcurrentSlots, MaxFrameAddresses>::RecordImpl(
    SchedulerPolicy&& scheduler, std::index_sequence<Is...> /*unused*/, Callables&&... callables
) -> std::expected<void, ErrorCode> {
    auto task_tuple = std::forward_as_tuple(std::forward<Callables>(callables)...);
    // Each worker only writes its own error slot; Dispatch joins before we read.
    std::array<ErrorCode, ConcurrentSlots> errors {};

    // Expand the lambda pack and dispatch them to the scheduler at compile-time.
    // Each lambda bakes the constant 'Is' directly into its generated class structure.
    std::forward<SchedulerPolicy>(scheduler).Dispatch([this, &task_tuple, &errors]() {
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

        // Do not set CONTINUE_BIT: these secondary buffers begin their own passes.
        auto recording = CommandRecorder::Begin(slot.cmd, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, &inherit_info);
        if (!recording) {
            errors[Is] = recording.error();
            return;
        }

        // Push data does not carry over from the primary: re-push the
        // per-frame device-address block in every secondary.
        if (_frameAddressCount > 0) {
            PushHeapFrameAddresses(
                slot.cmd, std::span<const uint32_t> {_frameAddressOffsets.data(), _frameAddressCount},
                std::span<const VkDeviceAddress> {_frameAddresses.data(), _frameAddressCount}
            );
        }

        std::get<Is>(task_tuple)(slot);
        if (auto executable = std::move(*recording).End(); !executable) {
            errors[Is] = executable.error();
        }
    }...);

    for (size_t i = 0; i < sizeof...(Callables); ++i) {
        if (errors[i]) { return std::unexpected(errors[i]); }
    }
    return {};
}

} // namespace ZHLN::Vk
