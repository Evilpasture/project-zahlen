// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PerfBaseline.hpp"
#include "TestsFramework.hpp"

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Core/Factory.h>
#include <Jolt/RegisterTypes.h>
#include <Zahlen/Audio.hpp>
#include <Zahlen/Camera.hpp>
#include <Zahlen/Clock.hpp>
#include <Zahlen/CommandLine.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/Atomic.hpp>
#include <Zahlen/Core/HashMap.hpp>
#include <Zahlen/Core/MemoryPool.hpp>
#include <Zahlen/Core/Print.hpp>
#include <Zahlen/Core/Queue.hpp>
#include <Zahlen/Core/RadixSort.hpp>
#include <Zahlen/Core/Ranges.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/gui/GUI.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Threading/Channel.hpp>
#include <Zahlen/Threading/Mutex.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <Zahlen/Types.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/ecs/EntityCommandBuffer.hpp>
#include <Zahlen/ecs/SystemGraph.hpp>

#include <Zahlen/physics/Physics.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <expected>
#include <numeric>
#include <random>
#include <span>
#include <string>
#include <vector>

// ============================================================================
// Error Codes
// ============================================================================

enum class PerfTestError : uint8_t {
    CoreContainersThroughputFailed ZHLN_ANNOTATION(ZHLN::Description<"Core container or algorithm benchmark failed throughput invariants."> {}) = 1,
    TaskSystemThroughputFailed     ZHLN_ANNOTATION(ZHLN::Description<"TaskSystem parallel dispatch or fiber scheduling failed under heavy load."> {}),
    ECSIterationThroughputFailed   ZHLN_ANNOTATION(ZHLN::Description<"ECS bulk lifecycle, ECB playback, or dense iteration failed performance criteria."> {}),
    SystemGraphContentionFailed    ZHLN_ANNOTATION(ZHLN::Description<"SystemGraph parallel execution encountered dependency or contention failure."> {}),
    PhysicsSimulationThroughputFailed ZHLN_ANNOTATION(ZHLN::Description<"Physics engine body simulation or mass raycasting failed performance gate."> {}),
    GUIRebuildThroughputFailed        ZHLN_ANNOTATION(ZHLN::Description<"Immediate-mode GUI tree rebuild or mark-and-sweep GC failed throughput criteria."> {}),
    AudioQueueThroughputFailed        ZHLN_ANNOTATION(ZHLN::Description<"Audio event queue failed to process high-throughput batch stream."> {}),
    UnifiedMasterSceneFailed          ZHLN_ANNOTATION(
        ZHLN::Description<"Unified multi-subsystem master benchmark failed stability, performance, or state integrity."> {}
    ),
};

// ============================================================================
// Local Test Mock Components
// ============================================================================

struct AgentHealthComponent {
    float currentHealth = 100.0f;
    float maxHealth     = 100.0f;
    float shield        = 50.0f;
    bool  isAlive       = true;
};

struct AgentCombatStateComponent {
    uint32_t targetEntityIndex = 0xFFFFFFFF;
    float    cooldown          = 0.0f;
    float    attackRange       = 15.0f;
    uint32_t kills             = 0;
};

struct SpatialPerceptionComponent {
    uint32_t  nearbyEnemyCount = 0;
    float     nearestDistance  = 999.0f;
    JPH::Vec3 threatDirection  = JPH::Vec3::sZero();
};

// ============================================================================
// Performance Test Suite
// ============================================================================

struct PerformanceTestSuite {
    PerformanceTestSuite() {
        ZHLN::Fiber::InitMainThread();
        ZHLN::TaskSystem::Init(4, 64, ZHLN::kMinimumFiberStackSize);

        JPH::RegisterDefaultAllocator();
        if (JPH::Factory::sInstance == nullptr) {
            JPH::Factory::sInstance = new JPH::Factory();
            JPH::RegisterTypes();
        }
    }

    ~PerformanceTestSuite() {
        ZHLN::TaskSystem::Shutdown();
        JPH::UnregisterTypes();
        if (JPH::Factory::sInstance != nullptr) {
            delete JPH::Factory::sInstance;
            JPH::Factory::sInstance = nullptr;
        }
    }

