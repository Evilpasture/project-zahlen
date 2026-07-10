#include "Surface.hpp"

namespace ZHLN::Vk {

// ============================================================================
// Surface Implementation
// ============================================================================

Surface::Surface(VkInstance instance, VkSurfaceKHR surface)
	: _instance(instance), _handle(surface) {}

Surface::~Surface() {
	if (_handle != VK_NULL_HANDLE) {
		vkDestroySurfaceKHR(_instance, _handle, nullptr);
	}
}

Surface::Surface(Surface&& other) noexcept
	: _instance(std::exchange(other._instance, VK_NULL_HANDLE)),
	  _handle(std::exchange(other._handle, VK_NULL_HANDLE)) {}

auto Surface::operator=(Surface&& other) noexcept -> Surface& {
	if (this != &other) {
		if (_handle != VK_NULL_HANDLE) {
			vkDestroySurfaceKHR(_instance, _handle, nullptr);
		}
		_instance = std::exchange(other._instance, VK_NULL_HANDLE);
		_handle = std::exchange(other._handle, VK_NULL_HANDLE);
	}
	return *this;
}

auto Surface::Get() const -> VkSurfaceKHR {
	return _handle;
}
} // namespace ZHLN::Vk
