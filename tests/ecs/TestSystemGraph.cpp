// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include "helpers/MockTaskScheduler.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Frame.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <Zahlen/ECS/SystemGraph.hpp>
#include <Zahlen/ECS/SystemParameters.hpp>
#include <atomic>
#include <expected>
#include <thread>
#include <type_traits>
#include <utility>

enum class SystemGraphTestError : uint8_t {
    HazardMismatch ZHLN_ANNOTATION(ZHLN::Description<"SystemGraph failed to detect a Read/Write or Write/Write component conflict."> {}) = 1,
    ExecutionOrderFailed ZHLN_ANNOTATION(ZHLN::Description<"Systems were executed out of dependency order."> {}),
    ExternalWriteAnchorFailed ZHLN_ANNOTATION(ZHLN::Description<"The external-write anchor did not register, or broke dispatch to its dependents."> {}),
    DeclarativeSignatureFailed ZHLN_ANNOTATION(ZHLN::Description<"Reflected system name or derived access pattern was incorrect."> {}),
    QueryIterationFailed ZHLN_ANNOTATION(ZHLN::Description<"A typed query did not preserve component access or intersection semantics."> {}),
    ParameterResolutionFailed ZHLN_ANNOTATION(ZHLN::Description<"A signature-driven system received the wrong context value."> {}),
    SchedulingObserverFailed ZHLN_ANNOTATION(ZHLN::Description<"The injected scheduler did not observe graph dispatch and completion."> {}),
};

// Mock components for access hazard tracking
struct TestCompA {
    int value = 0;
};
struct TestCompB {
    int value = 0;
};

// Constants for test
constexpr float TestDeltaTime = 0.016f;

using ReadQuery = ZHLN::ECS::Query<const TestCompA&, const TestCompB>;
using WriteQuery = ZHLN::ECS::Query<TestCompA&>;
static_assert(std::is_same_v<decltype(std::declval<ReadQuery>().Get<TestCompA>(ZHLN::Entity {})), ZHLN::Optional<const TestCompA&>>);
static_assert(std::is_same_v<decltype(std::declval<ReadQuery>().Raw<TestCompA>()), ZHLN::RestrictSpan<const TestCompA>>);
static_assert(std::is_same_v<decltype(std::declval<WriteQuery>().Get<TestCompA>(ZHLN::Entity {})), ZHLN::Optional<TestCompA&>>);

template <typename Q>
concept CanReadB = requires(Q q) { q.template Get<TestCompB>(ZHLN::Entity {}); };
template <typename Q>
concept CanProjectWriteA = requires(Q q) { q.template Select<TestCompA&>(); };
static_assert(!CanReadB<WriteQuery>);
static_assert(!CanProjectWriteA<ReadQuery>);
// Engine::GetRenderGraph returns this ECS graph, not the Vulkan frame graph.
static_assert(std::is_same_v<decltype(std::declval<ZHLN::Engine&>().GetRenderGraph()), ZHLN::RenderGraph&>);
static_assert(requires(ZHLN::RenderGraph& graph, ZHLN::ECS::GraphTaskScheduler* scheduler) {
    graph.SetTaskScheduler(scheduler);
});