    struct Tests {
        // ====================================================================
        // 1. ISOLATED: Core Containers, RadixSort & Memory Pools
        // ====================================================================
        auto isolated_01_core_containers_and_algorithms() -> std::expected<void, ZHLN::Error> {
            ZHLN::Println("\n  {}--- Subsystem 1: Core Containers & Algorithms ---{}", ZHLN::Color::Cyan, ZHLN::Color::Reset);

            // A. RadixSort64 (65,536 64-bit keys)
            constexpr uint32_t          kSortCount = 65536;
            std::vector<ZHLN::SortItem> items(kSortCount);
            std::vector<ZHLN::SortItem> temp(kSortCount);

            uint64_t seed = 0x85432901CDEF7103ULL;
            for (uint32_t i = 0; i < kSortCount; ++i) {
                seed     = seed * 6364136223846793005ULL + 1442695040888963407ULL;
                items[i] = {.key = {seed}, .payload = i};
            }

            auto sortStats = ZHLN::Test::Benchmark("cpu.radix_sort_65536")
                                 .Warmup(2)
                                 .Samples(7)
                                 .Items(kSortCount)
                                 .Run([&] { ZHLN::RadixSort64(items.data(), temp.data(), kSortCount); });

            // Verify Monotonic Sort Order
            for (uint32_t i = 1; i < kSortCount; ++i) {
                if (items[i - 1].key.value > items[i].key.value) {
                    return std::unexpected(PerfTestError::CoreContainersThroughputFailed);
                }
            }

            ZHLN::Println("    [RadixSort64] 65,536 keys sorted in {:.3f} ms ({:.2f} Mkeys/sec)", sortStats.minMs, sortStats.ItemsPerSecond() / 1'000'000.0);

            // B. ZHLN::HashMap Stress (30,000 Inserts & Lookups)
            constexpr uint32_t kMapOps = 30000;

            auto mapStats = ZHLN::Test::Benchmark("cpu.hashmap_30k_insert_find")
                                .Warmup(2)
                                .Samples(7)
                                .Items(kMapOps * 2)
                                .Run([&] {
                                    ZHLN::HashMap<uint32_t, uint32_t> map;
                                    for (uint32_t i = 0; i < kMapOps; ++i) {
                                        map.Insert(i, i * 7 + 3);
                                    }
                                    uint32_t foundCount = 0;
                                    for (uint32_t i = 0; i < kMapOps; ++i) {
                                        const uint32_t* val = map.Find(i);
                                        if (val && *val == (i * 7 + 3)) {
                                            foundCount++;
                                        }
                                    }
                                    ZHLN::Test::ExpectEq(foundCount, kMapOps);
                                });
            ZHLN::Println("    [HashMap] 30,000 Insert + Find operations in {:.3f} ms ({:.2f} kOps/sec)", mapStats.minMs, mapStats.ItemsPerSecond() / 1000.0);

            // C. ZHLN::ObjectPool Recycling
            struct TrackedNode {
                uint64_t data[4] {};
            };
            ZHLN::ObjectPool<TrackedNode, 128> pool;
            constexpr size_t                   kPoolAllocations = 50000;
            std::vector<TrackedNode*>          allocatedNodes;
            allocatedNodes.reserve(kPoolAllocations);

            auto poolStats = ZHLN::Test::Benchmark("cpu.object_pool_50k_cycles")
                                 .Warmup(2)
                                 .Samples(7)
                                 .Items(kPoolAllocations * 2)
                                 .Run([&] {
                                     allocatedNodes.clear();
                                     for (size_t i = 0; i < kPoolAllocations; ++i) {
                                         allocatedNodes.push_back(pool.Create());
                                     }
                                     for (size_t i = 0; i < kPoolAllocations; ++i) {
                                         pool.Destroy(allocatedNodes[i]);
                                     }
                                 });
            ZHLN::Println(
                "    [ObjectPool] 50,000 Alloc + Destroy cycles in {:.3f} ms ({:.2f} Mops/sec)", poolStats.minMs,
                poolStats.ItemsPerSecond() / 1'000'000.0
            );

            return {};
        }

