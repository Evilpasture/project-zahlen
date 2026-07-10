#include "Swapchain.hpp"
#include "RenderCore.h"
#include <span>
#include <vulkan/vulkan.h>

namespace ZHLN::Vk {

// ============================================================================
// SwapchainSupport & Swapchain Implementation
// ============================================================================

auto SwapchainSupport::Formats() const noexcept -> std::span<const VkSurfaceFormatKHR> {
    return {raw.formats, raw.format_count};
}

auto SwapchainSupport::PresentModes() const noexcept -> std::span<const VkPresentModeKHR> {
    return {raw.present_modes, raw.present_mode_count};
}

SwapchainSupport QuerySwapchainSupport(const VkPhysicalDevice physical, const VkSurfaceKHR surface) noexcept {
    const ZHLN_SwapchainSupportDesc desc = {.physical = physical, .surface = surface};
    return {ZHLN_QuerySwapchainSupport(&desc)};
}

Swapchain::Swapchain(const VkDevice device, const ZHLN_Swapchain raw) noexcept: _device(device), _raw(raw) {
}

Swapchain::~Swapchain() noexcept {
    Destroy();
}

Swapchain::Swapchain(Swapchain&& other) noexcept: _device(std::exchange(other._device, VK_NULL_HANDLE)), _raw(std::exchange(other._raw, {})) {
}

auto Swapchain::operator=(Swapchain&& other) noexcept -> Swapchain& {
    if (this != &other) {
        Destroy();
        _device = std::exchange(other._device, VK_NULL_HANDLE);
        _raw    = std::exchange(other._raw, {});
    }
    return *this;
}

auto Swapchain::Rebuild(const ZHLN_SwapchainDesc& desc) noexcept -> bool {
    _device = desc.device->handle;

    const ZHLN_SwapchainDesc rebuilt = {
        .device        = desc.device,
        .physical      = desc.physical,
        .surface       = desc.surface,
        .width         = desc.width,
        .height        = desc.height,
        .vsync         = desc.vsync,
        .old_swapchain = _raw.handle
    };

    const ZHLN_Swapchain next = ZHLN_CreateSwapchain(&rebuilt);
    if (next.handle == VK_NULL_HANDLE) {
        return false;
    }

    Destroy();
    _raw = next;
    return true;
}

void Swapchain::Destroy() noexcept {
    if (_raw.handle != VK_NULL_HANDLE) {
        ZHLN_DestroySwapchain(_device, &_raw);
    }
}
} // namespace ZHLN::Vk
