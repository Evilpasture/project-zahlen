// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Atomic.hpp>
#include <Zahlen/Core/Defer.hpp>
#include <Zahlen/Core/Math.hpp>
#include <Zahlen/Core/Pages.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Sync.hpp>
#include <Zahlen/Threading/Mutex.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

namespace ZHLN::ECS {

template <typename T>
class EventReader;

template <typename T>
class EventWriter;

// Page-slab-backed typed ECS event bus.
//
// Design:
//   * Channel descriptors are bump-allocated from page-aligned slabs, and each
//     typed channel owns a pair of contiguous buffers (`primary` and `scratch`).
//     The default memory resource maps OS pages; callers can supply a resource
//     to observe or customize all bus-owned allocations without examining the
//     buffers or changing the event API.
//   * Zero steady-state allocation across frames and `Drain<T>()`: draining swaps
//     `primary` with `scratch`, visits the captured events while allowing re-entrant
//     `Push<T>()` calls to land cleanly in the fresh `primary` buffer, destroys the
//     visited payloads, and retains both page mappings for reuse on future frames.
//   * Lock-free channel directory reads: channel nodes live at permanent slab
//     addresses in an atomic prepend-only intrusive list, so `View<T>()` and
//     warm `Writer<T>()` / `Push<T>()` lookups never acquire a mutex, and
//     `SystemGraph` can safely run `EventWriter<A>` and `EventWriter<B>` (or
//     multiple `EventReader<A>`s) concurrently across worker fibers.
class EventBus {
  public:
    // The supplied resource must return memory aligned to GetPageSize() for
    // allocations of page-rounded byte counts. It remains caller-owned and must
    // outlive this bus (including any bus the contents are moved into). A null
    // resource uses the OS-page-backed default.
    EventBus() noexcept: EventBus(nullptr) {
    }
    explicit EventBus(std::pmr::memory_resource* pages) noexcept:
        _pages(pages != nullptr ? pages : DefaultPageResource()) {
    }
    ~EventBus() noexcept {
        Clear();
    }

    EventBus(const EventBus&)                    = delete;
    auto operator=(const EventBus&) -> EventBus& = delete;

    EventBus(EventBus&& other) noexcept
        : _pages(other._pages),
          _head(other._head.exchange(nullptr, std::memory_order::acq_rel)),
          _slab(std::exchange(other._slab, nullptr)),
          _channelCount(other._channelCount.exchange(0, std::memory_order::acq_rel)) {
    }

    auto operator=(EventBus&& other) noexcept -> EventBus& {
        if (this != &other) {
            Clear();
            _pages = other._pages;
            _head.store(other._head.exchange(nullptr, std::memory_order::acq_rel), std::memory_order::release);
            _slab = std::exchange(other._slab, nullptr);
            _channelCount.store(other._channelCount.exchange(0, std::memory_order::acq_rel), std::memory_order::release);
        }
        return *this;
    }

    template <typename T>
        requires std::copy_constructible<T>
    void Push(const T& event) {
        static_assert(std::is_object_v<T> && !std::is_const_v<T>, "Event types must be non-const value types");
        EnsureChannel<T>().Emplace(event);
    }

    template <typename T>
        requires std::move_constructible<T>
    void Push(T&& event)
        requires(!std::is_lvalue_reference_v<T>)
    {
        static_assert(std::is_object_v<T> && !std::is_const_v<T>, "Event types must be non-const value types");
        EnsureChannel<T>().Emplace(std::move(event));
    }

    template <typename T, typename... Args>
        requires std::constructible_from<T, Args...>
    auto Emplace(Args&&... args) -> T& {
        static_assert(std::is_object_v<T> && !std::is_const_v<T>, "Event types must be non-const value types");
        return EnsureChannel<T>().Emplace(std::forward<Args>(args)...);
    }

    template <typename T>
        requires std::copy_constructible<T>
    void PushRange(std::span<const T> events) {
        static_assert(std::is_object_v<T> && !std::is_const_v<T>, "Event types must be non-const value types");
        if (events.empty()) {
            return;
        }
        EnsureChannel<T>().AppendRange(events);
    }

    // Pending events of type `T` in push order (empty if none are queued).
    template <typename T>
    [[nodiscard]] auto View() const noexcept -> std::span<const T> {
        static_assert(std::is_object_v<T> && !std::is_const_v<T>, "Event types must be non-const value types");
        if (const auto* channel = FindChannel<T>()) {
            return channel->View();
        }
        return {};
    }