        // ====================================================================
        // 2. ISOLATED: Fiber Task System & Concurrency Scaling
        // ====================================================================
        auto isolated_02_task_system_and_fiber_concurrency() -> std::expected<void, ZHLN::Error> {
            ZHLN::Println("\n  {}--- Subsystem 2: TaskSystem & Fiber Concurrency ---{}", ZHLN::Color::Cyan, ZHLN::Color::Reset);

            // A. High-Fanout ParallelFor (1,000,000 items in 1,024 chunk sizes)
            constexpr uint32_t kParallelCount = 1000000;
            std::vector<float> dataArray(kParallelCount, 1.0f);
            std::atomic<float> totalSum {0.0f};

            auto pForStats = ZHLN::Test::Benchmark("cpu.parallel_for_1m_items")
                                 .Warmup(2)
                                 .Samples(5)
                                 .Items(kParallelCount)
                                 .Run([&] {
                                     ZHLN::TaskSystem::ParallelFor(kParallelCount, 1024, [&](uint32_t start, uint32_t end, uint32_t) {
                                         float localAccum = 0.0f;
                                         for (uint32_t i = start; i < end; ++i) {
                                             dataArray[i] = std::sqrt(static_cast<float>(i) * 2.5f + 1.0f);
                                             localAccum += dataArray[i];
                                         }
                                         totalSum.fetch_add(localAccum, std::memory_order::relaxed);
                                     });
                                 });

            ZHLN::Test::ExpectTrue(totalSum.load() > 0.0f);
            ZHLN::Println(
                "    [ParallelFor] 1,000,000 sqrt math iterations in {:.3f} ms ({:.2f} Mitems/sec) [median {:.3f}, worst {:.3f}, n={}]", pForStats.minMs,
                pForStats.ItemsPerSecond() / 1'000'000.0, pForStats.medianMs, pForStats.maxMs, pForStats.samples
            );

            // B. Nested Parallel Dispatch (Fibers executing child tasks)
            constexpr size_t      kOuterTasks = 32;
            constexpr size_t      kInnerTasks = 256;
            std::atomic<uint32_t> nestedCounter {0};

            struct Payload {
                std::atomic<uint32_t>* counter;
            } payload {&nestedCounter};

            auto outerJob = [](void* raw) {
                auto* p = static_cast<Payload*>(raw);
                ZHLN::TaskSystem::ParallelFor(kInnerTasks, 16, [&](uint32_t start, uint32_t end, uint32_t) {
                    p->counter->fetch_add(end - start, std::memory_order::relaxed);
                });
            };

            std::array<ZHLN::TaskSystem::Task, kOuterTasks> tasks {};
            for (auto& t: tasks) {
                t = {.func = outerJob, .arg = &payload};
            }

            constexpr uint32_t kRepeats = 64;

            {
                ZHLN::TaskSystem::Counter warmCounter;
                ZHLN::TaskSystem::Dispatch(tasks, &warmCounter);
                ZHLN::TaskSystem::Wait(&warmCounter);
            }

            auto nestedStats = ZHLN::Test::Benchmark("cpu.nested_fibers_32x256_hot")
                                   .Warmup(2)
                                   .Samples(9)
                                   .Iterations(kRepeats)
                                   .Items(kOuterTasks * kInnerTasks)
                                   .Run([&] {
                                       ZHLN::TaskSystem::Counter syncCounter;
                                       ZHLN::TaskSystem::Dispatch(tasks, &syncCounter);
                                       ZHLN::TaskSystem::Wait(&syncCounter);
                                   });

            ZHLN::Test::ExpectTrue(nestedCounter.load() > 0);
            ZHLN::Println(
                "    [Nested Fibers] 32 x 256 child tasks dispatched & synced in {:.3f} ms/dispatch over {} back-to-back dispatches "
                "[median {:.3f}, worst {:.3f}, n={}]",
                nestedStats.minMs, kRepeats, nestedStats.medianMs, nestedStats.maxMs, nestedStats.samples
            );

            return {};
        }

