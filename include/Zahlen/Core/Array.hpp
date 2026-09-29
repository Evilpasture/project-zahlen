// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Platform.hpp"
#include <algorithm>
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


template <typename T>
struct DefaultAllocator {
    using value_type = T;

    constexpr DefaultAllocator() noexcept = default;
    template <typename U>
    constexpr DefaultAllocator(const DefaultAllocator<U>& ) noexcept {
    }

    [[nodiscard]] constexpr auto allocate(size_t n) -> T* {
        if (n == 0) {
            return nullptr;
        }
        auto* ptr = static_cast<T*>(::operator new[](n * sizeof(T), std::align_val_t {alignof(T)}, std::nothrow));
        if (ptr == nullptr) [[unlikely]] {
            DebugBreak();
        }
        return ptr;
    }

    constexpr void deallocate(T* p, size_t n) noexcept {
        if (p != nullptr) {
            ::operator delete[](p, n * sizeof(T), std::align_val_t {alignof(T)});
        }
    }
};


template <typename Alloc, typename T>
concept AllocatorHasReallocate = requires(Alloc& alloc, T* ptr, size_t old_cap, size_t new_cap) {
    { alloc.reallocate(ptr, old_cap, new_cap) } -> std::same_as<T*>;
};


// Budget roughly 128 bytes for typical small values; very large values
// still get one inline slot. InlineCap = 0 opts out of inline storage.
template <typename T>
consteval size_t DefaultInlineCapacity() noexcept {
    if constexpr (sizeof(T) <= 8) {
        return 16; // At most 128 inline bytes.
    } else if constexpr (sizeof(T) <= 16) {
        return 8;
    } else if constexpr (sizeof(T) <= 32) {
        return 4;
    } else if constexpr (sizeof(T) <= 64) {
        return 2;
    } else {
        return 1;
    }
}

template <typename T, size_t InlineCap = DefaultInlineCapacity<T>(), typename Allocator = DefaultAllocator<T>>
class Array {
  public:
    using value_type             = T;
    using allocator_type         = Allocator;
    using size_type              = size_t;
    using difference_type        = ptrdiff_t;
    using reference              = T&;
    using const_reference        = const T&;
    using pointer                = T*;
    using const_pointer          = const T*;
    using iterator               = T*;
    using const_iterator         = const T*;
    using reverse_iterator       = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

  private:
    using Traits = std::allocator_traits<allocator_type>;

  public:
    constexpr Array() noexcept(noexcept(Allocator())): _allocator() {
    }

    constexpr explicit Array(const Allocator& alloc) noexcept: _allocator(alloc) {
    }

    constexpr explicit Array(size_t count, const Allocator& alloc = Allocator()): _allocator(alloc) {
        resize(count);
    }

    constexpr Array(size_t count, const T& value, const Allocator& alloc = Allocator()): _allocator(alloc) {
        assign(count, value);
    }

    template <typename InputIt>
        requires(std::input_iterator<InputIt>)
    constexpr Array(InputIt first, InputIt last, const Allocator& alloc = Allocator()): _allocator(alloc) {
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

    constexpr Array(std::initializer_list<T> init, const Allocator& alloc = Allocator()): _allocator(alloc) {
        if (init.size() > 0) {
            allocate_storage(init.size());
            copy_construct_range(init.begin(), init.end(), _data);
            _size = init.size();
        }
    }

    constexpr ~Array() noexcept {
        clear_and_free();
    }

    constexpr Array(const Array& other): _allocator(other._allocator) {
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
            if (other._size != 0) {
                copy_construct_range(other._data, other._data + other._size, _data);
            }
            _size = other._size;
        }
        return *this;
    }

    constexpr Array(Array&& other) noexcept(std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_constructible_v<Allocator>):
        _allocator(std::move(other._allocator)) {
        if (other.IsInline()) {
            for (size_t i = 0; i < other._size; ++i) {
                Traits::construct(_allocator, _data + i, std::move(other._data[i]));
                ++_size;
            }
            other.clear();
        } else {
            _data     = std::exchange(other._data, other.InlineData());
            _size     = std::exchange(other._size, 0);
            _capacity = std::exchange(other._capacity, InlineCap);
        }
    }