    template <typename T>
    [[nodiscard]] auto Count() const noexcept -> size_t {
        return View<T>().size();
    }

    template <typename T>
    [[nodiscard]] auto Empty() const noexcept -> bool {
        return View<T>().empty();
    }

    template <typename T>
    [[nodiscard]] auto Reader() const noexcept -> EventReader<T>;

    template <typename T>
    [[nodiscard]] auto Writer() -> EventWriter<T>;

    // Visits every currently pending event of type `T` in push order and consumes
    // them while retaining the channel's mapped pages for reuse. Events pushed
    // re-entrantly from inside `fn` land in the next batch rather than invalidating
    // or disappearing during the current walk.
    template <typename T, typename Fn>
    auto Drain(Fn&& fn) -> size_t {
        static_assert(std::is_object_v<T> && !std::is_const_v<T>, "Event types must be non-const value types");
        auto* channel = FindChannel<T>();
        if (channel == nullptr) {
            return 0;
        }
        return channel->Drain(std::forward<Fn>(fn));
    }

    // Clears pending events for `T` while keeping its mapped page buffers warm.
    template <typename T>
    void ClearChannel() noexcept {
        static_assert(std::is_object_v<T> && !std::is_const_v<T>, "Event types must be non-const value types");
        if (auto* channel = FindChannel<T>()) {
            channel->ClearEvents();
        }
    }

    // Clears pending events across every registered channel while retaining all
    // channel descriptors and mapped page buffers for zero-allocation frame reuse.
    void ClearEvents() noexcept {
        for (ChannelNode* node = _head.load(std::memory_order::acquire); node != nullptr;
             node              = node->next.load(std::memory_order::acquire)) {
            node->clearEventsFn(node);
        }
    }

    // Drops every channel and releases all mapped pages, returning the bus to its
    // initial empty state.
    void Clear() noexcept {
        ChannelNode* node = _head.exchange(nullptr, std::memory_order::acq_rel);
        while (node != nullptr) {
            ChannelNode* next = node->next.load(std::memory_order::relaxed);
            node->destroyFn(node);
            node = next;
        }
        _channelCount.store(0, std::memory_order::release);

        SlabChunk* chunk = std::exchange(_slab, nullptr);
        while (chunk != nullptr) {
            SlabChunk*   prev  = chunk->prev;
            const size_t bytes = chunk->mappedBytes;
            std::destroy_at(chunk);
            _pages->deallocate(chunk, bytes, GetPageSize());
            chunk = prev;
        }
    }

    [[nodiscard]] auto ChannelCount() const noexcept -> size_t {
        return _channelCount.load(std::memory_order::acquire);
    }

  private:
    template <typename T>
    friend class EventWriter;

    class OsPageResource final : public std::pmr::memory_resource {
        auto do_allocate(size_t bytes, size_t alignment) -> void* override {
            Assert(alignment <= GetPageSize(), "EventBus page alignment exceeds the OS page size");
            void* const memory = AllocatePages(bytes);
            if (memory == nullptr) {
                Panic("EventBus could not map {} bytes of backing pages", bytes);
            }
            return memory;
        }

        void do_deallocate(void* memory, size_t bytes, size_t) override {
            FreePages(memory, bytes);
        }

        [[nodiscard]] auto do_is_equal(const std::pmr::memory_resource& other) const noexcept -> bool override {
            return this == &other;
        }
    };

    [[nodiscard]] static auto DefaultPageResource() noexcept -> std::pmr::memory_resource* {
        static OsPageResource pages;
        return &pages;
    }

    struct SlabChunk {
        SlabChunk* prev        = nullptr;
        size_t     mappedBytes = 0;
        size_t     offset      = 0;
    };

    struct ChannelNode {
        uint32_t             hash          = 0;
        Atomic<ChannelNode*> next          {nullptr};
        void (*destroyFn)(ChannelNode*) noexcept     = nullptr;
        void (*clearEventsFn)(ChannelNode*) noexcept = nullptr;
    };

    template <typename T>
    struct PageBuffer {
        std::pmr::memory_resource* pages       = nullptr;
        T*                         data        = nullptr;
        size_t                     count       = 0;
        size_t                     capacity    = 0;
        size_t                     mappedBytes = 0;

