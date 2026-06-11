#pragma once
#include "RenderCore.h"

#include <cstring>
#include <utility>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

namespace ZHLN::Vk {

class Context; // Forward declaration [3]

// ============================================================================
// Allocator RAII
// ============================================================================

class Allocator {
  public:
	Allocator() = default;
	~Allocator() noexcept {
		if (_handle != nullptr) {
			vmaDestroyAllocator(_handle);
		}
	}

	Allocator(const Allocator&) = delete;
	auto operator=(const Allocator&) -> Allocator& = delete;

	Allocator(Allocator&& other) noexcept : _handle(std::exchange(other._handle, nullptr)) {}

	auto operator=(Allocator&& other) noexcept -> Allocator& {
		if (this != &other) {
			if (_handle != nullptr) {
				vmaDestroyAllocator(_handle);
			}
			_handle = std::exchange(other._handle, nullptr);
		}
		return *this;
	}

	[[nodiscard]] auto Init(VkInstance instance, VkPhysicalDevice physical,
							VkDevice device) noexcept -> bool {
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
			.flags = 0,
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

	[[nodiscard]] auto Init(const Context& ctx) noexcept -> bool; // Deferred signature [3]

	[[nodiscard]] auto Get() const noexcept -> VmaAllocator { return _handle; }
	[[nodiscard]] auto Valid() const noexcept -> bool { return _handle != nullptr; }
	explicit operator bool() const noexcept { return Valid(); }

  private:
	VmaAllocator _handle = nullptr;
};

// ============================================================================
// Buffer RAII
// ============================================================================

class Buffer {
  public:
	Buffer() = default;

	~Buffer() noexcept {
		if (_handle != VK_NULL_HANDLE) {
			vmaDestroyBuffer(_allocator, _handle, _allocation);
		}
	}

	Buffer(const Buffer&) = delete;
	auto operator=(const Buffer&) -> Buffer& = delete;

	Buffer(Buffer&& other) noexcept
		: _allocator(other._allocator), _handle(std::exchange(other._handle, VK_NULL_HANDLE)),
		  _allocation(std::exchange(other._allocation, nullptr)), _info(other._info) {}

	auto operator=(Buffer&& other) noexcept -> Buffer& {
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

	[[nodiscard]] static auto Create(VmaAllocator allocator, size_t size, VkBufferUsageFlags usage,
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

	struct MappedRegion {
		MappedRegion(VmaAllocator alloc, VmaAllocation allocation, void* ptr) noexcept
			: data(ptr), _alloc(alloc), _allocation(allocation) {}

		~MappedRegion() noexcept {
			if (_alloc != VK_NULL_HANDLE) {
				vmaFlushAllocation(_alloc, _allocation, 0, VK_WHOLE_SIZE);
				vmaUnmapMemory(_alloc, _allocation);
			}
		}

		MappedRegion(const MappedRegion&) = delete;
		auto operator=(const MappedRegion&) -> MappedRegion& = delete;

		MappedRegion(MappedRegion&& other) noexcept
			: data(other.data), _alloc(other._alloc), _allocation(other._allocation) {
			other.data = nullptr;
			other._alloc = VK_NULL_HANDLE;
		}

		auto operator=(MappedRegion&& other) noexcept -> MappedRegion& {
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

		template <typename T> auto As() noexcept -> T* { return static_cast<T*>(data); }

		void* data = nullptr;

	  private:
		VmaAllocator _alloc = VK_NULL_HANDLE;
		VmaAllocation _allocation = VK_NULL_HANDLE;
	};

	[[nodiscard]] auto Map() noexcept -> MappedRegion {
		void* ptr = nullptr;
		vmaMapMemory(_allocator, _allocation, &ptr);
		return {_allocator, _allocation, ptr};
	}

	[[nodiscard]] auto Handle() const noexcept -> VkBuffer { return _handle; }
	[[nodiscard]] auto Size() const noexcept -> size_t { return _info.size; }
	[[nodiscard]] auto Valid() const noexcept -> bool { return _handle != VK_NULL_HANDLE; }
	explicit operator bool() const noexcept { return Valid(); }

  private:
	VmaAllocator _allocator = nullptr;
	VkBuffer _handle = VK_NULL_HANDLE;
	VmaAllocation _allocation = nullptr;
	VmaAllocationInfo _info = {};
};

[[nodiscard]]
inline auto UploadToBuffer(VmaAllocator allocator, VkCommandBuffer cmd, Buffer& dst,
						   const void* data, size_t size) noexcept -> Buffer {
	Buffer staging = Buffer::Create(allocator, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
									VMA_MEMORY_USAGE_CPU_ONLY);
	if (!staging.Valid()) {
		return {};
	}

	{
		auto mapped = staging.Map();
		if (mapped.data != nullptr) {
			memcpy(mapped.data, data, size);
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

class Image {
  public:
	Image() = default;
	Image(const Image&) = delete;
	auto operator=(const Image&) -> Image& = delete;
	~Image() {
		if (_handle != VK_NULL_HANDLE) {
			vmaDestroyImage(_allocator, _handle, _allocation);
		}
	}

	Image(Image&& other) noexcept
		: _allocator(other._allocator), _handle(std::exchange(other._handle, nullptr)),
		  _allocation(std::exchange(other._allocation, nullptr)) {}
	auto operator=(Image&& other) noexcept -> Image& {
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

	static auto Create(VmaAllocator allocator, const VkImageCreateInfo& info,
					   VmaMemoryUsage mem_usage) -> Image {
		Image img;
		img._allocator = allocator;
		VmaAllocationCreateInfo alloc_info = {};
		alloc_info.usage = mem_usage;
		if (vmaCreateImage(allocator, &info, &alloc_info, &img._handle, &img._allocation,
						   nullptr) != VK_SUCCESS) {
			return {};
		}
		return img;
	}

	[[nodiscard]] auto Valid() const noexcept -> bool { return _handle != VK_NULL_HANDLE; }
	explicit operator bool() const noexcept { return Valid(); }

	[[nodiscard]] auto Handle() const -> VkImage { return _handle; }

  private:
	VmaAllocator _allocator = nullptr;
	VkImage _handle = VK_NULL_HANDLE;
	VmaAllocation _allocation = nullptr;
};

} // namespace ZHLN::Vk