        // ====================================================================
        // 3. ISOLATED: ECS Bulk Lifecycle & Dense Archetype Iteration
        // ====================================================================
        auto isolated_03_ecs_bulk_lifecycle_and_iteration() -> std::expected<void, ZHLN::Error> {
            ZHLN::Println("\n  {}--- Subsystem 3: ECS & Entity Command Buffer ---{}", ZHLN::Color::Cyan, ZHLN::Color::Reset);

            ZHLN::ECS::Registry reg;
            reg.RegisterComponents<
                ZHLN::Components::TransformComponent, ZHLN::Components::MovementComponent, AgentHealthComponent,
                AgentCombatStateComponent>();

            constexpr size_t          kTotalEntities = 40000;
            std::vector<ZHLN::Entity> createdEntities;
            createdEntities.reserve(kTotalEntities);

            // A. Batch Entity Creation with Multiple Components
            auto createStats = ZHLN::Test::Benchmark("cpu.ecs_create_40k_entities")
                                   .Warmup(1)
                                   .Samples(3)
                                   .Items(kTotalEntities)
                                   .Run([&] {
                                       ZHLN::ECS::Registry benchReg;
                                       benchReg.RegisterComponents<
                                           ZHLN::Components::TransformComponent, ZHLN::Components::MovementComponent,
                                           AgentHealthComponent, AgentCombatStateComponent>();
                                       for (size_t i = 0; i < kTotalEntities; ++i) {
                                           (void) benchReg.Create(
                                               ZHLN::Components::TransformComponent {.position = JPH::Vec3(static_cast<float>(i), 1.0f, 0.0f)},
                                               ZHLN::Components::MovementComponent {.speed = 6.5f}, AgentHealthComponent {.currentHealth = 100.0f},
                                               AgentCombatStateComponent {.attackRange = 12.0f}
                                           );
                                       }
                                   });
            for (size_t i = 0; i < kTotalEntities; ++i) {
                createdEntities.push_back(reg.Create(
                    ZHLN::Components::TransformComponent {.position = JPH::Vec3(static_cast<float>(i), 1.0f, 0.0f)},
                    ZHLN::Components::MovementComponent {.speed = 6.5f}, AgentHealthComponent {.currentHealth = 100.0f},
                    AgentCombatStateComponent {.attackRange = 12.0f}
                ));
            }
            ZHLN::Println(
                "    [ECS Create] 40,000 Entities (4 Components each) created in {:.3f} ms ({:.2f} kEntities/sec)", createStats.minMs,
                createStats.ItemsPerSecond() / 1000.0
            );

            // B. Dense Array Direct Vectorized Iteration (GetRawArray & Patch)
            auto iterStats = ZHLN::Test::Benchmark("cpu.ecs_dense_iterate_10x40k")
                                 .Warmup(2)
                                 .Samples(5)
                                 .Items(10 * kTotalEntities)
                                 .Run([&] {
                                     auto healths = reg.GetRawArray<AgentHealthComponent>();
                                     auto moves   = reg.GetRawArray<ZHLN::Components::MovementComponent>();
                                     auto trans   = reg.GetRawArray<ZHLN::Components::TransformComponent>();

                                     for (size_t frame = 0; frame < 10; ++frame) {
                                         for (size_t i = 0; i < healths.size(); ++i) {
                                             trans[i].position.SetX(trans[i].position.GetX() + moves[i].speed * 0.016f);
                                             healths[i].currentHealth = std::min(healths[i].maxHealth, healths[i].currentHealth + 0.1f);
                                         }
                                     }
                                 });
            ZHLN::Println(
                "    [ECS Dense Iterate] 10 Frames x 40,000 Entities updated in {:.3f} ms ({:.2f} Mupdates/sec) [median {:.3f}, worst {:.3f}, n={}]",
                iterStats.minMs, iterStats.ItemsPerSecond() / 1'000'000.0, iterStats.medianMs, iterStats.maxMs, iterStats.samples
            );

            // C. EntityCommandBuffer Bulk Playback
            auto ecbStats = ZHLN::Test::Benchmark("cpu.ecb_playback_15k")
                                .Warmup(1)
                                .Samples(3)
                                .Items(15000)
                                .Run([&] {
                                    ZHLN::ECS::EntityCommandBuffer ecb(reg);
                                    for (size_t i = 0; i < 15000; ++i) {
                                        ZHLN::Entity tempE = ecb.CreateEntity(
                                            ZHLN::Components::TransformComponent {.position = JPH::Vec3(0.0f, 0.0f, 0.0f)},
                                            AgentHealthComponent {.currentHealth = 50.0f}
                                        );
                                        ecb.AddComponent<ZHLN::Components::MovementComponent>(tempE);
                                    }
                                    ecb.Playback();
                                });
            ZHLN::Println("    [ECB Playback] 15,000 Deferred Creations + Mutations executed in {:.3f} ms", ecbStats.minMs);

            return {};
        }

        // ====================================================================
        // 4. ISOLATED: System Graph Parallel Execution & Hazard Management
        // ====================================================================
        auto isolated_04_system_graph_scheduling_throughput() -> std::expected<void, ZHLN::Error> {
            ZHLN::Println("\n  {}--- Subsystem 4: SystemGraph Multi-Threading ---{}", ZHLN::Color::Cyan, ZHLN::Color::Reset);

            ZHLN::ECS::SystemGraph  graph;
            static std::atomic<int> readCounters[4] {};
            static std::atomic<int> writeCounter {0};

            for (auto& c: readCounters) {
                c.store(0);
            }
            writeCounter.store(0);

            // Add 4 Independent Reader Systems
            for (int r = 0; r < 4; ++r) {
                graph.AddSystem({
                    .update_func    = [](ZHLN::Engine&, float) { readCounters[0].fetch_add(1, std::memory_order::relaxed); },
                    .name           = "ReaderSystem",
                    .access_pattern = {ZHLN::ECS::Read<AgentHealthComponent>()},
                    .enabled        = true,
                });
            }

            // Add Dependent Writer System (Runs after all readers)
            graph.AddSystem({
                .update_func    = [](ZHLN::Engine&, float) { writeCounter.fetch_add(1, std::memory_order::relaxed); },
                .name           = "WriterSystem",
                .access_pattern = {ZHLN::ECS::Write<AgentHealthComponent>()},
                .enabled        = true,
            });

            graph.Compile();

            alignas(ZHLN::Engine) std::byte fakeEngineStorage[sizeof(ZHLN::Engine)] {};
            auto*                           fakeEngine = reinterpret_cast<ZHLN::Engine*>(fakeEngineStorage);

            constexpr int kGraphIterations = 2000;
            auto          graphStats       = ZHLN::Test::Benchmark("cpu.systemgraph_2000_evals")
                                  .Warmup(1)
                                  .Samples(3)
                                  .Items(kGraphIterations)
                                  .Run([&] {
                                      for (auto& c: readCounters) {
                                          c.store(0, std::memory_order::relaxed);
                                      }
                                      writeCounter.store(0, std::memory_order::relaxed);
                                      for (int i = 0; i < kGraphIterations; ++i) {
                                          graph.Execute(*fakeEngine, 0.016f);
                                      }
                                  });

            ZHLN::Test::ExpectEq(writeCounter.load(), kGraphIterations);
            ZHLN::Println(
                "    [SystemGraph] 2,000 graph evaluations (5 systems each) in {:.3f} ms ({:.2f} cycles/sec)", graphStats.minMs,
                (kGraphIterations * 1000.0) / graphStats.minMs
            );

            return {};
        }

