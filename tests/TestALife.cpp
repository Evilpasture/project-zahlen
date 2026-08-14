// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/alife/Factions.hpp>
#include <Zahlen/alife/GOAP.hpp>
#include <Zahlen/alife/Graph.hpp>
#include <Zahlen/alife/SpatialGrid.hpp>
#include <expected>
#include <memory_resource>

enum class ALifeTestError : uint32_t {
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
    };
};

int main() {
    return ZHLN::Test::Runner::Run<ALifeTestSuite>();
}
