// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// ALife (first-party extras) stress hooks for TestPerformance.
//
// IMPORTANT: this translation unit must NOT include <Zahlen/Components.hpp>
// — the core engine declares `ZHLN::Components` as a struct, while
// ALifeComponents.hpp declares it as a namespace. See PerfALife.hpp.

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <ALife/ALifeComponents.hpp>
#include <ALife/Simulator.hpp>
#include <Zahlen/Camera.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include "PerfALife.hpp"
#include <atomic>
#include <cmath>
#include <memory>
#include <random>
#include <vector>

namespace {

struct PerfALifeState {
    std::unique_ptr<ZHLN::ALife::Simulator> sim;
    std::vector<ZHLN::Entity>               creatures;
    std::vector<JPH::RVec3>                 waypoints;
    std::atomic<uint64_t>                   eventCount {0};
};

// File-local state: created in ALife_Init, consumed per frame by
// ALife_DriveFrame, outlived by the engine that owns the registry.
PerfALifeState s_state;

} // namespace

namespace ZHLN::Perf {

bool ALife_Init(Engine& engine, uint32_t creatureCount) {
    auto& reg = engine.GetRegistry();

    // Register the ALife component in the shared ECS (dense ID is global).
    reg.RegisterComponent<Components::ALifeComponent>("ALifeComponent");

    ALife::SimConfig simCfg {};
    simCfg.max_factions = 8;
    simCfg.grid_width   = 100;
    simCfg.grid_height  = 100;
    simCfg.cell_size    = 50.0f;
    simCfg.default_tuning.time_factor     = 2.0f;
    simCfg.default_tuning.switch_distance = 30.0f;

    s_state.sim = std::make_unique<ALife::Simulator>(simCfg);
    s_state.sim->SetRelation(0, 1, -1.0f);
    s_state.sim->SetRelation(2, 3, -1.0f);
    s_state.sim->on_event = [](ALife::Simulator&, const ALife::Event&) {
        s_state.eventCount.fetch_add(1, std::memory_order_relaxed);
    };
    s_state.sim->on_think = [](ALife::Simulator&, Entity) {
        // The per-tick think callback itself is the stress load.
    };
    // `reg` references the engine's registry, which outlives this lambda's
    // storage inside the simulator.
    s_state.sim->on_interaction = [&reg](ALife::Simulator& sim, Entity e1, Entity e2) {
        sim.ResolveOfflineInteraction(reg, e1, e2);
    };

    std::mt19937 rng(0xA11FE);

    // The ALife spatial grid only indexes the positive quadrant, so keep the
    // creature population (and its roaming waypoints) inside [0, 120] x [0, 120].
    for (uint32_t i = 0; i < creatureCount; ++i) {
        const JPH::RVec3 pos(static_cast<double>(rng() % 120), 0.0, static_cast<double>(rng() % 120));
        // self_entity is left as NullEntity; the ALife SpatialGrid caches the
        // real handle into it on first UpdateEntity.
        const Entity e = reg.Create(
            Components::ALifeComponent {
                .position     = pos,
                .state        = ALife::State::Offline,
                .travel_speed = 5.0f + static_cast<float>(rng() % 700) * 0.01f,
                .faction_id   = i % 4,
                .health       = 100,
                .power        = 10,
                .energy       = 100
            }
        );
        s_state.creatures.push_back(e);
        s_state.waypoints.push_back(pos);
    }

    return !s_state.creatures.empty();
}

void ALife_DriveFrame(Engine& engine) {
    if ((s_state.sim == nullptr) || s_state.creatures.empty()) {
        return;
    }

    auto& reg   = engine.GetRegistry();
    auto  ents  = reg.GetEntitiesWith<Components::ALifeComponent>();
    auto  comps = reg.GetRawArray<Components::ALifeComponent>();
    auto& wps   = s_state.waypoints;

    if (ents.empty()) {
        return;
    }

    ZHLN::TaskSystem::ParallelFor(static_cast<uint32_t>(ents.size()), 128, [&](uint32_t start, uint32_t end, uint32_t) {
        thread_local std::mt19937 localRng(0xC0FFEE);
        for (uint32_t i = start; i < end && i < wps.size(); ++i) {
            Components::ALifeComponent& c = comps[i];
            if (c.state == ALife::State::Dead) {
                continue;
            }

            // Respawn dead creatures (population churn).
            if (c.health <= 0) {
                c.state      = ALife::State::Offline;
                c.health     = 100;
                c.is_fleeing = false;
                c.is_looted  = false;
                c.wait_time  = 0;
                c.position   = JPH::RVec3(static_cast<double>(localRng() % 120), 0.0, static_cast<double>(localRng() % 120));
                wps[i]       = c.position;
            }

            // Steer toward the current waypoint (sim time factor 2.0x).
            const JPH::RVec3 delta = wps[i] - c.position;
            const double     dist2 = delta.LengthSq();
            if (dist2 < 4.0) { // arrived: pick a new waypoint
                wps[i] = JPH::RVec3(static_cast<double>(localRng() % 120), 0.0, static_cast<double>(localRng() % 120));
                continue;
            }
            const double dist = std::sqrt(dist2);
            const double step = static_cast<double>(c.travel_speed) * 0.033333;
            if (step >= dist) {
                c.position = wps[i];
            } else {
                c.position += delta * (step / dist);
            }
        }
    });

    // Simulator parallel phases: think/state-switch + spatial-grid rebuild +
    // offline faction interactions (combat/loot), observed at the camera.
    s_state.sim->Update(engine, 0.016666f, JPH::RVec3(engine.GetCamera().position));
}

uint64_t ALife_EventCount() {
    return s_state.eventCount.load(std::memory_order_relaxed);
}

} // namespace ZHLN::Perf
