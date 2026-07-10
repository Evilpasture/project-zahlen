#pragma once

#include "Handles.hpp"

namespace ZHLN::Vk {

// ============================================================================
// DeviceHandle Implementation
// ============================================================================

template <typename T, auto DeleterFn>
inline DeviceHandle<T, DeleterFn>::DeviceHandle(const VkDevice device, const T raw) noexcept
	: _device(device), _raw(raw) {}

template <typename T, auto DeleterFn> inline DeviceHandle<T, DeleterFn>::~DeviceHandle() noexcept {
	if (_raw != VK_NULL_HANDLE) {
		DeleterFn(_device, _raw);
	}
}

template <typename T, auto DeleterFn>
inline DeviceHandle<T, DeleterFn>::DeviceHandle(DeviceHandle&& other) noexcept
	: _device(std::exchange(other._device, VK_NULL_HANDLE)),
	  _raw(std::exchange(other._raw, VK_NULL_HANDLE)) {}

template <typename T, auto DeleterFn>
inline auto DeviceHandle<T, DeleterFn>::operator=(DeviceHandle&& other) noexcept -> DeviceHandle& {
	if (this != &other) {
		if (_raw != VK_NULL_HANDLE) {
			DeleterFn(_device, _raw);
		}
		_device = std::exchange(other._device, VK_NULL_HANDLE);
		_raw = std::exchange(other._raw, VK_NULL_HANDLE);
	}
	return *this;
}

template <typename T, auto DeleterFn>
constexpr auto DeviceHandle<T, DeleterFn>::Get() const noexcept -> T {
	return _raw;
}

template <typename T, auto DeleterFn>
constexpr auto DeviceHandle<T, DeleterFn>::Valid() const noexcept -> bool {
	return _raw != VK_NULL_HANDLE;
}

template <typename T, auto DeleterFn>
constexpr DeviceHandle<T, DeleterFn>::operator bool() const noexcept {
	return Valid();
}

template <typename T, auto DeleterFn>
constexpr auto DeviceHandle<T, DeleterFn>::Release() noexcept -> T {
	return std::exchange(_raw, VK_NULL_HANDLE);
}

} // namespace ZHLN::Vk
