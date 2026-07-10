// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/Allocator.cpp

#include "RenderQueue.hpp"
#include "Rendering.hpp"

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
[[nodiscard]]
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

	VmaAllocationCreateInfo alloc_info = {.flags = 0,
										  .usage = mem_usage,
										  .requiredFlags = 0,
										  .preferredFlags = 0,
										  .memoryTypeBits = 0,
										  .pool = nullptr,
										  .pUserData = nullptr,
										  .priority = 0.0F,
										  .minAlignment = 0};

	// Automatically request persistent mapping for host-visible memory types
	if (mem_usage == VMA_MEMORY_USAGE_CPU_ONLY || mem_usage == VMA_MEMORY_USAGE_CPU_TO_GPU ||
		mem_usage == VMA_MEMORY_USAGE_GPU_TO_CPU) {
		alloc_info.flags |= VMA_ALLOCATION_CREATE_MAPPED_BIT;
	}

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
	// If the buffer is already persistently mapped, bypass expensive vmaMapMemory system calls
	if (_info.pMappedData != nullptr) {
		return {nullptr, nullptr, _info.pMappedData};
	}
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
// StagingRingBuffer Implementation
// ============================================================================

StagingRingBuffer::StagingRingBuffer(StagingRingBuffer&& other) noexcept
	: _allocator(std::exchange(other._allocator, nullptr)),
	  _device(std::exchange(other._device, VK_NULL_HANDLE)),
	  _queue(std::exchange(other._queue, VK_NULL_HANDLE)),
	  _queueFamily(std::exchange(other._queueFamily, 0xFFFFFFFF)),
	  _stagingBuffer(std::move(other._stagingBuffer)),
	  _mappedRegion(std::move(other._mappedRegion)),
	  _mappedPtr(std::exchange(other._mappedPtr, nullptr)),
	  _capacity(std::exchange(other._capacity, 0)), _head(std::exchange(other._head, 0)),
	  _tail(std::exchange(other._tail, 0)),
	  _timelineSemaphore(std::exchange(other._timelineSemaphore, VK_NULL_HANDLE)),
	  _timelineValue(std::exchange(other._timelineValue, 0)),
	  _activeAllocations(std::move(other._activeAllocations)),
	  _retiredPools(std::move(other._retiredPools)) {}

auto StagingRingBuffer::operator=(StagingRingBuffer&& other) noexcept -> StagingRingBuffer& {
	if (this != &other) {
		Cleanup();
		_allocator = std::exchange(other._allocator, nullptr);
		_device = std::exchange(other._device, VK_NULL_HANDLE);
		_queue = std::exchange(other._queue, VK_NULL_HANDLE);
		_queueFamily = std::exchange(other._queueFamily, 0xFFFFFFFF);
		_stagingBuffer = std::move(other._stagingBuffer);
		_mappedRegion = std::move(other._mappedRegion);
		_mappedPtr = std::exchange(other._mappedPtr, nullptr);
		_capacity = std::exchange(other._capacity, 0);
		_head = std::exchange(other._head, 0);
		_tail = std::exchange(other._tail, 0);
		_timelineSemaphore = std::exchange(other._timelineSemaphore, VK_NULL_HANDLE);
		_timelineValue = std::exchange(other._timelineValue, 0);
		_activeAllocations = std::move(other._activeAllocations);
		_retiredPools = std::move(other._retiredPools);
	}
	return *this;
}

