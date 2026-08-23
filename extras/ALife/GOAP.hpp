// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>
#include <Zahlen/Core/String.hpp>
#include <cstdint>
#include <string_view>
#include <vector>

namespace ZHLN::ALife {

inline constexpr uint32_t MAX_GOAP_STATES = 64;
inline constexpr uint32_t MAX_PLAN_LENGTH = 8;

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

    [[nodiscard]] auto Matches(const WorldState& goal) const noexcept -> bool {
        return (values & goal.mask) == (goal.values & goal.mask);
    }
};

class ZHLN_API WorldStateRegistry {
  public:
    auto               RegisterKey(std::string_view name) -> uint32_t;
    [[nodiscard]] auto GetID(std::string_view name) const -> uint32_t;

  private:
    std::vector<String32> _keyNames;
};

struct Action {
    String32   name;
    WorldState preconditions;
    WorldState effects;
    int        cost {};
    uint32_t   task_id {};
};

struct Plan {
    Action   actions[MAX_PLAN_LENGTH];
    uint32_t count = 0;
};

struct PlanRequest {
    WorldState current;
    WorldState goal;
};

[[nodiscard]] ZHLN_API auto SolvePlan(const PlanRequest& request, const std::vector<Action>& actions) -> Plan;

} // namespace ZHLN::ALife
