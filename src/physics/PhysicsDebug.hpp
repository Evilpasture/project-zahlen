// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Renderer/DebugRendererSimple.h>
#include <Zahlen/physics/Physics.hpp>

namespace ZHLN::Physics {

class PhysicsDebugRenderer final: public JPH::DebugRendererSimple {
  public:
    PhysicsDebugRenderer() {
        Initialize();
    }
    ~PhysicsDebugRenderer() override = default;

    void DrawLine(JPH::RVec3Arg inFrom, JPH::RVec3Arg inTo, JPH::ColorArg inColor) override;
    void DrawTriangle(JPH::RVec3Arg inV1, JPH::RVec3Arg inV2, JPH::RVec3Arg inV3, JPH::ColorArg inColor, ECastShadow inCastShadow) override;
    void DrawText3D(JPH::RVec3Arg inPosition, const std::string_view& inString, JPH::ColorArg inColor, float inHeight) override;

    void Clear();

    JPH::Array<DebugVertex> lines;
    JPH::Array<DebugVertex> triangles;
};

} // namespace ZHLN::Physics
