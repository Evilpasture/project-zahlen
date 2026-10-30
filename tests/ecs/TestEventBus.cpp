// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include "helpers/TrackingMemoryResource.hpp"
#include <Zahlen/ecs/EventBus.hpp>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
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
    TrackingMemoryResource pages;
    {
        ECS::EventBus bus {&pages};
        Expect(bus.ChannelCount()) == 0u;
        Expect(bus.Empty<DamageEvent>());

        bus.Emplace<TrackedPayloadEvent>("alpha", 1);
        bus.Push(TrackedPayloadEvent {"beta", 2});
        Expect(bus.ChannelCount()) == 1u;
        Expect(bus.Count<TrackedPayloadEvent>()) == 2u;
        Expect(TrackedPayloadEvent::liveInstances) == 2;

        // Repeated drain/push cycles deliver each event exactly once. After
        // the first two cycles have warmed both buffers, no more pages should
        // be requested from the injected provider.
        size_t warmedAllocations = 0;
        for (uint32_t frame = 0; frame < 64; ++frame) {
            bus.Push(DamageEvent {.entityIndex = frame, .amount = static_cast<int32_t>(frame * 3)});
            const size_t drained = bus.Drain<DamageEvent>([&](const DamageEvent& ev) {
                Expect(ev.entityIndex) == frame;
                Expect(ev.amount) == static_cast<int32_t>(frame * 3);
            });
            Expect(drained) == 1u;
            Expect(bus.Empty<DamageEvent>());
            if (frame == 1) {
                warmedAllocations = pages.Allocations();
            } else if (frame > 1) {
                Expect(pages.Allocations()) == warmedAllocations;
            }
        }
        ExpectGt(warmedAllocations, 0u);
        const std::array<DamageEvent, 3> batch {
            DamageEvent {.entityIndex = 101, .amount = 1},
            DamageEvent {.entityIndex = 102, .amount = 2},
            DamageEvent {.entityIndex = 103, .amount = 3},
        };
        bus.PushRange<DamageEvent>(batch);
        Expect(bus.Count<DamageEvent>()) == batch.size();
        Expect(pages.Allocations()) == warmedAllocations;
        size_t batchIndex = 0;
        Expect(bus.Drain<DamageEvent>([&](const DamageEvent& event) {
            Expect(event.entityIndex) == batch[batchIndex].entityIndex;
            Expect(event.amount) == batch[batchIndex].amount;
            ++batchIndex;
        })) == batch.size();
        Expect(batchIndex) == batch.size();

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
        Expect(pages.LiveBlocks()) == 0u;
    }
    Expect(TrackedPayloadEvent::liveInstances) == 0;
    Expect(pages.LiveBlocks()) == 0u;
    Expect(pages.LiveBytes()) == 0u;
    Expect(pages.Deallocations()) == pages.Allocations();
}

void test_event_bus_reentrant_push_multi_page_growth_and_move() {
    TrackingMemoryResource pages;
    ECS::EventBus          bus {&pages};

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

    // A nested drain owns its temporary page through the same injected
    // provider, even after the channel and bus have moved.
    const size_t freesBeforeNested = pages.Deallocations();
    size_t       nestedCount       = 0;
    Expect(movedBus.Drain<DamageEvent>([&](const DamageEvent& ev) {
        Expect(ev.entityIndex) == 99u;
        movedBus.Push(DamageEvent {.entityIndex = 100, .amount = 200});
        nestedCount += movedBus.Drain<DamageEvent>([&](const DamageEvent& inner) {
            Expect(inner.entityIndex) == 100u;
        });
    })) == 1u;
    Expect(nestedCount) == 1u;
    ExpectGt(pages.Deallocations(), freesBeforeNested);

    // Move assignment releases the destination's old resource before adopting
    // the source's; neither allocator is read through EventBus internals.
    TrackingMemoryResource destinationPages;
    ECS::EventBus          destination {&destinationPages};
    destination.Push(DamageEvent {.entityIndex = 5});
    ExpectGt(destinationPages.LiveBlocks(), 0u);
    destination = std::move(movedBus);
    Expect(destinationPages.LiveBlocks()) == 0u;
    ExpectGt(pages.LiveBlocks(), 0u);
    destination.Clear();
    Expect(pages.LiveBlocks()) == 0u;
    Expect(pages.LiveBytes()) == 0u;
    Expect(pages.Deallocations()) == pages.Allocations();
    Expect(destinationPages.Deallocations()) == destinationPages.Allocations();
}