namespace {
std::atomic<int> typedOrder {0};
std::atomic<int> writerOrder {0};
std::atomic<int> readerOrder {0};
std::atomic<int> nullaryCalls {0};
float observedDt = 0.0f;
float observedAlpha = 0.0f;
uint64_t observedFrame = 0;
bool observedMissingAudio = false;

void DeclarativeWriter(WriteQuery query, ZHLN::FrameDt dt, ZHLN::FrameAlpha alpha, ZHLN::FrameIndex frame,
                       ZHLN::Optional<ZHLN::AudioContext&> audio) {
    observedDt = dt.value;
    observedAlpha = alpha.value;
    observedFrame = frame.value;
    observedMissingAudio = !audio;
    query.ForEach([](ZHLN::Entity, TestCompA& component) { component.value += 4; });
    writerOrder.store(typedOrder.fetch_add(1) + 1);
}

void DeclarativeReader(ReadQuery query) {
    query.ForEach([](ZHLN::Entity, const TestCompA& component, const TestCompB& value) {
        if (component.value == 5 && value.value == 9) {
            readerOrder.store(typedOrder.fetch_add(1) + 1);
        }
    });
}

void WildcardRegistrySystem(ZHLN::ECS::Registry&) {}
void ReadOnlyRegistrySystem(const ZHLN::ECS::Registry&) {}
void CommandBufferSystemA(ZHLN::ECS::Query<TestCompA&> query, ZHLN::ECS::EntityCommandBuffer& ecb) {
    query.ForEach([&](ZHLN::Entity e, TestCompA& comp) {
        comp.value += 10;
        ecb.AddComponent(e, TestCompB {.value = 77});
    });
    ecb.CreateEntity(TestCompA {.value = 99});
}
void CommandBufferSystemB(ZHLN::ECS::Query<const TestCompB> query, ZHLN::ECS::EntityCommandBuffer& ecb) {
    for (const ZHLN::Entity e: query.Entities<TestCompB>()) {
        if (const auto b = query.Get<TestCompB>(e); b && b->value == 42) {
            ecb.RemoveComponent<TestCompB>(e);
        }
    }
}
void NullarySystem() { nullaryCalls.fetch_add(1); }
void MultipleQueries(ZHLN::ECS::Query<const TestCompA>, ZHLN::ECS::Query<TestCompA&>,
                     ZHLN::ECS::Query<const TestCompB&>) {}
struct ReadFunctor {
    void operator()(ZHLN::ECS::Query<const TestCompB>) const {}
};
struct ReadStatic {
    static void Update(ZHLN::ECS::Query<const TestCompB>) {}
};
inline constexpr auto ReadLambda = [](ZHLN::ECS::Query<const TestCompA>) {};
inline constexpr auto ReadLambda2 = [](ZHLN::ECS::Query<const TestCompA>) {};
} // namespace

namespace {

struct SystemGraphTestSuite {
    SystemGraphTestSuite() {
        ZHLN::Fiber::InitMainThread();
        // Initialize a multi-threaded task system so parallel dispatch can be tested
        ZHLN::TaskSystem::Init(4, 32, ZHLN::kMinimumFiberStackSize);
    }

    ~SystemGraphTestSuite() {
        ZHLN::TaskSystem::Shutdown();
    }

    struct Tests {
        // --- 1. Conflict Detection & Compile Order ---
        std::expected<void, ZHLN::ErrorCode> hazard_conflict_detection() {
            ZHLN::ECS::Registry reg;
            ZHLN::ECS::SystemGraph<ZHLN::ECS::NoServices> graph {reg};

            static std::atomic<int> executionCounter {1};
            static std::atomic<int> orderA {0};
            static std::atomic<int> orderB {0};

            executionCounter.store(1);
            orderA.store(0);
            orderB.store(0);

            // System 1: Writes to TestCompA
            graph.AddSystem(
                {.update_func    = [](void*) { orderA.store(executionCounter.fetch_add(1, std::memory_order::seq_cst)); },
                 .name           = "WriterA",
                 .access_pattern = {ZHLN::ECS::Write<TestCompA>()},
                 .enabled        = true}
            );

            // System 2: Reads from TestCompA (Conflicting -> must run AFTER System 1)
            graph.AddSystem(
                {.update_func    = [](void*) { orderB.store(executionCounter.fetch_add(1, std::memory_order::seq_cst)); },
                 .name           = "ReaderA",
                 .access_pattern = {ZHLN::ECS::Read<TestCompA>()},
                 .enabled        = true}
            );

            graph.Compile();

            // Minimal frame: these systems only exercise ordering, never services.
            const ZHLN::Frame frame {.dt = TestDeltaTime};
            graph.Execute(frame);

            // Verification: WriterA must precede ReaderA
            if (!ZHLN::Test::ExpectGt(orderA.load(), 0)) {
                return std::unexpected(SystemGraphTestError::ExecutionOrderFailed);
            }

            if (!ZHLN::Test::ExpectGt(orderB.load(), orderA.load())) {
                return std::unexpected(SystemGraphTestError::ExecutionOrderFailed);
            }

            return {};
        }