        // ====================================================================
        // 5. ISOLATED: Physics Multi-Body Simulation & Raycasting
        // ====================================================================
        auto isolated_05_physics_simulation_and_raycasts() -> std::expected<void, ZHLN::Error> {
            ZHLN::Println("\n  {}--- Subsystem 5: Physics Simulation & Queries ---{}", ZHLN::Color::Cyan, ZHLN::Color::Reset);

            ZHLN::PhysicsConfig  cfg {.maxBodies = 1024, .maxBodyPairs = 2048, .maxContactConstraints = 2048, .tempAllocatorSize = 16 * 1024 * 1024};
            ZHLN::PhysicsContext pc(cfg);

            // Create Large Ground Plane
            auto groundShape = pc.GetOrCreateShape(ZHLN::Physics::ShapeType::Box, 200.0f, 1.0f, 200.0f);
            pc.CreateRigidBody(groundShape, JPH::RVec3(0, -1.0, 0), JPH::Quat::sIdentity(), JPH::EMotionType::Static, ZHLN::Layers::ID::NON_MOVING);

            // Populate 256 Dynamic Physics Bodies
            auto sphereShape = pc.GetOrCreateShape(ZHLN::Physics::ShapeType::Sphere, 0.5f);
            for (int x = -8; x < 8; ++x) {
                for (int z = -8; z < 8; ++z) {
                    pc.CreateRigidBody(
                        sphereShape, JPH::RVec3(x * 2.5, 5.0 + (x + z) * 0.2, z * 2.5), JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, ZHLN::Layers::ID::MOVING
                    );
                }
            }
            pc.OptimizeBroadphase();

            // A. Step 60 Simulation Frames
            auto stepStats = ZHLN::Test::Benchmark("cpu.physics_step_60f_256bodies")
                                 .Warmup(1)
                                 .Samples(3)
                                 .Items(60)
                                 .Run([&] {
                                     for (int i = 0; i < 60; ++i) {
                                         pc.Step(1.0f / 60.0f);
                                     }
                                 });
            ZHLN::Println("    [Physics Step] 60 frames (256 dynamic bodies) simulated in {:.3f} ms ({:.2f} FPS)", stepStats.minMs, 60000.0 / stepStats.minMs);

            // B. Mass Concurrent Raycast Queries (5,000 Raycasts via ParallelFor)
            constexpr uint32_t kRaycastCount = 5000;
            std::atomic<int>   hitCount {0};

            auto rayStats = ZHLN::Test::Benchmark("cpu.raycast_5000")
                                .Warmup(2)
                                .Samples(5)
                                .Items(kRaycastCount)
                                .Run([&] {
                                    hitCount.store(0, std::memory_order::relaxed);
                                    ZHLN::TaskSystem::ParallelFor(kRaycastCount, 128, [&](uint32_t start, uint32_t end, uint32_t) {
                                        int localHits = 0;
                                        for (uint32_t i = start; i < end; ++i) {
                                            float      posX = -20.0f + static_cast<float>(i % 100) * 0.4f;
                                            float      posZ = -20.0f + static_cast<float>(i / 100) * 0.8f;
                                            const auto hit  = pc.Raycast(JPH::RVec3(posX, 20.0, posZ), JPH::Vec3(0.0f, -1.0f, 0.0f), 40.0f);
                                            if (hit.hasHit) {
                                                localHits++;
                                            }
                                        }
                                        hitCount.fetch_add(localHits, std::memory_order::relaxed);
                                    });
                                });

            ZHLN::Test::ExpectTrue(hitCount.load() > 0);
            ZHLN::Println(
                "    [Raycast Fan-out] 5,000 Broadphase raycasts executed in {:.3f} ms ({:.2f} kRays/sec, Hits: {})", rayStats.minMs,
                rayStats.ItemsPerSecond() / 1000.0, hitCount.load()
            );

            return {};
        }