void test_event_writer_checked_lifetime_and_moves() {
    static_assert(std::is_default_constructible_v<ECS::EventWriter<DamageEvent>>);
    static_assert(std::is_void_v<decltype(std::declval<ECS::EventWriter<TrackedPayloadEvent>>().Emplace(std::string {}, 1))>);
    ECS::EventWriter<DamageEvent> unbound;
    Expect(!unbound.Valid());
    Expect(!unbound.TryPush(DamageEvent {.entityIndex = 0}));
    Expect(!unbound.TryPendingCount().has_value());

    // The handle owns its tiny lifetime token, not the bus's page resource.
    std::optional<ECS::EventWriter<DamageEvent>> retained;
    {
        TrackingMemoryResource pages;
        {
            ECS::EventBus bus {&pages};
            auto          writer = bus.Writer<DamageEvent>();
            auto          copy   = writer;
            ECS::EventWriter<DamageEvent> assigned;
            assigned = copy;
            Expect(assigned.Valid());
            Expect(writer.Valid());
            writer.Push(DamageEvent {.entityIndex = 1});
            const DamageEvent second {.entityIndex = 2};
            Expect(copy.TryPush(second));
            Expect(writer.TryEmplace(3u, 0));
            const std::array<DamageEvent, 2> batch {DamageEvent {.entityIndex = 4}, DamageEvent {.entityIndex = 5}};
            Expect(writer.TryPushRange(batch));
            Expect(writer.PendingCount()) == 5u;
            Expect(writer.TryPendingCount().value()) == 5u;

            // Nested writer creation and draining reuse the active read lease
            // while preserving push order and captured-batch semantics.
            std::vector<uint32_t> seen;
            const auto drained = writer.TryDrain([&](const DamageEvent& event) {
                seen.push_back(event.entityIndex);
                if (event.entityIndex == 1) {
                    auto nestedWriter = bus.Writer<DamageEvent>();
                    nestedWriter.Push(DamageEvent {.entityIndex = 99});
                    const auto nested = copy.TryDrain([&](const DamageEvent& next) {
                        Expect(next.entityIndex) == 99u;
                    });
                    Expect(nested.value()) == 1u;
                }
            });
            Expect(drained.value()) == 5u;
            Expect(seen == std::vector<uint32_t>({1, 2, 3, 4, 5}));
            Expect(writer.TryDrain([](const DamageEvent&) {}).value()) == 0u;

            writer.Push(DamageEvent {.entityIndex = 6});
            Expect(writer.TryClear());
            Expect(writer.PendingCount()) == 0u;
            writer.Push(DamageEvent {.entityIndex = 7});
            bus.ClearEvents(); // Does not retire a writer or free its channel.
            Expect(writer.Valid());
            Expect(writer.PendingCount()) == 0u;

            bus.Clear(); // Old handles cannot silently rebind to new channels.
            Expect(!writer.Valid());
            Expect(!assigned.Valid());
            Expect(!writer.TryPush(DamageEvent {.entityIndex = 8}));
            Expect(!writer.TryEmplace(9u, 0));
            Expect(!writer.TryPushRange(batch));
            bool called = false;
            Expect(!writer.TryDrain([&](const DamageEvent&) { called = true; }).has_value());
            Expect(!called);
            Expect(!writer.TryClear());
            Expect(!writer.TryPendingCount().has_value());
            retained.emplace(copy);

            auto fresh       = bus.Writer<DamageEvent>();
            auto movedWriter = std::move(fresh);
            Expect(!fresh.Valid());
            Expect(!fresh.TryPush(DamageEvent {.entityIndex = 10}));
            assigned = movedWriter; // Replaces an expired generation with a live one.
            Expect(assigned.Valid());
            movedWriter.Push(DamageEvent {.entityIndex = 11});
            Expect(bus.Count<DamageEvent>()) == 1u;
            Expect(!writer.Valid());
        }
        Expect(pages.LiveBlocks()) == 0u;
    }
    Expect(!retained->Valid()); // Safe even after the caller-owned resource dies.
    Expect(!retained->TryPush(DamageEvent {.entityIndex = 12}));
    retained.reset();

    TrackingMemoryResource sourcePages;
    TrackingMemoryResource destinationPages;
    {
        ECS::EventBus source {&sourcePages};
        auto          sourceWriter = source.Writer<DamageEvent>();
        sourceWriter.Push(DamageEvent {.entityIndex = 13});
        ECS::EventBus moved(std::move(source));
        Expect(sourceWriter.Valid());
        sourceWriter.Push(DamageEvent {.entityIndex = 14});
        Expect(moved.Count<DamageEvent>()) == 2u;
        auto movedFromWriter = source.Writer<DamageEvent>();
        movedFromWriter.Push(DamageEvent {.entityIndex = 15});
        Expect(source.Count<DamageEvent>()) == 1u;

        ECS::EventBus destination {&destinationPages};
        auto          retiredDestination = destination.Writer<DamageEvent>();
        retiredDestination.Push(DamageEvent {.entityIndex = 16});
        destination = std::move(moved);
        Expect(!retiredDestination.Valid());
        Expect(!retiredDestination.TryPush(DamageEvent {.entityIndex = 17}));
        Expect(sourceWriter.Valid());
        sourceWriter.Push(DamageEvent {.entityIndex = 18});
        Expect(destination.Count<DamageEvent>()) == 3u;
        Expect(movedFromWriter.Valid());
        destination.Clear();
        Expect(!sourceWriter.Valid());
    }
    Expect(sourcePages.LiveBlocks()) == 0u;
    Expect(destinationPages.LiveBlocks()) == 0u;

    // Clear cannot free storage while a checked drain callback is still using
    // it. A re-entrant push inside that callback remains legal.
    ECS::EventBus concurrentBus;
    auto          concurrentWriter = concurrentBus.Writer<DamageEvent>();
    concurrentWriter.Push(DamageEvent {.entityIndex = 19});
    std::atomic<bool> entered {false};
    std::atomic<bool> proceed {false};
    std::atomic<bool> clearStarted {false};
    std::atomic<bool> clearFinished {false};
    std::atomic<bool> nestedSucceeded {false};
    std::atomic<bool> drainedOne {false};
    std::thread worker([&] {
        const auto result = concurrentWriter.TryDrain([&](const DamageEvent& event) {
            nestedSucceeded.store(event.entityIndex == 19u, std::memory_order_relaxed);
            entered.store(true, std::memory_order_release);
            while (!proceed.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            nestedSucceeded.store(nestedSucceeded.load() && concurrentWriter.TryPush(DamageEvent {.entityIndex = 20}),
                                  std::memory_order_relaxed);
        });
        drainedOne.store(result == 1u, std::memory_order_release);
    });
    while (!entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::thread clearer([&] {
        clearStarted.store(true, std::memory_order_release);
        concurrentBus.Clear();
        clearFinished.store(true, std::memory_order_release);
    });
    while (!clearStarted.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    Expect(!clearFinished.load());
    proceed.store(true, std::memory_order_release);
    worker.join();
    clearer.join();
    Expect(drainedOne.load());
    Expect(nestedSucceeded.load());
    Expect(clearFinished.load());
    Expect(!concurrentWriter.Valid());
}

} // namespace ZHLN::Test
