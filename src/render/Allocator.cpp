// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/Allocator.cpp

#include "Allocator.hpp"

#include "RenderCore.hpp"

#include <cstring>

namespace ZHLN::Vk {

// ============================================================================
// Allocator RAII
// ============================================================================

Allocator::~Allocator() noexcept {
	if (_handle != nullptr) {
		vmaDestroyAllocator(_handle);
	}
}

Allocator::Allocator(Allocator&& other) noexcept : _handle(std::exchange(other._handle, nullptr)) {}

auto Allocator::operator=(Allocator&& other) noexcept -> Allocator& {
	if (this != &other) {
		if (_handle != nullptr) {
			vmaDestroyAllocator(_handle);
		}
		_handle = std::exchange(other._handle, nullptr);
	}
	return *this;
}

auto Allocator::Init(VkInstance instance, VkPhysicalDevice physical, VkDevice device) noexcept
	-> bool {
	const VmaVulkanFunctions vfuncs = {
		.vkGetInstanceProcAddr = &vkGetInstanceProcAddr,
		.vkGetDeviceProcAddr = &vkGetDeviceProcAddr,
		.vkGetPhysicalDeviceProperties = &vkGetPhysicalDeviceProperties,
		.vkGetPhysicalDeviceMemoryProperties = &vkGetPhysicalDeviceMemoryProperties,
		.vkAllocateMemory = &vkAllocateMemory,
		.vkFreeMemory = &vkFreeMemory,
		.vkMapMemory = &vkMapMemory,
		.vkUnmapMemory = &vkUnmapMemory,
		.vkFlushMappedMemoryRanges = &vkFlushMappedMemoryRanges,
		.vkInvalidateMappedMemoryRanges = &vkInvalidateMappedMemoryRanges,
		.vkBindBufferMemory = &vkBindBufferMemory,
		.vkBindImageMemory = &vkBindImageMemory,
		.vkGetBufferMemoryRequirements = &vkGetBufferMemoryRequirements,
		.vkGetImageMemoryRequirements = &vkGetImageMemoryRequirements,
		.vkCreateBuffer = &vkCreateBuffer,
		.vkDestroyBuffer = &vkDestroyBuffer,
		.vkCreateImage = &vkCreateImage,
		.vkDestroyImage = &vkDestroyImage,
		.vkCmdCopyBuffer = &vkCmdCopyBuffer,
		.vkGetBufferMemoryRequirements2KHR = &vkGetBufferMemoryRequirements2,
		.vkGetImageMemoryRequirements2KHR = &vkGetImageMemoryRequirements2,
		.vkBindBufferMemory2KHR = &vkBindBufferMemory2,
		.vkBindImageMemory2KHR = &vkBindImageMemory2,
		.vkGetPhysicalDeviceMemoryProperties2KHR = &vkGetPhysicalDeviceMemoryProperties2,
		.vkGetDeviceBufferMemoryRequirements = &vkGetDeviceBufferMemoryRequirements,
		.vkGetDeviceImageMemoryRequirements = &vkGetDeviceImageMemoryRequirements,
		.vkGetMemoryWin32HandleKHR = nullptr,
		.vkGetPhysicalDeviceProperties2KHR = {},
	};

	const VmaAllocatorCreateInfo info = {
		.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,
		.physicalDevice = physical,
		.device = device,
		.preferredLargeHeapBlockSize = 0,
		.pAllocationCallbacks = nullptr,
		.pDeviceMemoryCallbacks = nullptr,
		.pHeapSizeLimit = nullptr,
		.pVulkanFunctions = &vfuncs,
		.instance = instance,
		.vulkanApiVersion = VK_API_VERSION_1_3,
		.pTypeExternalMemoryHandleTypes = {},
	};

	return vmaCreateAllocator(&info, &_handle) == VK_SUCCESS;
}

auto Allocator::Init(const Context& ctx) noexcept -> bool {
	return Init(ctx.Instance(), ctx.Physical(), ctx.Device());
}

// ============================================================================
// Buffer RAII
// ============================================================================

Buffer::~Buffer() noexcept {
	if (_handle != VK_NULL_HANDLE) {
		vmaDestroyBuffer(_allocator, _handle, _allocation);
	}
}

Buffer::Buffer(Buffer&& other) noexcept
	: _allocator(other._allocator), _handle(std::exchange(other._handle, VK_NULL_HANDLE)),
	  _allocation(std::exchange(other._allocation, nullptr)), _info(other._info) {}

auto Buffer::operator=(Buffer&& other) noexcept -> Buffer& {
	if (this != &other) {
		if (_handle != VK_NULL_HANDLE) {
			vmaDestroyBuffer(_allocator, _handle, _allocation);
		}
		_allocator = other._allocator;
		_handle = std::exchange(other._handle, VK_NULL_HANDLE);
		_allocation = std::exchange(other._allocation, nullptr);
		_info = other._info;
	}
	return *this;
}

auto Buffer::Create(VmaAllocator allocator, size_t size, VkBufferUsageFlags usage,
					VmaMemoryUsage mem_usage) noexcept -> Buffer {
	Buffer b;
	b._allocator = allocator;

	const VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
											.pNext = nullptr,
											.flags = 0,
											.size = size,
											.usage = usage,
											.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
											.queueFamilyIndexCount = 0,
											.pQueueFamilyIndices = nullptr};

