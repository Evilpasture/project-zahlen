// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>

#ifndef NDEBUG
#include <cstdio>
#endif

namespace ZHLN {

// ============================================================================
// Default Freestanding Sized Allocator
// ============================================================================

template <typename T> struct DefaultAllocator {
	using value_type = T;

	constexpr DefaultAllocator() noexcept = default;
	template <typename U>
	constexpr DefaultAllocator(const DefaultAllocator<U>& /*unused*/) noexcept {}

	[[nodiscard]] constexpr auto allocate(size_t n) -> T* {
		if (n == 0) {
			return nullptr;
		}
		auto* ptr = static_cast<T*>(
			::operator new[](n * sizeof(T), std::align_val_t{alignof(T)}, std::nothrow));
		if (ptr == nullptr) [[unlikely]] {
#if defined(__clang__) || defined(__GNUC__)
			__builtin_trap();
#else
			std::abort();
#endif
		}
		return ptr;
	}

	constexpr void deallocate(T* p, size_t n) noexcept {
		if (p != nullptr) {
			::operator delete[](p, n * sizeof(T), std::align_val_t{alignof(T)});
		}
	}
};

// ============================================================================
// Allocator Features Detection
// ============================================================================

/// Concept verifying that the allocator supports an optimized reallocation protocol.
///
/// Allocators satisfying this concept must adhere to the following contract:
/// - `.reallocate(ptr, old_cap, new_cap)` must semantically preserve the data of the first
///   `std::min(old_cap, new_cap)` elements.
/// - It must automatically free/deallocate the old memory block at `ptr` if the block is
///   relocated to a new virtual memory address (mirroring `std::realloc` semantics).
template <typename Alloc, typename T>
concept AllocatorHasReallocate = requires(Alloc& alloc, T* ptr, size_t old_cap, size_t new_cap) {
	{ alloc.reallocate(ptr, old_cap, new_cap) } -> std::same_as<T*>;
};

// ============================================================================
// ZHLN::Array Container
// ============================================================================

template <typename T, typename Allocator = DefaultAllocator<T>> class Array {
  public:
	using value_type = T;
	using allocator_type = Allocator;
	using size_type = size_t;
	using difference_type = ptrdiff_t;
	using reference = T&;
	using const_reference = const T&;
	using pointer = T*;
	using const_pointer = const T*;
	using iterator = T*;
	using const_iterator = const T*;
	using reverse_iterator = std::reverse_iterator<iterator>;
	using const_reverse_iterator = std::reverse_iterator<const_iterator>;

  private:
	using Traits = std::allocator_traits<allocator_type>;

  public:
	constexpr Array() noexcept(noexcept(Allocator())) : _allocator() {}

	constexpr explicit Array(const Allocator& alloc) noexcept : _allocator(alloc) {}

	constexpr explicit Array(size_t count, const Allocator& alloc = Allocator())
		: _allocator(alloc) {
		resize(count);
	}

	constexpr Array(size_t count, const T& value, const Allocator& alloc = Allocator())
		: _allocator(alloc) {
		assign(count, value);
	}

	template <typename InputIt>
		requires(std::input_iterator<InputIt>)
	constexpr Array(InputIt first, InputIt last, const Allocator& alloc = Allocator())
		: _allocator(alloc) {
		size_t count = std::distance(first, last);
		if (count > 0) {
			allocate_storage(count);
			for (size_t i = 0; i < count; ++i) {
				Traits::construct(_allocator, _data + i, *first);
				first++;
			}
			_size = count;
		}
	}

	constexpr Array(std::initializer_list<T> init, const Allocator& alloc = Allocator())
		: _allocator(alloc) {
		if (init.size() > 0) {
			allocate_storage(init.size());
			copy_construct_range(init.begin(), init.end(), _data);
			_size = init.size();
		}
	}

	constexpr ~Array() noexcept { clear_and_free(); }

	// Copy semantics
	constexpr Array(const Array& other) : _allocator(other._allocator) {
		if (other._size > 0) {
			allocate_storage(other._size);
			copy_construct_range(other._data, other._data + other._size, _data);
			_size = other._size;
		}
	}

	constexpr auto operator=(const Array& other) -> Array& {
		if (this != &other) {
			if (other._size > _capacity) {
				clear_and_free();
				allocate_storage(other._size);
			} else {
				clear();
			}
			copy_construct_range(other._data, other._data + other._size, _data);
			_size = other._size;
		}
		return *this;
	}

	// Move semantics
	constexpr Array(Array&& other) noexcept
		: _data(std::exchange(other._data, nullptr)), _size(std::exchange(other._size, 0)),
		  _capacity(std::exchange(other._capacity, 0)), _allocator(std::move(other._allocator)) {}

	constexpr auto operator=(Array&& other) noexcept -> Array& {
		if (this != &other) {
			clear_and_free();
			_data = std::exchange(other._data, nullptr);
			_size = std::exchange(other._size, 0);
			_capacity = std::exchange(other._capacity, 0);
			_allocator = std::move(other._allocator);
		}
		return *this;
	}

	// Element Access
	[[nodiscard]] constexpr auto operator[](size_t index) noexcept -> reference {
		AssertBounds(index < _size);
		return _data[index];
	}

	[[nodiscard]] constexpr auto operator[](size_t index) const noexcept -> const_reference {
		AssertBounds(index < _size);
		return _data[index];
	}

	[[nodiscard]] constexpr auto front() noexcept -> reference {
		AssertBounds(_size > 0);
		return _data[0];
	}

	[[nodiscard]] constexpr auto front() const noexcept -> const_reference {
		AssertBounds(_size > 0);
		return _data[0];
	}

	[[nodiscard]] constexpr auto back() noexcept -> reference {
		AssertBounds(_size > 0);
		return _data[_size - 1];
	}

	[[nodiscard]] constexpr auto back() const noexcept -> const_reference {
		AssertBounds(_size > 0);
		return _data[_size - 1];
	}

	[[nodiscard]] constexpr auto data() noexcept -> pointer { return _data; }
	[[nodiscard]] constexpr auto data() const noexcept -> const_pointer { return _data; }

	// Iterators
	[[nodiscard]] constexpr auto begin() noexcept -> iterator { return _data; }
	[[nodiscard]] constexpr auto begin() const noexcept -> const_iterator { return _data; }
	[[nodiscard]] constexpr auto cbegin() const noexcept -> const_iterator { return _data; }

	[[nodiscard]] constexpr auto end() noexcept -> iterator { return _data + _size; }
	[[nodiscard]] constexpr auto end() const noexcept -> const_iterator { return _data + _size; }
	[[nodiscard]] constexpr auto cend() const noexcept -> const_iterator { return _data + _size; }

	[[nodiscard]] constexpr auto rbegin() noexcept -> reverse_iterator {
		return reverse_iterator(end());
	}
	[[nodiscard]] constexpr auto rbegin() const noexcept -> const_reverse_iterator {
		return const_reverse_iterator(end());
	}
	[[nodiscard]] constexpr auto crbegin() const noexcept -> const_reverse_iterator {
		return const_reverse_iterator(end());
	}

	[[nodiscard]] constexpr auto rend() noexcept -> reverse_iterator {
		return reverse_iterator(begin());
	}
	[[nodiscard]] constexpr auto rend() const noexcept -> const_reverse_iterator {
		return const_reverse_iterator(begin());
	}
	[[nodiscard]] constexpr auto crend() const noexcept -> const_reverse_iterator {
		return const_reverse_iterator(begin());
	}

	// Capacity
	[[nodiscard]] constexpr auto empty() const noexcept -> bool { return _size == 0; }
	[[nodiscard]] constexpr auto size() const noexcept -> size_t { return _size; }
	[[nodiscard]] constexpr auto capacity() const noexcept -> size_t { return _capacity; }
	[[nodiscard]] constexpr auto max_size() const noexcept -> size_t {
		return std::numeric_limits<size_t>::max() / sizeof(T);
	}

	// Ergonomic std::span implicit conversions
	[[nodiscard]] constexpr operator std::span<T>() noexcept { return {_data, _size}; }
	[[nodiscard]] constexpr operator std::span<const T>() const noexcept { return {_data, _size}; }

	// Allocator Access
	[[nodiscard]] constexpr auto get_allocator() const noexcept -> allocator_type {
		return _allocator;
	}

	constexpr void reserve(size_t new_cap) {
		if (new_cap > _capacity) {
			reallocate(new_cap);
		}
	}

	constexpr void shrink_to_fit() {
		if (_size < _capacity) {
			if (_size == 0) {
				clear_and_free();
			} else {
				reallocate(_size);
			}
		}
	}

	// Modifiers
	constexpr void clear() noexcept {
		destroy_range(_data, _data + _size);
		_size = 0;
	}

	template <typename... Args> constexpr auto emplace_back(Args&&... args) -> reference {
		if (_size >= _capacity) {
			grow();
		}
		pointer target = _data + _size;
		Traits::construct(_allocator, target, std::forward<Args>(args)...);
		_size++;
		return *target;
	}

	constexpr void push_back(const T& value) {
		AssertNoAliasing(std::addressof(value));
		emplace_back(value);
	}

	constexpr void push_back(T&& value) {
		// NOTE: Aliasing checks are intentionally bypassed for rvalue move operations.
		// If an element is moved from itself (e.g., `arr.push_back(std::move(arr[0]))`),
		// any reallocation will cause a use-after-free of the source element since it
		// gets relocated before the move constructor runs. This is considered undefined
		// behavior at the caller level, matching std::vector's behavior.
		emplace_back(std::move(value));
	}

	constexpr void pop_back() noexcept {
		AssertBounds(_size > 0);
		_size--;
		Traits::destroy(_allocator, _data + _size);
	}

	// Mid-container insertion & construction
	template <typename... Args>
	constexpr auto emplace(const_iterator pos, Args&&... args) -> iterator {
		size_t index = pos - begin();
		AssertBounds(index <= _size);
		if (_size >= _capacity) {
			grow_and_emplace(index, std::forward<Args>(args)...);
		} else {
			if (index < _size) {
				Traits::construct(_allocator, _data + _size, std::move(_data[_size - 1]));
				for (size_t i = _size - 1; i > index; --i) {
					_data[i] = std::move(_data[i - 1]);
				}
				Traits::destroy(_allocator, _data + index);
				Traits::construct(_allocator, _data + index, std::forward<Args>(args)...);
			} else {
				Traits::construct(_allocator, _data + index, std::forward<Args>(args)...);
			}
			_size++;
		}
		return begin() + index;
	}

	constexpr auto insert(const_iterator pos, const T& value) -> iterator {
		AssertNoAliasing(std::addressof(value));
		return emplace(pos, value);
	}

	constexpr auto insert(const_iterator pos, T&& value) -> iterator {
		return emplace(pos, std::move(value));
	}

	// Range-based insertion (Values)
	constexpr auto insert(const_iterator pos, size_t count, const T& value) -> iterator {
		size_t index = pos - begin();
		AssertBounds(index <= _size);
		if (count == 0) {
			return begin() + index;
		}

		AssertNoAliasing(std::addressof(value));

		if (_size + count > _capacity) {
			grow_and_insert_value(index, count, value);
		} else {
			if (index < _size) {
				if constexpr (std::is_trivially_copyable_v<T>) {
					std::memmove(static_cast<void*>(_data + index + count),
								 static_cast<const void*>(_data + index),
								 (_size - index) * sizeof(T));
				} else {
					for (size_t i = _size; i > index; --i) {
						size_t srcIdx = i - 1;
						size_t dstIdx = srcIdx + count;
						Traits::construct(_allocator, _data + dstIdx, std::move(_data[srcIdx]));
						Traits::destroy(_allocator, _data + srcIdx);
					}
				}
			}
			copy_construct_range_value(_data + index, _data + index + count, value);
			_size += count;
		}
		return begin() + index;
	}

	// Range-based insertion (Iterators)
	template <typename InputIt>
		requires(std::input_iterator<InputIt>)
	constexpr auto insert(const_iterator pos, InputIt first, InputIt last) -> iterator {
		size_t index = pos - begin();
		AssertBounds(index <= _size);
		size_t count = std::distance(first, last);
		if (count == 0) {
			return begin() + index;
		}

		if (_size + count > _capacity) {
			grow_and_insert_range(index, first, count);
		} else {
			if (index < _size) {
				if constexpr (std::is_trivially_copyable_v<T>) {
					std::memmove(static_cast<void*>(_data + index + count),
								 static_cast<const void*>(_data + index),
								 (_size - index) * sizeof(T));
				} else {
					for (size_t i = _size; i > index; --i) {
						size_t srcIdx = i - 1;
						size_t dstIdx = srcIdx + count;
						Traits::construct(_allocator, _data + dstIdx, std::move(_data[srcIdx]));
						Traits::destroy(_allocator, _data + srcIdx);
					}
				}
			}
			for (size_t i = 0; i < count; ++i) {
				Traits::construct(_allocator, _data + index + i, *first);
				first++;
			}
			_size += count;
		}
		return begin() + index;
	}

	constexpr auto insert(const_iterator pos, std::initializer_list<T> list) -> iterator {
		return insert(pos, list.begin(), list.end());
	}

	// Range-based erasure
	constexpr auto erase(const_iterator first, const_iterator last) noexcept -> iterator {
		size_t index = first - begin();
		size_t count = last - first;
		AssertBounds(index + count <= _size);
		if (count == 0) {
			return begin() + index;
		}

		destroy_range(_data + index, _data + index + count);

		if (index + count < _size) {
			if constexpr (std::is_trivially_copyable_v<T>) {
				std::memmove(static_cast<void*>(_data + index),
							 static_cast<const void*>(_data + index + count),
							 (_size - index - count) * sizeof(T));
			} else {
				for (size_t i = index; i < _size - count; ++i) {
					Traits::construct(_allocator, _data + i, std::move(_data[i + count]));
					Traits::destroy(_allocator, _data + i + count);
				}
			}
		}
		_size -= count;
		return begin() + index;
	}

	constexpr auto erase(const_iterator pos) noexcept -> iterator { return erase(pos, pos + 1); }

	constexpr void resize(size_t new_size) {
		if (new_size < _size) {
			destroy_range(_data + new_size, _data + _size);
			_size = new_size;
		} else if (new_size > _size) {
			if (new_size > _capacity) {
				reallocate(new_size);
			}
			default_construct_range(_data + _size, _data + new_size);
			_size = new_size;
		}
	}

	constexpr void resize(size_t new_size, const T& value) {
		AssertNoAliasing(std::addressof(value));
		if (new_size < _size) {
			destroy_range(_data + new_size, _data + _size);
			_size = new_size;
		} else if (new_size > _size) {
			if (new_size > _capacity) {
				reallocate(new_size);
			}
			copy_construct_range_value(_data + _size, _data + new_size, value);
			_size = new_size;
		}
	}

	constexpr void assign(size_t count, const T& value) {
		AssertNoAliasing(std::addressof(value));
		if (count > _capacity) {
			clear_and_free();
			allocate_storage(count);
		} else {
			clear();
		}
		copy_construct_range_value(_data, _data + count, value);
		_size = count;
	}

	template <typename InputIt>
		requires(std::input_iterator<InputIt>)
	constexpr void assign(InputIt first, InputIt last) {
		size_t count = std::distance(first, last);
		if (count > _capacity) {
			clear_and_free();
			allocate_storage(count);
		} else {
			clear();
		}
		for (size_t i = 0; i < count; ++i) {
			Traits::construct(_allocator, _data + i, *first);
			first++;
		}
		_size = count;
	}

	constexpr void assign(std::initializer_list<T> list) { assign(list.begin(), list.end()); }

	constexpr void swap(Array& other) noexcept {
		std::swap(_data, other._data);
		std::swap(_size, other._size);
		std::swap(_capacity, other._capacity);
		std::swap(_allocator, other._allocator);
	}

	friend constexpr void swap(Array& lhs, Array& rhs) noexcept { lhs.swap(rhs); }

	// Comparison Operators
	[[nodiscard]] constexpr auto operator==(const Array& other) const -> bool {
		if (_size != other._size) {
			return false;
		}
		for (size_t i = 0; i < _size; ++i) {
			if (!(_data[i] == other._data[i])) {
				return false;
			}
		}
		return true;
	}

	[[nodiscard]] constexpr auto operator<=>(const Array& other) const {
		return std::lexicographical_compare_three_way(begin(), end(), other.begin(), other.end());
	}

  private:
	pointer _data = nullptr;
	size_t _size = 0;
	size_t _capacity = 0;
	[[no_unique_address]] allocator_type _allocator;

	[[gnu::always_inline]] static constexpr void AssertBounds(bool condition) noexcept {
		if (!condition) [[unlikely]] {
#ifndef NDEBUG
			// Bypassed during constant evaluation to maintain compatibility with constexpr
			if (!std::is_constant_evaluated()) {
				std::fprintf(stderr, "[ZHLN::Array] Safety constraint violated!\n");
			}
#endif
#if defined(__clang__) || defined(__GNUC__)
			__builtin_trap();
#else
			std::abort();
#endif
		}
	}

	[[gnu::always_inline]] constexpr void AssertNoAliasing(const T* ptr) const noexcept {
		if (ptr != nullptr && _data != nullptr) [[likely]] {
			const bool is_before = std::less<const T*>{}(ptr, _data);
			const bool is_after = !std::less<const T*>{}(ptr, _data + _size);
			AssertBounds(is_before || is_after);
		}
	}

	constexpr void allocate_storage(size_t cap) {
		if (cap == 0) {
			return;
		}
		_data = Traits::allocate(_allocator, cap);
		_capacity = cap;
	}

	constexpr void clear_and_free() noexcept {
		if (_data != nullptr) {
			clear();
			Traits::deallocate(_allocator, _data, _capacity);
			_data = nullptr;
			_capacity = 0;
		}
	}

	constexpr void grow() {
		size_t new_cap = _capacity == 0 ? 8 : _capacity * 2;
		reallocate(new_cap);
	}

	constexpr void reallocate(size_t new_cap) {
		if (new_cap == 0) {
			clear_and_free();
			return;
		}

		pointer new_data = nullptr;

		if constexpr (AllocatorHasReallocate<allocator_type, T>) {
			if (_data != nullptr) [[likely]] {
				new_data = _allocator.reallocate(_data, _capacity, new_cap);
			} else {
				new_data = Traits::allocate(_allocator, new_cap);
			}
		} else {
			new_data = Traits::allocate(_allocator, new_cap);
			if (_data != nullptr) {
				if constexpr (std::is_trivially_move_constructible_v<T> &&
							  std::is_trivially_destructible_v<T>) {
					std::memcpy(new_data, _data, _size * sizeof(T));
				} else {
					for (size_t i = 0; i < _size; ++i) {
						Traits::construct(_allocator, &new_data[i], std::move(_data[i]));
						Traits::destroy(_allocator, &_data[i]);
					}
				}
				Traits::deallocate(_allocator, _data, _capacity);
			}
		}

		_data = new_data;
		_capacity = new_cap;
	}

	// Unified allocation & relocation pipeline helper
	template <typename ConstructFn>
	constexpr void relocate_reallocate(size_t insert_index, size_t insert_count,
									   ConstructFn&& construct_fn) {
		size_t new_cap = _capacity == 0 ? 8 : _capacity * 2;
		while (new_cap < _size + insert_count) {
			new_cap *= 2;
		}

		pointer new_data = nullptr;

		if constexpr (AllocatorHasReallocate<allocator_type, T>) {
			if (_data != nullptr) [[likely]] {
				new_data = _allocator.reallocate(_data, _capacity, new_cap);
			} else {
				new_data = Traits::allocate(_allocator, new_cap);
			}
			_data = new_data;
			_capacity = new_cap;

			// Shift old elements to make room for insertion
			if (insert_index < _size) {
				if constexpr (std::is_trivially_copyable_v<T>) {
					std::memmove(_data + insert_index + insert_count, _data + insert_index,
								 (_size - insert_index) * sizeof(T));
				} else {
					for (size_t i = _size; i > insert_index; --i) {
						size_t srcIdx = i - 1;
						size_t dstIdx = srcIdx + insert_count;
						Traits::construct(_allocator, _data + dstIdx, std::move(_data[srcIdx]));
						Traits::destroy(_allocator, _data + srcIdx);
					}
				}
			}

			std::forward<ConstructFn>(construct_fn)(_data + insert_index);
			_size += insert_count;
		} else {
			new_data = Traits::allocate(_allocator, new_cap);

			// 1. Relocate old elements preceding the insertion index
			if (_data != nullptr) {
				if constexpr (std::is_trivially_copyable_v<T>) {
					std::memcpy(new_data, _data, insert_index * sizeof(T));
				} else {
					for (size_t i = 0; i < insert_index; ++i) {
						Traits::construct(_allocator, &new_data[i], std::move(_data[i]));
						Traits::destroy(_allocator, &_data[i]);
					}
				}
			}

			// 2. Invoke context-specific element construction callback
			std::forward<ConstructFn>(construct_fn)(new_data + insert_index);

			// 3. Relocate old elements following the insertion index
			if (_data != nullptr) {
				if constexpr (std::is_trivially_copyable_v<T>) {
					std::memcpy(new_data + insert_index + insert_count, _data + insert_index,
								(_size - insert_index) * sizeof(T));
				} else {
					for (size_t i = insert_index; i < _size; ++i) {
						Traits::construct(_allocator, &new_data[i + insert_count],
										  std::move(_data[i]));
						Traits::destroy(_allocator, &_data[i]);
					}
				}
				Traits::deallocate(_allocator, _data, _capacity);
			}

			_data = new_data;
			_capacity = new_cap;
			_size += insert_count;
		}
	}

	template <typename... Args> constexpr void grow_and_emplace(size_t index, Args&&... args) {
		relocate_reallocate(index, 1, [&](pointer dst) {
			Traits::construct(_allocator, dst, std::forward<Args>(args)...);
		});
	}

	template <typename InputIt>
	constexpr void grow_and_insert_range(size_t index, InputIt first, size_t count) {
		relocate_reallocate(index, count, [&](pointer dst) {
			for (size_t i = 0; i < count; ++i) {
				Traits::construct(_allocator, dst + i, *first);
				first++;
			}
		});
	}

	constexpr void grow_and_insert_value(size_t index, size_t count, const T& value) {
		relocate_reallocate(index, count, [&](pointer dst) {
			for (size_t i = 0; i < count; ++i) {
				Traits::construct(_allocator, dst + i, value);
			}
		});
	}

	constexpr void destroy_range(pointer start, pointer end) noexcept {
		if constexpr (!std::is_trivially_destructible_v<T>) {
			while (start != end) {
				Traits::destroy(_allocator, start);
				start++;
			}
		}
	}

	constexpr void copy_construct_range(const_pointer start, const_pointer end, pointer dst) {
		if constexpr (std::is_trivially_copy_constructible_v<T>) {
			std::memcpy(static_cast<void*>(dst), static_cast<const void*>(start),
						(end - start) * sizeof(T));
		} else {
			while (start != end) {
				Traits::construct(_allocator, dst, *start);
				start++;
				dst++;
			}
		}
	}

	constexpr void copy_construct_range_value(pointer start, pointer end, const T& value) {
		if constexpr (std::is_trivially_copyable_v<T> && sizeof(T) == 1) {
			unsigned char byte_val = 0;
			std::memcpy(&byte_val, std::addressof(value), 1);
			std::memset(static_cast<void*>(start), byte_val, end - start);
		} else {
			while (start != end) {
				Traits::construct(_allocator, start, value);
				start++;
			}
		}
	}

	constexpr void default_construct_range(pointer start, pointer end) {
		if constexpr (std::is_trivially_copyable_v<T> &&
					  std::is_trivially_default_constructible_v<T>) {
			std::memset(static_cast<void*>(start), 0, (end - start) * sizeof(T));
		} else {
			while (start != end) {
				Traits::construct(_allocator, start);
				start++;
			}
		}
	}
};

} // namespace ZHLN
