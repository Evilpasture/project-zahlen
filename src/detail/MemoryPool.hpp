// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#include <algorithm>
#include <array>
#include <detail/Atomic.hpp>
#include <new>
#include <utility>

#if defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>
#endif

namespace ZHLN {

/**
 * @brief Thread-safe, growable Fixed-Size Object Pool.
 * Combines free-list memory reuse with a lightweight spinlock to achieve
 * O(1) allocation/deallocation completely immune to the ABA problem. [6]
 */
template <typename T, size_t BlockCount = 1024> class ObjectPool {
	static_assert(BlockCount > 0, "BlockCount must be greater than 0");

	// Ensure the object size is at least as large as a pointer so we can
	// safely overlay the free-list's Node pointers in unallocated slots. [6]
	static constexpr size_t ObjectSize = std::max(sizeof(T), sizeof(void*));
	static constexpr size_t Alignment = std::max(alignof(T), alignof(void*));

  public:
	struct Spinlock {
		ZHLN::Atomic<bool> locked{false};

		void lock() noexcept {
			bool expected = false;
			while (!locked.compare_exchange_weak(expected, true, std::memory_order_acquire,
												 std::memory_order_relaxed)) {
				expected = false;
#if defined(__x86_64__) || defined(_M_X64)
				_mm_pause();
#elif defined(__aarch64__)
				__asm__ __volatile__("yield" ::: "memory");
#else
				std::this_thread::yield();
#endif
			}
		}

		void unlock() noexcept { locked.store(false, std::memory_order_release); }
	};

  private:
	struct Node {
		Node* next;
	};

	struct Chunk {
		alignas(Alignment) std::array<std::byte, ObjectSize * BlockCount> storage;
		Chunk* next = nullptr;
	};

  public:
	ObjectPool() = default;

	~ObjectPool() noexcept {
		// Reclaims all raw memory chunks. Note that this allocator does not call
		// destructors of active objects. Users must call Destroy() first. [6]
		Chunk* curr = _chunks;
		while (curr != nullptr) {
			Chunk* next = curr->next;
			curr->~Chunk();
			::operator delete(curr, std::align_val_t{alignof(Chunk)});
			curr = next;
		}
	}

	// Non-copyable to prevent raw pointer aliasing
	ObjectPool(const ObjectPool&) = delete;
	auto operator=(const ObjectPool&) -> ObjectPool& = delete;

	// Move semantics
	ObjectPool(ObjectPool&& other) noexcept
		: _freeList(std::exchange(other._freeList, nullptr)),
		  _chunks(std::exchange(other._chunks, nullptr)) {}

	auto operator=(ObjectPool&& other) noexcept -> ObjectPool& {
		if (this != &other) {
			this->~ObjectPool();
			_freeList = std::exchange(other._freeList, nullptr);
			_chunks = std::exchange(other._chunks, nullptr);
		}
		return *this;
	}

	/**
	 * @brief Allocation-free Object Factory.
	 * Allocates raw memory from the pool and constructs the object inline. [6]
	 */
	template <typename... Args> [[nodiscard]] T* Create(Args&&... args) {
		void* mem = Allocate();
		return ::new (mem) T(std::forward<Args>(args)...);
	}

	/**
	 * @brief Type-safe object destructor.
	 * Invokes the object's destructor and returns its memory slot back to the pool. [6]
	 */
	void Destroy(T* ptr) noexcept {
		if (ptr == nullptr) {
			return;
		}
		ptr->~T();
		Deallocate(ptr);
	}

	/**
	 * @brief Allocates an uninitialized block of memory of size T.
	 */
	[[nodiscard]] void* Allocate() {
		_lock.lock();
		if (_freeList == nullptr) [[unlikely]] {
			AllocateChunk();
		}

		Node* node = _freeList;
		_freeList = _freeList->next;
		_lock.unlock();

		return reinterpret_cast<void*>(node);
	}

	/**
	 * @brief Returns an uninitialized memory block back to the pool's free list.
	 */
	void Deallocate(void* ptr) noexcept {
		if (ptr == nullptr) {
			return;
		}

		auto* node = reinterpret_cast<Node*>(ptr);
		_lock.lock();
		node->next = _freeList;
		_freeList = node;
		_lock.unlock();
	}

  private:
	void AllocateChunk() {
		// Allocate aligned memory for the Chunk structure
		void* mem = ::operator new(sizeof(Chunk), std::align_val_t{alignof(Chunk)});
		auto* chunk = ::new (mem) Chunk();

		chunk->next = _chunks;
		_chunks = chunk;

		// Link all slots in the new chunk together and attach them to the free list [6]
		std::byte* start = chunk->storage.data();
		for (size_t i = 0; i < BlockCount; ++i) {
			auto* node = reinterpret_cast<Node*>(start + (i * ObjectSize));
			node->next = _freeList;
			_freeList = node;
		}
	}

	Spinlock _lock;
	Node* _freeList = nullptr;
	Chunk* _chunks = nullptr;
};

} // namespace ZHLN
