// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/Allocator.hpp

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/render/Rendering.hpp> before including any other render headers."
#endif

namespace ZHLN::Vk {

class Context; // Forward declaration

// ============================================================================
// Allocator RAII
// ============================================================================

class Allocator {
  public:
	Allocator() = default;
	~Allocator() noexcept;

	Allocator(const Allocator&) = delete;
	auto operator=(const Allocator&) -> Allocator& = delete;

	Allocator(Allocator&& other) noexcept;
	auto operator=(Allocator&& other) noexcept -> Allocator&;

	[[nodiscard]] auto Init(VkInstance instance, VkPhysicalDevice physical,
							VkDevice device) noexcept -> bool;

	[[nodiscard]] auto Init(const Context& ctx) noexcept -> bool;

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
	~Buffer() noexcept = default;

	Buffer(const Buffer&) = delete;
	auto operator=(const Buffer&) -> Buffer& = delete;

	Buffer(Buffer&& other) noexcept = default;
	auto operator=(Buffer&& other) noexcept -> Buffer& = default;

	[[nodiscard]] static auto Create(VmaAllocator allocator, size_t size, VkBufferUsageFlags usage,
									 VmaMemoryUsage mem_usage) noexcept -> Buffer;

	struct MappedRegion {
		MappedRegion() = default;
		MappedRegion(VmaAllocator alloc, VmaAllocation allocation, void* ptr) noexcept;
		~MappedRegion() noexcept = default;

		MappedRegion(const MappedRegion&) = delete;
		auto operator=(const MappedRegion&) -> MappedRegion& = delete;

		MappedRegion(MappedRegion&& other) noexcept;
		auto operator=(MappedRegion&& other) noexcept -> MappedRegion&;

		template <typename T> auto As() noexcept -> T* { return static_cast<T*>(data); }

		void* data = nullptr;

	  private:
		VmaHandle<void*, VmaUnmapDeleter> _handle;
	};

	[[nodiscard]] auto Map() noexcept -> MappedRegion;

	[[nodiscard]] auto Handle() const noexcept -> VkBuffer { return _handle.Get(); }
	[[nodiscard]] auto Size() const noexcept -> size_t { return _info.size; }
	[[nodiscard]] auto Valid() const noexcept -> bool { return _handle.Valid(); }
	explicit operator bool() const noexcept { return Valid(); }

  private:
	VmaHandle<VkBuffer, vmaDestroyBuffer> _handle;
	VmaAllocationInfo _info = {};
};

[[nodiscard]] auto UploadToBuffer(VmaAllocator allocator, VkCommandBuffer cmd, Buffer& dst,
								  const void* data, size_t size) noexcept -> Buffer;

// ============================================================================
// Image RAII
// ============================================================================

class Image {
  public:
	Image() = default;
	Image(const Image&) = delete;
	auto operator=(const Image&) -> Image& = delete;
	~Image() noexcept = default;

	Image(Image&& other) noexcept = default;
	auto operator=(Image&& other) noexcept -> Image& = default;

	static auto Create(VmaAllocator allocator, const VkImageCreateInfo& info,
					   VmaMemoryUsage mem_usage) -> Image;

	[[nodiscard]] auto Valid() const noexcept -> bool { return _handle.Valid(); }
	explicit operator bool() const noexcept { return Valid(); }

	[[nodiscard]] auto Handle() const -> VkImage { return _handle.Get(); }

  private:
	VmaHandle<VkImage, vmaDestroyImage> _handle;
};

template <typename T = uint32_t>
void FillBuffer(VkCommandBuffer cmd, const Buffer& buffer, VkDeviceSize offset = 0, T data = 0) {
	static_assert(sizeof(T) % 4 == 0, "Type must be 4-byte aligned for vkCmdFillBuffer");

	vkCmdFillBuffer(cmd, buffer.Handle(), offset, sizeof(T), data);
}

// ============================================================================
// Staging Ring Buffer (Timeline Semaphore Synchronized)
// ============================================================================

class StagingRingBuffer {
  public:
	struct Allocation {
		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceSize offset = 0;
		void* mappedData = nullptr;
		uint64_t timelineValue = 0;
	};

	StagingRingBuffer() = default;
	~StagingRingBuffer() noexcept { Cleanup(); }

	StagingRingBuffer(const StagingRingBuffer&) = delete;
	auto operator=(const StagingRingBuffer&) -> StagingRingBuffer& = delete;

	StagingRingBuffer(StagingRingBuffer&& other) noexcept;
	auto operator=(StagingRingBuffer&& other) noexcept -> StagingRingBuffer&;

	[[nodiscard]] auto Init(VmaAllocator allocator, VkDevice device, VkQueue queue,
							uint32_t queueFamily, VkDeviceSize capacity) noexcept -> bool;
	void Cleanup() noexcept;

	[[nodiscard]] auto Allocate(VkDeviceSize size, VkDeviceSize alignment = 4) noexcept
		-> Allocation;
	auto Submit(VkCommandBuffer cmd) noexcept -> uint64_t;
	void Recycle() noexcept;

	void RetirePool(VkCommandPool pool, uint64_t timelineValue) noexcept;

	[[nodiscard]] auto GetSemaphore() const noexcept -> VkSemaphore { return _timelineSemaphore; }
	[[nodiscard]] auto GetCurrentValue() const noexcept -> uint64_t { return _timelineValue; }
	[[nodiscard]] auto GetQueueFamily() const noexcept -> uint32_t { return _queueFamily; }
	[[nodiscard]] auto Valid() const noexcept -> bool {
		return _timelineSemaphore != VK_NULL_HANDLE;
	}

  private:
	VmaAllocator _allocator = nullptr;
	VkDevice _device = VK_NULL_HANDLE;
	VkQueue _queue = VK_NULL_HANDLE;
	uint32_t _queueFamily = 0xFFFFFFFF;

	Buffer _stagingBuffer;
	Buffer::MappedRegion _mappedRegion;
	void* _mappedPtr = nullptr;
	VkDeviceSize _capacity = 0;

	VkDeviceSize _head = 0;
	VkDeviceSize _tail = 0;

	VkSemaphore _timelineSemaphore = VK_NULL_HANDLE;
	uint64_t _timelineValue = 0;

	struct ActiveAllocation {
		VkDeviceSize offset;
		VkDeviceSize size;
		uint64_t timelineValue;
	};
	std::vector<ActiveAllocation> _activeAllocations;

	struct RetiredPool {
		VkCommandPool pool;
		uint64_t timelineValue;
	};
	std::vector<RetiredPool> _retiredPools;
};

// ============================================================================
// Immediate Command RAII Wrapper (Staging/Initial Uploads)
// ============================================================================

class ImmediateCommand {
  public:
	ImmediateCommand(VkDevice dev, StagingRingBuffer& ringBuffer) noexcept;
	~ImmediateCommand() noexcept;

	[[nodiscard]] auto AllocateStaging(VkDeviceSize size, VkDeviceSize alignment = 4) noexcept
		-> StagingRingBuffer::Allocation;

	operator VkCommandBuffer() const noexcept;

	ImmediateCommand(const ImmediateCommand&) = delete;
	auto operator=(const ImmediateCommand&) -> ImmediateCommand& = delete;
	ImmediateCommand(ImmediateCommand&&) = delete;
	auto operator=(ImmediateCommand&&) -> ImmediateCommand& = delete;

  private:
	struct Impl;
	std::unique_ptr<Impl> _impl;
};

} // namespace ZHLN::Vk
