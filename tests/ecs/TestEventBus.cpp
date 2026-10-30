// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/ecs/EventBus.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace ZHLN::Test {

static_assert(!std::is_copy_constructible_v<ECS::EventBus>);
static_assert(!std::is_copy_assignable_v<ECS::EventBus>);
static_assert(std::is_nothrow_move_constructible_v<ECS::EventBus>);
static_assert(std::is_nothrow_move_assignable_v<ECS::EventBus>);

namespace {

struct DamageEvent {
    uint32_t entityIndex = 0;
    int32_t  amount      = 0;
};

struct alignas(64) CacheLineEvent {
    uint64_t sequence = 0;
    uint64_t payload  = 0;
};

struct TrackedPayloadEvent {
    inline static int liveInstances = 0;

    std::string label;
    int         code = 0;

    TrackedPayloadEvent(std::string l, int c) : label(std::move(l)), code(c) {
        ++liveInstances;
    }

    TrackedPayloadEvent(const TrackedPayloadEvent& other) : label(other.label), code(other.code) {
        ++liveInstances;
    }

    TrackedPayloadEvent(TrackedPayloadEvent&& other) noexcept : label(std::move(other.label)), code(other.code) {
        ++liveInstances;
    }

    auto operator=(const TrackedPayloadEvent&) -> TrackedPayloadEvent&     = default;
    auto operator=(TrackedPayloadEvent&&) noexcept -> TrackedPayloadEvent& = default;

    ~TrackedPayloadEvent() {
        --liveInstances;
    }
};

} // namespace

void test_event_bus_repeated_drains_and_destructor_accounting() {
    TrackedPayloadEvent::liveInstances = 0;
    {
        ECS::EventBus bus;
        Expect(bus.ChannelCount()) == 0u;
        Expect(bus.Empty<DamageEvent>());

        bus.Emplace<TrackedPayloadEvent>("alpha", 1);
        bus.Push(TrackedPayloadEvent {"beta", 2});
        Expect(bus.ChannelCount()) == 1u;
        Expect(bus.Count<TrackedPayloadEvent>()) == 2u;
        Expect(TrackedPayloadEvent::liveInstances) == 2;

        // Repeated drain/push cycles deliver each event exactly once, regardless
        // of how the bus stores or reuses its backing memory.
        for (uint32_t frame = 0; frame < 64; ++frame) {
            bus.Push(DamageEvent {.entityIndex = frame, .amount = static_cast<int32_t>(frame * 3)});
            const size_t drained = bus.Drain<DamageEvent>([&](const DamageEvent& ev) {
                Expect(ev.entityIndex) == frame;
                Expect(ev.amount) == static_cast<int32_t>(frame * 3);
            });
            Expect(drained) == 1u;
            Expect(bus.Empty<DamageEvent>());
        }

        // Draining TrackedPayloadEvent destroys every visited object.
        std::vector<int> codes;
        const size_t     drainedTracked = bus.Drain<TrackedPayloadEvent>([&](const TrackedPayloadEvent& ev) {
            codes.push_back(ev.code);
        });
        Expect(drainedTracked) == 2u;
        Expect(codes.size()) == 2u;
        Expect(codes[0]) == 1;
        Expect(codes[1]) == 2;
        Expect(TrackedPayloadEvent::liveInstances) == 0;

        // ClearEvents destroys pending payloads while keeping channels registered.
        bus.Emplace<TrackedPayloadEvent>("gamma", 3);
        Expect(TrackedPayloadEvent::liveInstances) == 1;
        bus.ClearEvents();
        Expect(TrackedPayloadEvent::liveInstances) == 0;
        Expect(bus.ChannelCount()) == 2u;

        // Clear releases all channels and mapped slab pages.
        bus.Emplace<TrackedPayloadEvent>("delta", 4);
        Expect(TrackedPayloadEvent::liveInstances) == 1;
        bus.Clear();
        Expect(TrackedPayloadEvent::liveInstances) == 0;
        Expect(bus.ChannelCount()) == 0u;
    }
    Expect(TrackedPayloadEvent::liveInstances) == 0;
}

void test_event_bus_reentrant_push_multi_page_growth_and_move() {
    ECS::EventBus bus;

    // Over-aligned 64-byte event type + multi-page growth (2048 events = 128 KiB).
    for (uint64_t i = 0; i < 2048; ++i) {
        bus.Push(CacheLineEvent {.sequence = i, .payload = i ^ 0xA5A5u});
    }
    const auto span = bus.View<CacheLineEvent>();
    Expect(span.size()) == 2048u;
    Expect((reinterpret_cast<uintptr_t>(span.data()) % alignof(CacheLineEvent)) == 0u);
    Expect(span[0].sequence) == 0u;
    Expect(span[2047].sequence) == 2047u;

    // Move construction transfers all channels and pending events.
    ECS::EventBus movedBus(std::move(bus));
    Expect(bus.ChannelCount()) == 0u;
    Expect(bus.Empty<CacheLineEvent>());
    Expect(movedBus.Count<CacheLineEvent>()) == 2048u;

    // Re-entrant Push during Drain lands in the next batch without invalidating the active walk.
    movedBus.Push(DamageEvent {.entityIndex = 1, .amount = 5});
    movedBus.Push(DamageEvent {.entityIndex = 2, .amount = 7});

    int32_t firstPassSum = 0;
    const size_t firstDrained = movedBus.Drain<DamageEvent>([&](const DamageEvent& ev) {
        firstPassSum += ev.amount;
        if (ev.entityIndex == 1) {
            movedBus.Push(DamageEvent {.entityIndex = 99, .amount = 100});
        }
    });
    Expect(firstDrained) == 2u;
    Expect(firstPassSum) == 12;
    Expect(movedBus.Count<DamageEvent>()) == 1u;

    const auto reader = movedBus.Reader<DamageEvent>();
    Expect(!reader.Empty());
    Expect(reader.Size()) == 1u;
    Expect(reader[0].entityIndex) == 99u;
    Expect(reader[0].amount) == 100;
}

} // namespace ZHLN::Test