	const VmaAllocationCreateInfo alloc_info = {.flags = 0,
												.usage = mem_usage,
												.requiredFlags = 0,
												.preferredFlags = 0,
												.memoryTypeBits = 0,
												.pool = nullptr,
												.pUserData = nullptr,
												.priority = 0.0F,
												.minAlignment = 0};

	if (vmaCreateBuffer(allocator, &buffer_info, &alloc_info, &b._handle, &b._allocation,
						&b._info) != VK_SUCCESS) {
		return {};
	}

	return b;
}

Buffer::MappedRegion::MappedRegion(VmaAllocator alloc, VmaAllocation allocation, void* ptr) noexcept
	: data(ptr), _alloc(alloc), _allocation(allocation) {}

Buffer::MappedRegion::~MappedRegion() noexcept {
	if (_alloc != VK_NULL_HANDLE) {
		vmaFlushAllocation(_alloc, _allocation, 0, VK_WHOLE_SIZE);
		vmaUnmapMemory(_alloc, _allocation);
	}
}

Buffer::MappedRegion::MappedRegion(MappedRegion&& other) noexcept
	: data(other.data), _alloc(other._alloc), _allocation(other._allocation) {
	other.data = nullptr;
	other._alloc = VK_NULL_HANDLE;
}

auto Buffer::MappedRegion::operator=(MappedRegion&& other) noexcept -> MappedRegion& {
	if (this != &other) {
		if (_alloc != VK_NULL_HANDLE) {
			vmaFlushAllocation(_alloc, _allocation, 0, VK_WHOLE_SIZE);
			vmaUnmapMemory(_alloc, _allocation);
		}

		data = other.data;
		_alloc = other._alloc;
		_allocation = other._allocation;

		other.data = nullptr;
		other._alloc = VK_NULL_HANDLE;
	}
	return *this;
}

auto Buffer::Map() noexcept -> MappedRegion {
	void* ptr = nullptr;
	vmaMapMemory(_allocator, _allocation, &ptr);
	return {_allocator, _allocation, ptr};
}

// ============================================================================
// UploadToBuffer Implementation
// ============================================================================

auto UploadToBuffer(VmaAllocator allocator, VkCommandBuffer cmd, Buffer& dst, const void* data,
					size_t size) noexcept -> Buffer {
	Buffer staging = Buffer::Create(allocator, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
									VMA_MEMORY_USAGE_CPU_ONLY);
	if (!staging.Valid()) {
		return {};
	}

	{
		auto mapped = staging.Map();
		if (mapped.data != nullptr) {
			std::memcpy(mapped.data, data, size);
		}
	}

	const ZHLN_BufferCopyDesc copy = {.src = staging.Handle(),
									  .dst = dst.Handle(),
									  .size = static_cast<VkDeviceSize>(size),
									  .src_offset = 0,
									  .dst_offset = 0};

	ZHLN_CmdCopyBuffer(cmd, &copy);
	return staging;
}

// ============================================================================
// Image RAII
// ============================================================================

Image::~Image() {
	if (_handle != VK_NULL_HANDLE) {
		vmaDestroyImage(_allocator, _handle, _allocation);
	}
}

Image::Image(Image&& other) noexcept
	: _allocator(other._allocator), _handle(std::exchange(other._handle, nullptr)),
	  _allocation(std::exchange(other._allocation, nullptr)) {}

auto Image::operator=(Image&& other) noexcept -> Image& {
	if (this != &other) {
		if (_handle != VK_NULL_HANDLE) {
			vmaDestroyImage(_allocator, _handle, _allocation);
		}
		_allocator = other._allocator;
		_handle = std::exchange(other._handle, nullptr);
		_allocation = std::exchange(other._allocation, nullptr);
	}
	return *this;
}

auto Image::Create(VmaAllocator allocator, const VkImageCreateInfo& info, VmaMemoryUsage mem_usage)
	-> Image {
	Image img;
	img._allocator = allocator;
	VmaAllocationCreateInfo alloc_info = {};
	alloc_info.usage = mem_usage;
	if (vmaCreateImage(allocator, &info, &alloc_info, &img._handle, &img._allocation, nullptr) !=
		VK_SUCCESS) {
		return {};
	}
	return img;
}

} // namespace ZHLN::Vk
