// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Atomic.hpp>
#include <Zahlen/Core/Defer.hpp>
#include <Zahlen/Core/Math.hpp>
#include <Zahlen/Core/PageVector.hpp>
#include <Zahlen/Core/Pages.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Sync.hpp>
#include <Zahlen/Threading/Mutex.hpp>
#include <Zahlen/Threading/SharedMutex.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <algorithm>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <memory_resource>
#include <new>
#include <optional>
#include <span>
#include <thread>
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
//   * Channel descriptors are bump-allocated from page-aligned slabs. Each
//     typed channel owns two PageVectors (`primary` and `scratch`); vector
//     lifetime/move semantics live in that reusable core primitive, not here.
//     The default memory resource maps OS pages; callers can supply a resource
//     to observe or customize all bus-owned allocations without examining the
//     buffers or changing the event API.
//   * Zero steady-state allocation across frames and `Drain<T>()`: draining swaps
//     `primary` with `scratch`, visits the captured events while allowing re-entrant
//     `Push<T>()` calls to land cleanly in the fresh `primary` buffer, destroys the
//     visited payloads, and retains both page mappings for reuse on future frames.
//   * Lock-free channel directory reads: channel nodes live at permanent slab
//     addresses in an atomic prepend-only intrusive list. `View<T>()` and
//     warm `Push<T>()` lookups never acquire a mutex; checked writers take a
//     read lease, so `Clear()` and moves wait for their in-flight operations.
//     `SystemGraph` can run writers for distinct event types concurrently.
class EventBus {
  public:
    // The supplied resource must return memory aligned to GetPageSize() for
    // allocations of page-rounded byte counts. It remains caller-owned and must
    // outlive this bus (including any bus the contents are moved into). A null
    // resource uses the OS-page-backed default.
    EventBus() noexcept: EventBus(nullptr) {
    }
    explicit EventBus(std::pmr::memory_resource* pages) noexcept:
        _pages(pages != nullptr ? pages : OsPageMemoryResource()) {
    }
    ~EventBus() noexcept {
        Clear();
    }

    EventBus(const EventBus&)                    = delete;
    auto operator=(const EventBus&) -> EventBus& = delete;

    EventBus(EventBus&& other) noexcept: EventBus(other._pages) {
        TakeFrom(other);
    }

