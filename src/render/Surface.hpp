// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace ZHLN::Vk {
// ============================================================================
// Surface helpers
// ============================================================================

class Surface {
  public:
    Surface() = default;
    Surface(VkInstance instance, VkSurfaceKHR surface);
    ~Surface();

    Surface(const Surface&)                    = delete;
    auto operator=(const Surface&) -> Surface& = delete;

    Surface(Surface&& other) noexcept;
    auto operator=(Surface&& other) noexcept -> Surface&;

    [[nodiscard]] auto Get() const -> VkSurfaceKHR;

  private:
    VkInstance   _instance = VK_NULL_HANDLE;
    VkSurfaceKHR _handle   = VK_NULL_HANDLE;
};
} // namespace ZHLN::Vk
