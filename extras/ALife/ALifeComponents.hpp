// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extras/ALife/ALifeComponents.hpp
#pragma once

#include "Types.hpp"
#include <Zahlen/Entity.hpp>
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/DVec3.h>
#include <Jolt/Math/Real.h>
#include <Jolt/Math/Vec3.h>
#include <cstdint>

namespace ZHLN::Components {

struct ALifeComponent {
    using enum ALife::State;
    using enum ALife::TaskType;

    JPH::RVec3   position     = JPH::RVec3::sZero();
    ALife::State state        = Offline;
    uint32_t     current_node = ALife::INVALID_GRAPH_NODE;
    uint32_t     target_node  = ALife::INVALID_GRAPH_NODE;
    float        travel_speed = 0.0f;
    uint32_t     faction_id   = 0;
    Entity       self_entity  = NullEntity;

    uint32_t path[ALife::MAX_PATH_LENGTH] {};
    uint32_t path_count = 0;
    uint32_t path_index = 0;

    int32_t wait_time   = 0;
    bool    is_thinking = false;

    uint32_t next_in_grid = ALife::END_OF_LIST;

    uint32_t        class_id      = 0;
    int32_t         health        = 100;
    int32_t         power         = 10;
    int32_t         money         = 0;
    int32_t         energy        = 100;
    int32_t         loot_value    = 0;
    ALife::TaskType active_task   = Idle;
    bool            is_looted     = false;
    bool            is_fleeing    = false;
    uint64_t        script_handle = 0;
};

} // namespace ZHLN::Components
