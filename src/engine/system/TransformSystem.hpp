// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Math/Mat44.h>
#include <Zahlen/Common.h>
#include <Zahlen/Entity.hpp>

namespace ZHLN {

namespace ECS {
class Registry;
}
namespace Physics {
struct PhysicsWorld;
}

class ZHLN_API TransformSystem {
  public:
    TransformSystem()                             = default;
    TransformSystem(TransformSystem&&)            = delete;
    TransformSystem& operator=(TransformSystem&&) = delete;
    ~TransformSystem()                            = default;

    TransformSystem(const TransformSystem&)            = delete;
    TransformSystem& operator=(const TransformSystem&) = delete;

    // Safely computes the final world-space matrix for an entity
    [[nodiscard]] JPH::Mat44 GetWorldTransform(const ECS::Registry& reg, Entity e) const noexcept;

    // Evaluates parent-child nodes and writes final transforms to MeshComponents
    void ResolveTransforms(ECS::Registry& reg) const noexcept;

    // Caches the current world matrices into prevTransform for TAA motion vectors
    void UpdateTransformHistory(ECS::Registry& reg) noexcept;
};

} // namespace ZHLN
