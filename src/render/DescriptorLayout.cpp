// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
// clang-format off
#include "Rendering.hpp"
// clang-format on

#include "DescriptorLayout.hpp"

#include <print>

namespace ZHLN::Vk {

void ReportBindlessRegistryExceeded(uint32_t bindingID, uint32_t capacity) noexcept {
	std::println(stderr, "[BindlessRegistry<{}>] FATAL: Exceeded capacity of {} slots.", bindingID,
				 capacity);
}

DescriptorPoolBuilder::DescriptorPoolBuilder(VkDevice device) noexcept : _device(device) {}

auto DescriptorPoolBuilder::Flags(VkDescriptorPoolCreateFlags flags) noexcept
	-> DescriptorPoolBuilder& {
	_flags = flags;
	return *this;
}

auto DescriptorPoolBuilder::MaxSets(uint32_t maxSets) noexcept -> DescriptorPoolBuilder& {
	_maxSets = maxSets;
	return *this;
}

auto DescriptorPoolBuilder::AddSize(VkDescriptorType type, uint32_t count) noexcept
	-> DescriptorPoolBuilder& {
	_sizes.push_back({.type = type, .descriptorCount = count});
	return *this;
}

auto DescriptorPoolBuilder::Build() const noexcept -> std::expected<DescriptorPool, VkResult> {
	const VkDescriptorPoolCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
											 .pNext = nullptr,
											 .flags = _flags,
											 .maxSets = _maxSets,
											 .poolSizeCount = static_cast<uint32_t>(_sizes.size()),
											 .pPoolSizes = _sizes.empty() ? nullptr : _sizes.data()};

	VkDescriptorPool pool = VK_NULL_HANDLE;
	VkResult res = vkCreateDescriptorPool(_device, &info, nullptr, &pool);
	if (res != VK_SUCCESS) {
		return std::unexpected(res);
	}
	return DescriptorPool(_device, pool);
}

} // namespace ZHLN::Vk
