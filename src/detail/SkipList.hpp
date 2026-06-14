// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Atomic.hpp"
#include "ControlFlow.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <new>
#include <threading/Mutex.hpp>
#include <utility>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>
#endif

namespace ZHLN {

template <typename Key, typename Value, typename Compare = std::less<Key>> class SkipList {
  public:
	static constexpr uint32_t MAX_LEVEL = 32;

	struct Spinlock {
		ZHLN::Atomic<bool> locked;

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

	struct SkipNode {
		Key key;
		Value value;
		uint32_t height;
		ZHLN::Atomic<bool> deleted;
		Spinlock lock;

		// Accessors to cast tail-allocated memory into atomic pointers safely
		[[nodiscard]] ZHLN::Atomic<SkipNode*>* GetForward() noexcept {
			return reinterpret_cast<ZHLN::Atomic<SkipNode*>*>(this + 1);
		}

		[[nodiscard]] const ZHLN::Atomic<SkipNode*>* GetForward() const noexcept {
			return reinterpret_cast<const ZHLN::Atomic<SkipNode*>*>(this + 1);
		}
	};

	explicit SkipList(Compare comp = Compare())
		: _head(CreateNode(Key{}, Value{}, MAX_LEVEL)), _compare(comp) {

		_level.store(1, std::memory_order_relaxed);
	}

	~SkipList() {
		Clear();
		DestroyNode(_head);
	}

	// Non-copyable to prevent pointer aliasing and double frees
	SkipList(const SkipList&) = delete;
	auto operator=(const SkipList&) -> SkipList& = delete;

	// Move semantics
	SkipList(SkipList&& other) noexcept
		: _head(std::exchange(other._head, nullptr)), _compare(std::move(other._compare)) {
		_level.store(other._level.load(std::memory_order_relaxed), std::memory_order_relaxed);
		_size.store(other._size.load(std::memory_order_relaxed), std::memory_order_relaxed);
	}

	auto operator=(SkipList&& other) noexcept -> SkipList& {
		if (this != &other) {
			Clear();
			if (_head) {
				DestroyNode(_head);
			}
			_head = std::exchange(other._head, nullptr);
			_compare = std::move(other._compare);
			_level.store(other._level.load(std::memory_order_relaxed), std::memory_order_relaxed);
			_size.store(other._size.load(std::memory_order_relaxed), std::memory_order_relaxed);
		}
		return *this;
	}

	/**
	 * @brief Lock-free Read path.
	 * 100% safe to call concurrently with Insert/Erase.
	 */
	[[nodiscard]] const Value* Find(const Key& key) const noexcept {
		const_cast<SkipList*>(this)->EnterReader();

		const SkipNode* curr = _head;
		uint32_t lvl = _level.load(std::memory_order_acquire);

		for (int i = static_cast<int>(lvl) - 1; i >= 0; --i) {
			const ZHLN::Atomic<SkipNode*>* forward = curr->GetForward();
			SkipNode* next = forward[i].load(std::memory_order_acquire);
			while (next && _compare(next->key, key)) {
				curr = next;
				next = curr->GetForward()[i].load(std::memory_order_acquire);
			}
		}

		const ZHLN::Atomic<SkipNode*>* forward = curr->GetForward();
		SkipNode* next = forward[0].load(std::memory_order_acquire);
		const Value* result = nullptr;

		if (next && !next->deleted.load(std::memory_order_acquire) && !_compare(next->key, key) &&
			!_compare(key, next->key)) {
			result = &next->value;
		}

		const_cast<SkipList*>(this)->ExitReader();
		return result;
	}

	/**
	 * @brief Thread-safe lock-free reader traversal.
	 * Safe to call concurrently with Insert/Erase.
	 */
	template <typename Func> void Iterate(Func&& func) const {
		// Enter QSR reader phase
		const_cast<SkipList*>(this)->EnterReader();

		const SkipNode* curr = _head->GetForward()[0].load(std::memory_order_acquire);
		while (curr != nullptr) {
			// Only yield the node if it has not been logically deleted
			if (!curr->deleted.load(std::memory_order_acquire)) {
				func(curr->key, curr->value);
			}
			curr = curr->GetForward()[0].load(std::memory_order_acquire);
		}

		// Exit QSR reader phase
		const_cast<SkipList*>(this)->ExitReader();
	}

	/**
	 * @brief Fully concurrent Multi-Writer insertion using optimistic fine-grained locks.
	 */
	void Insert(const Key& key, const Value& value) {
		uint32_t height = RandomHeight();
		SkipNode* newNode = nullptr;

		for (;;) {
			std::array<SkipNode*, MAX_LEVEL> predecessors{};
			std::array<SkipNode*, MAX_LEVEL> successors{};

			SkipNode* curr = _head;

			// Traversing from MAX_LEVEL - 1 ensures we do not miss concurrent updates
			for (int i = static_cast<int>(MAX_LEVEL) - 1; i >= 0; --i) {
				SkipNode* next = curr->GetForward()[i].load(std::memory_order_relaxed);
				while (next && _compare(next->key, key)) {
					curr = next;
					next = curr->GetForward()[i].load(std::memory_order_relaxed);
				}
				predecessors[i] = curr;
				successors[i] = next;
			}

			// Check if key already exists
			SkipNode* found = successors[0];
			if (found && !_compare(found->key, key) && !_compare(key, found->key)) {
				if (found->deleted.load(std::memory_order_acquire)) {
					continue; // Being deleted by another thread, spin-retry
				}
				found->value = value; // Overwrite
				if (newNode) {
					DestroyNode(newNode);
				}
				return;
			}

			// Lock all predecessors up to height bottom-to-top to avoid deadlocks
			std::vector<SkipNode*> locked;
			locked.reserve(height);
			bool valid = true;

			for (uint32_t i = 0; i < height; ++i) {
				SkipNode* pred = predecessors[i];
				if (std::find(locked.begin(), locked.end(), pred) == locked.end()) {
					pred->lock.lock();
					locked.push_back(pred);
				}

				// Validate: predecessor must not be deleted and must still point to our successor

				if (pred->deleted.load(std::memory_order_acquire) ||
					pred->GetForward()[i].load(std::memory_order_relaxed) != successors[i]) {
					valid = false;
					break;
				}
			}

			if (!valid) {
				for (auto* p : locked) {
					p->lock.unlock();
				}
				continue; // validation failed, spin-retry
			}

			if (newNode == nullptr) {
				newNode = CreateNode(key, value, height);
			}

			// Link new node's forward pointers
			for (uint32_t i = 0; i < height; ++i) {
				newNode->GetForward()[i].store(successors[i], std::memory_order_relaxed);
				predecessors[i]->GetForward()[i].store(newNode, std::memory_order_release);
			}

			uint32_t curLvl = _level.load(std::memory_order_relaxed);
			if (height > curLvl) {
				_level.store(height, std::memory_order_release);
			}

			for (auto* p : locked) {
				p->lock.unlock();
			}
			_size.fetch_add(1, std::memory_order_relaxed);
			return;
		}
	}

	/**
	 * @brief Fully concurrent Multi-Writer erasure using optimistic fine-grained locks.
	 */
	bool Erase(const Key& key) {
		for (;;) {
			std::array<SkipNode*, MAX_LEVEL> predecessors{};
			std::array<SkipNode*, MAX_LEVEL> successors{};

			SkipNode* curr = _head;
			for (int i = static_cast<int>(MAX_LEVEL) - 1; i >= 0; --i) {
				SkipNode* next = curr->GetForward()[i].load(std::memory_order_relaxed);
				while (next && _compare(next->key, key)) {
					curr = next;
					next = curr->GetForward()[i].load(std::memory_order_relaxed);
				}
				predecessors[i] = curr;
				successors[i] = next;
			}

			SkipNode* doomed = successors[0];
			if ((doomed == nullptr) || _compare(doomed->key, key) || _compare(key, doomed->key)) {
				return false; // Key not found
			}

			if (doomed->deleted.load(std::memory_order_acquire)) {
				continue; // Already unlinking on another thread, spin-retry [1]
			}

			std::vector<SkipNode*> locked;
			locked.reserve(doomed->height + 1);
			doomed->lock.lock();
			locked.push_back(doomed);

			if (doomed->deleted.load(std::memory_order_relaxed)) {
				doomed->lock.unlock();
				continue;
			}

			bool valid = true;
			for (uint32_t i = 0; i < doomed->height; ++i) {
				SkipNode* pred = predecessors[i];
				if (std::find(locked.begin(), locked.end(), pred) == locked.end()) {
					pred->lock.lock();
					locked.push_back(pred);
				}

				if (pred->deleted.load(std::memory_order_acquire) ||
					pred->GetForward()[i].load(std::memory_order_relaxed) != doomed) {
					valid = false;
					break;
				}
			}

			if (!valid) {
				for (auto* p : locked) {
					p->lock.unlock();
				}
				continue;
			}

			// Mark logically deleted so concurrent readers know the node is dead [1]
			doomed->deleted.store(true, std::memory_order_release);

			// Unlink from predecessors
			for (uint32_t i = 0; i < doomed->height; ++i) {
				SkipNode* succ = doomed->GetForward()[i].load(std::memory_order_relaxed);
				predecessors[i]->GetForward()[i].store(succ, std::memory_order_release);
			}

			uint32_t lvl = _level.load(std::memory_order_relaxed);
			while (lvl > 1 &&
				   _head->GetForward()[lvl - 1].load(std::memory_order_relaxed) == nullptr) {
				lvl--;
			}
			_level.store(lvl, std::memory_order_release);

			for (auto* p : locked) {
				p->lock.unlock();
			}

			_size.fetch_sub(1, std::memory_order_relaxed);

			// Safely defer memory reclamation until no concurrent readers exist [1]
			RetireNode(doomed);
			return true;
		}
	}

	void Clear() noexcept {
		SkipNode* curr = _head->GetForward()[0].load(std::memory_order_relaxed);
		while (curr != nullptr) {
			SkipNode* next = curr->GetForward()[0].load(std::memory_order_relaxed);
			DestroyNode(curr);
			curr = next;
		}

		for (uint32_t i = 0; i < MAX_LEVEL; ++i) {
			_head->GetForward()[i].store(nullptr, std::memory_order_relaxed);
		}
		_level.store(1, std::memory_order_relaxed);
		_size.store(0, std::memory_order_relaxed);

		ZHLN_LOCK(_retireMutex) {
			for (auto* node : _retireQueue) {
				DestroyNode(node);
			}
			_retireQueue.clear();
		}
	}

	[[nodiscard]] size_t Size() const noexcept { return _size.load(std::memory_order_relaxed); }
	[[nodiscard]] bool Empty() const noexcept { return Size() == 0; }

  private:
	// --- Quiescent State Reclamation (QSR) Garbage Collector ---
	void EnterReader() noexcept { _activeReaders.fetch_add(1, std::memory_order_acquire); }

	void ExitReader() noexcept {
		if (_activeReaders.fetch_sub(1, std::memory_order_release) == 1) {
			TryPurge(); // Last reader out, attempt garbage collection sweep
		}
	}

	void RetireNode(SkipNode* node) {
		ZHLN_LOCK(_retireMutex) {
			_retireQueue.push_back(node);
		}
		TryPurge();
	}

	void TryPurge() {
		// Fast-path check
		if (_activeReaders.load(std::memory_order_acquire) == 0) {
			std::vector<SkipNode*> localQueue;

			ZHLN_LOCK(_retireMutex) {
				// Re-verify under lock to prevent race conditions with newly entering readers
				if (_activeReaders.load(std::memory_order_acquire) == 0) {
					// Instantly steal the items, resetting the master tracker in O(1) time
					localQueue = std::move(_retireQueue);
				}
			}

			// Destroy the isolated batch safely outside the critical mutex zone
			for (auto* node : localQueue) {
				DestroyNode(node);
			}
		}
	}

	static SkipNode* CreateNode(const Key& key, const Value& value, uint32_t height) {
		size_t size = sizeof(SkipNode) + height * sizeof(ZHLN::Atomic<SkipNode*>);
		void* mem = ::operator new(size, std::align_val_t{alignof(SkipNode)});
		auto* node = ::new (mem)
			SkipNode{.key = key, .value = value, .height = height, .deleted = {}, .lock = {}};

		node->deleted.store(false, std::memory_order_relaxed);
		node->lock.locked.store(false, std::memory_order_relaxed);

		auto* forward = node->GetForward();
		for (uint32_t i = 0; i < height; ++i) {
			::new (&forward[i]) ZHLN::Atomic<SkipNode*>();
			forward[i].store(nullptr, std::memory_order_relaxed);
		}
		return node;
	}

	static void DestroyNode(SkipNode* node) noexcept {
		if (node == nullptr) {
			return;
		}
		node->~SkipNode();
		::operator delete(node, std::align_val_t{alignof(SkipNode)});
	}

	[[nodiscard]] uint32_t RandomHeight() noexcept {
		uint32_t height = 1;
		static thread_local uint32_t state = 2463534242u;
		while (height < MAX_LEVEL) {
			state ^= state << 13;
			state ^= state >> 17;
			state ^= state << 5;
			if ((state & 0x3) != 0) { // 25% height increase probability (p = 0.25)
				break;
			}
			height++;
		}
		return height;
	}

	SkipNode* _head = nullptr;
	Compare _compare;
	ZHLN::Atomic<uint32_t> _level{1};
	ZHLN::Atomic<size_t> _size{0};

	// Concurrent GC properties
	ZHLN::Atomic<uint32_t> _activeReaders{0};
	ZHLN::Mutex _retireMutex{};
	std::vector<SkipNode*> _retireQueue;
};

} // namespace ZHLN