        // ====================================================================
        // 6. ISOLATED: Immediate-Mode UI CPU Layout (Clay)
        // ====================================================================
        auto isolated_06_gui_hierarchy_and_gc_churn() -> std::expected<void, ZHLN::Error> {
            ZHLN::Println("\n  {}--- Subsystem 6: Immediate-Mode GUI & Layout ---{}", ZHLN::Color::Cyan, ZHLN::Color::Reset);

            ZHLN::ECS::Registry registry;
            constexpr uint64_t  kSimulatedFrames = 100;

            auto uiStats = ZHLN::Test::Benchmark("cpu.gui_100f_complex")
                               .Warmup(1)
                               .Samples(3)
                               .Items(kSimulatedFrames)
                               .Run([&] {
                                   for (uint64_t frame = 1; frame <= kSimulatedFrames; ++frame) {
                                       ZHLN::GUI::Context gui(registry);
                                       gui.BeginFrame(0.0166f);

                                       gui.Box(
                                           "MainDashboard",
                                           ZHLN::GUI::BoxConfig {.width = {.fixed = 800.0f}, .height = {.fixed = 600.0f}, .direction = ZHLN::GUI::Direction::Column},
                                           [&]() {
                                               for (int row = 0; row < 20; ++row) {
                                                   gui.Box("", ZHLN::GUI::BoxConfig {.height = {.fixed = 24.0f}, .direction = ZHLN::GUI::Direction::Row}, [&]() {
                                                       gui.Text(std::format("Telemetry Channel #{}", row), 14.0f);
                                                       gui.Button(std::format("Action {}", row));
                                                   });
                                               }
                                           }
                                       );

                                       gui.EndFrame();
                                   }
                               });

            ZHLN::Println(
                "    [GUI Context] 100 frames of complex UI (20 rows x 2 widgets) built in {:.3f} ms ({:.2f} frames/sec)", uiStats.minMs,
                (kSimulatedFrames * 1000.0) / uiStats.minMs
            );

            return {};
        }

        // ====================================================================
        // 7. ISOLATED: 3D Audio Event Queueing & Batch Dispatch
        // ====================================================================
        auto isolated_07_audio_event_queue_throughput() -> std::expected<void, ZHLN::Error> {
            ZHLN::Println("\n  {}--- Subsystem 7: Audio Event Pipeline ---{}", ZHLN::Color::Cyan, ZHLN::Color::Reset);

            ZHLN::AudioContext audio;
            constexpr uint32_t kAudioEvents = 20000;

            auto audioStats = ZHLN::Test::Benchmark("cpu.audio_20k_spatial")
                                  .Warmup(1)
                                  .Samples(3)
                                  .Items(kAudioEvents)
                                  .Run([&] {
                                      for (uint32_t i = 0; i < kAudioEvents; ++i) {
                                          audio.PostEvent(
                                              ZHLN::AudioEvent {
                                                  .type     = ZHLN::AudioEventType::OneShot3D,
                                                  .position = JPH::Vec3(static_cast<float>(i % 50), 1.0f, static_cast<float>(i / 50)),
                                                  .volume   = 0.8f,
                                                  .pitch    = 1.0f,
                                                  .duration = 0.25f,
                                                  .waveType = ZHLN::AudioWaveformType::Triangle,
                                              }
                                          );
                                      }
                                      audio.FlushEvents();
                                  });

            ZHLN::Println(
                "    [Audio Context] 20,000 3D spatial events queued & flushed in {:.3f} ms ({:.2f} kEvents/sec)", audioStats.minMs,
                audioStats.ItemsPerSecond() / 1000.0
            );

            return {};
        }

