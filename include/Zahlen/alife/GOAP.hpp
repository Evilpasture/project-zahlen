// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <detail/String.hpp>
#include <string_view>
#include <vector>

namespace ZHLN::ALife {

constexpr uint32_t MAX_GOAP_STATES = 64;
constexpr uint32_t MAX_PLAN_LENGTH = 8;

struct WorldState {
    uint64_t values = 0;
    uint64_t mask   = 0;

    void Set(uint32_t bit_id, bool val) noexcept {
        if (bit_id >= MAX_GOAP_STATES) {
            return;
        }
        uint64_t bit_mask = (1ULL << bit_id);
        values            = val ? (values | bit_mask) : (values & ~bit_mask);
        mask |= bit_mask;
    }

    [[nodiscard]] bool Matches(const WorldState& goal) const noexcept {
        return (values & goal.mask) == (goal.values & goal.mask);
    }
};

class WorldStateRegistry {
  public:
    uint32_t               RegisterKey(std::string_view name);
    [[nodiscard]] uint32_t GetID(std::string_view name) const;

  private:
    std::vector<String32> _keyNames;
};

struct Action {
    String32   name;
    WorldState preconditions;
    WorldState effects;
    int        cost;
    uint32_t   task_id;
};

struct Plan {
    Action   actions[MAX_PLAN_LENGTH];
    uint32_t count = 0;
};

struct PlanRequest {
    WorldState current;
    WorldState goal;
};

[[nodiscard]] Plan SolvePlan(const PlanRequest& request, const std::vector<Action>& actions);

} // namespace ZHLN::ALife