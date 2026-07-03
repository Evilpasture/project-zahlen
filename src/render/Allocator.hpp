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
	~Buffer() noexcept;

	Buffer(const Buffer&) = delete;
	auto operator=(const Buffer&) -> Buffer& = delete;

	Buffer(Buffer&& other) noexcept;
	auto operator=(Buffer&& other) noexcept -> Buffer&;

	[[nodiscard]] static auto Create(VmaAllocator allocator, size_t size, VkBufferUsageFlags usage,
									 VmaMemoryUsage mem_usage) noexcept -> Buffer;

	struct MappedRegion {
		MappedRegion(VmaAllocator alloc, VmaAllocation allocation, void* ptr) noexcept;
		~MappedRegion() noexcept;

		MappedRegion(const MappedRegion&) = delete;
		auto operator=(const MappedRegion&) -> MappedRegion& = delete;

		MappedRegion(MappedRegion&& other) noexcept;
		auto operator=(MappedRegion&& other) noexcept -> MappedRegion&;

		template <typename T> auto As() noexcept -> T* { return static_cast<T*>(data); }

		void* data = nullptr;

	  private:
		VmaAllocator _alloc = VK_NULL_HANDLE;
		VmaAllocation _allocation = VK_NULL_HANDLE;
	};

	[[nodiscard]] auto Map() noexcept -> MappedRegion;

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
	~Image();

	Image(Image&& other) noexcept;
	auto operator=(Image&& other) noexcept -> Image&;

	static auto Create(VmaAllocator allocator, const VkImageCreateInfo& info,
					   VmaMemoryUsage mem_usage) -> Image;

	[[nodiscard]] auto Valid() const noexcept -> bool { return _handle != VK_NULL_HANDLE; }
	explicit operator bool() const noexcept { return Valid(); }

	[[nodiscard]] auto Handle() const -> VkImage { return _handle; }

  private:
	VmaAllocator _allocator = nullptr;
	VkImage _handle = VK_NULL_HANDLE;
	VmaAllocation _allocation = nullptr;
};

template <typename T = uint32_t>
void FillBuffer(VkCommandBuffer cmd, const Buffer& buffer, VkDeviceSize offset = 0, T data = 0) {
	static_assert(sizeof(T) % 4 == 0, "Type must be 4-byte aligned for vkCmdFillBuffer");

	vkCmdFillBuffer(cmd, buffer.Handle(), offset, sizeof(T), data);
}

// ============================================================================
// Immediate Command RAII Wrapper (Staging/Initial Uploads)
// ============================================================================

class ImmediateCommand {
  public:
	ImmediateCommand(VkDevice dev, VkQueue q, uint32_t queueFamily) noexcept;
	~ImmediateCommand() noexcept;

	void KeepAlive(Buffer&& buffer);

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