        std::expected<void, ZHLN::ErrorCode> optional_system_insertion_before_named_phase() {
            ZHLN::ECS::Registry reg;
            ZHLN::ECS::SystemGraph<ZHLN::ECS::NoServices> graph {reg};
            static std::atomic<int> executionCounter {1};
            static std::atomic<int> extensionOrder {0};
            static std::atomic<int> anchorOrder {0};
            executionCounter.store(1);
            extensionOrder.store(0);
            anchorOrder.store(0);

            graph.AddSystem({
                .update_func    = [](void*) { anchorOrder.store(executionCounter.fetch_add(1)); },
                .name           = "GenericAnchor",
                .access_pattern = {ZHLN::ECS::Read<TestCompA>()},
                .enabled        = true,
            });
            const bool inserted = graph.AddSystemBefore(
                {
                    .update_func    = [](void*) { extensionOrder.store(executionCounter.fetch_add(1)); },
                    .name           = "OptionalExtension",
                    .access_pattern = {ZHLN::ECS::Write<TestCompA>()},
                    .enabled        = true,
                },
                "GenericAnchor"
            );
            const bool duplicateRejected = !graph.AddSystemBefore({.name = "OptionalExtension"}, "GenericAnchor");
            const bool missingRejected   = !graph.AddSystemBefore({.name = "MissingAnchorExtension"}, "DoesNotExist");
            if (!inserted || !duplicateRejected || !missingRejected) {
                return std::unexpected(SystemGraphTestError::ExecutionOrderFailed);
            }

            graph.Compile();
            const ZHLN::Frame frame {.dt = TestDeltaTime};
            graph.Execute(frame);
            if (!(extensionOrder.load() > 0 && anchorOrder.load() > extensionOrder.load())) {
                return std::unexpected(SystemGraphTestError::ExecutionOrderFailed);
            }
            return {};
        }

        // --- 2. Independent Systems Parallel Dispatch ---
        std::expected<void, ZHLN::ErrorCode> independent_systems_dispatch() {
            ZHLN::ECS::Registry reg;
            ZHLN::ECS::SystemGraph<ZHLN::ECS::NoServices> graph {reg};

            static std::atomic<int> executionCounter {1};
            static std::atomic<int> orderA {0};
            static std::atomic<int> orderB {0};
            static std::atomic<int> orderC {0};

            executionCounter.store(1);
            orderA.store(0);
            orderB.store(0);
            orderC.store(0);

            // SysA and SysB both READ TestCompA (No conflict, run parallel)
            graph.AddSystem(
                {.update_func =
                     [](void*) {
                         std::this_thread::sleep_for(std::chrono::milliseconds(2)); // Force a slight delay to prove overlap
                         orderA.store(executionCounter.fetch_add(1, std::memory_order::seq_cst));
                     },
                 .name           = "SysA_Read",
                 .access_pattern = {ZHLN::ECS::Read<TestCompA>()},
                 .enabled        = true}
            );

            graph.AddSystem(
                {.update_func =
                     [](void*) {
                         std::this_thread::sleep_for(std::chrono::milliseconds(2));
                         orderB.store(executionCounter.fetch_add(1, std::memory_order::seq_cst));
                     },
                 .name           = "SysB_Read",
                 .access_pattern = {ZHLN::ECS::Read<TestCompA>()},
                 .enabled        = true}
            );

            // SysC WRITES TestCompA (Conflict, must run after BOTH A and B)
            graph.AddSystem(
                {.update_func    = [](void*) { orderC.store(executionCounter.fetch_add(1, std::memory_order::seq_cst)); },
                 .name           = "SysC_Write",
                 .access_pattern = {ZHLN::ECS::Write<TestCompA>()},
                 .enabled        = true}
            );

            graph.Compile();

            const ZHLN::Frame frame {.dt = TestDeltaTime};
            graph.Execute(frame);

            // SysC MUST execute after both SysA and SysB complete.
            if (!(ZHLN::Test::ExpectGt(orderC.load(), orderA.load()) && ZHLN::Test::ExpectGt(orderC.load(), orderB.load()))) {
                return std::unexpected(SystemGraphTestError::ExecutionOrderFailed);
            }

            // The same public graph API used by Engine::GetRenderGraph accepts
            // a caller-owned scheduler. Inspect its dispatch/Wait observations,
            // not the graph's private task arrays or completion storage.
            ZHLN::Test::MockTaskScheduler scheduler;
            graph.SetTaskScheduler(&scheduler);
            executionCounter.store(1);
            orderA.store(0);
            orderB.store(0);
            orderC.store(0);
            graph.Execute(frame);
            graph.SetTaskScheduler(nullptr);

            if (!(ZHLN::Test::ExpectGt(orderC.load(), orderA.load()) && ZHLN::Test::ExpectGt(orderC.load(), orderB.load()))) {
                return std::unexpected(SystemGraphTestError::ExecutionOrderFailed);
            }
            if (!(ZHLN::Test::ExpectGt(scheduler.Dispatches(), 0u) && ZHLN::Test::ExpectGt(scheduler.Submitted(), 0u) &&
                  ZHLN::Test::ExpectEq(scheduler.Waits(), 1u) && ZHLN::Test::ExpectEq(scheduler.IncompleteWaits(), 0u))) {
                return std::unexpected(SystemGraphTestError::SchedulingObserverFailed);
            }

            return {};
        }