        explicit PageBuffer(std::pmr::memory_resource* resource) noexcept: pages(resource) {
        }
        ~PageBuffer() noexcept {
            ReleasePages();
        }

        PageBuffer(const PageBuffer&)                    = delete;
        auto operator=(const PageBuffer&) -> PageBuffer& = delete;

        PageBuffer(PageBuffer&& other) noexcept
            : pages(other.pages),
              data(std::exchange(other.data, nullptr)),
              count(std::exchange(other.count, 0)),
              capacity(std::exchange(other.capacity, 0)),
              mappedBytes(std::exchange(other.mappedBytes, 0)) {
        }

        auto operator=(PageBuffer&& other) noexcept -> PageBuffer& {
            if (this != &other) {
                ReleasePages();
                pages       = other.pages;
                data        = std::exchange(other.data, nullptr);
                count       = std::exchange(other.count, 0);
                capacity    = std::exchange(other.capacity, 0);
                mappedBytes = std::exchange(other.mappedBytes, 0);
            }
            return *this;
        }

        void Swap(PageBuffer& other) noexcept {
            std::swap(pages, other.pages);
            std::swap(data, other.data);
            std::swap(count, other.count);
            std::swap(capacity, other.capacity);
            std::swap(mappedBytes, other.mappedBytes);
        }

        void EnsureCapacity(size_t needed) {
            if (needed <= capacity) {
                return;
            }
            Assert(alignof(T) <= GetPageSize(), "EventBus::PageBuffer requires event alignment <= OS page size");

            const size_t minBytes  = std::max(GetPageSize(), needed * sizeof(T));
            const size_t nextBytes = AlignUpToPage(std::max(minBytes, mappedBytes * 2));
            void*        raw       = pages->allocate(nextBytes, GetPageSize());
            if (raw == nullptr) {
                Panic("EventBus could not map {} bytes of backing pages", nextBytes);
            }

            T* nextData = static_cast<T*>(raw);
            if (count > 0) {
                if constexpr (std::is_nothrow_move_constructible_v<T> || !std::is_copy_constructible_v<T>) {
                    std::uninitialized_move_n(data, count, nextData);
                } else {
                    std::uninitialized_copy_n(data, count, nextData);
                }
                if constexpr (!std::is_trivially_destructible_v<T>) {
                    std::destroy_n(data, count);
                }
            }
            if (data != nullptr) {
                pages->deallocate(data, mappedBytes, GetPageSize());
            }

            data        = nextData;
            mappedBytes = nextBytes;
            capacity    = nextBytes / sizeof(T);
        }

        template <typename... Args>
        auto EmplaceBack(Args&&... args) -> T& {
            EnsureCapacity(count + 1);
            T* slot = std::construct_at(data + count, std::forward<Args>(args)...);
            ++count;
            return *slot;
        }

        void AppendRange(std::span<const T> items)
            requires std::copy_constructible<T>
        {
            EnsureCapacity(count + items.size());
            std::uninitialized_copy_n(items.data(), items.size(), data + count);
            count += items.size();
        }

        void ClearRetainingPages() noexcept {
            if (count > 0) {
                if constexpr (!std::is_trivially_destructible_v<T>) {
                    std::destroy_n(data, count);
                }
                count = 0;
            }
        }

        void ReleasePages() noexcept {
            ClearRetainingPages();
            if (data != nullptr) {
                pages->deallocate(data, mappedBytes, GetPageSize());
                data        = nullptr;
                capacity    = 0;
                mappedBytes = 0;
            }
        }
    };

    template <typename T>
    struct TypedChannel final : ChannelNode {
        PageBuffer<T> primary;
        PageBuffer<T> scratch;
        bool          draining = false;

        explicit TypedChannel(uint32_t typeHash, std::pmr::memory_resource* pages) noexcept: primary(pages), scratch(pages) {
            hash          = typeHash;
            destroyFn     = &DestroySelf;
            clearEventsFn = &ClearSelf;
        }

        template <typename... Args>
        auto Emplace(Args&&... args) -> T& {
            return primary.EmplaceBack(std::forward<Args>(args)...);
        }

        void AppendRange(std::span<const T> events)
            requires std::copy_constructible<T>
        {
            primary.AppendRange(events);
        }

        [[nodiscard]] auto View() const noexcept -> std::span<const T> {
            return primary.count > 0 ? std::span<const T>(primary.data, primary.count) : std::span<const T> {};
        }

