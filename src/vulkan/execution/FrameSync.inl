// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "FrameSync.hpp"
#include <utility>

namespace ZHLN::Vk {

template <uint32_t N>
    requires(N > 0)
inline FrameSync<N>::~FrameSync() noexcept {
    Reset();
}

template <uint32_t N>
    requires(N > 0)
inline FrameSync<N>::FrameSync(FrameSync&& other) noexcept:
    _device(std::exchange(other._device, VK_NULL_HANDLE)),
    _frames(std::exchange(other._frames, {})),
    _timelineValues(std::exchange(other._timelineValues, {})),
    _computeValuesSubmitted(std::exchange(other._computeValuesSubmitted, {})),
    _graphicsTimelineValues(std::exchange(other._graphicsTimelineValues, {})),
    _graphicsValuesSubmitted(std::exchange(other._graphicsValuesSubmitted, {})) {}

template <uint32_t N>
    requires(N > 0)
inline auto FrameSync<N>::operator=(FrameSync&& other) noexcept -> FrameSync& {
    if (this != &other) {
        Reset();
        _device                 = std::exchange(other._device, VK_NULL_HANDLE);
        _frames                 = std::exchange(other._frames, {});
        _timelineValues         = std::exchange(other._timelineValues, {});
        _computeValuesSubmitted = std::exchange(other._computeValuesSubmitted, {});
        _graphicsTimelineValues = std::exchange(other._graphicsTimelineValues, {});
        _graphicsValuesSubmitted = std::exchange(other._graphicsValuesSubmitted, {});
    }
    return *this;
}

template <uint32_t N>
    requires(N > 0)
inline auto FrameSync<N>::Create(const VkDevice device) noexcept -> FrameSync {
    FrameSync sync;
    if (device == VK_NULL_HANDLE) {
        return sync;
    }

    sync._device = device;
    const VkSemaphoreCreateInfo binarySemaphoreInfo {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };
    const VkSemaphoreTypeCreateInfo timelineTypeInfo {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = 0,
    };
    const VkSemaphoreCreateInfo timelineSemaphoreInfo {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &timelineTypeInfo,
    };


    for (FrameSyncSlot& frame: sync._frames) {
        if (vkCreateSemaphore(device, &binarySemaphoreInfo, nullptr, &frame.imageAvailable) != VK_SUCCESS ||
            vkCreateSemaphore(device, &timelineSemaphoreInfo, nullptr, &frame.computeTimeline) != VK_SUCCESS ||
            vkCreateSemaphore(device, &timelineSemaphoreInfo, nullptr, &frame.graphicsTimeline) != VK_SUCCESS) {
            sync.Reset();
            return {};
        }
    }
    return sync;
}

template <uint32_t N>
    requires(N > 0)
inline void FrameSync<N>::Reset() noexcept {
    if (_device != VK_NULL_HANDLE) {
        for (FrameSyncSlot& frame: _frames) {
            if (frame.imageAvailable != VK_NULL_HANDLE) {
                vkDestroySemaphore(_device, frame.imageAvailable, nullptr);
            }
            if (frame.computeTimeline != VK_NULL_HANDLE) {
                vkDestroySemaphore(_device, frame.computeTimeline, nullptr);
            }
            if (frame.graphicsTimeline != VK_NULL_HANDLE) {
                vkDestroySemaphore(_device, frame.graphicsTimeline, nullptr);
            }
            frame = {};
        }
    }
    _device = VK_NULL_HANDLE;
    _timelineValues = {};
    _computeValuesSubmitted = {};
    _graphicsTimelineValues = {};
    _graphicsValuesSubmitted = {};
}

}
