// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Query results that cross the engine's public boundary. Every field is spelled
// in ZHLN value types, so this header names no JPH type and adds no Jolt
// include: ValueTypes.hpp supplies the aliases, PhysicsIDs.hpp is header
// only. Physics.hpp includes it, so existing code sees the same types in the
// same namespace. CharacterParams needs Jolt shapes and stays in Physics.hpp.

#include <Zahlen/Physics/PhysicsIDs.hpp>
#include <Zahlen/Math/ValueTypes.hpp>
#include <cstddef>
#include <cstdint>

namespace ZHLN::Physics {

struct DebugVertex {
    float    x, y, z;
    uint32_t color;
};

struct DebugDrawData {
    const DebugVertex* lines     = nullptr;
    size_t             lineCount = 0;
    const DebugVertex* triangles = nullptr;
    size_t             triangleCount = 0;
};

struct BodyStateSnapshot {
    ZHLN::Float3 previousPosition {0.0f, 0.0f, 0.0f};
    ZHLN::Float3 currentPosition {0.0f, 0.0f, 0.0f};
    ZHLN::Quat   previousRotation {0.0f, 0.0f, 0.0f, 1.0f};
    ZHLN::Quat   currentRotation {0.0f, 0.0f, 0.0f, 1.0f};
    bool         isCharacter = false;
    bool         valid       = false;
};

struct RaycastResult {
    BodyID        bodyID;
    ZHLN::Float3  normal;
    ZHLN::Double3 position;
    float         fraction;
    bool          hasHit;
};

} // namespace ZHLN::Physics
