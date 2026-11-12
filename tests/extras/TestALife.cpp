// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Math/Vec3.hpp>
#include <ALife/ALifeComponents.hpp>
#include <ALife/Factions.hpp>
#include <ALife/GOAP.hpp>
#include <ALife/Graph.hpp>
#include <ALife/SpatialGrid.hpp>
#include <ALife/Simulator.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <atomic>
#include <algorithm>
#include <expected>
#include <memory_resource>
#include <utility>

enum class ALifeTestError : uint8_t {
    GOAPPlanFailed ZHLN_ANNOTATION(ZHLN::Description<"GOAP solver failed to compute valid action sequence.">{}) = 1,
    PathfindingFailed ZHLN_ANNOTATION(ZHLN::Description<"LevelGraph A* failed to find shortest path.">{}),
    SpatialGridQueryFailed ZHLN_ANNOTATION(ZHLN::Description<"SpatialGrid spatial query missed expected entity.">{})
};

namespace {

using ZHLN::DVec3;

struct ALifeTestSuite {
    struct Tests {
        // --- 1. GOAP Action Planner ---
        std::expected<void, ZHLN::ErrorCode> goap_solver_planning() {
            ZHLN::ALife::WorldStateRegistry reg;
            uint32_t                        hasWeapon    = reg.RegisterKey("hasWeapon");
            uint32_t                        enemySpotted = reg.RegisterKey("enemySpotted");
            uint32_t                        enemyDead    = reg.RegisterKey("enemyDead");

            // Setup World States
            ZHLN::ALife::WorldState current;
            current.Set(enemySpotted, true);

            ZHLN::ALife::WorldState goal;
            goal.Set(enemyDead, true);

            // Action 1: Pickup Weapon
            ZHLN::ALife::Action getGun;
            getGun.name = "PickupGun";
            getGun.cost = 1;
            getGun.effects.Set(hasWeapon, true);

            // Action 2: Shoot Enemy (Requires weapon & spotted enemy)
            ZHLN::ALife::Action killEnemy;
            killEnemy.name = "ShootEnemy";
            killEnemy.cost = 2;
            killEnemy.preconditions.Set(hasWeapon, true);
            killEnemy.preconditions.Set(enemySpotted, true);
            killEnemy.effects.Set(enemyDead, true);

            std::vector<ZHLN::ALife::Action> actions = {getGun, killEnemy};

            ZHLN::ALife::PlanRequest request {.current = current, .goal = goal};
            ZHLN::ALife::Plan        plan = ZHLN::ALife::SolvePlan(request, actions);

            // Plan should contain 2 actions: PickupGun -> ShootEnemy
            ZHLN::Test::ExpectEq(plan.count, 2u);
            if (plan.count == 2) {
                ZHLN::Test::ExpectEq(std::string_view(plan.actions[0].name.c_str()), std::string_view("PickupGun"));
                ZHLN::Test::ExpectEq(std::string_view(plan.actions[1].name.c_str()), std::string_view("ShootEnemy"));
            }

            return {};
        }

        // --- 2. LevelGraph A* Pathfinding ---
        std::expected<void, ZHLN::ErrorCode> graph_astar_pathfinding() {
            // Create a 4-node linear graph: 0 -- 1 -- 2 -- 3
            ZHLN::ALife::LevelGraph graph(4);
            graph.GetNode(0).position = {0, 0, 0};
            graph.GetNode(1).position = {10, 0, 0};
            graph.GetNode(2).position = {20, 0, 0};
            graph.GetNode(3).position = {30, 0, 0};

            graph.Connect(0, 1);
            graph.Connect(1, 2);
            graph.Connect(2, 3);

            std::pmr::synchronized_pool_resource poolResource;
            ZHLN::ALife::PathWorkspace           workspace(4, &poolResource);

            uint32_t path[ZHLN::ALife::MAX_PATH_LENGTH];
            uint32_t pathLen = graph.FindPath(0, 3, path, workspace);

            // Expected path: 0 -> 1 -> 2 -> 3 (4 nodes)
            ZHLN::Test::ExpectEq(pathLen, 4u);
            if (pathLen == 4) {
                ZHLN::Test::ExpectEq(path[0], 0u);
                ZHLN::Test::ExpectEq(path[1], 1u);
                ZHLN::Test::ExpectEq(path[2], 2u);
                ZHLN::Test::ExpectEq(path[3], 3u);
            }

            return {};
        }

