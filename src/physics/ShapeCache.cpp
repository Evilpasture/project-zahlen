// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ShapeCache.hpp"

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Geometry/Plane.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/PlaneShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Zahlen/Log.hpp>
#include <Zahlen/Threading/Mutex.hpp>

namespace ZHLN::Physics {

auto ShapeCache::GetOrCreate(ShapeType type, float p1, float p2, float p3, float p4) -> JPH::ShapeRefC {
    const float np1 = (p1 < 1e-3f && type != ShapeType::Plane) ? 1e-3f : p1;
    const float np2 = (p2 < 1e-3f && type != ShapeType::Plane) ? 1e-3f : p2;
    const float np3 = (p3 < 1e-3f && type != ShapeType::Plane) ? 1e-3f : p3;
    const float np4 = (type == ShapeType::Plane) ? p4 : 0.0f;

    const ShapeKey key {
        .type = static_cast<uint32_t>(type),
        .p1   = np1,
        .p2   = np2,
        .p3   = np3,
        .p4   = np4,
    };

    {
        ZHLN::MutexGuard guard(mutex);
        for (const auto& entry: entries) {
            if (entry.key == key) {
                return entry.shape;
            }
        }
    }

    JPH::ShapeRefC newShape;
    switch (type) {
        case ShapeType::Box: {
            JPH::BoxShapeSettings s(JPH::Vec3(np1, np2, np3), 0.05f);
            newShape = s.Create().Get();
            break;
        }
        case ShapeType::Sphere: {
            JPH::SphereShapeSettings s(np1);
            newShape = s.Create().Get();
            break;
        }
        case ShapeType::Capsule: {
            JPH::CapsuleShapeSettings s(np1, np2);
            newShape = s.Create().Get();
            break;
        }
        case ShapeType::Cylinder: {
            JPH::CylinderShapeSettings s(np1, np2, 0.05f);
            newShape = s.Create().Get();
            break;
        }
        case ShapeType::Plane: {
            JPH::Plane              plane(JPH::Vec3(np1, np2, np3), np4);
            JPH::PlaneShapeSettings s(plane, nullptr, 1000.0f);
            newShape = s.Create().Get();
            break;
        }
    }

    if (newShape == nullptr) {
        ZHLN::Log("Failed to create Jolt Shape! Degenerate parameters?");
        return nullptr;
    }

    ZHLN::MutexGuard guard(mutex);
    for (const auto& entry: entries) {
        if (entry.key == key) {
            return entry.shape;
        }
    }
    entries.push_back(ShapeEntry {.key = key, .shape = newShape});
    return newShape;
}

void ShapeCache::Clear() noexcept {
    ZHLN::MutexGuard guard(mutex);
    entries.clear();
}

} // namespace ZHLN::Physics
