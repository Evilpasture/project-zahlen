// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Pages.hpp>
#include <Zahlen/Log.hpp>
#include <algorithm>
#include <concepts>
#include <cstddef>
#include <limits>
#include <memory_resource>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace ZHLN {

// An STL allocator that requests whole, page-aligned mappings from a caller-
// owned memory_resource. Store the resource *in the allocator*: std::vector
// then carries the deallocator with its storage through moves and swaps.
// The resource must outlive every container using it.
template <typename T>
class PageAllocator {
  public:
    using value_type                             = T;
    using is_always_equal                        = std::false_type;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap            = std::true_type;

    explicit PageAllocator(std::pmr::memory_resource* pages = nullptr) noexcept:
        _pages(pages != nullptr ? pages : OsPageMemoryResource()) {
    }

    template <typename U>
    PageAllocator(const PageAllocator<U>& other) noexcept: _pages(other.resource()) {
    }

    [[nodiscard]] auto allocate(size_t count) -> T* {
        if (count == 0) {
            return nullptr;
        }
        if (alignof(T) > GetPageSize()) {
            Panic("PageAllocator cannot satisfy alignment {} (page size {})", alignof(T), GetPageSize());
        }
        if (count > max_size()) {
            Panic("PageAllocator request overflows page rounding: {} elements of {} bytes", count, sizeof(T));
        }
        const size_t bytes  = AlignUpToPage(count * sizeof(T));
        void* const  memory = _pages->allocate(bytes, GetPageSize());
        if (memory == nullptr) {
            Panic("PageAllocator could not map {} bytes of backing pages", bytes);
        }
        return static_cast<T*>(memory);
    }

    void deallocate(T* memory, size_t count) noexcept {
        if (memory != nullptr) {
            _pages->deallocate(memory, AlignUpToPage(count * sizeof(T)), GetPageSize());
        }
    }

    [[nodiscard]] auto max_size() const noexcept -> size_t {
        return (std::numeric_limits<size_t>::max() - (GetPageSize() - 1)) / sizeof(T);
    }

    [[nodiscard]] auto resource() const noexcept -> std::pmr::memory_resource* {
        return _pages;
    }

    template <typename U>
    [[nodiscard]] auto operator==(const PageAllocator<U>& other) const noexcept -> bool {
        return _pages == other.resource();
    }

  private:
    std::pmr::memory_resource* _pages;
};

// A vector whose *storage* grows in page-sized steps. std::vector, rather than
// this wrapper, owns element lifetime, copying, moves, and deallocation: no
// hand-rolled Rule-of-Five code for clients or for EventBus channels.
// clear() retains the allocation, while destruction releases it. The resource
// receives page-rounded byte counts and page alignment even for odd-sized T.
template <typename T>
class PageVector {
  public:
    using value_type      = T;
    using allocator_type  = PageAllocator<T>;
    using storage_type    = std::vector<T, allocator_type>;
    using iterator        = typename storage_type::iterator;
    using const_iterator  = typename storage_type::const_iterator;

    PageVector() noexcept = default;
    explicit PageVector(std::pmr::memory_resource* pages) noexcept: _items(allocator_type {pages}) {
    }

    [[nodiscard]] auto empty() const noexcept -> bool { return _items.empty(); }
    [[nodiscard]] auto size() const noexcept -> size_t { return _items.size(); }
    [[nodiscard]] auto capacity() const noexcept -> size_t { return _items.capacity(); }
    [[nodiscard]] auto data() noexcept -> T* { return _items.data(); }
    [[nodiscard]] auto data() const noexcept -> const T* { return _items.data(); }
    [[nodiscard]] auto resource() const noexcept -> std::pmr::memory_resource* { return _items.get_allocator().resource(); }
    [[nodiscard]] auto begin() noexcept -> iterator { return _items.begin(); }
    [[nodiscard]] auto end() noexcept -> iterator { return _items.end(); }
    [[nodiscard]] auto begin() const noexcept -> const_iterator { return _items.begin(); }
    [[nodiscard]] auto end() const noexcept -> const_iterator { return _items.end(); }
    [[nodiscard]] auto operator[](size_t index) noexcept -> T& { return _items[index]; }
    [[nodiscard]] auto operator[](size_t index) const noexcept -> const T& { return _items[index]; }
    [[nodiscard]] auto view() noexcept -> std::span<T> { return {_items.data(), _items.size()}; }
    [[nodiscard]] auto view() const noexcept -> std::span<const T> { return {_items.data(), _items.size()}; }

    // The underlying vector only knows about elements. Reserve enough elements
    // to fill the next page mapping, doubling mapped bytes when already warm.
    void reserve(size_t needed) {
        if (needed <= capacity()) {
            return;
        }
        const size_t maxSlots = std::min(_items.max_size(), _items.get_allocator().max_size());
        if (needed > maxSlots) {
            Panic("PageVector cannot reserve {} elements of {} bytes", needed, sizeof(T));
        }

        const size_t page      = GetPageSize();
        const size_t oldBytes  = AlignUpToPage(capacity() * sizeof(T));
        const size_t maxBytes  = std::numeric_limits<size_t>::max() - (page - 1);
        size_t       nextBytes = std::max(page, needed * sizeof(T));
        if (oldBytes != 0 && oldBytes <= maxBytes / 2) {
            nextBytes = std::max(nextBytes, oldBytes * 2);
        }
        nextBytes = AlignUpToPage(nextBytes);
        const size_t slots = std::max(needed, std::min(maxSlots, nextBytes / sizeof(T)));
        _items.reserve(slots);
    }

    template <typename... Args>
        requires std::constructible_from<T, Args...>
    auto emplace_back(Args&&... args) -> T& {
        reserve(size() + 1);
        return _items.emplace_back(std::forward<Args>(args)...);
    }

    void push_back(const T& item)
        requires std::copy_constructible<T>
    {
        emplace_back(item);
    }

    void push_back(T&& item)
        requires std::move_constructible<T>
    {
        emplace_back(std::move(item));
    }

    void append(std::span<const T> items)
        requires std::copy_constructible<T>
    {
        if (items.empty()) {
            return;
        }
        if (items.size() > _items.max_size() - size()) {
            Panic("PageVector append exceeds maximum size");
        }
        // Like std::vector::insert, the source must not alias this vector.
        reserve(size() + items.size());
        for (const T& item: items) {
            _items.emplace_back(item);
        }
    }

    void clear() noexcept { _items.clear(); }
    void swap(PageVector& other) noexcept { _items.swap(other._items); }

  private:
    storage_type _items;
};

} // namespace ZHLN
