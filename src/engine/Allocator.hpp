// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#include <cstddef>
#include <detail/Atomic.hpp>
#include <memory_resource>

namespace ZHLN {

/**
 * @brief Helper to align a value upward to the nearest power of two.
 */
[[nodiscard]] constexpr size_t AlignUp(size_t size, size_t alignment) noexcept {
	return (size + (alignment - 1)) & ~(alignment - 1);
}

/**
 * @brief High-performance Linear (Arena) Allocator.
 *
 * Perfect for per-frame temporary data. Compatible with std::pmr containers.
 * Usage: std::pmr::vector<int> myVec(&myArena);
 */
class LinearAllocator : public std::pmr::memory_resource {
  public:
	LinearAllocator(size_t capacity) noexcept;
	~LinearAllocator() override;

	// Non-copyable
	LinearAllocator(const LinearAllocator&) = delete;
	LinearAllocator& operator=(const LinearAllocator&) = delete;

	/**
	 * @brief Resets the offset to zero. Does not free the backing memory.
	 * Call this at the start/end of your engine frame.
	 */
	void Reset() noexcept { _offset = 0; }

	[[nodiscard]] size_t GetCapacity() const noexcept { return _capacity; }
	[[nodiscard]] size_t GetAllocated() const noexcept { return _offset; }

  protected:
	void* do_allocate(size_t bytes, size_t alignment) override;
	void do_deallocate(void* p, size_t bytes, size_t alignment) override;
	bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override;

  private:
	std::byte* _buffer = nullptr;
	size_t _capacity = 0;
	size_t _offset = 0;
};

/**
 * @brief Simple tracking proxy for global memory statistics.
 */
struct MemoryStats {
	static inline ZHLN::Atomic<size_t> TotalAllocated{0};
	static inline ZHLN::Atomic<size_t> TotalFreed{0};

	static size_t GetCurrentUsage() { return TotalAllocated.load() - TotalFreed.load(); }
};

} // namespace ZHLN