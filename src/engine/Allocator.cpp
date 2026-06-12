// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#include "Allocator.hpp"

#include <Zahlen/Log.hpp>
#include <new> // Required for std::align_val_t

namespace ZHLN {

LinearAllocator::LinearAllocator(size_t capacity) noexcept : _capacity(capacity) {
	// Allocate over-aligned memory (64-byte for Cache Line / SIMD)
	// operator new with std::nothrow returns nullptr instead of throwing on failure.
	void* raw = ::operator new(capacity, std::align_val_t{64}, std::nothrow);

	if (!raw) [[unlikely]] {
		ZHLN::Panic("FATAL: LinearAllocator failed to allocate {} bytes\n", capacity);
	}

	_buffer = static_cast<std::byte*>(raw);
	MemoryStats::TotalAllocated.fetch_add(_capacity, std::memory_order_relaxed);
}

LinearAllocator::~LinearAllocator() {
	if (_buffer) {
		// We MUST use the matching over-aligned operator delete
		::operator delete(_buffer, std::align_val_t{64});
		MemoryStats::TotalFreed.fetch_add(_capacity, std::memory_order_relaxed);
	}
}

void* LinearAllocator::do_allocate(size_t bytes, size_t alignment) {
	// Calculate current pointer address
	uintptr_t currentAddr = reinterpret_cast<uintptr_t>(_buffer) + _offset;

	// Align the address upward
	uintptr_t alignedAddr = (currentAddr + (alignment - 1)) & ~(alignment - 1);

	// Calculate new offset relative to buffer start
	size_t newOffset = (alignedAddr - reinterpret_cast<uintptr_t>(_buffer)) + bytes;

	if (newOffset > _capacity) [[unlikely]] {
		ZHLN::Log("ERROR: LinearAllocator overflow! (Cap: {}, Requested: {})\n", _capacity, bytes);
		return nullptr; // PMR expects nullptr or throw on failure
	}

	_offset = newOffset;
	return reinterpret_cast<void*>(alignedAddr);
}

void LinearAllocator::do_deallocate(void* /*p*/, size_t /*bytes*/, size_t /*alignment*/) {
	// No-op: Linear allocators reset all at once
}

bool LinearAllocator::do_is_equal(const std::pmr::memory_resource& other) const noexcept {
	return this == &other;
}

} // namespace ZHLN

void* operator new(std::size_t size) {
	ZHLN::MemoryStats::TotalAllocated.fetch_add(size, std::memory_order_relaxed);
	return std::malloc(size);
}

void operator delete(void* p, std::size_t size) noexcept {
	ZHLN::MemoryStats::TotalFreed.fetch_add(size, std::memory_order_relaxed);
	std::free(p);
}

void operator delete(void* p) noexcept {
	std::free(p);
}