        // --- 3. External writes performed outside the graph ---
        // Mirrors the real engine: an imperative frame phase writes a
        // component, then a system inside the update graph reads it. Without a
        // declared write the graph sees a reader with no writer and builds no
        // edge, so the dependency lives only in the surrounding call order.
        std::expected<void, ZHLN::ErrorCode> external_write_anchor_reaches_dependents() {
            ZHLN::ECS::Registry reg;
            ZHLN::ECS::SystemGraph<ZHLN::ECS::NoServices> graph {reg};

            static std::atomic<int> executionCounter {1};
            static std::atomic<int> orderReader {0};
            static std::atomic<int> orderWriter {0};
            executionCounter.store(1);
            orderReader.store(0);
            orderWriter.store(0);

            // The write happens in an imperative phase, before Execute(). The
            // anchor carries no update function, so DispatchNode() must skip it
            // and still propagate to every dependent.
            graph.DeclareExternalWrites("ExternalPreUpdateWrites", {ZHLN::ECS::Write<TestCompA>()});

            graph.AddSystem(
                {.update_func    = [](void*) { orderReader.store(executionCounter.fetch_add(1, std::memory_order::seq_cst)); },
                 .name           = "ReaderOfExternal",
                 .access_pattern = {ZHLN::ECS::Read<TestCompA>()},
                 .enabled        = true}
            );
            graph.AddSystem(
                {.update_func    = [](void*) { orderWriter.store(executionCounter.fetch_add(1, std::memory_order::seq_cst)); },
                 .name           = "WriterOfExternal",
                 .access_pattern = {ZHLN::ECS::Write<TestCompA>()},
                 .enabled        = true}
            );

            // Anchor + reader + writer.
            if (graph.GetSystemCount() != 3) {
                return std::unexpected(SystemGraphTestError::ExternalWriteAnchorFailed);
            }

            graph.Compile();
            const ZHLN::Frame frame {.dt = TestDeltaTime};
            graph.Execute(frame);

            // Both systems ran exactly once: the null-function anchor neither
            // crashed dispatch nor stranded its dependents.
            if (!(ZHLN::Test::ExpectGt(orderReader.load(), 0) && ZHLN::Test::ExpectGt(orderWriter.load(), 0))) {
                return std::unexpected(SystemGraphTestError::ExecutionOrderFailed);
            }
            // Registration order still decides the reader/writer tie-break.
            if (!ZHLN::Test::ExpectGt(orderWriter.load(), orderReader.load())) {
                return std::unexpected(SystemGraphTestError::ExecutionOrderFailed);
            }

            // Guards: an empty access set or a null label must add no node, so a
            // mis-built declaration cannot silently create a phantom dependency.
            ZHLN::ECS::SystemGraph<ZHLN::ECS::NoServices> empty {reg};
            empty.DeclareExternalWrites("NothingWritten", {});
            empty.DeclareExternalWrites(nullptr, {ZHLN::ECS::Write<TestCompA>()});
            if (empty.GetSystemCount() != 0) {
                return std::unexpected(SystemGraphTestError::ExternalWriteAnchorFailed);
            }

            return {};
        }

