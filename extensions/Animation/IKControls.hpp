// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Zahlen/ECS/Entity.hpp>
#include <Zahlen/Math/Quat.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <algorithm>
#include <array>
#include <span>
#include <type_traits>
namespace ZHLN::IK {
constexpr size_t kMaxTwoBoneIKChains = 4;

// One solvable chain: upper -> lower -> end bone indices into the prefab's
// node list, plus the target (fixed position or a tracked entity) and the
// pole hint.
struct TwoBoneChain {
    int32_t upperNodeIndex = -1;
    int32_t lowerNodeIndex = -1;
    int32_t endNodeIndex   = -1;

    FVec3 targetPosition = FVec3::Zero();
    Quat  targetRotation = Quat::Identity();
    FVec3 poleVector     = {0.0f, -1.0f, 0.0f};

    Entity targetEntity = Entity::Null();
    FVec3  targetOffset = FVec3::Zero();

    float weight            = 1.0f;
    bool  orientEndEffector = true;
};
static_assert(std::is_trivially_copyable_v<TwoBoneChain>, "TwoBoneChain must remain trivially copyable.");

struct TwoBoneComponent {
    std::array<TwoBoneChain, kMaxTwoBoneIKChains> chains {};
    uint32_t                                        chainCount = 0;

    constexpr void ClearChains() noexcept {
        chainCount = 0;
    }

    constexpr auto AddChain(const TwoBoneChain& chain) noexcept -> bool {
        if (chainCount >= chains.size()) {
            return false;
        }
        chains[chainCount++] = chain;
        return true;
    }

    [[nodiscard]] constexpr auto ActiveChains() const noexcept -> std::span<const TwoBoneChain> {
        const uint32_t bounded = std::min<uint32_t>(chainCount, static_cast<uint32_t>(chains.size()));
        return std::span<const TwoBoneChain> {chains.data(), bounded};
    }
};
static_assert(std::is_trivially_copyable_v<TwoBoneComponent>, "TwoBoneComponent must never own heap memory or views.");

} // namespace ZHLN::IK
