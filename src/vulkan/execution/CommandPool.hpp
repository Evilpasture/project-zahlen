// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "../VkError.hpp"
#include "RenderQueue.hpp"

#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Log.hpp>
#include <array>
#include <cstdint>
#include <expected>
#include <vector>

namespace ZHLN::Vk {

enum class CommandPoolError : uint8_t {
    PoolNotReady ZHLN_ANNOTATION(ZHLN::Description<"Command pool device handle is not initialized">{}) = 1,
    NullDevice ZHLN_ANNOTATION(ZHLN::Description<"Cannot create a command pool with a null device">{}),
};

template <Vk::QueueType QType = Vk::QueueType::Graphics>
class CommandPool {
  public:
    CommandPool() noexcept = default;
    ~CommandPool() noexcept;

    CommandPool(const CommandPool&)                    = delete;
    auto operator=(const CommandPool&) -> CommandPool& = delete;

    CommandPool(CommandPool&& other) noexcept;
    auto operator=(CommandPool&& other) noexcept -> CommandPool&;

    [[nodiscard]] static auto Create(VkDevice device, uint32_t queueFamily) noexcept -> std::expected<CommandPool, ErrorCode>;

    [[nodiscard]] auto Valid() const noexcept -> bool {
        return _device != VK_NULL_HANDLE && _pool != VK_NULL_HANDLE;
    }
    explicit operator bool() const noexcept {
        return Valid();
    }

    [[nodiscard]] auto EnsureValid() const noexcept -> std::expected<void, ErrorCode>;
    [[nodiscard]] auto Allocate(uint32_t count) noexcept -> std::expected<void, ErrorCode>;
    [[nodiscard]] auto AllocateSecondary(uint32_t count) noexcept -> std::expected<void, ErrorCode>;
    [[nodiscard]] auto Reset(VkCommandPoolResetFlags flags = 0) noexcept -> std::expected<void, ErrorCode>;

    [[nodiscard]] auto operator[](uint32_t index) const noexcept -> Vk::CommandBuffer<QType> {
        ZHLN::Assert(index < _buffers.size(), "Command buffer index out of range");
        return Vk::CommandBuffer<QType> {_buffers[index]};
    }
    [[nodiscard]] auto Size() const noexcept -> size_t {
        return _buffers.size();
    }

  private:
    [[nodiscard]] auto AllocateLevel(uint32_t count, VkCommandBufferLevel level) noexcept -> std::expected<void, ErrorCode>;
    void Destroy() noexcept;

    VkDevice                     _device = VK_NULL_HANDLE;
    VkCommandPool                _pool   = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> _buffers;
};

template <uint32_t N, Vk::QueueType QType = Vk::QueueType::Graphics>
    requires(N > 0)
class CommandPools {
  public:
    struct Description {
        uint32_t queueFamily    = 0;
        uint32_t buffersPerPool = 1;
    };

    CommandPools() noexcept = default;

    [[nodiscard]] static auto Create(VkDevice device, const Description& desc) noexcept -> std::expected<CommandPools, ErrorCode>;

    [[nodiscard]] auto operator[](uint32_t frame) noexcept -> CommandPool<QType>& {
        return _pools[frame % N];
    }
    [[nodiscard]] auto operator[](uint32_t frame) const noexcept -> const CommandPool<QType>& {
        return _pools[frame % N];
    }
    [[nodiscard]] auto Cmd(uint32_t frame) const noexcept -> Vk::CommandBuffer<QType> {
        return _pools[frame % N][0];
    }
    [[nodiscard]] auto Valid() const noexcept -> bool {
        for (const auto& pool: _pools) {
            if (!pool.Valid()) {
                return false;
            }
        }
        return true;
    }

  private:
    std::array<CommandPool<QType>, N> _pools {};
};

} // namespace ZHLN::Vk

#include "CommandPool.inl"