        void ClearEvents() noexcept {
            primary.ClearRetainingPages();
        }

        template <typename Fn>
        auto Drain(Fn&& fn) -> size_t {
            if (primary.count == 0) {
                return 0;
            }

            if (!draining) {
                draining = true;
                ZHLN::defer _([&]() noexcept {
                    scratch.ClearRetainingPages();
                    draining = false;
                });
                primary.Swap(scratch);
                const size_t drained = scratch.count;
                for (size_t i = 0; i < drained; ++i) {
                    fn(scratch.data[i]);
                }
                return drained;
            }

            // Re-entrant `Drain<T>` inside a `Drain<T>` callback: isolate into a
            // temporary page buffer so the outer walk's `scratch` stays intact.
            PageBuffer<T> nested {primary.pages};
            primary.Swap(nested);
            const size_t drained = nested.count;
            for (size_t i = 0; i < drained; ++i) {
                fn(nested.data[i]);
            }
            return drained;
        }

        static void DestroySelf(ChannelNode* base) noexcept {
            std::destroy_at(static_cast<TypedChannel<T>*>(base));
        }

        static void ClearSelf(ChannelNode* base) noexcept {
            static_cast<TypedChannel<T>*>(base)->ClearEvents();
        }
    };

    [[nodiscard]] auto AllocateInSlabLocked(size_t bytes, size_t alignment) -> void* {
        Assert(alignment <= GetPageSize(), "EventBus channel alignment exceeds OS page size");

        if (_slab != nullptr) {
            const size_t aligned = Math::AlignUp(_slab->offset, alignment);
            if (aligned + bytes <= _slab->mappedBytes) {
                _slab->offset = aligned + bytes;
                return reinterpret_cast<std::byte*>(_slab) + aligned;
            }
        }

        const size_t headerEnd  = Math::AlignUp(sizeof(SlabChunk), alignment);
        const size_t chunkBytes = AlignUpToPage(std::max(GetPageSize(), headerEnd + bytes));
        void*        raw        = _pages->allocate(chunkBytes, GetPageSize());
        if (raw == nullptr) {
            Panic("EventBus could not map {} bytes for channel slab", chunkBytes);
        }

        auto* chunk = std::construct_at(
            static_cast<SlabChunk*>(raw),
            SlabChunk {
                .prev        = _slab,
                .mappedBytes = chunkBytes,
                .offset      = headerEnd + bytes,
            }
        );
        _slab = chunk;
        return reinterpret_cast<std::byte*>(raw) + headerEnd;
    }

    template <typename T>
    [[nodiscard]] auto FindChannel() const noexcept -> const TypedChannel<T>* {
        const uint32_t target = GetTypeHash<T>();
        for (const ChannelNode* node = _head.load(std::memory_order::acquire); node != nullptr;
             node                    = node->next.load(std::memory_order::acquire)) {
            if (node->hash == target) {
                return static_cast<const TypedChannel<T>*>(node);
            }
        }
        return nullptr;
    }

    template <typename T>
    [[nodiscard]] auto FindChannel() noexcept -> TypedChannel<T>* {
        return const_cast<TypedChannel<T>*>(std::as_const(*this).FindChannel<T>());
    }

    template <typename T>
    auto EnsureChannel() -> TypedChannel<T>& {
        if (auto* existing = FindChannel<T>()) {
            return *existing;
        }
        return *Lock(_channelCreateMutex, [&]() -> TypedChannel<T>* {
            if (auto* existing = FindChannel<T>()) {
                return existing;
            }
            void* memory  = AllocateInSlabLocked(sizeof(TypedChannel<T>), alignof(TypedChannel<T>));
            auto* channel = std::construct_at(static_cast<TypedChannel<T>*>(memory), GetTypeHash<T>(), _pages);
            channel->next.store(_head.load(std::memory_order::relaxed), std::memory_order::relaxed);
            _head.store(channel, std::memory_order::release);
            _channelCount.fetch_add(1, std::memory_order::release);
            return channel;
        });
    }

    std::pmr::memory_resource* _pages        = nullptr;
    Atomic<ChannelNode*>      _head         {nullptr};
    SlabChunk*                 _slab         = nullptr;
    Atomic<size_t>             _channelCount {0};
    mutable Mutex              _channelCreateMutex;
};

// Read-only view of pending events of type `T` for ECS systems (`SystemGraph`).
// Multiple systems declaring `EventReader<T>` share `Access::Read` on `EventToken<T>`
// and run in parallel; they are automatically ordered against `EventWriter<T>`.
template <typename T>
class EventReader {
  public:
    using EventType = T;
    static_assert(std::is_object_v<T> && !std::is_const_v<T>, "EventReader<T> requires a non-const value type T");

