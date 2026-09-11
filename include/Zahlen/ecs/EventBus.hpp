// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// ===========================================================================
// Typed event queues for the ECS.
//
// Systems do not call each other. A producer Push<T>()s a POD (or otherwise
// copyable) event; a consumer Drain<T>()s the queue later in the frame. The
// GUI ActionRegistry is one producer: a document names "inventory.use_potion"
// and Bind stores a UseItemEvent that Invoke copies onto this bus.
//
// Queues are keyed by GetTypeHash<T>(), not ComponentFamily, so event types
// do not steal sparse-set family ids from components.
// ===========================================================================

#include <Zahlen/ecs/ECS.hpp>
#include <span>
#include <utility>
#include <vector>

namespace ZHLN::ECS {

class EventBus {
  public:
    EventBus() = default;
    ~EventBus() {
        Clear();
    }

    EventBus(const EventBus&)                    = delete;
    auto operator=(const EventBus&) -> EventBus& = delete;
    EventBus(EventBus&&)                         = delete;
    auto operator=(EventBus&&) -> EventBus&      = delete;

    template <typename T>
    void Push(T event) {
        auto* q = static_cast<std::vector<T>*>(Ensure<T>().storage);
        q->push_back(std::move(event));
    }

    template <typename T>
    [[nodiscard]] auto View() const -> std::span<const T> {
        const Queue* slot = Find(GetTypeHash<T>());
        if (slot == nullptr || slot->storage == nullptr) {
            return {};
        }
        const auto* q = static_cast<const std::vector<T>*>(slot->storage);
        return {q->data(), q->size()};
    }

    /// Calls @p fn for each pending T, then drops that queue. Other types stay.
    template <typename T, typename Fn>
    void Drain(Fn&& fn) {
        Queue* slot = Find(GetTypeHash<T>());
        if (slot == nullptr || slot->storage == nullptr) {
            return;
        }
        auto* q = static_cast<std::vector<T>*>(slot->storage);
        for (T& event: *q) {
            fn(event);
        }
        q->clear();
    }

    template <typename T>
    void Clear() {
        Queue* slot = Find(GetTypeHash<T>());
        if (slot == nullptr || slot->storage == nullptr) {
            return;
        }
        static_cast<std::vector<T>*>(slot->storage)->clear();
    }

    void Clear() {
        for (Queue& slot: _queues) {
            if (slot.storage != nullptr && slot.destroy != nullptr) {
                slot.destroy(slot.storage);
                slot.storage = nullptr;
            }
        }
        _queues.clear();
    }

  private:
    struct Queue {
        uint32_t hash     = 0;
        void*    storage  = nullptr;
        void (*destroy)(void*) = nullptr;
    };

    std::vector<Queue> _queues;

    [[nodiscard]] auto Find(uint32_t hash) const -> const Queue* {
        for (const Queue& slot: _queues) {
            if (slot.hash == hash) {
                return &slot;
            }
        }
        return nullptr;
    }

    [[nodiscard]] auto Find(uint32_t hash) -> Queue* {
        return const_cast<Queue*>(std::as_const(*this).Find(hash));
    }

    template <typename T>
    auto Ensure() -> Queue& {
        const uint32_t hash = GetTypeHash<T>();
        if (Queue* existing = Find(hash)) {
            return *existing;
        }
        Queue slot;
        slot.hash    = hash;
        slot.storage = new std::vector<T>();
        slot.destroy = [](void* p) -> void { delete static_cast<std::vector<T>*>(p); };
        _queues.push_back(slot);
        return _queues.back();
    }
};

} // namespace ZHLN::ECS
