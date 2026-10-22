// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <Zahlen/Core/ErrorCode.hpp>
#include <cstdint>
#include <expected>
#include <type_traits>
#include <utility>

namespace ZHLN::Vk {

struct DestroyPipeline {
    void operator()(VkDevice device, VkPipeline handle) const noexcept;
};

struct DestroyPipelineBinary {
    void operator()(VkDevice device, VkPipelineBinaryKHR handle) const noexcept;
};

struct DestroySemaphore {
    void operator()(VkDevice device, VkSemaphore handle) const noexcept;
};

struct DestroyAttachmentView {
    void operator()(VkDevice device, VkImageView handle) const noexcept;
};

struct DestroyAccelerationStructure {
    void operator()(VkDevice device, VkAccelerationStructureKHR handle) const noexcept;
};

template <typename T, typename Deleter>
class DeviceHandle {
  public:
    DeviceHandle() noexcept = default;
    DeviceHandle(VkDevice device, T raw) noexcept: _device(device), _raw(raw) {}
    ~DeviceHandle() noexcept {
        Reset();
    }

    DeviceHandle(const DeviceHandle&)                    = delete;
    auto operator=(const DeviceHandle&) -> DeviceHandle& = delete;

    DeviceHandle(DeviceHandle&& other) noexcept:
        _device(std::exchange(other._device, VK_NULL_HANDLE)), _raw(std::exchange(other._raw, VK_NULL_HANDLE)) {}

    auto operator=(DeviceHandle&& other) noexcept -> DeviceHandle& {
        if (this != &other) {
            Reset();
            _device = std::exchange(other._device, VK_NULL_HANDLE);
            _raw    = std::exchange(other._raw, VK_NULL_HANDLE);
        }
        return *this;
    }

    [[nodiscard]] constexpr auto Get() const noexcept -> T {
        return _raw;
    }
    [[nodiscard]] constexpr auto Valid() const noexcept -> bool {
        return _device != VK_NULL_HANDLE && _raw != VK_NULL_HANDLE;
    }
    constexpr explicit operator bool() const noexcept {
        return Valid();
    }

    [[nodiscard]] auto Release() noexcept -> T {
        _device = VK_NULL_HANDLE;
        return std::exchange(_raw, VK_NULL_HANDLE);
    }

    void Reset() noexcept {
        if (_device != VK_NULL_HANDLE && _raw != VK_NULL_HANDLE) {
            Deleter {}(_device, _raw);
        }
        _device = VK_NULL_HANDLE;
        _raw    = VK_NULL_HANDLE;
    }

  private:
    VkDevice _device = VK_NULL_HANDLE;
    T        _raw    = VK_NULL_HANDLE;
};

/// Owning, move-only semaphore. This is the resource side of semaphore
/// synchronization; the submission side is the non-owning SemaphoreSubmit value
/// type in execution/RenderQueue.hpp, which stores a borrowed VkSemaphore.
class Semaphore: public DeviceHandle<VkSemaphore, DestroySemaphore> {
  public:
    using DeviceHandle::DeviceHandle;

    /// Creates a binary semaphore (unsignaled).
    [[nodiscard]] static auto CreateBinary(VkDevice device) noexcept -> std::expected<Semaphore, ErrorCode>;

    /// Creates a timeline semaphore starting at `initialValue`.
    [[nodiscard]] static auto CreateTimeline(VkDevice device, uint64_t initialValue = 0) noexcept -> std::expected<Semaphore, ErrorCode>;
};

static_assert(!std::is_copy_constructible_v<Semaphore> && !std::is_copy_assignable_v<Semaphore>);
static_assert(std::is_nothrow_move_constructible_v<Semaphore> && std::is_nothrow_move_assignable_v<Semaphore>);

using Pipeline              = DeviceHandle<VkPipeline, DestroyPipeline>;
using PipelineBinary        = DeviceHandle<VkPipelineBinaryKHR, DestroyPipelineBinary>;
using AttachmentViewHandle  = DeviceHandle<VkImageView, DestroyAttachmentView>;
using AccelerationStructure = DeviceHandle<VkAccelerationStructureKHR, DestroyAccelerationStructure>;

} // namespace ZHLN::Vk
