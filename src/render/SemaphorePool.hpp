// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace ZHLN::Vk {

// ============================================================================
// Semaphore Helpers
// ============================================================================

class alignas(64) SemaphorePool {
  public:
    SemaphorePool() noexcept = default;
    ~SemaphorePool() noexcept;

    SemaphorePool(const SemaphorePool&)                    = delete;
    auto operator=(const SemaphorePool&) -> SemaphorePool& = delete;

    SemaphorePool(SemaphorePool&& other) noexcept;
    auto operator=(SemaphorePool&& other) noexcept -> SemaphorePool&;

    void Rebuild(const VkDevice device, const uint32_t count) noexcept;
    [[nodiscard("Semaphore access must be checked for bounds; invalid indices will crash")]]
    auto operator[](const uint32_t index) const noexcept -> VkSemaphore;

    [[nodiscard]] auto Count() const noexcept -> uint32_t;
    [[nodiscard("Verify semaphore pool is initialized before use")]]
    auto Valid() const noexcept -> bool;

  private:
    void Cleanup() noexcept;

    VkDevice                   _device     = VK_NULL_HANDLE;
    uint32_t                   _count      = 0;
    [[maybe_unused]] uint32_t  _padding    = 0;
    std::array<VkSemaphore, 6> _semaphores = {};
};
} // namespace ZHLN::Vk
