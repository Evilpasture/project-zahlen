// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ParallelRecorder.hpp"

namespace ZHLN::Vk {

template <size_t ConcurrentSlots>
auto ParallelCommandRecorder<ConcurrentSlots>::Init(VkDevice device, uint32_t queueFamily) noexcept -> std::expected<void, ErrorCode> {
    _device = device;
    for (size_t i = 0; i < ConcurrentSlots; ++i) {
        auto created = CommandPool<QueueType::Graphics>::Create(_device, queueFamily);
        if (!created) [[unlikely]] {
            return std::unexpected(created.error());
        }
        _pools[i] = std::move(*created);

        auto alloc = _pools[i].AllocateSecondary(8);
        if (!alloc) [[unlikely]] {
            return std::unexpected(alloc.error());
        }
        _cmds[i] = _pools[i][0];
    }
    _nextGroup = 0;
    return {};
}

template <size_t ConcurrentSlots>
auto ParallelCommandRecorder<ConcurrentSlots>::Reset() noexcept -> std::expected<void, ErrorCode> {
    for (auto& pool: _pools) {
        if (auto reset = pool.Reset(); !reset) [[unlikely]] {
            return std::unexpected(reset.error());
        }
    }
    _nextGroup = 0;
    return {};
}

template <size_t ConcurrentSlots>
template <typename SchedulerPolicy, typename... Callables>
auto ParallelCommandRecorder<ConcurrentSlots>::Record(SchedulerPolicy&& scheduler, Callables&&... callables) -> RecordingBatch {
    static_assert(
        sizeof...(Callables) <= ConcurrentSlots, "The number of recording tasks exceeds the allocated "
                                                 "ParallelCommandRecorder slots."
    );

    return RecordImpl(std::forward<SchedulerPolicy>(scheduler), std::make_index_sequence<sizeof...(Callables)> {}, std::forward<Callables>(callables)...);
}

template <size_t ConcurrentSlots>
template <typename SchedulerPolicy, size_t... Is, typename... Callables>
auto ParallelCommandRecorder<ConcurrentSlots>::RecordImpl(SchedulerPolicy&& scheduler, std::index_sequence<Is...>, Callables&&... callables)
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

        auto recording = _context.Begin(slot.cmd, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
        if (!recording) {
            errors[Is] = recording.error();
            return;
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