    constexpr auto operator=(Array&& other) noexcept(std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_assignable_v<Allocator>) -> Array& {
        if (this != &other) {
            clear_and_free();
            _allocator = std::move(other._allocator);
            if (other.IsInline()) {
                for (size_t i = 0; i < other._size; ++i) {
                    Traits::construct(_allocator, _data + i, std::move(other._data[i]));
                    ++_size;
                }
                other.clear();
            } else {
                _data     = std::exchange(other._data, other.InlineData());
                _size     = std::exchange(other._size, 0);
                _capacity = std::exchange(other._capacity, InlineCap);
            }
        }
        return *this;
    }

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

    [[nodiscard]] constexpr auto data() noexcept -> pointer {
        return _data;
    }
    [[nodiscard]] constexpr auto data() const noexcept -> const_pointer {
        return _data;
    }

    [[nodiscard]] constexpr auto begin() noexcept -> iterator {
        return _data;
    }
    [[nodiscard]] constexpr auto begin() const noexcept -> const_iterator {
        return _data;
    }
    [[nodiscard]] constexpr auto cbegin() const noexcept -> const_iterator {
        return _data;
    }

    [[nodiscard]] constexpr auto end() noexcept -> iterator {
        return _data == nullptr ? nullptr : _data + _size;
    }
    [[nodiscard]] constexpr auto end() const noexcept -> const_iterator {
        return _data == nullptr ? nullptr : _data + _size;
    }
    [[nodiscard]] constexpr auto cend() const noexcept -> const_iterator {
        return end();
    }

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

    [[nodiscard]] constexpr auto empty() const noexcept -> bool {
        return _size == 0;
    }
    [[nodiscard]] constexpr auto size() const noexcept -> size_t {
        return _size;
    }
    [[nodiscard]] constexpr auto capacity() const noexcept -> size_t {
        return _capacity;
    }
    [[nodiscard]] constexpr auto max_size() const noexcept -> size_t {
        return std::numeric_limits<size_t>::max() / sizeof(T);
    }

    [[nodiscard]] constexpr operator std::span<T>() noexcept {
        return {_data, _size};
    }
    [[nodiscard]] constexpr operator std::span<const T>() const noexcept {
        return {_data, _size};
    }

    [[nodiscard]] constexpr auto get_allocator() const noexcept -> allocator_type {
        return _allocator;
    }

    constexpr void reserve(size_t new_cap) {
        if (new_cap > _capacity) {
            reallocate(new_cap);
        }
    }

    constexpr void shrink_to_fit() {
        if (_size <= InlineCap && !IsInline()) {
            reallocate(InlineCap); // Return small arrays to their own inline storage.
        } else if (_size > InlineCap && _size < _capacity) {
            reallocate(_size);
        }
    }

    constexpr void clear() noexcept {
        if (_size != 0) {
            destroy_range(_data, _data + _size);
        }
        _size = 0;
    }

    template <typename... Args>
    constexpr auto emplace_back(Args&&... args) -> reference {
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
        emplace_back(std::move(value));
    }

    constexpr void pop_back() noexcept {
        AssertBounds(_size > 0);
        _size--;
        Traits::destroy(_allocator, _data + _size);
    }

    // Unordered erase: replace the erased element with the last one.
    constexpr void swap_remove(size_t index) noexcept(
        std::is_nothrow_destructible_v<T> &&
        (std::is_move_assignable_v<T> ? std::is_nothrow_move_assignable_v<T> : std::is_nothrow_move_constructible_v<T>)
    ) requires(std::is_move_constructible_v<T>) {
        AssertBounds(index < _size);
        if (index != _size - 1) {
            if constexpr (std::is_move_assignable_v<T>) {
                _data[index] = std::move(_data[_size - 1]);
            } else {
                Traits::destroy(_allocator, _data + index);
                Traits::construct(_allocator, _data + index, std::move(_data[_size - 1]));
            }
        }
        pop_back();
    }

