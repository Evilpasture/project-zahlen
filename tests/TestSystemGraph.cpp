// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/ecs/SystemGraph.hpp>
#include <atomic>
#include <expected>
#include <thread>

enum class SystemGraphTestError : uint8_t {
    HazardMismatch[[= ZHLN::Reflect::Description<"SystemGraph failed to detect a Read/Write or Write/Write component conflict.">{}]] = 1,
    ExecutionOrderFailed[[= ZHLN::Reflect::Description<"Systems were executed out of dependency order.">{}]],
};

// Mock components for access hazard tracking
struct TestCompA {
    int value = 0;
};
struct TestCompB {
    int value = 0;
};

// Constants for test
constexpr float     TestDeltaTime = 0.016f;
constexpr uintptr_t FakeEnginePtr = 0x12345678;

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
        std::expected<void, ZHLN::Error> hazard_conflict_detection() {
            ZHLN::ECS::SystemGraph graph;

            static std::atomic<int> executionCounter {1};
            static std::atomic<int> orderA {0};
            static std::atomic<int> orderB {0};

            executionCounter.store(1);
            orderA.store(0);
            orderB.store(0);

            // System 1: Writes to TestCompA
            graph.AddSystem(
                {.update_func    = [](ZHLN::Engine&, float) { orderA.store(executionCounter.fetch_add(1, std::memory_order::seq_cst)); },
                 .name           = "WriterA",
                 .access_pattern = {ZHLN::ECS::Write<TestCompA>()},
                 .enabled        = true}
            );

            // System 2: Reads from TestCompA (Conflicting -> must run AFTER System 1)
            graph.AddSystem(
                {.update_func    = [](ZHLN::Engine&, float) { orderB.store(executionCounter.fetch_add(1, std::memory_order::seq_cst)); },
                 .name           = "ReaderA",
                 .access_pattern = {ZHLN::ECS::Read<TestCompA>()},
                 .enabled        = true}
            );

            graph.Compile();

            // Mock minimal engine execution context (systems don't actually use the engine ref)
            auto* fakeEngine = reinterpret_cast<ZHLN::Engine*>(FakeEnginePtr);
            graph.Execute(*fakeEngine, TestDeltaTime);

            // Verification: WriterA must precede ReaderA
            auto chkA = ZHLN::Test::AssertTrue(orderA.load() > 0);
            if (!chkA) {
                return chkA;
            }

            auto chkB = ZHLN::Test::AssertTrue(orderB.load() > orderA.load());
            if (!chkB) {
                return chkB;
            }

            return {};
        }

        std::expected<void, ZHLN::Error> optional_system_insertion_before_named_phase() {
            ZHLN::ECS::SystemGraph  graph;
            static std::atomic<int> executionCounter {1};
            static std::atomic<int> extensionOrder {0};
            static std::atomic<int> anchorOrder {0};
            executionCounter.store(1);
            extensionOrder.store(0);
            anchorOrder.store(0);

            graph.AddSystem({
                .update_func    = [](ZHLN::Engine&, float) { anchorOrder.store(executionCounter.fetch_add(1)); },
                .name           = "GenericAnchor",
                .access_pattern = {ZHLN::ECS::Read<TestCompA>()},
                .enabled        = true,
            });
            const bool inserted = graph.AddSystemBefore(
                {
                    .update_func    = [](ZHLN::Engine&, float) { extensionOrder.store(executionCounter.fetch_add(1)); },
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
            auto* fakeEngine = reinterpret_cast<ZHLN::Engine*>(FakeEnginePtr);
            graph.Execute(*fakeEngine, TestDeltaTime);
            if (!(extensionOrder.load() > 0 && anchorOrder.load() > extensionOrder.load())) {
                return std::unexpected(SystemGraphTestError::ExecutionOrderFailed);
            }
            return {};
        }

        // --- 2. Independent Systems Parallel Dispatch ---
        std::expected<void, ZHLN::Error> independent_systems_dispatch() {
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
                     [](ZHLN::Engine&, float) {
                         std::this_thread::sleep_for(std::chrono::milliseconds(2)); // Force a slight delay to prove overlap
                         orderA.store(executionCounter.fetch_add(1, std::memory_order::seq_cst));
                     },
                 .name           = "SysA_Read",
                 .access_pattern = {ZHLN::ECS::Read<TestCompA>()},
                 .enabled        = true}
            );

            graph.AddSystem(
                {.update_func =
                     [](ZHLN::Engine&, float) {
                         std::this_thread::sleep_for(std::chrono::milliseconds(2));
                         orderB.store(executionCounter.fetch_add(1, std::memory_order::seq_cst));
                     },
                 .name           = "SysB_Read",
                 .access_pattern = {ZHLN::ECS::Read<TestCompA>()},
                 .enabled        = true}
            );

            // SysC WRITES TestCompA (Conflict, must run after BOTH A and B)
            graph.AddSystem(
                {.update_func    = [](ZHLN::Engine&, float) { orderC.store(executionCounter.fetch_add(1, std::memory_order::seq_cst)); },
                 .name           = "SysC_Write",
                 .access_pattern = {ZHLN::ECS::Write<TestCompA>()},
                 .enabled        = true}
            );

            graph.Compile();

            auto* fakeEngine = reinterpret_cast<ZHLN::Engine*>(FakeEnginePtr);
            graph.Execute(*fakeEngine, TestDeltaTime);

            // SysC MUST execute after both SysA and SysB complete
            auto chkC = ZHLN::Test::AssertTrue(orderC.load() > orderA.load() && orderC.load() > orderB.load());
            if (!chkC) {
                return chkC;
            }

            return {};
        }
    };
};

int main() {
    return ZHLN::Test::Runner::Run<SystemGraphTestSuite>();
}
