// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Zahlen/Common.h>
#include <Zahlen/Math/Matrix.hpp>
#include <Zahlen/ECS/Entity.hpp>
#include <physics/Physics.hpp>
#include <functional>
#include <span>
#include <unordered_map>
#include <vector>

namespace ZHLN {
class PhysicsContext;
struct Skeleton;
namespace ECS { class Registry; }

// Per-ragdoll CPU articulation state, sized to that ragdoll's skeleton.
// GPU palette indexing is owned by the renderer (JointRangeAllocator);
// this state is keyed by RagdollID and never indexes by jointOffset.
struct RagdollJointState {
    std::vector<float>  blendWeights;
    std::vector<float>  stiffness;
    std::vector<float>  blendDecay;
    std::vector<Mat4x4> inverseBindMatrices;
};

// World-owned articulation state. Scheduled systems borrow it as a resource;
// no system instance owns simulation state.
class ArticulationContext {
  public:
    ArticulationContext() = default;
    ArticulationContext(const ArticulationContext&) = delete;
    ArticulationContext& operator=(const ArticulationContext&) = delete;

    [[nodiscard]] bool AttachRagdoll(
        Entity rootEntity, ECS::Registry& reg, PhysicsContext& pc, const Skeleton& skeleton, std::span<const Physics::RagdollPartParams> authoredParts,
        uint32_t jointOffset
    );

    [[nodiscard]] auto JointState(Physics::RagdollID id) noexcept -> RagdollJointState* {
        auto it = _ragdollStates.find(id);
        return it != _ragdollStates.end() ? &it->second : nullptr;
    }
    [[nodiscard]] auto JointState(Physics::RagdollID id) const noexcept -> const RagdollJointState* {
        auto it = _ragdollStates.find(id);
        return it != _ragdollStates.end() ? &it->second : nullptr;
    }
    void RemoveJointState(Physics::RagdollID id) noexcept { _ragdollStates.erase(id); }

  private:
    friend class ArticulationSystem;
    void BindSkeleton(Physics::RagdollID id, const Skeleton& skeleton) noexcept;

    struct RagdollIDHash {
        [[nodiscard]] auto operator()(Physics::RagdollID id) const noexcept -> size_t {
            return std::hash<uint64_t> {}(static_cast<uint64_t>(id));
        }
    };
    std::unordered_map<Physics::RagdollID, RagdollJointState, RagdollIDHash> _ragdollStates;
};

} // namespace ZHLN