    auto operator=(EventBus&& other) noexcept -> EventBus& {
        if (this != &other) {
            MutexGuard init(_writerInitMutex);
            ClearLocked(); // Retire destination handles before adopting the source.
            TakeFrom(other);
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

    // Drops every channel and releases all mapped pages. Previously issued
    // writers fail safely; a subsequent Writer<T>() obtains a fresh binding.
    void Clear() noexcept {
        MutexGuard init(_writerInitMutex);
        ClearLocked();
    }

    [[nodiscard]] auto ChannelCount() const noexcept -> size_t {
        return _channelCount.load(std::memory_order::acquire);
    }

  private:
    template <typename T>
    friend class EventWriter;

    // Fiber stacks can resume on another worker thread. A TLS-only nesting
    // guard would lose an active lease on migration (or mistake a paused fiber
    // on this thread for the current one), so track the context in the binding.
    // Main fibers use OS thread identity, including before TaskSystem::Init.
    struct WriterCallContext {
        Fiber*          fiber = nullptr;
        std::thread::id thread {};
    };

    [[nodiscard]] static auto WriterContext() noexcept -> WriterCallContext {
        if (auto* fiber = GetCurrentFiber(); fiber != nullptr && !fiber->isMain) {
            return {.fiber = fiber};
        }
        return {.thread = std::this_thread::get_id()};
    }

    struct WriterCallFrame {
        WriterCallContext context;
        WriterCallFrame*  next = nullptr;
    };

    // Intrusively reference counted and allocated separately from _pages:
    // retained writers can outlive both the bus and its caller-owned resource.
    // The bus slot and each writer/operation own one reference; no event storage
    // lives here, only a checked bus reference and its lifecycle lock.
    struct WriterBinding {
        Atomic<size_t>   references {1};
        SharedMutex      lifecycle;
        Mutex            callsMutex {};
        WriterCallFrame* calls = nullptr;
        EventBus*        bus;

        explicit WriterBinding(EventBus& owner) noexcept: bus(&owner) {
        }

        void Retain() noexcept {
            const size_t previous = references.fetch_add(1, std::memory_order::relaxed);
            if (previous == std::numeric_limits<size_t>::max()) {
                Panic("EventBus writer binding reference count overflow");
            }
        }

        void Release() noexcept {
            if (references.fetch_sub(1, std::memory_order::acq_rel) == 1) {
                delete this;
            }
        }

        [[nodiscard]] auto HasActiveCall() noexcept -> bool {
            const auto context = WriterContext();
            MutexGuard guard(callsMutex);
            for (auto* frame = calls; frame != nullptr; frame = frame->next) {
                if (frame->context.fiber == context.fiber && frame->context.thread == context.thread) {
                    return true;
                }
            }
            return false;
        }

        void AddCall(WriterCallFrame& frame) noexcept {
            MutexGuard guard(callsMutex);
            frame.next = calls;
            calls      = &frame;
        }

        void RemoveCall(WriterCallFrame& frame) noexcept {
            MutexGuard guard(callsMutex);
            auto**     link = &calls;
            while (*link != &frame) {
                if (*link == nullptr) {
                    Panic("EventBus writer lease bookkeeping is inconsistent");
                }
                link = &(*link)->next;
            }
            *link = frame.next;
        }
    };

    // One owning pointer; copying it retains the binding, moving transfers the
    // reference, and the last release destroys it. Adopt takes the initial
    // reference produced by new (rather than incrementing it again).
    struct WriterBindingRef {
        WriterBindingRef() noexcept = default;

        [[nodiscard]] static auto Adopt(WriterBinding* binding) noexcept -> WriterBindingRef {
            return WriterBindingRef(binding);
        }

        WriterBindingRef(const WriterBindingRef& other) noexcept: _binding(other._binding) {
            if (_binding != nullptr) {
                _binding->Retain();
            }
        }

        WriterBindingRef(WriterBindingRef&& other) noexcept: _binding(std::exchange(other._binding, nullptr)) {
        }

        ~WriterBindingRef() noexcept {
            if (_binding != nullptr) {
                _binding->Release();
            }
        }

        auto operator=(WriterBindingRef other) noexcept -> WriterBindingRef& {
            std::swap(_binding, other._binding);
            return *this;
        }

        [[nodiscard]] explicit operator bool() const noexcept {
            return _binding != nullptr;
        }

        [[nodiscard]] auto operator->() const noexcept -> WriterBinding* {
            return _binding;
        }

      private:
        explicit WriterBindingRef(WriterBinding* binding) noexcept: _binding(binding) {
        }

        WriterBinding* _binding = nullptr;
    };

    // The slot lock serializes taking a new intrusive reference with releasing
    // the bus's last reference on Clear(). Never hold it while acquiring the
    // lifecycle lock: an active callback can request another writer while Clear
    // waits for that callback's read lease.
    struct WriterBindingSlot {
        mutable Mutex     mutex {};
        WriterBindingRef value;

        [[nodiscard]] auto load() const noexcept -> WriterBindingRef {
            MutexGuard guard(mutex);
            return value;
        }

        void store(WriterBindingRef next) noexcept {
            MutexGuard guard(mutex);
            value = std::move(next);
        }
    };

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
    struct TypedChannel final : ChannelNode {
        PageVector<T> primary;
        PageVector<T> scratch;
        bool          draining = false;

        explicit TypedChannel(uint32_t typeHash, std::pmr::memory_resource* pages) noexcept: primary(pages), scratch(pages) {
            hash          = typeHash;
            destroyFn     = &DestroySelf;
            clearEventsFn = &ClearSelf;
        }

        template <typename... Args>
        auto Emplace(Args&&... args) -> T& {
            return primary.emplace_back(std::forward<Args>(args)...);
        }

        void AppendRange(std::span<const T> events)
            requires std::copy_constructible<T>
        {
            primary.append(events);
        }

        [[nodiscard]] auto View() const noexcept -> std::span<const T> {
            return primary.empty() ? std::span<const T> {} : primary.view();
        }

        void ClearEvents() noexcept {
            primary.clear();
        }

        template <typename Fn>
        auto Drain(Fn&& fn) -> size_t {
            if (primary.empty()) {
                return 0;
            }

            if (!draining) {
                draining = true;
                ZHLN::defer _([&]() noexcept {
                    scratch.clear();
                    draining = false;
                });
                primary.swap(scratch);
                const size_t drained = scratch.size();
                for (size_t i = 0; i < drained; ++i) {
                    fn(scratch[i]);
                }
                return drained;
            }

            // Re-entrant `Drain<T>` inside a `Drain<T>` callback: isolate into a
            // temporary page buffer so the outer walk's `scratch` stays intact.
            PageVector<T> nested {primary.resource()};
            primary.swap(nested);
            const size_t drained = nested.size();
            for (size_t i = 0; i < drained; ++i) {
                fn(nested[i]);
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

    // Caller holds _writerInitMutex; keep it across the entire retire/transfer
    // sequence during move assignment, so a new Writer cannot be issued into
    // the destination between the two steps.
    void ClearLocked() noexcept {
        auto binding = _writerBinding.load();
        if (binding) {
            if (binding->HasActiveCall()) {
                Panic("EventBus::Clear cannot run inside an active EventWriter callback");
            }
            WriteLockGuard lifetime(binding->lifecycle);
            binding->bus = nullptr;
            _writerBinding.store({});
            ClearStorage();
        } else {
            ClearStorage();
        }
    }

    void ClearStorage() noexcept {
        ChannelNode* node = _head.exchange(nullptr, std::memory_order_acq_rel);
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

    void TakeFrom(EventBus& other) noexcept {
        MutexGuard init(other._writerInitMutex);
        auto       binding = other._writerBinding.load();
        if (binding) {
            if (binding->HasActiveCall()) {
                Panic("EventBus cannot move from inside an active EventWriter callback");
            }
            WriteLockGuard lifetime(binding->lifecycle);
            TransferStorage(other);
            other._writerBinding.store({});
            _writerBinding.store(binding);
            binding->bus = this;
        } else {
            TransferStorage(other);
        }
    }

    void TransferStorage(EventBus& other) noexcept {
        _pages = other._pages;
        _head.store(other._head.exchange(nullptr, std::memory_order::acq_rel), std::memory_order::release);
        _slab = std::exchange(other._slab, nullptr);
        _channelCount.store(other._channelCount.exchange(0, std::memory_order::acq_rel), std::memory_order::release);
    }

    template <typename T>
    [[nodiscard]] auto BindWriter() -> WriterBindingRef {
        // A drain callback may request another writer while Clear is waiting
        // for this fiber's read lease. The slot's snapshot stays owned even
        // when another fiber is retiring it; only the fiber that already holds
        // the read lease may bypass the initialization mutex.
        auto binding = _writerBinding.load();
        if (binding && binding->HasActiveCall()) {
            EnsureChannel<T>();
            return binding;
        }

        MutexGuard init(_writerInitMutex);
        binding = _writerBinding.load();
        if (!binding) {
            auto* fresh = new (std::nothrow) WriterBinding(*this);
            if (fresh == nullptr) {
                Panic("EventBus could not allocate writer binding");
            }
            binding = WriterBindingRef::Adopt(fresh);
            _writerBinding.store(binding);
        }
        SharedLockGuard lifetime(binding->lifecycle);
        EnsureChannel<T>(); // Preserve Writer<T>()'s eager channel registration.
        return binding;
    }

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

    std::pmr::memory_resource*                 _pages        = nullptr;
    Atomic<ChannelNode*>                      _head         {nullptr};
    SlabChunk*                                 _slab         = nullptr;
    Atomic<size_t>                             _channelCount {0};
    WriterBindingSlot                          _writerBinding;
    mutable Mutex                              _writerInitMutex {};
    mutable Mutex                              _channelCreateMutex {};
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

// Checked writer/consumer handle for events of type `T` in ECS systems (`SystemGraph`).
// Declares `Access::Write` on `EventToken<T>` — serializing against other writers or readers
// of `T`, while running in parallel with writers/readers of unrelated event types `U != T`.
// A checked logical handle, not a pointer into the bus's page slab. Copying a
// writer keeps only the small lifetime binding alive; Clear() closes that
// binding before freeing its channels. Moves transfer the binding to the new
// bus, so existing writers follow the moved events. The handle can outlive the
// bus *and its injected page resource* and still report expiry safely.
template <typename T>
class EventWriter {
  public:
    using EventType = T;
    static_assert(std::is_object_v<T> && !std::is_const_v<T>, "EventWriter<T> requires a non-const value type T");

    EventWriter() noexcept = default; // Unbound handles fail checked operations; Push always panics.
    explicit EventWriter(EventBus& bus): _binding(bus.BindWriter<T>()) {
    }

    [[nodiscard]] auto Valid() const noexcept -> bool {
        return Visit([](EventBus&) {});
    }

    [[nodiscard]] auto TryPush(const T& event) const -> bool
        requires std::copy_constructible<T>
    {
        return Visit([&](EventBus& bus) { bus.Push<T>(event); });
    }

    [[nodiscard]] auto TryPush(T&& event) const -> bool
        requires std::move_constructible<T>
    {
        return Visit([&](EventBus& bus) { bus.Push<T>(std::move(event)); });
    }

    void Push(const T& event) const
        requires std::copy_constructible<T>
    {
        if (!TryPush(event)) {
            Panic("EventWriter<T>::Push called with an expired or moved-from writer");
        }
    }

    void Push(T&& event) const
        requires std::move_constructible<T>
    {
        if (!TryPush(std::move(event))) {
            Panic("EventWriter<T>::Push called with an expired or moved-from writer");
        }
    }

    // Emplace deliberately does not return a reference into the bus's storage:
    // such a reference would become invalid after a push, drain, or Clear().
    template <typename... Args>
        requires std::constructible_from<T, Args...>
    [[nodiscard]] auto TryEmplace(Args&&... args) const -> bool {
        return Visit([&](EventBus& bus) { (void)bus.Emplace<T>(std::forward<Args>(args)...); });
    }

    template <typename... Args>
        requires std::constructible_from<T, Args...>
    void Emplace(Args&&... args) const {
        if (!TryEmplace(std::forward<Args>(args)...)) {
            Panic("EventWriter<T>::Emplace called with an expired or moved-from writer");
        }
    }

    [[nodiscard]] auto TryPushRange(std::span<const T> events) const -> bool
        requires std::copy_constructible<T>
    {
        return Visit([&](EventBus& bus) { bus.PushRange<T>(events); });
    }

    void PushRange(std::span<const T> events) const
        requires std::copy_constructible<T>
    {
        if (!TryPushRange(events)) {
            Panic("EventWriter<T>::PushRange called with an expired or moved-from writer");
        }
    }

    template <typename Fn>
    [[nodiscard]] auto TryDrain(Fn&& fn) const -> std::optional<size_t> {
        std::optional<size_t> result;
        if (!Visit([&](EventBus& bus) { result = bus.Drain<T>(std::forward<Fn>(fn)); })) {
            return std::nullopt;
        }
        return result;
    }

    template <typename Fn>
    auto Drain(Fn&& fn) const -> size_t {
        auto result = TryDrain(std::forward<Fn>(fn));
        if (!result.has_value()) {
            Panic("EventWriter<T>::Drain called with an expired or moved-from writer");
        }
        return *result;
    }

    [[nodiscard]] auto TryClear() const noexcept -> bool {
        return Visit([](EventBus& bus) { bus.ClearChannel<T>(); });
    }

    void Clear() const noexcept {
        if (!TryClear()) {
            Panic("EventWriter<T>::Clear called with an expired or moved-from writer");
        }
    }

    [[nodiscard]] auto TryPendingCount() const noexcept -> std::optional<size_t> {
        std::optional<size_t> count;
        if (!Visit([&](EventBus& bus) { count = bus.Count<T>(); })) {
            return std::nullopt;
        }
        return count;
    }

    [[nodiscard]] auto PendingCount() const noexcept -> size_t {
        const auto count = TryPendingCount();
        if (!count.has_value()) {
            Panic("EventWriter<T>::PendingCount called with an expired or moved-from writer");
        }
        return *count;
    }

  private:
    template <typename Fn>
    auto Visit(Fn&& fn) const -> bool {
        const auto binding = _binding;
        if (!binding) {
            return false;
        }

        // A Drain callback may push or drain the same type again. The outer
        // invocation already holds a read lease, so taking another while Clear
        // is waiting for a write lease would deadlock the current fiber.
        if (binding->HasActiveCall()) {
            std::forward<Fn>(fn)(*binding->bus);
            return true;
        }

        SharedLockGuard lifetime(binding->lifecycle);
        if (binding->bus == nullptr) {
            return false;
        }
        EventBus::WriterCallFrame frame {EventBus::WriterContext()};
        binding->AddCall(frame);
        ZHLN::defer _([&]() noexcept { binding->RemoveCall(frame); });
        std::forward<Fn>(fn)(*binding->bus);
        return true;
    }

    EventBus::WriterBindingRef _binding;
};

template <typename T>
[[nodiscard]] inline auto EventBus::Reader() const noexcept -> EventReader<T> {
    return EventReader<T>(View<T>());
}

template <typename T>
[[nodiscard]] inline auto EventBus::Writer() -> EventWriter<T> {
    return EventWriter<T>(*this);
}

} // namespace ZHLN::ECS
