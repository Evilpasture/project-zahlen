// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/SystemContext.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/ecs/SystemGraph.hpp>
#include <Zahlen/ecs/SystemParameters.hpp>
#include <atomic>
#include <expected>
#include <string_view>
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
static_assert(std::is_same_v<decltype(std::declval<ReadQuery>().Get<TestCompA>(ZHLN::Entity {})), const TestCompA*>);
static_assert(std::is_same_v<decltype(std::declval<ReadQuery>().Raw<TestCompA>()), ZHLN::RestrictSpan<const TestCompA>>);
static_assert(std::is_same_v<decltype(std::declval<WriteQuery>().Get<TestCompA>(ZHLN::Entity {})), TestCompA*>);

template <typename Q>
concept CanReadB = requires(Q q) { q.template Get<TestCompB>(ZHLN::Entity {}); };
template <typename Q>
concept CanProjectWriteA = requires(Q q) { q.template Select<TestCompA&>(); };
static_assert(!CanReadB<WriteQuery>);
static_assert(!CanProjectWriteA<ReadQuery>);

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
                       ZHLN::ECS::OptionRes<ZHLN::AudioContext> audio) {
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
void PlainCallable(int, const double&) {} // callable inspection does not require ECS types
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
            ZHLN::ECS::SystemGraph graph;

            static std::atomic<int> executionCounter {1};
            static std::atomic<int> orderA {0};
            static std::atomic<int> orderB {0};

            executionCounter.store(1);
            orderA.store(0);
            orderB.store(0);

            // System 1: Writes to TestCompA
            graph.AddSystem(
                {.update_func    = [](ZHLN::SystemContext&) { orderA.store(executionCounter.fetch_add(1, std::memory_order::seq_cst)); },
                 .name           = "WriterA",
                 .access_pattern = {ZHLN::ECS::Write<TestCompA>()},
                 .enabled        = true}
            );

            // System 2: Reads from TestCompA (Conflicting -> must run AFTER System 1)
            graph.AddSystem(
                {.update_func    = [](ZHLN::SystemContext&) { orderB.store(executionCounter.fetch_add(1, std::memory_order::seq_cst)); },
                 .name           = "ReaderA",
                 .access_pattern = {ZHLN::ECS::Read<TestCompA>()},
                 .enabled        = true}
            );

            graph.Compile();

            // Minimal context: these systems only exercise ordering, never services.
            ZHLN::ECS::Registry reg;
            ZHLN::SystemContext ctx {.registry = reg, .dt = TestDeltaTime};
            graph.Execute(ctx);

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
            ZHLN::ECS::SystemGraph  graph;
            static std::atomic<int> executionCounter {1};
            static std::atomic<int> extensionOrder {0};
            static std::atomic<int> anchorOrder {0};
            executionCounter.store(1);
            extensionOrder.store(0);
            anchorOrder.store(0);

            graph.AddSystem({
                .update_func    = [](ZHLN::SystemContext&) { anchorOrder.store(executionCounter.fetch_add(1)); },
                .name           = "GenericAnchor",
                .access_pattern = {ZHLN::ECS::Read<TestCompA>()},
                .enabled        = true,
            });
            const bool inserted = graph.AddSystemBefore(
                {
                    .update_func    = [](ZHLN::SystemContext&) { extensionOrder.store(executionCounter.fetch_add(1)); },
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
            ZHLN::ECS::Registry reg;
            ZHLN::SystemContext ctx {.registry = reg, .dt = TestDeltaTime};
            graph.Execute(ctx);
            if (!(extensionOrder.load() > 0 && anchorOrder.load() > extensionOrder.load())) {
                return std::unexpected(SystemGraphTestError::ExecutionOrderFailed);
            }
            return {};
        }

        // --- 2. Independent Systems Parallel Dispatch ---
        std::expected<void, ZHLN::ErrorCode> independent_systems_dispatch() {
            ZHLN::ECS::SystemGraph graph;

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
                     [](ZHLN::SystemContext&) {
                         std::this_thread::sleep_for(std::chrono::milliseconds(2)); // Force a slight delay to prove overlap
                         orderA.store(executionCounter.fetch_add(1, std::memory_order::seq_cst));
                     },
                 .name           = "SysA_Read",
                 .access_pattern = {ZHLN::ECS::Read<TestCompA>()},
                 .enabled        = true}
            );

            graph.AddSystem(
                {.update_func =
                     [](ZHLN::SystemContext&) {
                         std::this_thread::sleep_for(std::chrono::milliseconds(2));
                         orderB.store(executionCounter.fetch_add(1, std::memory_order::seq_cst));
                     },
                 .name           = "SysB_Read",
                 .access_pattern = {ZHLN::ECS::Read<TestCompA>()},
                 .enabled        = true}
            );

            // SysC WRITES TestCompA (Conflict, must run after BOTH A and B)
            graph.AddSystem(
                {.update_func    = [](ZHLN::SystemContext&) { orderC.store(executionCounter.fetch_add(1, std::memory_order::seq_cst)); },
                 .name           = "SysC_Write",
                 .access_pattern = {ZHLN::ECS::Write<TestCompA>()},
                 .enabled        = true}
            );

            graph.Compile();

            ZHLN::ECS::Registry reg;
            ZHLN::SystemContext ctx {.registry = reg, .dt = TestDeltaTime};
            graph.Execute(ctx);

            // SysC MUST execute after both SysA and SysB complete
            if (!(ZHLN::Test::ExpectGt(orderC.load(), orderA.load()) && ZHLN::Test::ExpectGt(orderC.load(), orderB.load()))) {
                return std::unexpected(SystemGraphTestError::ExecutionOrderFailed);
            }

            return {};
        }

        // --- 3. External writes performed outside the graph ---
        // Mirrors the real engine: an imperative frame phase writes a
        // component, then a system inside the update graph reads it. Without a
        // declared write the graph sees a reader with no writer and builds no
        // edge, so the dependency lives only in the surrounding call order.
        std::expected<void, ZHLN::ErrorCode> external_write_anchor_reaches_dependents() {
            ZHLN::ECS::SystemGraph graph;

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
                {.update_func    = [](ZHLN::SystemContext&) { orderReader.store(executionCounter.fetch_add(1, std::memory_order::seq_cst)); },
                 .name           = "ReaderOfExternal",
                 .access_pattern = {ZHLN::ECS::Read<TestCompA>()},
                 .enabled        = true}
            );
            graph.AddSystem(
                {.update_func    = [](ZHLN::SystemContext&) { orderWriter.store(executionCounter.fetch_add(1, std::memory_order::seq_cst)); },
                 .name           = "WriterOfExternal",
                 .access_pattern = {ZHLN::ECS::Write<TestCompA>()},
                 .enabled        = true}
            );

            // Anchor + reader + writer.
            if (graph.GetSystemCount() != 3) {
                return std::unexpected(SystemGraphTestError::ExternalWriteAnchorFailed);
            }

            graph.Compile();
            ZHLN::ECS::Registry reg;
            ZHLN::SystemContext ctx {.registry = reg, .dt = TestDeltaTime};
            graph.Execute(ctx);

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
            ZHLN::ECS::SystemGraph empty;
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
            if (matches != 1 || reg.Get<TestCompB>(both)->value != 14 || query.Get<TestCompB>(onlyA) != nullptr ||
                query.Get<TestCompA>(both) != reg.Get<TestCompA>(both)) {
                return std::unexpected(SystemGraphTestError::QueryIterationFailed);
            }
            // A system can be called with just its declared dependencies,
            // without constructing a SystemContext or any engine services.
            DeclarativeWriter(WriteQuery(reg), ZHLN::FrameDt {0.02f}, ZHLN::FrameAlpha {0.5f},
                              ZHLN::FrameIndex {4}, ZHLN::ECS::OptionRes<ZHLN::AudioContext> {});
            if (reg.Get<TestCompA>(both)->value != 5 || reg.Get<TestCompA>(onlyA)->value != 6) {
                return std::unexpected(SystemGraphTestError::QueryIterationFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> reflected_hazards_and_names() {
            using ZHLN::ECS::Access;
            using ZHLN::ECS::ComponentAccess;
            using ZHLN::ECS::ComponentFamily;
            using ZHLN::ECS::SystemSignature;
            using ZHLN::Reflect::CallableInspector;

            if (CallableInspector<&DeclarativeWriter>::Name() != "DeclarativeWriter" ||
                CallableInspector<&DeclarativeReader>::Name() != "DeclarativeReader" ||
                CallableInspector<&ReadStatic::Update>::Name() != "ReadStatic" ||
                CallableInspector<ReadFunctor {}>::Name() != "ReadFunctor" ||
                CallableInspector<ReadLambda>::Name().empty() ||
                std::string_view(CallableInspector<ReadLambda>::NameCString()) == CallableInspector<ReadLambda2>::NameCString() ||
                CallableInspector<&PlainCallable>::Name() != "PlainCallable") {
                return std::unexpected(SystemGraphTestError::DeclarativeSignatureFailed);
            }

            int parameterCount = 0;
            bool plainParameterTypesMatch = true;
            CallableInspector<&PlainCallable>::ForEachParameter([&]<typename Param>() {
                plainParameterTypesMatch &= parameterCount == 0 ? std::is_same_v<Param, int> : std::is_same_v<Param, const double&>;
                ++parameterCount;
            });
            if (parameterCount != 2 || !plainParameterTypesMatch) {
                return std::unexpected(SystemGraphTestError::DeclarativeSignatureFailed);
            }

            std::vector<ComponentAccess> writer, reader, multiple, wildcard, functor, lambda, member, nullary;
            SystemSignature<&DeclarativeWriter>::PopulateAccessPattern(writer);
            SystemSignature<&DeclarativeReader>::PopulateAccessPattern(reader);
            SystemSignature<&MultipleQueries>::PopulateAccessPattern(multiple);
            SystemSignature<&WildcardRegistrySystem>::PopulateAccessPattern(wildcard);
            SystemSignature<ReadFunctor {}>::PopulateAccessPattern(functor);
            SystemSignature<ReadLambda>::PopulateAccessPattern(lambda);
            SystemSignature<&ReadStatic::Update>::PopulateAccessPattern(member);
            SystemSignature<&NullarySystem>::PopulateAccessPattern(nullary);
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
            if (!ZHLN::ECS::SystemGraph::HasConflict(writeInfo, readInfo) ||
                !ZHLN::ECS::SystemGraph::HasConflict(wildInfo, readInfo) ||
                ZHLN::ECS::SystemGraph::HasConflict(readInfo, readInfo)) {
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

            ZHLN::ECS::SystemGraph graph;
            graph.AddSystem<&DeclarativeWriter>();
            graph.AddSystem<&DeclarativeReader>();
            if (!graph.IsSystemEnabled("DeclarativeWriter") || graph.GetSystemCount() != 2) {
                return std::unexpected(SystemGraphTestError::DeclarativeSignatureFailed);
            }
            graph.Compile();
            ZHLN::SystemContext ctx {.registry = reg, .frame = 42, .alpha = 0.25f, .dt = TestDeltaTime};
            graph.Execute(ctx);
            if (observedDt != TestDeltaTime || observedAlpha != 0.25f || observedFrame != 42 || !observedMissingAudio ||
                reg.Get<TestCompA>(entity)->value != 5 || writerOrder.load() == 0 || readerOrder.load() <= writerOrder.load()) {
                return std::unexpected(SystemGraphTestError::ParameterResolutionFailed);
            }
            graph.SetSystemEnabled("DeclarativeWriter", false);
            if (graph.IsSystemEnabled("DeclarativeWriter")) {
                return std::unexpected(SystemGraphTestError::DeclarativeSignatureFailed);
            }
            ZHLN::ECS::SystemGraph before;
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
                !before.IsSystemEnabled("ReadFunctor") ||
                !before.IsSystemEnabled(ZHLN::ECS::SystemSignature<ReadLambda>::NameCString()) ||
                !before.IsSystemEnabled(ZHLN::ECS::SystemSignature<ReadLambda2>::NameCString())) {
                return std::unexpected(SystemGraphTestError::DeclarativeSignatureFailed);
            }
            before.Compile();
            before.Execute(ctx); // all four callable kinds use the same thunk path
            if (nullaryCalls.load() != 1) {
                return std::unexpected(SystemGraphTestError::ParameterResolutionFailed);
            }
            return {};
        }
    };
};

// Exported for the ecs group binary (RunEcsTests.cpp), which
// aggregates every suite in this directory through Runner::RunDeferred.
auto RunSystemGraphSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<SystemGraphTestSuite>();
}

