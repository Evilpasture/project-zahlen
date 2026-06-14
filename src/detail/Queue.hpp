// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdlib>
#include <new>
#include <threading/Mutex.hpp>
#include <type_traits>

namespace ZHLN {

/**
 * @brief Thread-safe, growable circular queue (FIFO) designed for -fno-exceptions.
 * Uses ZHLN::Mutex for low-overhead, fast internal synchronization.
 * Enforces power-of-two capacities to use fast bitwise masking.
 */
template <typename T, size_t InitialCapacity = 16> class Queue {
	static_assert((InitialCapacity & (InitialCapacity - 1)) == 0,
				  "InitialCapacity must be a power of two!");

  public:
	Queue() : _capacity(InitialCapacity) { AllocateStorage(_capacity); }

	~Queue() noexcept { ClearAndFree(); }

	// Copy Constructor (Thread-Safe)
	Queue(const Queue& other) : _tail(other._size), _capacity(other._capacity), _size(other._size) {
		MutexGuard otherGuard(const_cast<Queue&>(other)._mutex);

		_data = static_cast<T*>(
			::operator new[](_capacity * sizeof(T), std::align_val_t{alignof(T)}, std::nothrow));
		if (_data == nullptr) [[unlikely]] {
			TrapAllocationFailure();
		}

		for (size_t i = 0; i < _size; ++i) {
			size_t srcIdx = (other._head + i) & (other._capacity - 1);
			::new (static_cast<void*>(&_data[i])) T(other._data[srcIdx]);
		}
	}

	// Copy Assignment (Deadlock-Free & Thread-Safe)
	Queue& operator=(const Queue& other) {
		if (this != &other) {
			Queue* first = this;
			auto* second = const_cast<Queue*>(&other);
			if (first > second) {
				Queue* temp = first;
				first = second;
				second = temp;
			}
			MutexGuard lock1(first->_mutex);
			MutexGuard lock2(second->_mutex);

			T* newData = static_cast<T*>(::operator new[](
				other._capacity * sizeof(T), std::align_val_t{alignof(T)}, std::nothrow));
			if (newData == nullptr) [[unlikely]] {
				TrapAllocationFailure();
			}

			for (size_t i = 0; i < other._size; ++i) {
				size_t srcIdx = (other._head + i) & (other._capacity - 1);
				::new (static_cast<void*>(&newData[i])) T(other._data[srcIdx]);
			}

			ClearAndFree();
			_data = newData;
			_capacity = other._capacity;
			_size = other._size;
			_head = 0;
			_tail = other._size;
		}
		return *this;
	}

	// Move Constructor (Thread-Safe)
	Queue(Queue&& other) noexcept
		: _data(other._data), _head(other._head), _tail(other._tail), _capacity(other._capacity),
		  _size(other._size) {
		MutexGuard otherGuard(other._mutex);

		other._data = nullptr;
		other._head = 0;
		other._tail = 0;
		other._capacity = 0;
		other._size = 0;
	}

	// Move Assignment (Deadlock-Free & Thread-Safe)
	Queue& operator=(Queue&& other) noexcept {
		if (this != &other) {
			Queue* first = this;
			Queue* second = &other;
			if (first > second) {
				Queue* temp = first;
				first = second;
				second = temp;
			}
			MutexGuard lock1(first->_mutex);
			MutexGuard lock2(second->_mutex);

			ClearAndFree();

			_data = other._data;
			_head = other._head;
			_tail = other._tail;
			_capacity = other._capacity;
			_size = other._size;

			other._data = nullptr;
			other._head = 0;
			other._tail = 0;
			other._capacity = 0;
			other._size = 0;
		}
		return *this;
	}

	template <typename... Args> void emplace(Args&&... args) {
		MutexGuard guard(_mutex);
		if (_size >= _capacity) [[unlikely]] {
			Grow();
		}
		::new (static_cast<void*>(&_data[_tail])) T(static_cast<Args&&>(args)...);
		_tail = (_tail + 1) & (_capacity - 1);
		_size++;
	}

	void push(const T& value) { emplace(value); }

	void push(T&& value) { emplace(static_cast<T&&>(value)); }

	/**
	 * @brief Atomically extracts the front item from the queue.
	 * @param outValue Receives the moved front item on success.
	 * @return true if an item was successfully popped, false if empty.
	 */
	bool try_pop(T& outValue) noexcept {
		MutexGuard guard(_mutex);
		if (_size == 0) {
			return false;
		}
		outValue = static_cast<T&&>(_data[_head]);
		_data[_head].~T();
		_head = (_head + 1) & (_capacity - 1);
		_size--;
		return true;
	}

	[[nodiscard]] bool empty() const noexcept {
		MutexGuard guard(const_cast<Mutex&>(_mutex));
		return _size == 0;
	}

	[[nodiscard]] size_t size() const noexcept {
		MutexGuard guard(const_cast<Mutex&>(_mutex));
		return _size;
	}

	[[nodiscard]] size_t capacity() const noexcept {
		MutexGuard guard(const_cast<Mutex&>(_mutex));
		return _capacity;
	}

	void clear() noexcept {
		MutexGuard guard(_mutex);
		for (size_t i = 0; i < _size; ++i) {
			size_t idx = (_head + i) & (_capacity - 1);
			_data[idx].~T();
		}
		_head = 0;
		_tail = 0;
		_size = 0;
	}

  private:
	[[noreturn]] [[gnu::cold]] void TrapAllocationFailure() const noexcept {
#if defined(__clang__) || defined(__GNUC__)
		__builtin_trap();
#else
		std::abort();
#endif
	}

	void AllocateStorage(size_t cap) {
		if (cap == 0) [[unlikely]] {
			_data = nullptr;
			return;
		}
		_data = static_cast<T*>(
			::operator new[](cap * sizeof(T), std::align_val_t{alignof(T)}, std::nothrow));
		if (_data == nullptr) [[unlikely]] {
			TrapAllocationFailure();
		}
	}

	void ClearAndFree() noexcept {
		if (_data != nullptr) {
			for (size_t i = 0; i < _size; ++i) {
				size_t idx = (_head + i) & (_capacity - 1);
				_data[idx].~T();
			}
			::operator delete[](_data, std::align_val_t{alignof(T)});
			_data = nullptr;
		}
	}

	void Grow() {
		size_t newCapacity = _capacity == 0 ? InitialCapacity : _capacity * 2;
		T* oldData = _data;
		size_t oldCapacity = _capacity;
		size_t oldHead = _head;

		T* newData = static_cast<T*>(
			::operator new[](newCapacity * sizeof(T), std::align_val_t{alignof(T)}, std::nothrow));
		if (newData == nullptr) [[unlikely]] {
			TrapAllocationFailure();
		}

		constexpr bool use_move =
			std::is_nothrow_move_constructible_v<T> || !std::is_copy_constructible_v<T>;

		// OPTIMIZED SINGLE-PASS RELOCATION LOOP
		for (size_t i = 0; i < _size; ++i) {
			size_t srcIdx = (oldHead + i) & (oldCapacity - 1);
			if constexpr (use_move) {
				::new (static_cast<void*>(&newData[i])) T(static_cast<T&&>(oldData[srcIdx]));
			} else {
				::new (static_cast<void*>(&newData[i])) T(oldData[srcIdx]);
			}
			// Destroy the old item immediately while its cache line is scorching hot!
			oldData[srcIdx].~T();
		}

		if (oldData != nullptr) {
			::operator delete[](oldData, std::align_val_t{alignof(T)});
		}

		// Atomic assignment of states
		_data = newData;
		_head = 0;
		_tail = _size;
		_capacity = newCapacity;
	}

	T* _data = nullptr;
	size_t _head = 0;
	size_t _tail = 0;
	size_t _capacity = 0;
	size_t _size = 0;
	mutable Mutex _mutex{};
};

} // namespace ZHLN
