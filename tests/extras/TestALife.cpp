// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <ALife/ALifeComponents.hpp>
#include <ALife/Simulator.hpp>
#include <ALife/SpatialGrid.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/ILogicWorld.hpp>
#include <Zahlen/COM/LogicWorldFactory.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <atomic>
#include <expected>
#include <vector>

namespace {

struct ALifeTestSuite {
    ZHLN::TaskSystem::Scope taskScope {1, 16};

    struct Tests {
        std::expected<void, ZHLN::ErrorCode> spatial_grid_inserts_moves_and_handles_generation_reuse() {
            using namespace ZHLN;
            using namespace ZHLN::ALife;
            auto worldRes = COM::CreateLogicWorldRef();
            Test::ExpectTrue(worldRes.has_value());
            auto& reg = *(*worldRes)->Entities();

            SpatialGrid grid(64, 64, 50.0f);

            const Entity e1 = reg.CreateEntity();
            auto*        c1 = reg.Attach(e1, ALifeComponent {.position = {100.0, 0.0, 100.0}, .self_entity = e1});
            grid.UpdateEntity(reg, e1, DVec3 {-1.0, -1.0, -1.0});

            const Entity e2 = reg.CreateEntity();
            auto*        c2 = reg.Attach(e2, ALifeComponent {.position = {110.0, 0.0, 100.0}, .self_entity = e2});
            grid.UpdateEntity(reg, e2, DVec3 {-1.0, -1.0, -1.0});

            const Entity e3 = reg.CreateEntity();
            (void)reg.Attach(e3, ALifeComponent {.position = {800.0, 0.0, 800.0}, .self_entity = e3});
            grid.UpdateEntity(reg, e3, DVec3 {-1.0, -1.0, -1.0});

            Test::ExpectTrue(c1 != nullptr && c2 != nullptr);
            std::vector<Entity> results = grid.Query(reg, DVec3 {100.0, 0.0, 100.0}, 20.0f);
            Test::ExpectEq(results.size(), size_t {2});

            const DVec3 oldPos = c2->position;
            c2->position       = {300.0, 0.0, 300.0};
            grid.UpdateEntity(reg, e2, oldPos);

            results = grid.Query(reg, DVec3 {100.0, 0.0, 100.0}, 20.0f, std::move(results));
            Test::ExpectEq(results.size(), size_t {1});
            if (results.size() == 1) Test::ExpectEq(results[0], e1);

            const DVec3 movedFrom = c2->position;
            c2->position          = {103.0, 0.0, 100.0};
            grid.UpdateEntity(reg, e2, movedFrom);
            grid.RemoveEntity(reg, e1);
            reg.Despawn(e1);
            (*worldRes)->Flush();

            const Entity replacement = reg.CreateEntity();
            (void)reg.Attach(replacement, ALifeComponent {.position = {105.0, 0.0, 100.0}, .self_entity = replacement});
            grid.UpdateEntity(reg, replacement, DVec3 {-1.0, -1.0, -1.0});

            results = grid.Query(reg, DVec3 {100.0, 0.0, 100.0}, 20.0f, std::move(results));
            Test::ExpectEq(replacement.index, e1.index);
            Test::ExpectNe(replacement.generation, e1.generation);
            Test::ExpectEq(results.size(), size_t {2});
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> simulator_updates_agents_saves_and_reloads_via_com() {
            using namespace ZHLN;
            using namespace ZHLN::ALife;

            auto worldRes = COM::CreateLogicWorldRef();
            Test::ExpectTrue(worldRes.has_value());
            auto& reg = *(*worldRes)->Entities();

            const Entity a = reg.CreateEntity();
            (void)reg.Attach(a, ALifeComponent {.position = {100.0, 0.0, 100.0}, .self_entity = a});
            const Entity b = reg.CreateEntity();
            (void)reg.Attach(b, ALifeComponent {.position = {105.0, 0.0, 100.0}, .self_entity = b});

            Simulator sim {};
            std::atomic<uint32_t> thinks {0};
            sim.on_think = [&](Simulator&, Entity) { thinks.fetch_add(1, std::memory_order::relaxed); };
            sim.SetRelation(1, 2, -0.75f);
            sim.Update(reg, 0.5f, DVec3 {0.0, 0.0, 0.0});

            const auto nearby = sim.GetGrid().Query(reg, DVec3 {102.0, 0.0, 100.0}, 25.0f);
            Test::ExpectEq(nearby.size(), size_t {2});

            Test::TempDir tmp;
            const auto savePath = (tmp.Path() / "alife_state.bin").string();
            Test::ExpectTrue(sim.Save(reg, savePath.c_str()));

            auto world2Res = COM::CreateLogicWorldRef();
            auto& reg2 = *(*world2Res)->Entities();
            Simulator sim2 {};
            Test::ExpectTrue(sim2.Load(reg2, savePath.c_str()));
            Test::ExpectEq(reg2.EntitiesWith<ALifeComponent>().size(), size_t {2});
            return {};
        }
    };
};

} // namespace
