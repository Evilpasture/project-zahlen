// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/Allocator.cpp
// clang-format off
#include "Rendering.hpp"
// clang-format on
#include "Allocator.hpp"

#include "RenderCore.hpp"

#include <cstring>
#include <sys/types.h>
#include <vector>

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
#if VMA_GET_PHYSICAL_DEVICE_PROPERTIES2
		.vkGetPhysicalDeviceProperties2KHR = nullptr,
#endif
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

auto Buffer::Create(VmaAllocator allocator, size_t size, VkBufferUsageFlags usage,
					VmaMemoryUsage mem_usage) noexcept -> Buffer {
	VkBuffer buffer = VK_NULL_HANDLE;
	VmaAllocation alloc = nullptr;
	VmaAllocationInfo info = {};

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

	if (vmaCreateBuffer(allocator, &buffer_info, &alloc_info, &buffer, &alloc, &info) !=
		VK_SUCCESS) {
		return {};
	}

	Buffer b;
	b._handle = {allocator, buffer, alloc};
	b._info = info;
	return b;
}

Buffer::MappedRegion::MappedRegion(VmaAllocator alloc, VmaAllocation allocation, void* ptr) noexcept
	: data(ptr), _handle(alloc, ptr, allocation) {}

Buffer::MappedRegion::MappedRegion(MappedRegion&& other) noexcept
	: data(std::exchange(other.data, nullptr)), _handle(std::move(other._handle)) {}

auto Buffer::MappedRegion::operator=(MappedRegion&& other) noexcept -> MappedRegion& {
	if (this != &other) {
		data = std::exchange(other.data, nullptr);
		_handle = std::move(other._handle);
	}
	return *this;
}

auto Buffer::Map() noexcept -> MappedRegion {
	void* ptr = nullptr;
	vmaMapMemory(_handle.Allocator(), _handle.Allocation(), &ptr);
	return {_handle.Allocator(), _handle.Allocation(), ptr};
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

auto Image::Create(VmaAllocator allocator, const VkImageCreateInfo& info, VmaMemoryUsage mem_usage)
	-> Image {
	VkImage img = VK_NULL_HANDLE;
	VmaAllocation alloc = nullptr;
	const VmaAllocationCreateInfo alloc_info = {.flags = {},
												.usage = mem_usage,
												.requiredFlags = {},
												.preferredFlags = {},
												.memoryTypeBits = {},
												.pool = {},
												.pUserData = {},
												.priority = {},
												.minAlignment = {}};
	if (vmaCreateImage(allocator, &info, &alloc_info, &img, &alloc, nullptr) != VK_SUCCESS) {
		return {};
	}
	Image res;
	res._handle = {allocator, img, alloc};
	return res;
}

// ============================================================================
// Immediate Command PIMPL Implementation
// ============================================================================

struct ImmediateCommand::Impl {
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	std::vector<Buffer> resources;

	Impl(Impl&& other) noexcept
		: device(std::exchange(other.device, VK_NULL_HANDLE)),
		  queue(std::exchange(other.queue, VK_NULL_HANDLE)),
		  pool(std::exchange(other.pool, VK_NULL_HANDLE)),
		  cmd(std::exchange(other.cmd, VK_NULL_HANDLE)), resources(std::move(other.resources)) {}
	Impl& operator=(Impl&& other) noexcept {
		if (this != &other) {
			// Let the destructor handle existing resources cleanly
			this->~Impl();

			device = std::exchange(other.device, VK_NULL_HANDLE);
			queue = std::exchange(other.queue, VK_NULL_HANDLE);
			pool = std::exchange(other.pool, VK_NULL_HANDLE);
			cmd = std::exchange(other.cmd, VK_NULL_HANDLE);
			resources = std::move(other.resources);
		}
		return *this;
	}
	Impl(const Impl&) = delete;
	Impl& operator=(const Impl&) = delete;
	Impl(VkDevice dev, VkQueue q, uint32_t queueFamily) noexcept : device(dev), queue(q) {
		VkCommandPoolCreateInfo poolInfo = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			.pNext = {},
			.flags = {},
			.queueFamilyIndex = queueFamily,
		};
		vkCreateCommandPool(device, &poolInfo, nullptr, &pool);

		VkCommandBufferAllocateInfo allocInfo = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.pNext = {},
			.commandPool = pool,
			.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			.commandBufferCount = 1,
		};
		vkAllocateCommandBuffers(device, &allocInfo, &cmd);

		VkCommandBufferBeginInfo beginInfo = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.pNext = {},
			.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
			.pInheritanceInfo = {},
		};
		vkBeginCommandBuffer(cmd, &beginInfo);
	}

	~Impl() noexcept {
		if (cmd != VK_NULL_HANDLE) {
			vkEndCommandBuffer(cmd);

			VkCommandBufferSubmitInfo subInfo = {
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
				.pNext = {},
				.commandBuffer = cmd,
				.deviceMask = {},
			};
			VkSubmitInfo2 submit = {
				.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
				.pNext = {},
				.flags = {},
				.waitSemaphoreInfoCount = {},
				.pWaitSemaphoreInfos = {},
				.commandBufferInfoCount = 1,
				.pCommandBufferInfos = &subInfo,
				.signalSemaphoreInfoCount = {},
				.pSignalSemaphoreInfos = {},
			};

			vkQueueSubmit2(queue, 1, &submit, VK_NULL_HANDLE);
			vkQueueWaitIdle(queue);
		}

		// Staging buffers are safely destroyed here, after the queue becomes idle
		resources.clear();

		if (pool != VK_NULL_HANDLE) {
			vkDestroyCommandPool(device, pool, nullptr);
		}
	}
};

ImmediateCommand::ImmediateCommand(VkDevice dev, VkQueue q, uint32_t queueFamily) noexcept
	: _impl(std::make_unique<Impl>(dev, q, queueFamily)) {}

ImmediateCommand::~ImmediateCommand() noexcept = default;

void ImmediateCommand::KeepAlive(Buffer&& buffer) {
	_impl->resources.push_back(std::move(buffer));
}

ImmediateCommand::operator VkCommandBuffer() const noexcept {
	return _impl->cmd;
}

} // namespace ZHLN::Vk
