#pragma once

#include "FrameSync.hpp"

namespace ZHLN::Vk {

template <uint32_t N>
	requires(N > 0 && N <= 8)
inline FrameSync<N>::~FrameSync() noexcept {
	if (_device != VK_NULL_HANDLE) {
		ZHLN_DestroyFrameSync(_device, _frames.data(), N);
	}
}

template <uint32_t N>
	requires(N > 0 && N <= 8)
inline FrameSync<N>::FrameSync(FrameSync&& other) noexcept
	: _device(std::exchange(other._device, VK_NULL_HANDLE)),
	  _frames(std::exchange(other._frames, {})) {}

template <uint32_t N>
	requires(N > 0 && N <= 8)
inline auto FrameSync<N>::operator=(FrameSync&& other) noexcept -> FrameSync& {
	if (this != &other) {
		if (_device != VK_NULL_HANDLE) {
			ZHLN_DestroyFrameSync(_device, _frames.data(), N);
		}
		_device = std::exchange(other._device, VK_NULL_HANDLE);
		_frames = std::exchange(other._frames, {});
	}
	return *this;
}

template <uint32_t N>
	requires(N > 0 && N <= 8)
inline auto FrameSync<N>::Create(const VkDevice device) noexcept -> FrameSync {
	FrameSync fs;
	const ZHLN_FrameSyncDesc desc = {.device = device, .frame_count = N};
	if (!ZHLN_CreateFrameSync(&desc, fs._frames.data())) {
		return {};
	}
	fs._device = device;
	return fs;
}
} // namespace ZHLN::Vk
