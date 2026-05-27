#pragma once
#include "RenderCore.h"
#include "RenderCore.hpp"

#include <cstring>
#include <utility>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>

namespace ZHLN::Vk {

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
		// Explicitly map Vulkan functions to VMA.
		// This prevents VMA from trying to "guess" or "dynamically load" them.
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
			// Even in Vulkan 1.3, VMA names these fields with KHR for compatibility
			.vkGetBufferMemoryRequirements2KHR = &vkGetBufferMemoryRequirements2,
			.vkGetImageMemoryRequirements2KHR = &vkGetImageMemoryRequirements2,
			.vkBindBufferMemory2KHR = &vkBindBufferMemory2,
			.vkBindImageMemory2KHR = &vkBindImageMemory2,
			.vkGetPhysicalDeviceMemoryProperties2KHR = &vkGetPhysicalDeviceMemoryProperties2,
			.vkGetDeviceBufferMemoryRequirements = &vkGetDeviceBufferMemoryRequirements,
			.vkGetDeviceImageMemoryRequirements = &vkGetDeviceImageMemoryRequirements,
			.vkGetMemoryWin32HandleKHR = nullptr,
			// Buffer Device Address is NOT a field in the struct;
			// VMA loads it via GetDeviceProcAddr internally.
		};

		const VmaAllocatorCreateInfo info = {
			.flags = 0,
			.physicalDevice = physical,
			.device = device,
			.preferredLargeHeapBlockSize = 0, // 0 = default (256 MiB)
			.pAllocationCallbacks = nullptr,
			.pDeviceMemoryCallbacks = nullptr,
			.pHeapSizeLimit = nullptr,
			.pVulkanFunctions = &vfuncs, // VMA will fetch functions internally
			.instance = instance,
			.vulkanApiVersion = VK_API_VERSION_1_3,
#if VMA_EXTERNAL_MEMORY
			.pTypeExternalMemoryHandleTypes = nullptr
#endif
		};

		return vmaCreateAllocator(&info, &_handle) == VK_SUCCESS;
	}

	[[nodiscard]] auto Init(const Context& ctx) noexcept -> bool {
		return Init(ctx.Instance(), ctx.Physical(), ctx.Device());
	}

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

		// Fully explicit VkBufferCreateInfo (Vulkan 1.3)
		const VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
												.pNext = nullptr,
												.flags = 0,
												.size = size,
												.usage = usage,
												.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
												.queueFamilyIndexCount = 0,
												.pQueueFamilyIndices = nullptr};

		// Fully explicit VmaAllocationCreateInfo (VMA 3.x)
		const VmaAllocationCreateInfo alloc_info = {
			.flags = 0,
			.usage = mem_usage,
			.requiredFlags = 0,
			.preferredFlags = 0,
			.memoryTypeBits = 0,
			.pool = nullptr,
			.pUserData = nullptr,
			.priority = 0.0F,
			.minAlignment = 0 // 0 = Use default Vulkan requirements
		};

		if (vmaCreateBuffer(allocator, &buffer_info, &alloc_info, &b._handle, &b._allocation,
							&b._info) != VK_SUCCESS) {
			return {};
		}

		return b;
	}

	// --- Mapped Region ---

	struct MappedRegion {
		MappedRegion(VmaAllocator alloc, VmaAllocation allocation, void* ptr) noexcept
			: data(ptr), _alloc(alloc), _allocation(allocation) {}

		~MappedRegion() noexcept {
			if (_alloc != VK_NULL_HANDLE) {
				// Dynamic memory flush guarantees GPU coherence on discrete NVIDIA/AMD cards
				vmaFlushAllocation(_alloc, _allocation, 0, VK_WHOLE_SIZE);
				vmaUnmapMemory(_alloc, _allocation);
			}
		}

		// Disable copying
		MappedRegion(const MappedRegion&) = delete;
		auto operator=(const MappedRegion&) -> MappedRegion& = delete;

		// Enable moving
		MappedRegion(MappedRegion&& other) noexcept
			: data(other.data), _alloc(other._alloc), _allocation(other._allocation) {
			// Nullify the source so the destructor doesn't unmap the memory we just moved
			other.data = nullptr;
			other._alloc = VK_NULL_HANDLE;
		}

		auto operator=(MappedRegion&& other) noexcept -> MappedRegion& {
			if (this != &other) {
				// Clean up our current resource before taking the new one
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

// ============================================================================
// Staging upload helper
// Records a CPU->GPU copy into cmd. Staging buffer lifetime must outlive
// queue execution — caller must not destroy before submit completes.
// ============================================================================

[[nodiscard]]
inline auto UploadToBuffer(VmaAllocator allocator, VkCommandBuffer cmd, Buffer& dst,
						   const void* data, size_t size) noexcept -> Buffer {
	// 1. Create staging buffer
	Buffer staging = Buffer::Create(allocator, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
									VMA_MEMORY_USAGE_CPU_ONLY);
	if (!staging.Valid()) {
		return {};
	}

	// 2. Map and copy data
	{
		auto mapped = staging.Map();
		if (mapped.data != nullptr) {
			memcpy(mapped.data, data, size);
		}
	}

	// 3. Define the copy with explicit offsets
	const ZHLN_BufferCopyDesc copy = {
		.src = staging.Handle(),
		.dst = dst.Handle(),
		.size = static_cast<VkDeviceSize>(size),
		.src_offset = 0, // Explicitly start at the beginning of staging
		.dst_offset = 0	 // Explicitly start at the beginning of dst
	};

	// 4. Record the command (Dumb C Execution)
	ZHLN_CmdCopyBuffer(cmd, &copy);

	// 5. Return staging buffer.
	// This is vital: the GPU hasn't executed the copy yet.
	// The caller must keep 'staging' alive until the command buffer submit fence is signaled.
	return staging;
}

// ============================================================================
// Image RAII
// ============================================================================

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

	// Move only
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