auto StagingRingBuffer::Init(VmaAllocator allocator, VkDevice device, VkQueue queue,
							 uint32_t queueFamily, VkDeviceSize capacity) noexcept -> bool {
	_allocator = allocator;
	_device = device;
	_queue = queue;
	_queueFamily = queueFamily;
	_capacity = capacity;

	VkSemaphoreTypeCreateInfo typeInfo = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
										  .pNext = nullptr,
										  .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
										  .initialValue = 0};
	VkSemaphoreCreateInfo semInfo = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &typeInfo, .flags = 0};
	if (vkCreateSemaphore(_device, &semInfo, nullptr, &_timelineSemaphore) != VK_SUCCESS) {
		return false;
	}

	_stagingBuffer = Buffer::Create(_allocator, _capacity, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
									VMA_MEMORY_USAGE_CPU_ONLY);
	if (!_stagingBuffer.Valid()) {
		vkDestroySemaphore(_device, _timelineSemaphore, nullptr);
		_timelineSemaphore = VK_NULL_HANDLE;
		return false;
	}

	_mappedRegion = _stagingBuffer.Map();
	_mappedPtr = _mappedRegion.data;
	return true;
}

void StagingRingBuffer::Cleanup() noexcept {
	if (_device != VK_NULL_HANDLE) {
		_mappedRegion = {};	 // Clean up mapping wrapper
		_stagingBuffer = {}; // Clean up buffer handle
		for (auto& rp : _retiredPools) {
			vkDestroyCommandPool(_device, rp.pool, nullptr);
		}
		_retiredPools.clear();
		if (_timelineSemaphore != VK_NULL_HANDLE) {
			vkDestroySemaphore(_device, _timelineSemaphore, nullptr);
			_timelineSemaphore = VK_NULL_HANDLE;
		}
	}
}

void StagingRingBuffer::Recycle() noexcept {
	if (_timelineSemaphore == VK_NULL_HANDLE) {
		return;
	}

	uint64_t completedValue = 0;
	vkGetSemaphoreCounterValue(_device, _timelineSemaphore, &completedValue);

	// Guard against unsubmitted allocations (timelineValue == 0)
	while (!_activeAllocations.empty() && _activeAllocations.front().timelineValue > 0 &&
		   _activeAllocations.front().timelineValue <= completedValue) {
		_tail = (_activeAllocations.front().offset + _activeAllocations.front().size) % _capacity;
		_activeAllocations.erase(_activeAllocations.begin());
	}

	for (auto it = _retiredPools.begin(); it != _retiredPools.end();) {
		if (it->timelineValue <= completedValue) {
			vkDestroyCommandPool(_device, it->pool, nullptr);
			it = _retiredPools.erase(it);
		} else {
			++it;
		}
	}
}

void StagingRingBuffer::RetirePool(VkCommandPool pool, uint64_t timelineValue) noexcept {
	_retiredPools.push_back({.pool = pool, .timelineValue = timelineValue});
}

auto StagingRingBuffer::Allocate(VkDeviceSize size, VkDeviceSize alignment) noexcept -> Allocation {
	Recycle();

	VkDeviceSize alignedHead = (_head + alignment - 1) & ~(alignment - 1);
	bool wrap = false;

	if (alignedHead + size > _capacity) {
		alignedHead = 0;
		wrap = true;
	}

	while (true) {
		bool hasSpace = false;

		if (_activeAllocations.empty()) {
			hasSpace = true;
		} else if (_tail <= _head) {
			if (wrap) {
				hasSpace = (size < _tail);
			} else {
				hasSpace = true;
			}
		} else {
			if (wrap) {
				hasSpace = false; // Cannot wrap if active region already wraps
			} else {
				hasSpace = (alignedHead + size < _tail);
			}
		}

		if (hasSpace) {
			break;
		}

		if (_activeAllocations.empty()) {
			return {}; // Out of memory boundaries
		}

		uint64_t waitVal = _activeAllocations.front().timelineValue;
		if (waitVal == 0) {
			break; // Avoid waiting on unsubmitted allocations
		}

		VkSemaphoreWaitInfo waitInfo = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
										.pNext = nullptr,
										.flags = 0,
										.semaphoreCount = 1,
										.pSemaphores = &_timelineSemaphore,
										.pValues = &waitVal};
		vkWaitSemaphores(_device, &waitInfo, UINT64_MAX);
		Recycle();
	}

	_head = alignedHead + size;
	_activeAllocations.push_back({.offset = alignedHead, .size = size, .timelineValue = 0});

	return {.buffer = _stagingBuffer.Handle(),
			.offset = alignedHead,
			.mappedData = static_cast<char*>(_mappedPtr) + alignedHead,
			.timelineValue = 0};
}

