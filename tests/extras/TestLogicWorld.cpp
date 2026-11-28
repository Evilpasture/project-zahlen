// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#include "TestsFramework.hpp"
#include <Benchmark/Benchmark.hpp>
#include <Zahlen/COM/LogicWorldFactory.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <cmath>
#include <limits>
namespace {
using namespace ZHLN;
using Test::ExpectEq;
using Test::ExpectTrue;
auto Body(COM::ISimulation& sim, DVec3 position = {0, 8, 0}) {
    return sim.CreateBody(Physics::Box {}, Physics::DynamicBody {.position = position});
}
struct LogicWorldSuite {
    struct Tests {
        std::expected<void, ErrorCode> renderer_free_factory_validates_chain_and_configuration() {
            auto invalid = COM::CreateLogicWorld(COM::InChain(PhysicsConfig {.maxBodies = 0}));
            ExpectTrue(!invalid);
            auto unsupported = COM::CreateLogicWorld(COM::InChain(COM::LogicWorldInfo {}));
            ExpectTrue(!unsupported);
            auto made = COM::CreateLogicWorld();
            if (!ExpectTrue(made.has_value()))
                return {};
            COM::LogicWorldInfo info {};
            ExpectTrue((*made)->GetInfo(COM::OutChain(info)).HasValue());
            ExpectEq(info.physics.maxBodies, PhysicsConfig {}.maxBodies);
            ExpectEq(info.activePhysicsBodies, uint32_t(0));
            ExpectTrue(!(*made)->Advance(0).HasValue());
            ExpectTrue(!(*made)->Advance(-1).HasValue());
            ExpectTrue(!(*made)->Advance(std::numeric_limits<float>::quiet_NaN()).HasValue());
            return {};
        }
        std::expected<void, ErrorCode> retained_capability_owns_world_and_real_body_motion() {
            auto made = COM::CreateLogicWorld();
            if (!made)
                return std::unexpected(made.error());
            auto sim  = made->cast<COM::ISimulation>();
            auto body = Body(*sim.Get());
            if (!ExpectTrue(body.HasValue()))
                return {};
            *made       = {}; // queried simulation retains engine-owned runtime
            auto before = sim->BodyPosition(body.value);
            sim->Step(1.f / 30);
            auto after = sim->BodyPosition(body.value);
            ExpectTrue(before.HasValue() && after.HasValue());
            if (before.HasValue() && after.HasValue())
                ExpectTrue(after.value.y < before.value.y);
            sim->DestroyBody(body.value);
            ExpectTrue(!sim->BodyPosition(body.value).HasValue());
            return {};
        }
        std::expected<void, ErrorCode> typed_physics_replacement_detach_and_deferred_despawn_release_bodies() {
            auto made = COM::CreateLogicWorld();
            if (!made)
                return std::unexpected(made.error());
            auto&      world    = *made->Get();
            auto*      entities = world.Entities();
            auto*      sim      = world.Simulation();
            const auto e        = entities->CreateEntity();
            auto       a        = Body(*sim);
            auto       b        = Body(*sim);
            if (!ExpectTrue(a.HasValue() && b.HasValue()))
                return {};
            ExpectTrue(entities->Attach(e, Components::DynamicBodyComponent {.bodyID = a.value}) != nullptr);
            ExpectTrue(entities->Attach(e, Components::DynamicBodyComponent {.bodyID = b.value}) != nullptr);
            ExpectTrue(!sim->BodyPosition(a.value).HasValue());
            ExpectTrue(entities->Detach<Components::DynamicBodyComponent>(e));
            ExpectTrue(!sim->BodyPosition(b.value).HasValue());
            auto c = Body(*sim);
            if (!c.HasValue())
                return {};
            entities->Attach(e, Components::DynamicBodyComponent {.bodyID = c.value});
            const auto child = entities->CreateEntity();
            entities->Attach(child, Components::HierarchyComponent {.parent = e});
            entities->Despawn(e);
            ExpectTrue(entities->IsAlive(e));
            world.Flush();
            ExpectTrue(!entities->IsAlive(e));
            ExpectTrue(!entities->IsAlive(child));
            ExpectTrue(!sim->BodyPosition(c.value).HasValue());
            return {};
        }
        std::expected<void, ErrorCode> layout_rejection_and_dynamic_storage_do_not_expose_native_components() {
            auto made = COM::CreateLogicWorld();
            if (!made)
                return std::unexpected(made.error());
            auto*      e      = (*made)->Entities();
            auto       entity = e->CreateEntity();
            const auto name   = COM::IEntities::ComponentName<Components::DynamicBodyComponent>();
            ExpectTrue(e->AttachRaw(entity, name, sizeof(Components::DynamicBodyComponent), alignof(Components::DynamicBodyComponent), 0) == nullptr);
            ExpectTrue(e->AttachRaw(entity, "DynamicBodyComponent", sizeof(Components::DynamicBodyComponent), alignof(Components::DynamicBodyComponent),
                                    Reflect::LayoutHash<Components::DynamicBodyComponent>()) == nullptr);
            ExpectTrue(e->Attach(entity, Components::DecalComponent {}) == nullptr);
            ExpectTrue(e->Attach(entity, Components::CSGCutterComponent {}) == nullptr);
            ExpectTrue(e->Attach(entity, Components::EnvironmentMapComponent {}) == nullptr);
            auto* value = static_cast<uint64_t*>(e->AttachRaw(entity, "bench.counter", 8, 8, 0));
            if (ExpectTrue(value != nullptr))
                *value = 42;
            auto* fetched = static_cast<uint64_t*>(e->GetRaw(entity, "bench.counter"));
            if (ExpectTrue(fetched != nullptr))
                ExpectEq(*fetched, uint64_t(42));
            ExpectEq(e->EntitiesWithRaw("bench.counter", nullptr, 0), uint32_t(1));
            ExpectTrue(e->DetachRaw(entity, "bench.counter"));
            (*made)->Clear();
            ExpectTrue(!e->IsAlive(entity));
            return {};
        }
        std::expected<void, ErrorCode> per_world_dynamic_schema_is_isolated_across_worlds() {
            constexpr std::string_view kSharedName = "logic_world.isolated_dynamic";
            {
                auto worldA = COM::CreateLogicWorld();
                if (!worldA)
                    return std::unexpected(worldA.error());
                auto*      e  = (*worldA)->Entities();
                const auto id = e->CreateEntity();
                auto*      p  = static_cast<uint64_t*>(e->AttachRaw(id, kSharedName, sizeof(uint64_t), alignof(uint64_t), 0x1111ULL));
                if (!ExpectTrue(p != nullptr))
                    return {};
                *p = 0xCAFEBABEULL;
                // Same world rejects conflicting layout for the same name.
                ExpectTrue(e->AttachRaw(id, kSharedName, 32, 8, 0x2222ULL) == nullptr);
                auto* got = static_cast<uint64_t*>(e->GetRaw(id, kSharedName));
                if (ExpectTrue(got != nullptr))
                    ExpectEq(*got, uint64_t(0xCAFEBABEULL));
            }
            {
                auto worldB = COM::CreateLogicWorld();
                if (!worldB)
                    return std::unexpected(worldB.error());
                auto*      e  = (*worldB)->Entities();
                const auto id = e->CreateEntity();
                // Separate world owns an independent Registry schema and accepts
                // a different size/layoutHash under the same dynamic name.
                auto* p = static_cast<uint64_t*>(e->AttachRaw(id, kSharedName, 32, 8, 0x2222ULL));
                if (!ExpectTrue(p != nullptr))
                    return {};
                p[0] = 0x12345678ULL;
                p[3] = 0x87654321ULL;
                ExpectTrue(e->AttachRaw(id, kSharedName, sizeof(uint64_t), alignof(uint64_t), 0x1111ULL) == nullptr);
                auto* got = static_cast<uint64_t*>(e->GetRaw(id, kSharedName));
                if (ExpectTrue(got != nullptr)) {
                    ExpectEq(got[0], uint64_t(0x12345678ULL));
                    ExpectEq(got[3], uint64_t(0x87654321ULL));
                }
            }
            return {};
        }
        std::expected<void, ErrorCode> real_benchmark_preserves_sizing_sync_history_reset_and_reinitialization() {
            Bench::LogicBenchmark benchmark;
            benchmark.Step(1.f / 60);
            ExpectEq(benchmark.GetMetrics().totalTicks, uint64_t(0));
            if (!ExpectTrue(benchmark.Initialize(64, PhysicsConfig {.maxBodies = 1})))
                return {};
            auto*               world = benchmark.GetWorld();
            COM::LogicWorldInfo info {};
            ExpectTrue(world->GetInfo(COM::OutChain(info)).HasValue());
            ExpectTrue(info.physics.maxBodies >= 64 + Bench::LogicBenchmark::kBodyReserve);
            ExpectTrue(info.physics.maxBodyPairs >= 4 * (64 + Bench::LogicBenchmark::kBodyReserve));
            auto entities = world->Entities()->EntitiesWith<Components::TransformComponent>();
            ExpectEq(entities.size(), size_t(64));
            if (entities.empty())
                return {};
            const auto first  = entities.front();
            const auto before = world->Entities()->Get<Components::TransformComponent>(first)->position.y;
            benchmark.Step(1.f / 60);
            auto after = world->Entities()->Get<Components::TransformComponent>(first)->position.y;
            ExpectTrue(after < before);
            for (size_t i = 1; i < 270; ++i)
                benchmark.Step(1.f / 60);
            ExpectEq(benchmark.GetMetrics().totalTicks, uint64_t(270));
            ExpectEq(benchmark.GetMetrics().entityCount, size_t(64));
            ExpectEq(benchmark.GetTickHistory().size(), Bench::LogicBenchmark::kHistoryCapacity);
            ExpectTrue(std::isfinite(benchmark.GetMetrics().avgTickTimeMs));
            ExpectTrue(benchmark.GetMetrics().maxTickTimeMs >= benchmark.GetMetrics().minTickTimeMs);
            benchmark.Reset();
            ExpectTrue(!benchmark.HasWorld());
            ExpectTrue(benchmark.GetTickHistory().empty());
            ExpectTrue(benchmark.Initialize(0));
            benchmark.SpawnWorkload(100000);
            ExpectEq(benchmark.GetMetrics().entityCount, size_t(959)); // floor occupies one of 960 available slots
            benchmark.Step(1.f / 60);
            ExpectEq(benchmark.GetMetrics().totalTicks, uint64_t(1));
            return {};
        }
    };
};
} // namespace