        // --- 3. Faction Relationship Matrix ---
        std::expected<void, ZHLN::ErrorCode> faction_relations() {
            ZHLN::ALife::FactionRegistry factions(8);

            uint32_t stalkers = factions.Register("Stalkers");
            uint32_t bandits  = factions.Register("Bandits");

            ZHLN::Test::ExpectTrue(stalkers != 0xFFFFFFFF);
            ZHLN::Test::ExpectTrue(bandits != 0xFFFFFFFF);

            // Default self-relation is 1.0 (allied)
            ZHLN::Test::ExpectEq(factions.GetRelation(stalkers, stalkers), 1.0f);

            // Default cross-relation is 0.0 (neutral)
            ZHLN::Test::ExpectEq(factions.GetRelation(stalkers, bandits), 0.0f);

            // Set hostility
            factions.SetRelation(stalkers, bandits, -1.0f);
            ZHLN::Test::ExpectEq(factions.GetRelation(stalkers, bandits), -1.0f);
            ZHLN::Test::ExpectEq(factions.GetRelation(bandits, stalkers), -1.0f); // Symmetric

            return {};
        }

        std::expected<void, ZHLN::ErrorCode> typed_simulator_preserves_phase_order_and_grid_membership() {
            using namespace ZHLN;
            using namespace ZHLN::ALife;
            ECS::Registry reg;
            reg.RegisterComponent<ALifeComponent>("ALifeComponent");
            Simulator sim;
            sim.GetTuning().switch_distance = 1.0f;
            const Entity first = reg.Create(ALifeComponent {.position = {100, 0, 100}, .state = State::Offline});
            const Entity second = reg.Create(ALifeComponent {.position = {110, 0, 100}, .state = State::Online});
            const Entity waiting = reg.Create(ALifeComponent {.position = {300, 0, 300}, .state = State::Online, .wait_time = 10});
            const Entity dead = reg.Create(ALifeComponent {.position = {800, 0, 800}, .state = State::Dead});
            SpatialGrid::WriteQuery query {reg};
            int thinkCount = 0, eventCount = 0, interactionCount = 0;
            bool phaseOrder = true;
            sim.on_think = [&](Simulator&, Entity e) {
                phaseOrder = phaseOrder && (e == first || e == second);
                ++thinkCount;
            };
            sim.on_event = [&](Simulator&, const Event&) { ++eventCount; };
            sim.on_interaction = [&](Simulator& current, Entity e, Entity neighbor) {
                const auto matches = current.GetGrid().Query(query.As<SpatialGrid::ReadQuery>(), {100, 0, 100}, 20.0f);
                phaseOrder = phaseOrder && thinkCount == 2 && eventCount == 1 && matches.size() == 2 &&
                    ((e == first && neighbor == second) || (e == second && neighbor == first));
                ++interactionCount;
            };
            sim.Update(query, 0.5f, {0, 0, 0});
            ZHLN::Test::ExpectTrue(phaseOrder && thinkCount == 2 && eventCount == 1 && interactionCount == 2);
            ZHLN::Test::ExpectEq(sim.GetGameTimeMS(), uint64_t {5000});
            ZHLN::Test::ExpectTrue(query.Get<ALifeComponent>(second)->state == State::Offline);
            ZHLN::Test::ExpectTrue(query.Get<ALifeComponent>(waiting)->state == State::Online);
            ZHLN::Test::ExpectEq(query.Get<ALifeComponent>(waiting)->wait_time, 5);
            ZHLN::Test::ExpectTrue(query.Get<ALifeComponent>(dead)->state == State::Dead);
            for (const Entity entity: {first, second, waiting, dead}) {
                ZHLN::Test::ExpectEq(query.Get<ALifeComponent>(entity)->self_entity, entity);
            }
            // A typed unlink remains compatible with the legacy const-registry query.
            sim.GetGrid().RemoveEntity(query, first);
            const ECS::Registry& readOnlyRegistry = reg;
            const auto remaining = sim.GetGrid().Query(readOnlyRegistry, {100, 0, 100}, 20.0f);
            ZHLN::Test::ExpectTrue(remaining.size() == 1 && remaining[0] == second);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> typed_and_legacy_interaction_entry_points_unlink_looted_entities() {
            using namespace ZHLN;
            using namespace ZHLN::ALife;
            ECS::Registry reg;
            reg.RegisterComponent<ALifeComponent>("ALifeComponent");
            Simulator sim;
            const Entity living = reg.Create(ALifeComponent {.position = {100, 0, 100}, .state = State::Offline, .power = 10});
            const Entity corpse1 = reg.Create(ALifeComponent {.position = {101, 0, 100}, .state = State::Dead, .loot_value = 30});
            const Entity corpse2 = reg.Create(ALifeComponent {.position = {102, 0, 100}, .state = State::Dead, .loot_value = 40});
            SpatialGrid::WriteQuery query {reg};
            for (Entity e: {living, corpse1, corpse2}) sim.GetGrid().UpdateEntity(query, e, {-1, -1, -1});
            sim.ResolveOfflineInteraction(query, living, corpse1);
            sim.ResolveOfflineInteraction(reg, corpse2, living);
            ZHLN::Test::ExpectEq(query.Get<ALifeComponent>(living)->power, 17);
            ZHLN::Test::ExpectTrue(query.Get<ALifeComponent>(corpse1)->is_looted && query.Get<ALifeComponent>(corpse2)->is_looted);
            const auto remaining = sim.GetGrid().Query(query.As<SpatialGrid::ReadQuery>(), {100, 0, 100}, 20.0f);
            ZHLN::Test::ExpectTrue(remaining.size() == 1 && remaining[0] == living);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> typed_simulator_parallel_chunks_join_before_grid_and_interactions() {
            using namespace ZHLN;
            using namespace ZHLN::ALife;
            TaskSystem::Scope tasks {2, 64, kMinimumFiberStackSize};
            ECS::Registry reg;
            reg.RegisterComponent<ALifeComponent>("ALifeComponent");
            Simulator sim;
            sim.GetTuning().switch_distance = 1.0f;
            constexpr uint32_t count = 300; // more than the 256-item chunk
            for (uint32_t i = 0; i < count; ++i) {
                reg.Create(ALifeComponent {.position = {100.0 + static_cast<double>(i) * 0.01, 0, 100}, .state = State::Offline});
            }
            std::atomic<uint32_t> thinkCount {0}, interactionCount {0};
            std::atomic<bool> phaseOrder {true};
            sim.on_think = [&](Simulator&, Entity) { thinkCount.fetch_add(1, std::memory_order_relaxed); };
            sim.on_interaction = [&](Simulator&, Entity, Entity) {
                if (thinkCount.load(std::memory_order_relaxed) != count) phaseOrder.store(false, std::memory_order_relaxed);
                interactionCount.fetch_add(1, std::memory_order_relaxed);
            };
            SpatialGrid::WriteQuery query {reg};
            sim.Update(query, 0.1f, {0, 0, 0});
            const auto nearby = sim.GetGrid().Query(query.As<SpatialGrid::ReadQuery>(), {100, 0, 100}, 20.0f);
            ZHLN::Test::ExpectTrue(phaseOrder.load() && thinkCount.load() == count && interactionCount.load() == count);
            ZHLN::Test::ExpectEq(nearby.size(), static_cast<size_t>(count));
            return {};
        }

        // --- 4. Spatial Grid Partitioning & Radial Query ---
        std::expected<void, ZHLN::ErrorCode> spatial_grid_queries() {
            ZHLN::ECS::Registry reg;
            reg.RegisterComponent<ZHLN::ALife::ALifeComponent>("ALifeComponent");

            // 100x100 grid with 50.0m cells
            ZHLN::ALife::SpatialGrid grid(100, 100, 50.0f);

            // Entity 1 at (100, 0, 100) -> Cell (2, 2)
            ZHLN::Entity e1 = reg.Create();
            auto&        c1 = reg.Add<ZHLN::ALife::ALifeComponent>(e1);
            c1.position = {100.0, 0.0, 100.0};
            c1.self_entity  = e1;
            grid.UpdateEntity(reg, e1, DVec3{-1, -1, -1});

            // Entity 2 at (110, 0, 100) (Distance 10m from e1)
            ZHLN::Entity e2 = reg.Create();
            auto&        c2 = reg.Add<ZHLN::ALife::ALifeComponent>(e2);
            c2.position = {110.0, 0.0, 100.0};
            c2.self_entity  = e2;
            grid.UpdateEntity(reg, e2, DVec3{-1, -1, -1});

            // Entity 3 at (800, 0, 800) (Far away)
            ZHLN::Entity e3 = reg.Create();
            auto&        c3 = reg.Add<ZHLN::ALife::ALifeComponent>(e3);
            c3.position = {800.0, 0.0, 800.0};
            c3.self_entity  = e3;
            grid.UpdateEntity(reg, e3, DVec3{-1, -1, -1});

            // Query within 20m of (100, 0, 100)
            std::vector<ZHLN::Entity> results = grid.Query(reg, DVec3{100.0, 0.0, 100.0}, 20.0f);

            // Should find e1 and e2, but NOT e3
            ZHLN::Test::ExpectEq(results.size(), size_t {2});

            // Move e2 outside the radius
            DVec3 oldPos = c2.position;
            c2.position = {300.0, 0.0, 300.0};
            grid.UpdateEntity(reg, e2, oldPos);

            results = grid.Query(reg, DVec3{100.0, 0.0, 100.0}, 20.0f, std::move(results));
            ZHLN::Test::ExpectEq(results.size(), size_t {1});
            if (results.size() == 1) {
                ZHLN::Test::ExpectEq(results[0], e1);
            }

            // Unlink a non-head entry, then reuse its ECS slot. A grid link
            // must carry the generation, not accidentally find the old entity
            // through a fabricated generation-0 handle.
            const DVec3 movedFrom = c2.position;
            c2.position = {103.0, 0.0, 100.0};
            grid.UpdateEntity(reg, e2, movedFrom);
            grid.RemoveEntity(reg, e1);
            reg.Destroy(e1);
            const ZHLN::Entity replacement = reg.Create();
            auto& replacementComp = reg.Add<ZHLN::ALife::ALifeComponent>(replacement);
            replacementComp.position = {105.0, 0.0, 100.0};
            grid.UpdateEntity(reg, replacement, DVec3{-1, -1, -1});

            results = grid.Query(reg, DVec3{100.0, 0.0, 100.0}, 20.0f, std::move(results));
            ZHLN::Test::ExpectEq(replacement.index, e1.index);
            ZHLN::Test::ExpectNe(replacement.generation, e1.generation);
            ZHLN::Test::ExpectEq(results.size(), size_t {2});
            ZHLN::Test::ExpectTrue(std::ranges::find(results, e2) != results.end());
            ZHLN::Test::ExpectTrue(std::ranges::find(results, replacement) != results.end());
            ZHLN::Test::ExpectTrue(std::ranges::find(results, e1) == results.end());

            return {};
        }
    };
};

} // namespace