    constexpr EventReader() noexcept = default;
    constexpr explicit EventReader(std::span<const T> events) noexcept : _events(events) {
    }
    explicit EventReader(const EventBus& bus) noexcept : _events(bus.View<T>()) {
    }

    [[nodiscard]] constexpr auto View() const noexcept -> std::span<const T> {
        return _events;
    }

    [[nodiscard]] constexpr auto Empty() const noexcept -> bool {
        return _events.empty();
    }
    [[nodiscard]] constexpr auto empty() const noexcept -> bool {
        return _events.empty();
    }

    [[nodiscard]] constexpr auto Size() const noexcept -> size_t {
        return _events.size();
    }
    [[nodiscard]] constexpr auto size() const noexcept -> size_t {
        return _events.size();
    }

    [[nodiscard]] constexpr auto begin() const noexcept -> const T* {
        return _events.data();
    }
    [[nodiscard]] constexpr auto end() const noexcept -> const T* {
        return _events.data() + _events.size();
    }

    [[nodiscard]] constexpr auto operator[](size_t index) const noexcept -> const T& {
        return _events[index];
    }

  private:
    std::span<const T> _events {};
};

// Direct channel writer/consumer handle for events of type `T` in ECS systems (`SystemGraph`).
// Declares `Access::Write` on `EventToken<T>` — serializing against other writers or readers
// of `T`, while running in parallel with writers/readers of unrelated event types `U != T`.
template <typename T>
class EventWriter {
  public:
    using EventType = T;
    static_assert(std::is_object_v<T> && !std::is_const_v<T>, "EventWriter<T> requires a non-const value type T");

    constexpr EventWriter() noexcept = default;
    explicit EventWriter(EventBus& bus) : _channel(&bus.EnsureChannel<T>()) {
    }

    [[nodiscard]] constexpr auto Valid() const noexcept -> bool {
        return _channel != nullptr;
    }

    void Push(const T& event) const
        requires std::copy_constructible<T>
    {
        Assert(_channel != nullptr, "EventWriter<T>::Push called on an unbound writer");
        _channel->Emplace(event);
    }

    void Push(T&& event) const
        requires std::move_constructible<T>
    {
        Assert(_channel != nullptr, "EventWriter<T>::Push called on an unbound writer");
        _channel->Emplace(std::move(event));
    }

    template <typename... Args>
        requires std::constructible_from<T, Args...>
    auto Emplace(Args&&... args) const -> T& {
        Assert(_channel != nullptr, "EventWriter<T>::Emplace called on an unbound writer");
        return _channel->Emplace(std::forward<Args>(args)...);
    }

    void PushRange(std::span<const T> events) const
        requires std::copy_constructible<T>
    {
        Assert(_channel != nullptr, "EventWriter<T>::PushRange called on an unbound writer");
        if (!events.empty()) {
            _channel->AppendRange(events);
        }
    }

    template <typename Fn>
    auto Drain(Fn&& fn) const -> size_t {
        Assert(_channel != nullptr, "EventWriter<T>::Drain called on an unbound writer");
        return _channel->Drain(std::forward<Fn>(fn));
    }

    void Clear() const noexcept {
        Assert(_channel != nullptr, "EventWriter<T>::Clear called on an unbound writer");
        _channel->ClearEvents();
    }

    [[nodiscard]] auto PendingCount() const noexcept -> size_t {
        return _channel != nullptr ? _channel->View().size() : 0;
    }

  private:
    friend class EventBus;
    using Channel = EventBus::TypedChannel<T>;

    constexpr explicit EventWriter(Channel* channel) noexcept : _channel(channel) {
    }

    Channel* _channel = nullptr;
};

template <typename T>
[[nodiscard]] inline auto EventBus::Reader() const noexcept -> EventReader<T> {
    return EventReader<T>(View<T>());
}

template <typename T>
[[nodiscard]] inline auto EventBus::Writer() -> EventWriter<T> {
    return EventWriter<T>(&EnsureChannel<T>());
}

} // namespace ZHLN::ECS
