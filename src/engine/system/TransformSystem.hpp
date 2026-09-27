// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
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

    [[nodiscard]] JPH::Mat44 GetWorldTransform(const ECS::Registry& reg, Entity e) const noexcept;

    void ResolveTransforms(ECS::Registry& reg) const noexcept;

    void UpdateTransformHistory(ECS::Registry& reg) noexcept;
};

}