        // --- 4. Query vocabulary: intersection, optional lookups, constness ---
        std::expected<void, ZHLN::ErrorCode> typed_query_intersection_and_direct_call() {
            ZHLN::ECS::Registry reg;
            const auto both  = reg.Create(TestCompA {.value = 1}, TestCompB {.value = 9});
            const auto onlyA = reg.Create(TestCompA {.value = 2});
            reg.Create(TestCompB {.value = 3});

            ZHLN::ECS::Query<const TestCompA&, TestCompB&> query(reg);
            size_t matches = 0;
            query.ForEach([&](ZHLN::Entity e, const TestCompA& a, TestCompB& b) {
                if (e == both && a.value == 1) {
                    ++matches;
                    b.value += 5;
                }
            });
            if (matches != 1 || reg.Get<TestCompB>(both)->value != 14 || query.Get<TestCompB>(onlyA).has_value() ||
                &*query.Get<TestCompA>(both) != &*reg.Get<TestCompA>(both)) {
                return std::unexpected(SystemGraphTestError::QueryIterationFailed);
            }
            // A system can be called with just its declared dependencies,
            // without constructing a context or any engine services.
            DeclarativeWriter(WriteQuery(reg), ZHLN::FrameDt {0.02f}, ZHLN::FrameAlpha {0.5f},
                              ZHLN::FrameIndex {4}, ZHLN::Optional<ZHLN::AudioContext&> {});
            if (reg.Get<TestCompA>(both)->value != 5 || reg.Get<TestCompA>(onlyA)->value != 6) {
                return std::unexpected(SystemGraphTestError::QueryIterationFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> declarative_access_patterns_detect_hazards() {
            using ZHLN::ECS::Access;
            using ZHLN::ECS::ComponentAccess;
            using ZHLN::ECS::ComponentFamily;
            using ZHLN::ECS::SystemSignature;

            const auto writer = SystemSignature<&DeclarativeWriter>::PopulateAccessPattern();
            const auto reader = SystemSignature<&DeclarativeReader>::PopulateAccessPattern();
            const auto multiple = SystemSignature<&MultipleQueries>::PopulateAccessPattern();
            const auto wildcard = SystemSignature<&WildcardRegistrySystem>::PopulateAccessPattern();
            const auto functor = SystemSignature<ReadFunctor {}>::PopulateAccessPattern();
            const auto lambda = SystemSignature<ReadLambda>::PopulateAccessPattern();
            const auto member = SystemSignature<&ReadStatic::Update>::PopulateAccessPattern();
            const auto nullary = SystemSignature<&NullarySystem>::PopulateAccessPattern();
            const uint32_t idA = ComponentFamily::GetTypeID<TestCompA>();
            const uint32_t idB = ComponentFamily::GetTypeID<TestCompB>();
            if (writer.size() != 1 || writer[0].familyId != idA || writer[0].mode != Access::Write ||
                reader.size() != 2 || reader[0].familyId != idA || reader[0].mode != Access::Read ||
                reader[1].familyId != idB || reader[1].mode != Access::Read ||
                multiple.size() != 2 || multiple[0].mode != Access::Write ||
                wildcard.size() != 1 || wildcard[0].familyId != ZHLN::ECS::AllComponents ||
                functor.size() != 1 || functor[0].familyId != idB || functor[0].mode != Access::Read ||
                lambda.size() != 1 || lambda[0].familyId != idA || lambda[0].mode != Access::Read ||
                member.size() != 1 || member[0].familyId != idB || member[0].mode != Access::Read || !nullary.empty()) {
                return std::unexpected(SystemGraphTestError::DeclarativeSignatureFailed);
            }
            ZHLN::ECS::SystemInfo writeInfo {.access_pattern = writer};
            ZHLN::ECS::SystemInfo readInfo {.access_pattern = reader};
            ZHLN::ECS::SystemInfo wildInfo {.access_pattern = wildcard};
            if (!ZHLN::ECS::SystemGraphCore::HasConflict(writeInfo, readInfo) ||
                !ZHLN::ECS::SystemGraphCore::HasConflict(wildInfo, readInfo) ||
                ZHLN::ECS::SystemGraphCore::HasConflict(readInfo, readInfo)) {
                return std::unexpected(SystemGraphTestError::HazardMismatch);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> reflected_thunk_resolves_context_and_orders_writes() {
            ZHLN::ECS::Registry reg;
            const auto entity = reg.Create(TestCompA {.value = 1}, TestCompB {.value = 9});
            typedOrder.store(0);
            writerOrder.store(0);
            readerOrder.store(0);
            observedDt = observedAlpha = 0.0f;
            observedFrame = 0;
            observedMissingAudio = false;

            ZHLN::ECS::SystemGraph<ZHLN::ECS::NoServices> graph {reg};
            graph.AddSystem<&DeclarativeWriter>();
            graph.AddSystem<&DeclarativeReader>();
            if (!graph.IsSystemEnabled("DeclarativeWriter") || graph.GetSystemCount() != 2) {
                return std::unexpected(SystemGraphTestError::DeclarativeSignatureFailed);
            }
            graph.Compile();
            const ZHLN::Frame frame {.frame = 42, .alpha = 0.25f, .dt = TestDeltaTime};
            graph.Execute(frame);
            if (observedDt != TestDeltaTime || observedAlpha != 0.25f || observedFrame != 42 || !observedMissingAudio ||
                reg.Get<TestCompA>(entity)->value != 5 || writerOrder.load() == 0 || readerOrder.load() <= writerOrder.load()) {
                return std::unexpected(SystemGraphTestError::ParameterResolutionFailed);
            }
            graph.SetSystemEnabled("DeclarativeWriter", false);
            if (graph.IsSystemEnabled("DeclarativeWriter")) {
                return std::unexpected(SystemGraphTestError::DeclarativeSignatureFailed);
            }
            ZHLN::ECS::SystemGraph<ZHLN::ECS::NoServices> before {reg};
            before.AddSystem<&DeclarativeReader>();
            if (!before.AddSystemBefore<&DeclarativeWriter>("DeclarativeReader") ||
                before.AddSystemBefore<&DeclarativeWriter>("DeclarativeReader") ||
                before.AddSystemBefore<&WildcardRegistrySystem>("MissingAnchor")) {
                return std::unexpected(SystemGraphTestError::DeclarativeSignatureFailed);
            }
            before.AddSystem<&ReadStatic::Update>();
            before.AddSystem<ReadFunctor {}>();
            before.AddSystem<ReadLambda>();
            before.AddSystem<ReadLambda2>();
            before.AddSystem<&NullarySystem>();
            nullaryCalls.store(0);
            if (before.GetSystemCount() != 7 || !before.IsSystemEnabled("ReadStatic") ||
                !before.IsSystemEnabled("ReadFunctor")) {
                return std::unexpected(SystemGraphTestError::DeclarativeSignatureFailed);
            }
            before.Compile();
            before.Execute(frame); // all four callable kinds use the same thunk path
            if (nullaryCalls.load() != 1) {
                return std::unexpected(SystemGraphTestError::ParameterResolutionFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> command_buffer_and_const_registry_hazards_and_playback() {
            using ZHLN::ECS::Access;
            using ZHLN::ECS::SystemSignature;

            const auto constWildcard = SystemSignature<&ReadOnlyRegistrySystem>::PopulateAccessPattern();
            const auto ecbAccessA    = SystemSignature<&CommandBufferSystemA>::PopulateAccessPattern();
            const auto ecbAccessB    = SystemSignature<&CommandBufferSystemB>::PopulateAccessPattern();
            const uint32_t idA       = ZHLN::ECS::ComponentFamily::GetTypeID<TestCompA>();
            const uint32_t idB       = ZHLN::ECS::ComponentFamily::GetTypeID<TestCompB>();

            if (constWildcard.size() != 1 || constWildcard[0].familyId != ZHLN::ECS::AllComponents || constWildcard[0].mode != Access::Read ||
                ecbAccessA.size() != 1 || ecbAccessA[0].familyId != idA || ecbAccessA[0].mode != Access::Write ||
                ecbAccessB.size() != 1 || ecbAccessB[0].familyId != idB || ecbAccessB[0].mode != Access::Read) {
                return std::unexpected(SystemGraphTestError::DeclarativeSignatureFailed);
            }

            ZHLN::ECS::SystemInfo constWildInfo {.access_pattern = constWildcard};
            ZHLN::ECS::SystemInfo ecbInfoA {.access_pattern = ecbAccessA};
            ZHLN::ECS::SystemInfo ecbInfoB {.access_pattern = ecbAccessB};
            if (ZHLN::ECS::SystemGraphCore::HasConflict(constWildInfo, constWildInfo) ||
                !ZHLN::ECS::SystemGraphCore::HasConflict(constWildInfo, ecbInfoA) ||
                ZHLN::ECS::SystemGraphCore::HasConflict(constWildInfo, ecbInfoB) ||
                ZHLN::ECS::SystemGraphCore::HasConflict(ecbInfoA, ecbInfoB)) {
                return std::unexpected(SystemGraphTestError::HazardMismatch);
            }

            ZHLN::ECS::Registry reg;
            const auto          e1 = reg.Create(TestCompA {.value = 5});
            const auto          e2 = reg.Create(TestCompB {.value = 42});

            ZHLN::ECS::SystemGraph<ZHLN::ECS::NoServices> graph {reg};
            graph.AddSystem<&CommandBufferSystemA>();
            graph.AddSystem<&CommandBufferSystemB>();
            graph.Compile();
            graph.Execute(ZHLN::Frame {.frame = 1, .alpha = 0.0f, .dt = TestDeltaTime});

            if (!reg.Get<TestCompA>(e1) || reg.Get<TestCompA>(e1)->value != 15 ||
                !reg.Get<TestCompB>(e1) || reg.Get<TestCompB>(e1)->value != 77 ||
                reg.Get<TestCompB>(e2).has_value() ||
                reg.GetEntitiesWith<TestCompA>().size() != 2) {
                return std::unexpected(SystemGraphTestError::ParameterResolutionFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> event_reader_writer_hazards_and_parallel_execution() {
            using ZHLN::ECS::EventBus;
            using ZHLN::ECS::EventReader;
            using ZHLN::ECS::EventWriter;
            using ZHLN::ECS::ResMut;
            using ZHLN::ECS::SystemInfo;
            using ZHLN::ECS::SystemSignature;

            struct DamageEvt {
                int value = 0;
            };
            struct AudioEvt {
                int cue = 0;
            };
            struct BusServices {
                EventBus& events;
            };

            static_assert(!ZHLN::ECS::ResolvableFrom<ZHLN::ECS::Carrier<ZHLN::ECS::NoServices>, EventReader<DamageEvt>>);
            static_assert(!ZHLN::ECS::ResolvableFrom<ZHLN::ECS::Carrier<ZHLN::ECS::NoServices>, EventWriter<DamageEvt>>);
            static_assert(ZHLN::ECS::ResolvableFrom<ZHLN::ECS::Carrier<BusServices>, EventReader<DamageEvt>>);
            static_assert(ZHLN::ECS::ResolvableFrom<ZHLN::ECS::Carrier<BusServices>, EventWriter<DamageEvt>>);

            static constexpr auto writeDamage = [](EventWriter<DamageEvt> out) {
                out.Push({.value = 10});
                out.Push({.value = 32});
            };
            static constexpr auto writeAudio = [](EventWriter<AudioEvt> out) {
                out.Push({.cue = 7});
            };
            static std::atomic<int> sumReader1 {0};
            static std::atomic<int> sumReader2 {0};
            static constexpr auto   readDamage1 = [](EventReader<DamageEvt> in) {
                int total = 0;
                for (const DamageEvt& e: in) {
                    total += e.value;
                }
                sumReader1.store(total, std::memory_order_relaxed);
            };
            static constexpr auto readDamage2 = [](EventReader<DamageEvt> in) {
                int total = 0;
                for (const DamageEvt& e: in) {
                    total += e.value * 2;
                }
                sumReader2.store(total, std::memory_order_relaxed);
            };
            static constexpr auto mutateWholeBus = [](ResMut<EventBus> bus) {
                bus->ClearEvents();
            };
            static constexpr auto invalidAliased = [](EventReader<DamageEvt>, EventWriter<DamageEvt>) {};
            static_assert(!ZHLN::ECS::TemplatedDetail::EventParametersAreDisjoint<invalidAliased>());
            static_assert(ZHLN::ECS::TemplatedDetail::EventParametersAreDisjoint<writeDamage>());

            const SystemInfo infoWriteDmg {.access_pattern = SystemSignature<writeDamage>::PopulateAccessPattern()};
            const SystemInfo infoWriteAud {.access_pattern = SystemSignature<writeAudio>::PopulateAccessPattern()};
            const SystemInfo infoReadDmg1 {.access_pattern = SystemSignature<readDamage1>::PopulateAccessPattern()};
            const SystemInfo infoReadDmg2 {.access_pattern = SystemSignature<readDamage2>::PopulateAccessPattern()};
            const SystemInfo infoMutBus {.access_pattern = SystemSignature<mutateWholeBus>::PopulateAccessPattern()};

            // Multi-reader on same event and multi-writer on distinct events must NOT conflict;
            // same-event Writer/Reader, Writer/Writer, and ResMut<EventBus> MUST conflict!
            if (ZHLN::ECS::SystemGraphCore::HasConflict(infoReadDmg1, infoReadDmg2) ||
                ZHLN::ECS::SystemGraphCore::HasConflict(infoWriteDmg, infoWriteAud) ||
                !ZHLN::ECS::SystemGraphCore::HasConflict(infoWriteDmg, infoReadDmg1) ||
                !ZHLN::ECS::SystemGraphCore::HasConflict(infoWriteDmg, infoWriteDmg) ||
                !ZHLN::ECS::SystemGraphCore::HasConflict(infoReadDmg1, infoMutBus) ||
                !ZHLN::ECS::SystemGraphCore::HasConflict(infoWriteAud, infoMutBus)) {
                return std::unexpected(SystemGraphTestError::HazardMismatch);
            }

            ZHLN::ECS::Registry reg;
            EventBus            bus;
            BusServices         services {.events = bus};
            sumReader1.store(0);
            sumReader2.store(0);

            ZHLN::ECS::SystemGraph<BusServices> graph {reg, services};
            graph.AddSystem<writeDamage>();
            graph.AddSystem<writeAudio>();
            graph.AddSystem<readDamage1>();
            graph.AddSystem<readDamage2>();
            graph.Compile();
            graph.Execute(ZHLN::Frame {.frame = 1, .alpha = 0.0f, .dt = TestDeltaTime});

            if (sumReader1.load() != 42 || sumReader2.load() != 84 || bus.Count<AudioEvt>() != 1 || bus.View<AudioEvt>()[0].cue != 7) {
                return std::unexpected(SystemGraphTestError::ExecutionOrderFailed);
            }

            // Also verify ExtensionServices fallback when EventBus is installed dynamically.
            struct ExtOnlyServices {
                ZHLN::ECS::ExtensionServices& extensions;
            };
            ZHLN::ECS::ExtensionServices ext;
            auto&                        extBus = ext.Emplace<EventBus>();
            ExtOnlyServices              extServices {.extensions = ext};
            sumReader1.store(0);

            ZHLN::ECS::SystemGraph<ExtOnlyServices> extGraph {reg, extServices};
            extGraph.AddSystem<writeDamage>();
            extGraph.AddSystem<readDamage1>();
            extGraph.Compile();
            extGraph.Execute(ZHLN::Frame {.frame = 2, .alpha = 0.0f, .dt = TestDeltaTime});

            if (sumReader1.load() != 42 || extBus.Count<DamageEvt>() != 2) {
                return std::unexpected(SystemGraphTestError::ParameterResolutionFailed);
            }
            return {};
        }
    };
};

} // namespace