    // For trivially default-constructible, trivially copyable elements only.
    // Starts their lifetimes without initializing their bytes. The returned
    // region must be fully written before its contents are read.
    [[nodiscard]] constexpr auto extend_uninitialized(size_t count) -> std::span<T>
        requires(std::is_trivially_default_constructible_v<T> && std::is_trivially_copyable_v<T>) {
        AssertBounds(count <= max_size() - _size);
        if (count == 0) {
            return {_size == 0 ? _data : _data + _size, size_t {0}};
        }
        if (_size + count > _capacity) {
            reallocate(grown_capacity(_size + count));
        }
        pointer first = _data + _size;
        std::uninitialized_default_construct_n(first, count);
        _size += count;
        return {first, count};
    }

    // Append a batch with one capacity check. Trivially copyable elements use
    // memcpy; others are copy-constructed. Self-appends are rebased after
    // growth so a span into our own storage cannot dangle on reallocation.
    constexpr void append(std::span<const T> values) {
        const size_t count = values.size();
        if (count == 0) {
            return;
        }
        AssertBounds(count <= max_size() - _size);

        bool self = false;
        size_t offset = 0;
        if (_size != 0) {
            const std::less<const_pointer> less {};
            self = !less(values.data(), _data) && less(values.data(), _data + _size);
            if (self) {
                offset = static_cast<size_t>(values.data() - _data);
                AssertBounds(count <= _size - offset);
            }
        }
        if (_size + count > _capacity) {
            reallocate(grown_capacity(_size + count));
        }
        const_pointer src = self ? _data + offset : values.data();
        pointer dst = _data + _size;
        if constexpr (std::is_trivially_copyable_v<T>) {
            std::memcpy(static_cast<void*>(dst), static_cast<const void*>(src), count * sizeof(T));
            _size += count;
        } else {
            for (size_t i = 0; i < count; ++i) {
                Traits::construct(_allocator, dst + i, src[i]);
                ++_size;
            }
        }
    }

