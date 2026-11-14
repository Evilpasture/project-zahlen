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
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Zahlen/Render/Vertex.hpp>
#include <vector>
#include <Zahlen/Log.hpp>
#include <Zahlen/Threading/Mutex.hpp>
#include <algorithm>

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
            JPH::BoxShapeSettings s(JPH::Vec3(np1, np2, np3), std::min({0.05f, np1, np2, np3}));
            const auto result = s.Create();
            if (result.IsValid()) newShape = result.Get();
            break;
        }
        case ShapeType::Sphere: {
            JPH::SphereShapeSettings s(np1);
            const auto result = s.Create();
            if (result.IsValid()) newShape = result.Get();
            break;
        }
        case ShapeType::Capsule: {
            JPH::CapsuleShapeSettings s(np1, np2);
            const auto result = s.Create();
            if (result.IsValid()) newShape = result.Get();
            break;
        }
        case ShapeType::Cylinder: {
            JPH::CylinderShapeSettings s(np1, np2, 0.05f);
            const auto result = s.Create();
            if (result.IsValid()) newShape = result.Get();
            break;
        }
        case ShapeType::Plane: {
            JPH::Plane              plane(JPH::Vec3(np1, np2, np3), np4);
            JPH::PlaneShapeSettings s(plane, nullptr, 1000.0f);
            const auto result = s.Create();
            if (result.IsValid()) newShape = result.Get();
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

namespace {
auto BuildMeshShape(std::span<const FVec3> vertices, std::span<const uint32_t> indices) -> JPH::ShapeRefC {
    if (vertices.size() > UINT32_MAX || indices.size() > UINT32_MAX) return nullptr;
    for (auto index: indices) if (index >= vertices.size()) return nullptr;
    std::vector<VertexPosition> positions;
    positions.reserve(vertices.size());
    for (auto v: vertices) positions.push_back(VertexPosition {{v.x, v.y, v.z}});
    return Physics::CreateMeshShape(positions.data(), static_cast<uint32_t>(positions.size()), indices.data(), static_cast<uint32_t>(indices.size()));
}
} // namespace

void ShapeCache::BindCollisionAssetResolver(CollisionAssetResolver lookup, void* context) noexcept {
    ZHLN::MutexGuard guard(mutex);
    resolver = lookup;
    resolverContext = context;
    assetMeshes.clear();
}

auto ShapeCache::Resolve(const AnyPrimitiveShape& shape) -> JPH::ShapeRefC {
    return std::visit([this](const auto& s) -> JPH::ShapeRefC {
        using S = std::remove_cvref_t<decltype(s)>;
        if constexpr (std::is_same_v<S, Shape<Box>>) return GetOrCreate(ShapeType::Box, s.halfExtents.x, s.halfExtents.y, s.halfExtents.z, 0);
        else if constexpr (std::is_same_v<S, Shape<Sphere>>) return GetOrCreate(ShapeType::Sphere, s.radius, 0, 0, 0);
        else if constexpr (std::is_same_v<S, Shape<Capsule>>) return GetOrCreate(ShapeType::Capsule, s.halfHeight, s.radius, 0, 0);
        else if constexpr (std::is_same_v<S, Shape<Cylinder>>) return GetOrCreate(ShapeType::Cylinder, s.halfHeight, s.radius, 0, 0);
        else if constexpr (std::is_same_v<S, Shape<Plane>>) return GetOrCreate(ShapeType::Plane, s.normal.x, s.normal.y, s.normal.z, s.constant);
        else if constexpr (std::is_same_v<S, Shape<HeightField>>) {
            if (s.sampleCount < 2 || s.heights.size() < static_cast<size_t>(s.sampleCount) * s.sampleCount) return nullptr;
            return Physics::CreateHeightFieldShape(s.heights.data(), static_cast<int>(s.sampleCount), s.worldSize);
        }
        else {
            if (s.asset != CollisionAssetID::Invalid) {
                CollisionAssetResolver lookup;
                void* context;
                {
                    ZHLN::MutexGuard guard(mutex);
                    lookup = resolver;
                    context = resolverContext;
                }
                if (!lookup) return nullptr;
                auto snapshot = lookup(context, s.asset);
                if (!snapshot) return nullptr;
                {
                    ZHLN::MutexGuard guard(mutex);
                    const auto found = assetMeshes.find(s.asset);
                    if (found != assetMeshes.end() && found->second.snapshot.lock() == snapshot) return found->second.shape;
                }
                auto created = BuildMeshShape(snapshot->vertices, snapshot->indices);
                if (!created) return nullptr;
                ZHLN::MutexGuard guard(mutex);
                auto found = assetMeshes.find(s.asset);
                if (found != assetMeshes.end() && found->second.snapshot.lock() == snapshot) return found->second.shape;
                assetMeshes.insert_or_assign(s.asset, AssetShapeEntry {snapshot, created});
                return created;
            }
            if (s.vertices.size() > UINT32_MAX || s.indices.size() > UINT32_MAX) return nullptr;
            for (auto index: s.indices) if (index >= s.vertices.size()) return nullptr;
            // Hash borrowed data once, outside the cache lock. No input
            // storage or pointers are retained in the cache. Fingerprint
            // equality is probabilistic (hash collisions cannot be checked
            // exactly without retaining or reloading the source geometry).
            static_assert(sizeof(FVec3) == 3 * sizeof(float));
            const MeshKey key {
                Hash64(reinterpret_cast<const char*>(s.vertices.data()), s.vertices.size_bytes()),
                Hash64(reinterpret_cast<const char*>(s.indices.data()), s.indices.size_bytes()),
                static_cast<uint32_t>(s.vertices.size()), static_cast<uint32_t>(s.indices.size())
            };
            {
                ZHLN::MutexGuard guard(mutex);
                if (const auto found = meshes.find(key); found != meshes.end()) return found->second;
            }
            auto created = BuildMeshShape(s.vertices, s.indices);
            if (!created) return nullptr;
            ZHLN::MutexGuard guard(mutex);
            return meshes.emplace(key, created).first->second;
        }
    }, shape);
}

auto ShapeCache::Resolve(const AnyShape& shape) -> JPH::ShapeRefC {
    return std::visit([this](const auto& s) -> JPH::ShapeRefC {
        using S = std::remove_cvref_t<decltype(s)>;
        if constexpr (std::is_same_v<S, Shape<Compound>>) {
            if (s.children.empty()) return nullptr;
            JPH::StaticCompoundShapeSettings settings;
            for (const auto& child: s.children) {
                auto resolved = Resolve(child.shape);
                if (!resolved) return nullptr;
                const auto& p = child.position;
                const auto& r = child.rotation;
                settings.AddShape(JPH::Vec3(p.x, p.y, p.z), JPH::Quat(r.x, r.y, r.z, r.w), resolved);
            }
            auto result = settings.Create();
            return result.IsValid() ? result.Get() : nullptr;
        } else return Resolve(AnyPrimitiveShape(s));
    }, shape);
}

void ShapeCache::ClearMeshes() noexcept {
    ZHLN::MutexGuard guard(mutex);
    meshes.clear();
    assetMeshes.clear();
}

void ShapeCache::Clear() noexcept {
    ZHLN::MutexGuard guard(mutex);
    entries.clear();
    meshes.clear();
    assetMeshes.clear();
}

} // namespace ZHLN::Physics