        // ====================================================================
        // 8. UNIFIED MASTER INTEGRATION BENCHMARK (All Subsystems Concurrently)
        // ====================================================================
        auto unified_08_master_multisubsystem_benchmark() -> std::expected<void, ZHLN::Error> {
            ZHLN::Test::SetTimeout(60);
            ZHLN::Println("\n  {}================================================================{}", ZHLN::Color::Yellow, ZHLN::Color::Reset);
            ZHLN::Println("  {}--- UNIFIED MASTER BENCHMARK: All Subsystems Concurrently ---{}", ZHLN::Color::Yellow, ZHLN::Color::Reset);
            ZHLN::Println("  {}================================================================{}", ZHLN::Color::Yellow, ZHLN::Color::Reset);

            // 1. Initialize Subsystem Environments (CPU Only)
            ZHLN::PhysicsConfig physCfg {
                .maxBodies = 2048, .maxBodyPairs = 4096, .maxContactConstraints = 4096, .tempAllocatorSize = 32 * 1024 * 1024
            };
            ZHLN::ECS::Registry  registry;
            ZHLN::AudioContext   audio;
            ZHLN::PhysicsContext physicsContext(physCfg);
            ZHLN::Camera         mainCamera;

            mainCamera.position = JPH::Vec3(0.0f, 25.0f, -50.0f);
            mainCamera.yaw      = 90.0f;
            mainCamera.pitch    = -20.0f;

            // Register Custom Test Components
            registry.RegisterComponents<AgentHealthComponent, AgentCombatStateComponent, SpatialPerceptionComponent>();

            // 2. Setup Static Physics World Boundary
            auto groundShape = physicsContext.GetOrCreateShape(ZHLN::Physics::ShapeType::Box, 150.0f, 1.0f, 150.0f);
            physicsContext.CreateRigidBody(groundShape, JPH::RVec3(0, -1.0, 0), JPH::Quat::sIdentity(), JPH::EMotionType::Static, ZHLN::Layers::ID::NON_MOVING);

            // 3. Spawn 1,000 Active Dynamic Agents with Full Component State
            constexpr size_t          kAgentCount = 1000;
            std::vector<ZHLN::Entity> agentEntities;
            agentEntities.reserve(kAgentCount);

            auto agentShape = physicsContext.GetOrCreateShape(ZHLN::Physics::ShapeType::Capsule, 0.8f, 0.35f);

            std::mt19937                          rng(42);
            std::uniform_real_distribution<float> posDist(-60.0f, 60.0f);

            for (size_t i = 0; i < kAgentCount; ++i) {
                float      spawnX = posDist(rng);
                float      spawnZ = posDist(rng);
                JPH::RVec3 spawnPos(spawnX, 1.5, spawnZ);

                ZHLN::Entity bodyHandle =
                    physicsContext.CreateRigidBody(agentShape, spawnPos, JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, ZHLN::Layers::ID::MOVING);

                ZHLN::Entity agent = registry.Create(
                    ZHLN::Components::TransformComponent {.position = JPH::Vec3(spawnPos)},
                    ZHLN::Components::MovementComponent {.speed = 5.0f + static_cast<float>(i % 5)},
                    ZHLN::Components::PhysicsComponent {.physicsHandle = bodyHandle, .isStatic = false},
                    AgentHealthComponent {.currentHealth = 100.0f, .maxHealth = 100.0f},
                    AgentCombatStateComponent {.attackRange = 8.0f + static_cast<float>(i % 6)}, SpatialPerceptionComponent {}
                );

                physicsContext.SetBodyOwner(bodyHandle, agent);
                agentEntities.push_back(agent);
            }
            physicsContext.OptimizeBroadphase();

            ZHLN::Println("    [Setup] 1,000 Agents spawned across Physics, ECS, and Audio spatial environments.");

            // 4. Build and Compile Multi-Threaded SystemGraph for Unified Execution
            ZHLN::ECS::SystemGraph systemGraph;

            // System A: Perception & Spatial Raycasting (Parallel over Tasks)
            systemGraph.AddSystem({
                .update_func =
                    [](ZHLN::Engine&, float) {
                        // Handled in main loop for fine-grained multi-system sync
                    },
                .name           = "PerceptionSystem",
                .access_pattern = {ZHLN::ECS::Write<SpatialPerceptionComponent>(), ZHLN::ECS::Read<ZHLN::Components::TransformComponent>()},
                .enabled        = true,
            });

            // System B: Combat Logic & Health Management
            systemGraph.AddSystem({
                .update_func =
                    [](ZHLN::Engine&, float) {
                        // Handled in main loop
                    },
                .name           = "CombatSystem",
                .access_pattern = {ZHLN::ECS::Write<AgentHealthComponent>(), ZHLN::ECS::Read<AgentCombatStateComponent>()},
                .enabled        = true,
            });

            systemGraph.Compile();

            // 5. Execute 120 Continuous Simulation Frames (2 Full Seconds at 60 FPS)
            constexpr int         kTotalFrames = 120;
            constexpr float       kFixedDt     = 1.0f / 60.0f;
            std::atomic<uint64_t> totalRaysCast {0};
            std::atomic<uint64_t> totalAudioEvents {0};

            ZHLN::Test::BenchmarkTimer masterBenchmarkTimer;

            auto stats = ZHLN::Test::BenchmarkFrames("cpu.master_integrated")
                             .Warmup(0)
                             .Frames(kTotalFrames)
                             .Run([&](uint32_t frame) {
                                 // --- PHASE 1: Multi-Threaded Agent AI & Raycast Perception ---
                                 ZHLN::TaskSystem::ParallelFor(kAgentCount, 64, [&](uint32_t start, uint32_t end, uint32_t) {
                                     for (uint32_t i = start; i < end; ++i) {
                                         ZHLN::Entity e       = agentEntities[i];
                                         auto*        trans   = registry.Get<ZHLN::Components::TransformComponent>(e);
                                         auto*        percept = registry.Get<SpatialPerceptionComponent>(e);
                                         auto*        combat  = registry.Get<AgentCombatStateComponent>(e);
                                         auto*        phys    = registry.Get<ZHLN::Components::PhysicsComponent>(e);

                                         if (!trans || !percept || !combat) {
                                             continue;
                                         }

                                         // Raycast to probe surrounding terrain & dynamic obstacles
                                         JPH::Vec3  forward(std::sin(static_cast<float>(frame + i) * 0.05f), 0.0f, std::cos(static_cast<float>(frame + i) * 0.05f));
                                         const auto rayHit =
                                             physicsContext.Raycast(JPH::RVec3(trans->position + JPH::Vec3(0, 0.5f, 0)), forward, 10.0f, phys->physicsHandle);
                                         totalRaysCast.fetch_add(1, std::memory_order::relaxed);

                                         if (rayHit.hasHit) {
                                             percept->nearestDistance = rayHit.fraction * 10.0f;
                                             percept->threatDirection = rayHit.normal;
                                         }

                                         // Apply movement impulse based on perception
                                         physicsContext.SetLinearVelocity(phys->physicsHandle, forward * 4.0f);
                                     }
                                 });

                                 // --- PHASE 2: Physics World Simulation Step ---
                                 physicsContext.Step(kFixedDt);

                                 // --- PHASE 3: Sub-frame Position Extraction & State Sync ---
                                 for (size_t i = 0; i < kAgentCount; ++i) {
                                     ZHLN::Entity e     = agentEntities[i];
                                     auto*        trans = registry.Get<ZHLN::Components::TransformComponent>(e);
                                     auto*        phys  = registry.Get<ZHLN::Components::PhysicsComponent>(e);
                                     if (trans && phys) {
                                         JPH::RVec3 pos;
                                         if (physicsContext.TryGetBodyPosition(phys->physicsHandle, pos)) {
                                             trans->position = JPH::Vec3(pos);
                                         }
                                     }
                                 }

                                 // --- PHASE 4: Spatial Audio Event Queuing ---
                                 if (frame % 2 == 0) {
                                     for (size_t a = 0; a < 25; ++a) {
                                         size_t      idx   = (frame * 25 + a) % kAgentCount;
                                         const auto* trans = registry.Get<ZHLN::Components::TransformComponent>(agentEntities[idx]);
                                         if (trans) {
                                             audio.PostEvent(
                                                 ZHLN::AudioEvent {
                                                     .type     = ZHLN::AudioEventType::OneShot3D,
                                                     .position = trans->position,
                                                     .volume   = 0.5f,
                                                     .duration = 0.15f,
                                                 }
                                             );
                                             totalAudioEvents.fetch_add(1, std::memory_order::relaxed);
                                         }
                                     }
                                     audio.FlushEvents();
                                 }

                                 // --- PHASE 5: Immediate-Mode HUD / GUI Rebuild ---
                                 {
                                     ZHLN::GUI::Context gui(registry);
                                     gui.BeginFrame(kFixedDt);

                                     gui.Box(
                                         "UnifiedBenchmarkHUD",
                                         ZHLN::GUI::BoxConfig {.width = {.fixed = 300.0f}, .height = {.fixed = 200.0f}, .direction = ZHLN::GUI::Direction::Column},
                                         [&]() {
                                             gui.Text(std::format("Simulation Frame: {}", frame), 14.0f);
                                             gui.Text(std::format("Active Agents: {}", kAgentCount), 14.0f);
                                             gui.Text(std::format("Rays Processed: {}", totalRaysCast.load()), 14.0f);

                                             gui.Box("", ZHLN::GUI::BoxConfig {.height = {.fixed = 30.0f}, .direction = ZHLN::GUI::Direction::Row}, [&]() {
                                                 gui.Button("Pause Sim");
                                                 gui.Button("Dump Stats");
                                             });
                                         }
                                     );

                                     gui.EndFrame();
                                 }
                             });

            double totalBenchmarkDurationSec = masterBenchmarkTimer.ElapsedSeconds();

            ZHLN::Println("    [Throughput] Simulation Speed: {:.2f} FPS (Target: >= 60.0 FPS)", kTotalFrames / totalBenchmarkDurationSec);
            ZHLN::Println("    [Telemetry] Total Raycasts: {}, Total Audio Events: {}", totalRaysCast.load(), totalAudioEvents.load());

            // Master Verification Gates
            ZHLN::Test::ExpectTrue(totalRaysCast.load() == static_cast<uint64_t>(kTotalFrames * kAgentCount));
            ZHLN::Test::ExpectTrue(totalAudioEvents.load() > 0);
            ZHLN::Test::ExpectTrue((kTotalFrames / totalBenchmarkDurationSec) > 30.0); // Minimum throughput sanity gate

            if ((kTotalFrames / totalBenchmarkDurationSec) <= 30.0) {
                return std::unexpected(PerfTestError::UnifiedMasterSceneFailed);
            }

            return {};
        }
    };
};

// Exported for the perf group binary (RunPerfTests.cpp), which
// aggregates every suite in this directory through Runner::RunDeferred.
auto RunPerformanceSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<PerformanceTestSuite>();
}
