// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ParallelRecorder.hpp"

namespace ZHLN::Vk {

template <size_t ConcurrentSlots, size_t MaxFrameAddresses>
auto ParallelCommandRecorder<ConcurrentSlots, MaxFrameAddresses>::Init(VkDevice device, uint32_t queueFamily) noexcept -> std::expected<void, ErrorCode> {
    _device = device;
    for (size_t i = 0; i < ConcurrentSlots; ++i) {
        _pools[i] = CommandPool(_device, queueFamily);

        auto alloc = _pools[i].AllocateSecondary(8);
        if (!alloc) [[unlikely]] {
            return std::unexpected(alloc.error());
        }
        _cmds[i] = _pools[i][0];
    }
    _nextGroup = 0;
    return {};
}

template <size_t ConcurrentSlots, size_t MaxFrameAddresses>
void ParallelCommandRecorder<ConcurrentSlots, MaxFrameAddresses>::Reset() noexcept {
    for (auto& pool: _pools) {
        pool.Reset();
    }
    _nextGroup = 0;
}

template <size_t ConcurrentSlots, size_t MaxFrameAddresses>
template <typename SchedulerPolicy, typename... Callables>
auto ParallelCommandRecorder<ConcurrentSlots, MaxFrameAddresses>::Record(SchedulerPolicy&& scheduler, Callables&&... callables) -> RecordingBatch {
    static_assert(
        sizeof...(Callables) <= ConcurrentSlots, "The number of recording tasks exceeds the allocated "
                                                 "ParallelCommandRecorder slots."
    );

    return RecordImpl(std::forward<SchedulerPolicy>(scheduler), std::make_index_sequence<sizeof...(Callables)> {}, std::forward<Callables>(callables)...);
}

template <size_t ConcurrentSlots, size_t MaxFrameAddresses>
template <typename SchedulerPolicy, size_t... Is, typename... Callables>
auto ParallelCommandRecorder<ConcurrentSlots, MaxFrameAddresses>::RecordImpl(SchedulerPolicy&& scheduler, std::index_sequence<Is...>, Callables&&... callables)
    -> RecordingBatch {
    const uint32_t groupIdx = (_pools[0].Size() > 0 && _nextGroup < _pools[0].Size()) ? _nextGroup++ : 0u;
    for (size_t i = 0; i < ConcurrentSlots; ++i) {
        _cmds[i] = _pools[i][groupIdx];
    }

    auto task_tuple = std::forward_as_tuple(std::forward<Callables>(callables)...);

    // Each worker owns one element of each array, so the recording threads never
    // touch shared state; the batch is assembled below, after they have joined.
    // The two arrays are what the batch is for: the old shape kept the same
    // `errors` array and then threw away everything but the first entry.
    std::array<ErrorCode, ConcurrentSlots> errors {};
    std::array<bool, ConcurrentSlots>      recorded {};

    std::forward<SchedulerPolicy>(scheduler).Dispatch([this, &task_tuple, &errors, &recorded]() {
        RecordingSlot slot {.cmd = _cmds[Is], .slotIndex = static_cast<uint32_t>(Is)};

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

        auto recording = CommandRecorder::Begin(slot.cmd, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, &inherit_info);
        if (!recording) {
            errors[Is] = recording.error();
            return;
        }

        if (_frameAddressCount > 0) {
            PushHeapFrameAddresses(
                slot.cmd, std::span<const uint32_t> {_frameAddressOffsets.data(), _frameAddressCount},
                std::span<const VkDeviceAddress> {_frameAddresses.data(), _frameAddressCount}
            );
        }

        std::get<Is>(task_tuple)(slot);
        if (auto executable = std::move(*recording).End(); !executable) {
            errors[Is] = executable.error();
            return;
        }
        recorded[Is] = true;
    }...);

    RecordingBatch outcome;
    outcome.Reserve(sizeof...(Callables), sizeof...(Callables));
    for (size_t i = 0; i < sizeof...(Callables); ++i) {
        // Every callable gets an answer: a slot that recorded is a success, and
        // a slot that did not is a failure with whatever error it reported --
        // never a silent gap in the ledger.
        if (recorded[i]) {
            outcome.AddSuccess(static_cast<uint32_t>(i));
        } else {
            outcome.EmplaceError(RecordingFailure {.index = static_cast<uint32_t>(i), .error = errors[i]});
        }
    }
    return outcome;
}
}