    template <typename... Args>
    constexpr auto emplace(const_iterator pos, Args&&... args) -> iterator {
        const size_t index = OffsetOf(pos);
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

    constexpr auto insert(const_iterator pos, size_t count, const T& value) -> iterator {
        const size_t index = OffsetOf(pos);
        if (count == 0) {
            return pos == nullptr ? nullptr : begin() + index;
        }
        AssertBounds(count <= max_size() - _size);

        AssertNoAliasing(std::addressof(value));

        if (_size + count > _capacity) {
            grow_and_insert_value(index, count, value);
        } else {
            if (index < _size) {
                if constexpr (std::is_trivially_copyable_v<T>) {
                    std::memmove(static_cast<void*>(_data + index + count), static_cast<const void*>(_data + index), (_size - index) * sizeof(T));
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

    template <typename InputIt>
        requires(std::input_iterator<InputIt>)
    constexpr auto insert(const_iterator pos, InputIt first, InputIt last) -> iterator {
        const size_t index = OffsetOf(pos);
        size_t count = std::distance(first, last);
        if (count == 0) {
            return pos == nullptr ? nullptr : begin() + index;
        }
        AssertBounds(count <= max_size() - _size);

        if (_size + count > _capacity) {
            grow_and_insert_range(index, first, count);
        } else {
            if (index < _size) {
                if constexpr (std::is_trivially_copyable_v<T>) {
                    std::memmove(static_cast<void*>(_data + index + count), static_cast<const void*>(_data + index), (_size - index) * sizeof(T));
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

    constexpr auto erase(const_iterator first, const_iterator last) noexcept -> iterator {
        const size_t index = OffsetOf(first);
        const size_t count = first == last ? 0 : static_cast<size_t>(last - first);
        AssertBounds(index + count <= _size);
        if (count == 0) {
            return first == nullptr ? nullptr : begin() + index;
        }

        destroy_range(_data + index, _data + index + count);

        if (index + count < _size) {
            if constexpr (std::is_trivially_copyable_v<T>) {
                std::memmove(static_cast<void*>(_data + index), static_cast<const void*>(_data + index + count), (_size - index - count) * sizeof(T));
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

    constexpr auto erase(const_iterator pos) noexcept -> iterator {
        return erase(pos, pos + 1);
    }

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
        if (count != 0) {
            copy_construct_range_value(_data, _data + count, value);
        }
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

    constexpr void assign(std::initializer_list<T> list) {
        assign(list.begin(), list.end());
    }

    constexpr void swap(Array& other) noexcept(
        std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_constructible_v<Allocator> &&
        std::is_nothrow_move_assignable_v<Allocator> && std::is_nothrow_swappable_v<Allocator>
    ) {
        if (this == &other) {
            return;
        }
        if (!IsInline() && !other.IsInline()) {
            std::swap(_data, other._data);
            std::swap(_size, other._size);
            std::swap(_capacity, other._capacity);
            std::swap(_allocator, other._allocator);
        } else {
            // A pointer into this object's inline bytes cannot be exchanged.
            Array temp(std::move(*this));
            *this = std::move(other);
            other = std::move(temp);
        }
    }

    friend constexpr void swap(Array& lhs, Array& rhs) noexcept(noexcept(lhs.swap(rhs))) {
        lhs.swap(rhs);
    }

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
    static_assert(InlineCap <= std::numeric_limits<size_t>::max() / sizeof(T));

    // Keep storage in the array object, not in the allocator: a heap allocation
    // can be stolen on move, but a pointer into another object's inline bytes
    // must never be stolen or deallocated.
    alignas(T) std::byte _inlineStorage[InlineCap == 0 ? 1 : InlineCap * sizeof(T)];
    pointer             _data     = InlineData();
    size_t              _size     = 0;
    size_t              _capacity = InlineCap;
    [[no_unique_address]] allocator_type _allocator;

    [[nodiscard]] constexpr auto InlineData() noexcept -> pointer {
        if constexpr (InlineCap == 0) {
            return nullptr;
        } else {
            return reinterpret_cast<pointer>(_inlineStorage);
        }
    }

    [[nodiscard]] constexpr auto InlineData() const noexcept -> const_pointer {
        if constexpr (InlineCap == 0) {
            return nullptr;
        } else {
            return reinterpret_cast<const_pointer>(_inlineStorage);
        }
    }

    [[nodiscard]] constexpr auto IsInline() const noexcept -> bool { return _data == InlineData(); }

    [[nodiscard]] constexpr auto OffsetOf(const_iterator pos) const noexcept -> size_t {
        if (_size == 0) {
            AssertBounds(pos == _data);
            return 0; // Do not subtract two null pointers for Array<T, 0>.
        }
        const size_t index = static_cast<size_t>(pos - _data);
        AssertBounds(index <= _size);
        return index;
    }

    [[gnu::always_inline]] static constexpr void AssertBounds(bool condition) noexcept {
        if (!condition) [[unlikely]] {
#ifndef NDEBUG
            if (!std::is_constant_evaluated()) {
                std::fprintf(stderr, "[ZHLN::Array] Safety constraint violated!\n");
            }
#endif
            DebugBreak();
        }
    }

    [[gnu::always_inline]] constexpr void AssertNoAliasing(const T* ptr) const noexcept {
        if (ptr != nullptr && _data != nullptr) [[likely]] {
            const bool is_before = std::less<const T*> {}(ptr, _data);
            const bool is_after  = !std::less<const T*> {}(ptr, _data + _size);
            AssertBounds(is_before || is_after);
        }
    }

    constexpr void allocate_storage(size_t cap) {
        AssertBounds(cap <= max_size());
        if (cap > InlineCap) {
            _data     = Traits::allocate(_allocator, cap);
            _capacity = cap;
        }
    }

    constexpr void clear_and_free() noexcept {
        clear();
        if (!IsInline()) {
            Traits::deallocate(_allocator, _data, _capacity);
            _data     = InlineData();
            _capacity = InlineCap;
        }
    }

    // Returns a capacity large enough for a batch without making each append
    // allocate separately. Use the allocator's reallocate only for heap-backed,
    // trivially copyable elements; it must never receive inline storage.
    [[nodiscard]] constexpr auto grown_capacity(size_t needed) const noexcept -> size_t {
        AssertBounds(needed <= max_size());
        size_t cap = _capacity == 0 ? std::min<size_t>(8, max_size()) : _capacity;
        while (cap < needed) {
            cap = cap > max_size() / 2 ? max_size() : cap * 2;
        }
        return cap;
    }

    constexpr void grow() {
        AssertBounds(_size < max_size());
        reallocate(grown_capacity(_size + 1));
    }

    constexpr void relocate_elements(pointer dst, pointer src, size_t count) {
        if (count == 0) {
            return;
        }
        if constexpr (std::is_trivially_copyable_v<T>) {
            std::memcpy(static_cast<void*>(dst), static_cast<const void*>(src), count * sizeof(T));
        } else {
            for (size_t i = 0; i < count; ++i) {
                Traits::construct(_allocator, dst + i, std::move(src[i]));
            }
            destroy_range(src, src + count);
        }
    }

    constexpr void reallocate(size_t new_cap) {
        AssertBounds(new_cap >= _size && new_cap <= max_size());
        if (new_cap <= InlineCap) {
            if constexpr (InlineCap == 0) {
                clear_and_free();
            } else if (!IsInline()) {
                pointer target = InlineData();
                relocate_elements(target, _data, _size);
                Traits::deallocate(_allocator, _data, _capacity);
                _data     = target;
                _capacity = InlineCap;
            }
            return;
        }

        if constexpr (AllocatorHasReallocate<allocator_type, T> && std::is_trivially_copyable_v<T>) {
            if (!IsInline()) {
                _data     = _allocator.reallocate(_data, _capacity, new_cap);
                _capacity = new_cap;
                return;
            }
        }

        pointer target = Traits::allocate(_allocator, new_cap);
        relocate_elements(target, _data, _size);
        if (!IsInline()) {
            Traits::deallocate(_allocator, _data, _capacity);
        }
        _data     = target;
        _capacity = new_cap;
    }

    template <typename ConstructFn>
    constexpr void relocate_reallocate(size_t insert_index, size_t insert_count, ConstructFn&& construct_fn) {
        AssertBounds(insert_count <= max_size() - _size);
        const size_t new_cap = grown_capacity(_size + insert_count);
        pointer target = Traits::allocate(_allocator, new_cap);

        // Construct inserted elements first: their arguments may refer to an
        // element in the old storage, which the following moves destroy.
        std::forward<ConstructFn>(construct_fn)(target + insert_index);
        if (insert_index != 0) {
            relocate_elements(target, _data, insert_index);
        }
        if (insert_index != _size) {
            relocate_elements(target + insert_index + insert_count, _data + insert_index, _size - insert_index);
        }
        if (!IsInline()) {
            Traits::deallocate(_allocator, _data, _capacity);
        }
        _data     = target;
        _capacity = new_cap;
        _size += insert_count;
    }

    template <typename... Args>
    constexpr void grow_and_emplace(size_t index, Args&&... args) {
        relocate_reallocate(index, 1, [&](pointer dst) -> auto { Traits::construct(_allocator, dst, std::forward<Args>(args)...); });
    }

    template <typename InputIt>
    constexpr void grow_and_insert_range(size_t index, InputIt first, size_t count) {
        relocate_reallocate(index, count, [&](pointer dst) -> auto {
            for (size_t i = 0; i < count; ++i) {
                Traits::construct(_allocator, dst + i, *first);
                first++;
            }
        });
    }

    constexpr void grow_and_insert_value(size_t index, size_t count, const T& value) {
        relocate_reallocate(index, count, [&](pointer dst) -> auto {
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
        if (start == end) {
            return;
        }
        if constexpr (std::is_trivially_copyable_v<T>) {
            std::memcpy(static_cast<void*>(dst), static_cast<const void*>(start), (end - start) * sizeof(T));
        } else {
            while (start != end) {
                Traits::construct(_allocator, dst, *start);
                start++;
                dst++;
            }
        }
    }

    constexpr void copy_construct_range_value(pointer start, pointer end, const T& value) {
        if (start == end) {
            return;
        }
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
        if constexpr (std::is_trivially_copyable_v<T> && std::is_trivially_default_constructible_v<T>) {
            std::memset(static_cast<void*>(start), 0, (end - start) * sizeof(T));
        } else {
            while (start != end) {
                Traits::construct(_allocator, start);
                start++;
            }
        }
    }
};

}
