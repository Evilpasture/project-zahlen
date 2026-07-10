#pragma once

namespace ZHLN::Vk {
// ============================================================================
// Swapchain RAII
// ============================================================================

struct SwapchainSupport {
    ZHLN_SwapchainSupport raw;
    [[nodiscard]] auto    Formats() const noexcept -> std::span<const VkSurfaceFormatKHR>;
    [[nodiscard]] auto    PresentModes() const noexcept -> std::span<const VkPresentModeKHR>;
};

[[nodiscard]] SwapchainSupport QuerySwapchainSupport(const VkPhysicalDevice physical, const VkSurfaceKHR surface) noexcept;

class Swapchain {
  public:
    Swapchain() noexcept = default;
    Swapchain(const VkDevice device, const ZHLN_Swapchain raw) noexcept;
    ~Swapchain() noexcept;

    Swapchain(const Swapchain&)                    = delete;
    auto operator=(const Swapchain&) -> Swapchain& = delete;

    Swapchain(Swapchain&& other) noexcept;
    auto operator=(Swapchain&& other) noexcept -> Swapchain&;

    [[nodiscard]] constexpr auto Get() const noexcept -> const ZHLN_Swapchain& {
        return _raw;
    }
    [[nodiscard("Verify swapchain validity before use")]]
    constexpr auto Valid() const noexcept -> bool {
        return _raw.handle != VK_NULL_HANDLE;
    }
    constexpr explicit operator bool() const noexcept {
        return Valid();
    }

    auto Rebuild(const ZHLN_SwapchainDesc& desc) noexcept -> bool;

  private:
    void Destroy() noexcept;

    VkDevice       _device = VK_NULL_HANDLE;
    ZHLN_Swapchain _raw    = {};
};
} // namespace ZHLN::Vk
