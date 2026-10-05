// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once


#include <Zahlen/ecs/ECS.hpp>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace ZHLN::ECS {

// One queue per event type, keyed by the type's GetTypeHash -- the same consteval hash
// the registry uses for dense component IDs. A host pushes what happened and the systems
// that care drain it; extensions/UI's ActionRegistry is the worked example, where a
// document names an action and the host pushes the typed event it bound. There are no
// callbacks and no std::function in the path: an event type that was never pushed reads
// as empty rather than being an error.
//
// Nothing in src/ pushes to a bus today -- the consumers are app/ and extensions/UI -- and
// it stays in Core because that is where what it is built on lives (GetTypeHash) and
// because a Core header consumed only by the optional layers is an established thing here
// (Core/EnumFlags, Threading/Channel, physics/PhysicsHandles). Nothing includes it on a
// caller's behalf, not even ecs/ECS.hpp: a host that wants a bus names it.
//
// The queues are type-erased but owned. An entry is a {hash, unique_ptr} pair, and the
// queue it points at -- TypedQueue<T> -- holds its std::vector<T> inline. The per-queue
// cost matches the erased version this replaces (a queue object plus the vector's buffer,
// and a 16-byte entry instead of 24): what changes is that ownership is expressed in the
// type system -- a unique_ptr to a private polymorphic base instead of a void* with a
// hand-written destroy function -- so the destructor, the deleted copy and the move
// operations are the compiler's rather than written out. The version this replaces had to
// delete its move operations, because a defaulted move would have left two buses owning
// the same pointers. With the entries owning their queues, the defaults are what the
// class wants: moving a bus transfers the entries and leaves the source empty, so the
// moved-from destructor has nothing to free.
//
// The queues themselves are individually allocated, so a reference or pointer obtained
// from the bus stays valid across pushes of other event types; only the entry array moves.
class EventBus {
  public:
    EventBus() = default;
    ~EventBus() = default;

    EventBus(const EventBus&)                    = delete;
    auto operator=(const EventBus&) -> EventBus& = delete;
    EventBus(EventBus&&)                         = default;
    auto operator=(EventBus&&) -> EventBus&      = default;

    // Appends to the type's queue, created on first push.
    template <typename T>
    void Push(T event) {
        EnsureQueue<T>().items.push_back(std::move(event));
    }

    // The pending events of one type in push order, empty if none were pushed. The span
    // points into the queue, so the next Push<T> invalidates it.
    template <typename T>
    [[nodiscard]] auto View() const -> std::span<const T> {
        if (const auto* queue = FindQueue<T>()) {
            return std::span<const T>(queue->items);
        }
        return {};
    }

    // Hands every pending event of one type to @p fn, then drops them. The drain takes
    // the queue's buffer rather than clearing it in place, so a callback may push events
    // of the same type without invalidating the walk: they are seen by the next drain
    // rather than this one, and -- unlike clearing the queue after the walk -- they are
    // not lost. The events that were pending when the walk started are exactly the events
    // it visits. The cost of taking the buffer is that a queue pushed to again after a
    // drain allocates once more.
    template <typename T, typename Fn>
    void Drain(Fn&& fn) {
        auto* queue = FindQueue<T>();
        if (queue == nullptr) {
            return;
        }
        auto pending = std::move(queue->items);
        for (T& event: pending) {
            fn(event);
        }
    }

    // Drops every queue along with the events in them, returning the bus to the state it
    // was in before the first push. A queue is recreated by the next Push of its type.
    void Clear() {
        _queues.clear();
    }

  private:
    // What the bus needs of a queue it is not addressing by type: to destroy it. The
    // dynamic type is always a TypedQueue<T> whose T hashes to the entry's hash.
    struct Queue {
        virtual ~Queue() = default;
    };

    template <typename T>
    struct TypedQueue final: Queue {
        std::vector<T> items;
    };

    struct Entry {
        uint32_t               hash;
        std::unique_ptr<Queue> queue;
    };

    std::vector<Entry> _queues;

    // Type-keyed lookup, with the hash written and read together with its queue. The
    // static_cast back to TypedQueue<T> is sound because an entry is only ever found
    // under T's own hash -- the same collision exposure the registry's dense IDs have.
    // The entries are one per event type a host uses, so the scan is a handful of integer
    // compares with no hashing and no node chase; the indirection worth removing was the
    // one the hand-rolled erase added, not this loop.
    template <typename T>
    [[nodiscard]] auto FindQueue() const -> const TypedQueue<T>* {
        const uint32_t hash = GetTypeHash<T>();
        for (const Entry& entry: _queues) {
            if (entry.hash == hash) {
                return static_cast<const TypedQueue<T>*>(entry.queue.get());
            }
        }
        return nullptr;
    }

    // The const scan is the only one written; this is for the bus's own mutating users
    // (Drain and EnsureQueue), which own what they are handed.
    template <typename T>
    [[nodiscard]] auto FindQueue() -> TypedQueue<T>* {
        return const_cast<TypedQueue<T>*>(std::as_const(*this).FindQueue<T>());
    }

    template <typename T>
    auto EnsureQueue() -> TypedQueue<T>& {
        if (auto* existing = FindQueue<T>()) {
            return *existing;
        }
        _queues.push_back(Entry {.hash = GetTypeHash<T>(), .queue = std::make_unique<TypedQueue<T>>()});
        return static_cast<TypedQueue<T>&>(*_queues.back().queue);
    }
};

}
