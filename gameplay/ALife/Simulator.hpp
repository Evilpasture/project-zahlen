// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Math/Vec3.hpp>
#include "Factions.hpp"
#include "Graph.hpp"
#include "SpatialGrid.hpp"
#include "Types.hpp"
#include <Zahlen/Common.h>
#include <functional>

namespace ZHLN::ALife {

struct SimTuning {
    float    time_factor           = 10.0f;
    float    switch_distance       = 150.0f;
    uint32_t objects_per_tick      = 100;
    int32_t  combat_hit_chance     = 30;
    int32_t  combat_flee_threshold = 30;
    int32_t  combat_wait_time      = 2;
};

struct SimConfig {
    uint32_t  max_factions = 32;
    uint32_t  grid_width   = 100;
    uint32_t  grid_height  = 100;
    float     cell_size    = 50.0f;
    SimTuning default_tuning;
};

class ZHLN_STATIC_API Simulator {
  public:
    explicit Simulator(const SimConfig& config = SimConfig {});

    // The caller owns isolation across all three phases and their callbacks.
    // Movement and interaction callbacks may run concurrently. They must not
    // structurally mutate the queried storage, and shared callback state and
    // cross-entity writes require their own synchronization.
    void Update(SpatialGrid::WriteQuery query, float dt, DVec3 observer_pos);

    void SetRelation(uint32_t a, uint32_t b, float value);
    void BroadcastEvent(const Event& event);
    void ResolveOfflineInteraction(SpatialGrid::WriteQuery query, Entity e1, Entity e2);

    auto Save(const char* filename) const -> bool;
    auto Load(ECS::Registry& reg, const char* filename) -> bool;

    // Callbacks
    std::function<void(Simulator&, Entity, Entity)> on_interaction;
    std::function<void(Simulator&, const Event&)>   on_event;
    std::function<void(Simulator&, Entity)>         on_think;
    std::function<void(Simulator&, Entity)>         on_task_completed;

    // Encapsulated state accessors
    [[nodiscard]] auto GetGrid() noexcept -> SpatialGrid& {
        return _grid;
    }
    [[nodiscard]] auto GetGraph() noexcept -> LevelGraph& {
        return _levelGraph;
    }
    [[nodiscard]] auto GetFactions() noexcept -> FactionRegistry& {
        return _factionRegistry;
    }

    [[nodiscard]] auto GetTuning() noexcept -> SimTuning& {
        return _tuning;
    }
    [[nodiscard]] auto GetTuning() const noexcept -> const SimTuning& {
        return _tuning;
    }

    [[nodiscard]] auto GetGameTimeMS() const noexcept -> uint64_t {
        return _gameTimeMS;
    }

  private:
    SpatialGrid     _grid;
    LevelGraph      _levelGraph;
    FactionRegistry _factionRegistry;
    SimTuning       _tuning;

    uint64_t _gameTimeMS     = 0;
    uint32_t _currentTickIdx = 0;
};

} // namespace ZHLN::ALife
