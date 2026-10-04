// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Core suite for the structure-of-arrays layout and the arenas behind it:
// ZHLN::LinearArena, ZHLN::WorkerScratchPool, ZHLN::SoABlock and
// ZHLN::ECS::SoAScratch. The reflected layout is the part no other suite covers,
// because it is the part that only exists once a component type is reflected:
// what is pinned here is that a synthesized stream struct addresses the same
// fields as the component it was transformed from, and that the arena hands the
// block memory it can actually use at the alignment the layout asked for.
#include "TestsFramework.hpp"
#include <Zahlen/Config.hpp>
#include <Zahlen/Core/Reflection/Core.hpp>
#include <Zahlen/Core/Arena.hpp>
#include <Zahlen/Core/SoA.hpp>
#include <Zahlen/ecs/SystemParameters.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <type_traits>

namespace {

enum class SoATestError : uint8_t {
    ArenaAlignmentFailed ZHLN_ANNOTATION(ZHLN::Description<"An arena allocation did not come back at the requested alignment."> {}) = 1,
    ArenaResetFailed ZHLN_ANNOTATION(ZHLN::Description<"Reset did not hand the whole arena back."> {}) = 2,
    PoolIsolationFailed ZHLN_ANNOTATION(ZHLN::Description<"Two worker indices resolved to the same arena, or a reset missed one."> {}) = 3,
    LayoutSizeMismatch ZHLN_ANNOTATION(ZHLN::Description<"ComputeByteSize and Bind disagree about the layout they describe."> {}) = 4,
    StreamAliasingDetected ZHLN_ANNOTATION(ZHLN::Description<"Two streams of one block overlap in memory."> {}) = 5,
    ProxyAccessFailed ZHLN_ANNOTATION(ZHLN::Description<"Writing through the proxy reference did not reach the stream."> {}) = 6,
    ScratchBoundsFailed ZHLN_ANNOTATION(ZHLN::Description<"SoAScratch accepted or reported a size outside its capacity."> {}) = 7,
};

// A stand-in for an ECS component: plain aggregate, named members, no references.
struct SoAPosition {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct SoAElement {
    float    value = 0.0f;
    uint32_t tag   = 0u;
};

struct SoATestSuite {
    struct Tests {
        std::expected<void, ZHLN::ErrorCode> arena_allocates_at_the_requested_alignment() {
            ZHLN::LinearArena arena {64 * 1024};

            void*  first     = arena.Allocate(3, 1);
            void*  overAligned = arena.Allocate(128, 128);
            void*  tail      = arena.Allocate(16, 8);

            if (reinterpret_cast<uintptr_t>(first) % 1 != 0 || reinterpret_cast<uintptr_t>(overAligned) % 128 != 0) {
                return std::unexpected(SoATestError::ArenaAlignmentFailed);
            }
            if (reinterpret_cast<uintptr_t>(tail) % 8 != 0) return std::unexpected(SoATestError::ArenaAlignmentFailed);
            if (arena.AllocatedBytes() == 0 || arena.AllocatedBytes() > arena.Capacity()) return std::unexpected(SoATestError::ArenaResetFailed);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> reset_reuses_the_block_from_the_start() {
            ZHLN::LinearArena arena {4096};

            void* first = arena.Allocate(1024, 16);
            const size_t afterFirst = arena.AllocatedBytes();
            arena.Reset();
            if (arena.AllocatedBytes() != 0) return std::unexpected(SoATestError::ArenaResetFailed);

            void* second = arena.Allocate(1024, 16);
            if (second != first) return std::unexpected(SoATestError::ArenaResetFailed);
            if (arena.AllocatedBytes() != afterFirst) return std::unexpected(SoATestError::ArenaResetFailed);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> arena_move_transfers_ownership_once() {
            ZHLN::LinearArena source(4096);
            void*               pointer  = source.Allocate(64, 64);
            ZHLN::LinearArena   moved    = std::move(source);
            void*               fromMoved = moved.Allocate(64, 64);

            if (fromMoved == pointer) return std::unexpected(SoATestError::ArenaResetFailed);
            if (moved.Capacity() != 4096 || source.Capacity() != 0) return std::unexpected(SoATestError::ArenaResetFailed);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> worker_pool_keeps_one_arena_per_index() {
            ZHLN::WorkerScratchPool pool(8192, 3);
            if (pool.WorkerCount() != 3 || pool.PerWorkerCapacity() != 8192) return std::unexpected(SoATestError::PoolIsolationFailed);

            void* zero = pool.GetWorkerArena(0).Allocate(256, 64);
            void* two  = pool.GetWorkerArena(2).Allocate(256, 64);
            if (zero == two) return std::unexpected(SoATestError::PoolIsolationFailed);

            pool.ResetAll();
            for (size_t index = 0; index < pool.WorkerCount(); ++index) {
                if (pool.GetWorkerArena(static_cast<uint32_t>(index)).AllocatedBytes() != 0) {
                    return std::unexpected(SoATestError::PoolIsolationFailed);
                }
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> pool_sizes_itself_for_at_least_one_worker() {
            ZHLN::WorkerScratchPool pool(1024, 0);
            if (pool.WorkerCount() != 1) return std::unexpected(SoATestError::PoolIsolationFailed);
            // A worker index the pool was not sized for is a panic, not a silent
            // miss, so the contract tested here is only the clamp above.
            (void)pool.GetWorkerArena(0).Allocate(64, 8);
            return {};
        }

#if ZHLN_REFLECTION_AVAILABLE

        std::expected<void, ZHLN::ErrorCode> block_layout_matches_its_own_size_calculation() {
            constexpr size_t         Capacity = 128;
            // Braces, not parentheses: a parenthesized call result here is
            // disambiguated as a function declaration, which puts the member
            // lookup -- and with it the instantiation that defines the stream
            // struct -- in a parameter type, where defining a class is ill-formed.
            ZHLN::LinearArena        arena {ZHLN::SoABlock<SoAPosition>::ComputeByteSize(Capacity)};
            ZHLN::SoABlock<SoAPosition> block = ZHLN::SoABlock<SoAPosition>::Bind(
                arena.Allocate(ZHLN::SoABlock<SoAPosition>::ComputeByteSize(Capacity), ZHLN::SoABlock<SoAPosition>::StreamAlignment), Capacity
            );

            if (block.Capacity() != Capacity) return std::unexpected(SoATestError::LayoutSizeMismatch);
            if (arena.AllocatedBytes() != ZHLN::SoABlock<SoAPosition>::ComputeByteSize(Capacity)) {
                return std::unexpected(SoATestError::LayoutSizeMismatch);
            }
            // The three streams are three separate arrays, each cache-line
            // aligned: the first two element addresses must not overlap.
            auto& streams = block.GetStreams();
            const auto xAt = reinterpret_cast<uintptr_t>(streams.x);
            const auto yAt = reinterpret_cast<uintptr_t>(streams.y);
            if (xAt % ZHLN::SoABlock<SoAPosition>::StreamAlignment != 0) return std::unexpected(SoATestError::ArenaAlignmentFailed);
            if (yAt < xAt + sizeof(float) * Capacity) return std::unexpected(SoATestError::StreamAliasingDetected);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> proxy_writes_reach_the_streams_and_nothing_else() {
            constexpr size_t         Capacity = 8;
            ZHLN::LinearArena        arena {ZHLN::SoABlock<SoAElement>::ComputeByteSize(Capacity)};
            ZHLN::SoABlock<SoAElement> block = ZHLN::SoABlock<SoAElement>::Bind(
                arena.Allocate(ZHLN::SoABlock<SoAElement>::ComputeByteSize(Capacity), ZHLN::SoABlock<SoAElement>::StreamAlignment), Capacity
            );

            block.Set(2, SoAElement {.value = 1.5f, .tag = 7u});
            auto& streams = block.GetStreams();
            if (streams.value[2] != 1.5f || streams.tag[2] != 7u) return std::unexpected(SoATestError::ProxyAccessFailed);

            // One element's fields are in different arrays: writing element 2 did
            // not disturb element 3, which is the whole point of the layout.
            auto element3View = block[3]; // a view: the proxy's members are references
            element3View.value = -1.0f;
            if (streams.value[2] != 1.5f || streams.value[3] != -1.0f) return std::unexpected(SoATestError::StreamAliasingDetected);
            if (streams.tag[3] != 0u) return std::unexpected(SoATestError::ProxyAccessFailed);

            // The proxy is a view: Set/Get round-trip one element as the
            // component type, the shape a caller that has a component in hand
            // needs and the proxy cannot express.
            block.Set(1, SoAElement {.value = 2.5f, .tag = 4u});
            const SoAElement readBack = block.Get(1);
            if (readBack.value != 2.5f || readBack.tag != 4u) return std::unexpected(SoATestError::ProxyAccessFailed);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> scratch_reports_and_bounds_its_fill() {
            constexpr size_t                      Capacity = 4;
            ZHLN::LinearArena                     arena {ZHLN::SoABlock<SoAElement>::ComputeByteSize(Capacity)};
            ZHLN::ECS::SoAScratch<SoAElement, Capacity> scratch {
                arena.Allocate(ZHLN::SoABlock<SoAElement>::ComputeByteSize(Capacity), ZHLN::SoABlock<SoAElement>::StreamAlignment)
            };

            if (scratch.capacity() != Capacity) return std::unexpected(SoATestError::ScratchBoundsFailed);
            if (scratch.size() != 0) return std::unexpected(SoATestError::ScratchBoundsFailed);

            scratch[0].value = 3.0f;
            scratch.set_size(1);
            if (scratch.size() != 1) return std::unexpected(SoATestError::ScratchBoundsFailed);
            if (scratch.GetStreams().value[0] != 3.0f) return std::unexpected(SoATestError::ProxyAccessFailed);
            return {};
        }

#endif

    };
};

#if ZHLN_REFLECTION_AVAILABLE

// The transform is a compiled-in property of the plan: an SoA block of a
// reference-carrying or non-aggregate type must not be instantiable at all.
struct NotAnAggregate {
    explicit NotAnAggregate(float) {
    }
    float value = 0.0f;
};

static_assert(ZHLN::Reflect::IsStreamableStruct<SoAPosition>());
static_assert(!ZHLN::Reflect::IsStreamableStruct<NotAnAggregate>());
static_assert(!ZHLN::Reflect::IsStreamableStruct<float>());
static_assert(std::is_same_v<ZHLN::SoABlock<SoAPosition>::ProxyRef, ZHLN::Reflect::TransformedStruct<SoAPosition, std::add_lvalue_reference_t>>);

#endif

} // namespace

auto RunSoASuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<SoATestSuite>();
}
