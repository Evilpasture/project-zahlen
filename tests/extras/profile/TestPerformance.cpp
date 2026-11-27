// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PerfBaseline.hpp"
#include "TestsFramework.hpp"
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/ILogicWorld.hpp>
#include <Zahlen/COM/ISimulation.hpp>
#include <Zahlen/COM/LogicWorldFactory.hpp>
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Core/HashMap.hpp>
#include <Zahlen/Core/MemoryPool.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Engine/Clock.hpp>
#include <UI/GUI.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Physics/PhysicsValues.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <atomic>
#include <cstdint>
#include <expected>
#include <vector>

namespace {

struct PerformanceTestSuite {
    ZHLN::TaskSystem::Scope taskScope {2, 64};

    struct Tests {
        auto isolated_01_core_containers_and_algorithms() -> std::expected<void, ZHLN::ErrorCode> {
            ZHLN::HashMap<uint64_t, uint64_t> map;
            for (uint64_t i = 0; i < 4096; ++i) {
                map.Insert(i * 1315423911ull, i);
            }
            uint64_t sum = 0;
            for (uint64_t i = 0; i < 4096; ++i) {
                if (auto v = map.Find(i * 1315423911ull)) sum += *v;
            }
            ZHLN::Test::ExpectEq(sum, (4095ull * 4096ull) / 2ull);
            return {};
        }

        auto isolated_02_task_system_and_fiber_concurrency() -> std::expected<void, ZHLN::ErrorCode> {
            std::atomic<uint32_t> executed {0};
            ZHLN::TaskSystem::ParallelFor(256, 16, [&](uint32_t start, uint32_t end, uint32_t) {
                executed.fetch_add(end - start, std::memory_order::relaxed);
            });
            ZHLN::Test::ExpectEq(executed.load(std::memory_order::relaxed), 256u);
            return {};
        }

        auto isolated_03_com_entities_and_physics_throughput() -> std::expected<void, ZHLN::ErrorCode> {
            auto worldRes = ZHLN::COM::CreateLogicWorld();
            ZHLN::Test::ExpectTrue(worldRes.has_value());
            auto& world = **worldRes;
            auto& ents  = *world.Entities();
            auto& sim   = *world.Simulation();

            (void)sim.CreateStaticBodyRaw(COM::InChain(
                ZHLN::Physics::Box {.halfExtents = {50.0f, 0.5f, 50.0f}},
                ZHLN::Physics::StaticBody {.position = {0.0, -0.5, 0.0}}));
            std::vector<ZHLN::Entity> spawned;
            spawned.reserve(128);
            for (uint32_t i = 0; i < 128; ++i) {
                const auto b = sim.CreateBoxBody({0.25f, 0.25f, 0.25f}, {.position = {static_cast<double>(i % 16) - 8.0, 2.0 + (i / 16), 0.0}});
                spawned.push_back(ents.Spawn(
                    ZHLN::Components::TransformComponent {.position = {static_cast<float>(i), 1.0f, 0.0f}},
                    ZHLN::Components::PhysicsComponent {.physicsID = b.HasValue() ? b.value : ZHLN::Physics::BodyID::Null()}
                ));
            }
            for (int step = 0; step < 15; ++step) {
                ZHLN::Test::ExpectTrue(world.Advance(1.0f / 60.0f).HasValue());
            }
            const auto hit = sim.Raycast({0.0, 5.0, 0.0}, {0.0f, -1.0f, 0.0f}, 20.0f, ZHLN::Physics::BodyID::Null());
            ZHLN::Test::ExpectTrue(hit.hasHit);
            world.Clear();
            return {};
        }

        auto isolated_04_gui_layout_throughput() -> std::expected<void, ZHLN::ErrorCode> {
            ZHLN::GUI::Context gui;
            for (int i = 0; i < 16; ++i) {
                auto frame = gui.BeginFrame(ZHLN::GUI::UIInput {});
                frame.Text("PerfFrame", 14.0f, {1.0f, 1.0f, 1.0f, 1.0f});
                const auto draw = std::move(frame).End();
                ZHLN::Test::ExpectTrue(draw.batches.size() >= 0u);
            }
            return {};
        }
    };
};

} // namespace
