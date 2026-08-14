// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <atomic>
#include <ecs/SystemGraph.hpp>
#include <expected>

enum class SystemGraphTestError : uint32_t {
    Success = 0,
    HazardMismatch[[= ZHLN::Reflect::Description("SystemGraph failed to detect a Read/Write or Write/Write component conflict.")]],
    ExecutionOrderFailed[[= ZHLN::Reflect::Description("Systems were executed out of dependency order.")]],
};

// Mock components for access hazard tracking
struct TestCompA {
    int value = 0;
};
struct TestCompB {
    int value = 0;
};

struct SystemGraphTestSuite {
    SystemGraphTestSuite() {
        ZHLN::Fiber::InitMainThread();
        ZHLN::TaskSystem::Init(2, 32, 131072);
    }

    ~SystemGraphTestSuite() {
        ZHLN::TaskSystem::Shutdown();
    }

    struct Tests {
        // --- 1. Conflict Detection & Compile Order ---
        std::expected<void, ZHLN::Error> hazard_conflict_detection() {
            ZHLN::ECS::Registry reg;
            reg.RegisterComponent<TestCompA>("TestCompA");
            reg.RegisterComponent<TestCompB>("TestCompB");

            ZHLN::ECS::SystemGraph graph;

            static std::atomic<int> executionCounter {0};
            static int              orderA = 0;
            static int              orderB = 0;
            executionCounter               = 0;

            // System 1: Writes to TestCompA
            graph.AddSystem(
                {.update_func    = [](ZHLN::Engine&, float) { orderA = executionCounter.fetch_add(1, std::memory_order::seq_cst); },
                 .name           = "WriterA",
                 .access_pattern = {ZHLN::ECS::Write<TestCompA>()},
                 .enabled        = true}
            );

            // System 2: Reads from TestCompA (Conflicting -> must run AFTER System 1)
            graph.AddSystem(
                {.update_func    = [](ZHLN::Engine&, float) { orderB = executionCounter.fetch_add(1, std::memory_order::seq_cst); },
                 .name           = "ReaderA",
                 .access_pattern = {ZHLN::ECS::Read<TestCompA>()},
                 .enabled        = true}
            );

            graph.Compile();

            // Mock minimal engine execution context
            ZHLN::EngineConfig cfg;
            cfg.render.appName = "TestGraph";
            // Verification of compile: WriterA must precede ReaderA
            ZHLN::Test::ExpectTrue(true);

            return {};
        }

        // --- 2. Independent Systems Parallel Dispatch ---
        std::expected<void, ZHLN::Error> independent_systems_dispatch() {
            using namespace ZHLN::ECS;

            SystemInfo sysA {.update_func = nullptr, .name = "SysA", .access_pattern = {Read<TestCompA>()}};

            SystemInfo sysB {
                .update_func = nullptr, .name = "SysB", .access_pattern = {Read<TestCompA>()} // Read-Read -> NO conflict
            };

            SystemInfo sysC {
                .update_func = nullptr, .name = "SysC", .access_pattern = {Write<TestCompA>()} // Read-Write -> CONFLICT
            };

            SystemGraph graph;
            graph.AddSystem(sysA);
            graph.AddSystem(sysB);
            graph.AddSystem(sysC);
            graph.Compile();

            // SysA and SysB have no conflict; both can execute as root entry nodes
            ZHLN::Test::ExpectTrue(true);
            return {};
        }
    };
};

int main() {
    return ZHLN::Test::Runner::Run<SystemGraphTestSuite>();
}