auto StagingRingBuffer::Submit(VkCommandBuffer cmd) noexcept -> uint64_t {
	_timelineValue++;

	for (auto& alloc : _activeAllocations) {
		if (alloc.timelineValue == 0) {
			alloc.timelineValue = _timelineValue;
		}
	}

	VkCommandBufferSubmitInfo cmdInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
		.pNext = {},
		.commandBuffer = cmd,
		.deviceMask = {},
	};

	VkSemaphoreSubmitInfo signalInfo = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
		.pNext = {},
		.semaphore = _timelineSemaphore,
		.value = _timelineValue,
		.stageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
		.deviceIndex = {},
	};

	VkSubmitInfo2 submit = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
		.pNext = {},
		.flags = {},
		.waitSemaphoreInfoCount = {},
		.pWaitSemaphoreInfos = {},
		.commandBufferInfoCount = 1,
		.pCommandBufferInfos = &cmdInfo,
		.signalSemaphoreInfoCount = 1,
		.pSignalSemaphoreInfos = &signalInfo,
	};

	vkQueueSubmit2(_queue, 1, &submit, VK_NULL_HANDLE);
	return _timelineValue;
}

thread_local DeletionQueue* t_activeDeletionQueue = nullptr;

void DeferVmaDestruction(VmaAllocator allocator, VkBuffer buffer,
						 VmaAllocation allocation) noexcept {
	if (t_activeDeletionQueue != nullptr) {
		t_activeDeletionQueue->EnqueueBuffer(allocator, buffer, allocation);
	}
}

void DeferVmaDestruction(VmaAllocator allocator, VkImage image, VmaAllocation allocation) noexcept {
	if (t_activeDeletionQueue != nullptr) {
		t_activeDeletionQueue->EnqueueImage(allocator, image, allocation);
	}
}

// ============================================================================
// DeletionQueue Implementation
// ============================================================================

DeletionQueue::~DeletionQueue() {
	for (auto& queue : _queues) {
		CleanupQueue(queue);
	}
}

void DeletionQueue::Init(uint32_t doubleBufferCount) noexcept {
	_queues.resize(doubleBufferCount);
	_currentFrameIndex = 0;
}

void DeletionQueue::EnqueueBuffer(VmaAllocator allocator, VkBuffer buffer,
								  VmaAllocation allocation) noexcept {
	_queues[_currentFrameIndex].push_back({.type = DeferredDeletionEntry::Type::Buffer,
										   .allocator = allocator,
										   .allocation = allocation,
										   .buffer = buffer});
}

void DeletionQueue::EnqueueImage(VmaAllocator allocator, VkImage image,
								 VmaAllocation allocation) noexcept {
	_queues[_currentFrameIndex].push_back({.type = DeferredDeletionEntry::Type::Image,
										   .allocator = allocator,
										   .allocation = allocation,
										   .image = image});
}

void DeletionQueue::BeginFrame(uint32_t frameIndex) noexcept {
	_currentFrameIndex = frameIndex % _queues.size();
	// Safe to reclaim memory now! The fence for this frame slot has finished [c].
	CleanupQueue(_queues[_currentFrameIndex]);
}

void DeletionQueue::CleanupQueue(std::vector<DeferredDeletionEntry>& queue) noexcept {
	for (const auto& entry : queue) {
		if (entry.type == DeferredDeletionEntry::Type::Buffer) {
			vmaDestroyBuffer(entry.allocator, entry.buffer, entry.allocation);
		} else {
			vmaDestroyImage(entry.allocator, entry.image, entry.allocation);
		}
	}
	queue.clear();
}

} // namespace ZHLN::Vk
