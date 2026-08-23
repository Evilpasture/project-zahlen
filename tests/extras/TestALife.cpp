// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <ALife/ALifeComponents.hpp>
#include <ALife/Factions.hpp>
#include <ALife/GOAP.hpp>
#include <ALife/Graph.hpp>
#include <ALife/SpatialGrid.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <expected>
#include <memory_resource>

enum class ALifeTestError : uint8_t {
    Success = 0,
    GOAPPlanFailed[[= ZHLN::Reflect::Description("GOAP solver failed to compute valid action sequence.")]],
    PathfindingFailed[[= ZHLN::Reflect::Description("LevelGraph A* failed to find shortest path.")]],
    SpatialGridQueryFailed[[= ZHLN::Reflect::Description("SpatialGrid spatial query missed expected entity.")]]
};

struct ALifeTestSuite {
    struct Tests {
        // --- 1. GOAP Action Planner ---
        std::expected<void, ZHLN::Error> goap_solver_planning() {
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
        std::expected<void, ZHLN::Error> graph_astar_pathfinding() {
            // Create a 4-node linear graph: 0 -- 1 -- 2 -- 3
            ZHLN::ALife::LevelGraph graph(4);
            graph.GetNode(0).position = JPH::RVec3(0, 0, 0);
            graph.GetNode(1).position = JPH::RVec3(10, 0, 0);
            graph.GetNode(2).position = JPH::RVec3(20, 0, 0);
            graph.GetNode(3).position = JPH::RVec3(30, 0, 0);

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
        std::expected<void, ZHLN::Error> faction_relations() {
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

        // --- 4. Spatial Grid Partitioning & Radial Query ---
        std::expected<void, ZHLN::Error> spatial_grid_queries() {
            ZHLN::ECS::Registry reg;
            reg.RegisterComponent<ZHLN::ALife::ALifeComponent>("ALifeComponent");

            // 100x100 grid with 50.0m cells
            ZHLN::ALife::SpatialGrid grid(100, 100, 50.0f);

            // Entity 1 at (100, 0, 100) -> Cell (2, 2)
            ZHLN::Entity e1 = reg.Create();
            auto&        c1 = reg.Add<ZHLN::ALife::ALifeComponent>(e1);
            c1.position     = JPH::RVec3(100.0, 0.0, 100.0);
            c1.self_entity  = e1;
            grid.UpdateEntity(reg, e1, JPH::RVec3(-1, -1, -1));

            // Entity 2 at (110, 0, 100) (Distance 10m from e1)
            ZHLN::Entity e2 = reg.Create();
            auto&        c2 = reg.Add<ZHLN::ALife::ALifeComponent>(e2);
            c2.position     = JPH::RVec3(110.0, 0.0, 100.0);
            c2.self_entity  = e2;
            grid.UpdateEntity(reg, e2, JPH::RVec3(-1, -1, -1));

            // Entity 3 at (800, 0, 800) (Far away)
            ZHLN::Entity e3 = reg.Create();
            auto&        c3 = reg.Add<ZHLN::ALife::ALifeComponent>(e3);
            c3.position     = JPH::RVec3(800.0, 0.0, 800.0);
            c3.self_entity  = e3;
            grid.UpdateEntity(reg, e3, JPH::RVec3(-1, -1, -1));

            // Query within 20m of (100, 0, 100)
            std::vector<ZHLN::Entity> results;
            uint32_t                  count = grid.Query(reg, JPH::RVec3(100.0, 0.0, 100.0), 20.0f, results);

            // Should find e1 and e2, but NOT e3
            ZHLN::Test::ExpectEq(count, 2u);
            ZHLN::Test::ExpectEq(results.size(), static_cast<size_t>(2));

            // Move e2 outside the radius
            JPH::RVec3 oldPos = c2.position;
            c2.position       = JPH::RVec3(300.0, 0.0, 300.0);
            grid.UpdateEntity(reg, e2, oldPos);

            results.clear();
            count = grid.Query(reg, JPH::RVec3(100.0, 0.0, 100.0), 20.0f, results);
            ZHLN::Test::ExpectEq(count, 1u);
            if (count == 1) {
                ZHLN::Test::ExpectEq(results[0], e1);
            }

            return {};
        }
    };
};

int main() {
    return ZHLN::Test::Runner::Run<ALifeTestSuite>();
}
