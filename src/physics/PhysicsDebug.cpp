// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PhysicsDebug.hpp"

namespace ZHLN::Physics {

void PhysicsDebugRenderer::DrawLine(JPH::RVec3Arg inFrom, JPH::RVec3Arg inTo, JPH::ColorArg inColor) {
    uint32_t color = inColor.GetUInt32();
    lines.push_back({.x = static_cast<float>(inFrom.GetX()), .y = static_cast<float>(inFrom.GetY()), .z = static_cast<float>(inFrom.GetZ()), .color = color});
    lines.push_back({.x = static_cast<float>(inTo.GetX()), .y = static_cast<float>(inTo.GetY()), .z = static_cast<float>(inTo.GetZ()), .color = color});
}

void PhysicsDebugRenderer::DrawTriangle(JPH::RVec3Arg inV1, JPH::RVec3Arg inV2, JPH::RVec3Arg inV3, JPH::ColorArg inColor, ECastShadow /*inCastShadow*/) {
    uint32_t color = inColor.GetUInt32();
    triangles.push_back({.x = static_cast<float>(inV1.GetX()), .y = static_cast<float>(inV1.GetY()), .z = static_cast<float>(inV1.GetZ()), .color = color});
    triangles.push_back({.x = static_cast<float>(inV2.GetX()), .y = static_cast<float>(inV2.GetY()), .z = static_cast<float>(inV2.GetZ()), .color = color});
    triangles.push_back({.x = static_cast<float>(inV3.GetX()), .y = static_cast<float>(inV3.GetY()), .z = static_cast<float>(inV3.GetZ()), .color = color});
}

void PhysicsDebugRenderer::DrawText3D(JPH::RVec3Arg inPosition, const std::string_view& inString, JPH::ColorArg inColor, float inHeight) {
    // Usually ignored or printed to stdout in simple engines.
}

void PhysicsDebugRenderer::Clear() {
    lines.clear();
    triangles.clear();
}

} // namespace ZHLN::Physics
