// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
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
#include <Zahlen/Threading/Mutex.hpp>
#include <algorithm>

namespace ZHLN::Physics {

auto ShapeCache::GetOrCreate(CachedShape type, float p1, float p2, float p3, float p4) -> JPH::ShapeRefC {
    const float np1 = (p1 < 1e-3f && type != CachedShape::Plane) ? 1e-3f : p1;
    const float np2 = (p2 < 1e-3f && type != CachedShape::Plane) ? 1e-3f : p2;
    const float np3 = (p3 < 1e-3f && type != CachedShape::Plane) ? 1e-3f : p3;
    const float np4 = (type == CachedShape::Plane) ? p4 : 0.0f;

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
        case CachedShape::Box: {
            JPH::BoxShapeSettings s(JPH::Vec3(np1, np2, np3), std::min({0.05f, np1, np2, np3}));
            const auto result = s.Create();
            if (result.IsValid()) newShape = result.Get();
            break;
        }
        case CachedShape::Sphere: {
            JPH::SphereShapeSettings s(np1);
            const auto result = s.Create();
            if (result.IsValid()) newShape = result.Get();
            break;
        }
        case CachedShape::Capsule: {
            JPH::CapsuleShapeSettings s(np1, np2);
            const auto result = s.Create();
            if (result.IsValid()) newShape = result.Get();
            break;
        }
        case CachedShape::Cylinder: {
            JPH::CylinderShapeSettings s(np1, np2, 0.05f);
            const auto result = s.Create();
            if (result.IsValid()) newShape = result.Get();
            break;
        }
        case CachedShape::Plane: {
            JPH::Plane              plane(JPH::Vec3(np1, np2, np3), np4);
            JPH::PlaneShapeSettings s(plane, nullptr, 1000.0f);
            const auto result = s.Create();
            if (result.IsValid()) newShape = result.Get();
            break;
        }
    }

    if (newShape == nullptr) {
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

auto ShapeCache::Resolve(const Box& shape) -> JPH::ShapeRefC {
    return GetOrCreate(CachedShape::Box, shape.halfExtents.x, shape.halfExtents.y, shape.halfExtents.z, 0.0f);
}

auto ShapeCache::Resolve(const Sphere& shape) -> JPH::ShapeRefC {
    return GetOrCreate(CachedShape::Sphere, shape.radius, 0.0f, 0.0f, 0.0f);
}

auto ShapeCache::Resolve(const Capsule& shape) -> JPH::ShapeRefC {
    return GetOrCreate(CachedShape::Capsule, shape.halfHeight, shape.radius, 0.0f, 0.0f);
}

auto ShapeCache::Resolve(const Cylinder& shape) -> JPH::ShapeRefC {
    return GetOrCreate(CachedShape::Cylinder, shape.halfHeight, shape.radius, 0.0f, 0.0f);
}

auto ShapeCache::Resolve(const Plane& shape) -> JPH::ShapeRefC {
    return GetOrCreate(CachedShape::Plane, shape.normal.x, shape.normal.y, shape.normal.z, shape.constant);
}

auto ShapeCache::Resolve(const TriangleMesh& shape) -> JPH::ShapeRefC {
    if (shape.vertices.size() > UINT32_MAX || shape.indices.size() > UINT32_MAX) return nullptr;
    for (auto index: shape.indices) if (index >= shape.vertices.size()) return nullptr;
    // Hash borrowed data once, outside the cache lock. No input
    // storage or pointers are retained in the cache. Fingerprint
    // equality is probabilistic (hash collisions cannot be checked
    // exactly without retaining or reloading the source geometry).
    static_assert(sizeof(FVec3) == 3 * sizeof(float));
    const MeshKey key {
        Hash64(reinterpret_cast<const char*>(shape.vertices.data()), shape.vertices.size * sizeof(FVec3)),
        Hash64(reinterpret_cast<const char*>(shape.indices.data()), shape.indices.size * sizeof(uint32_t)),
        static_cast<uint32_t>(shape.vertices.size()), static_cast<uint32_t>(shape.indices.size())
    };
    {
        ZHLN::MutexGuard guard(mutex);
        if (const auto found = meshes.find(key); found != meshes.end()) return found->second;
    }
    auto created = BuildMeshShape(shape.vertices, shape.indices);
    if (!created) return nullptr;
    ZHLN::MutexGuard guard(mutex);
    return meshes.emplace(key, created).first->second;
}

auto ShapeCache::Resolve(const MeshAsset& shape) -> JPH::ShapeRefC {
    if (shape.id == CollisionAssetID::Invalid) return nullptr;
    CollisionAssetResolver lookup;
    void* context;
    {
        ZHLN::MutexGuard guard(mutex);
        lookup = resolver;
        context = resolverContext;
    }
    if (!lookup) return nullptr;
    auto snapshot = lookup(context, shape.id);
    if (!snapshot) return nullptr;
    {
        ZHLN::MutexGuard guard(mutex);
        const auto found = assetMeshes.find(shape.id);
        if (found != assetMeshes.end() && found->second.snapshot.lock() == snapshot) return found->second.shape;
    }
    auto created = BuildMeshShape(snapshot->vertices, snapshot->indices);
    if (!created) return nullptr;
    ZHLN::MutexGuard guard(mutex);
    auto found = assetMeshes.find(shape.id);
    if (found != assetMeshes.end() && found->second.snapshot.lock() == snapshot) return found->second.shape;
    assetMeshes.insert_or_assign(shape.id, AssetShapeEntry {snapshot, created});
    return created;
}

auto ShapeCache::Resolve(const HeightField& shape) -> JPH::ShapeRefC {
    if (shape.sampleCount < 2 || shape.heights.size() < static_cast<size_t>(shape.sampleCount) * shape.sampleCount) return nullptr;
    return Physics::CreateHeightFieldShape(shape.heights.data(), static_cast<int>(shape.sampleCount), shape.worldSize);
}

namespace {

// One pass over a Compound's children in insertion order (the SubShapeID
// order), stamp-verified through the shared dispatch: every child must carry
// the ChainInNode identity of a Placed<T> payload, nested compounds recurse
// through this same builder (bounded by kMaxCompoundDepth), and any foreign
// stamp or unresolvable child fails the whole shape. This is structural
// only -- role policy lives at the CreateBody boundary.
[[nodiscard]] auto MakeCompoundShape(ShapeCache& cache, const Compound& compound, uint32_t depth) -> JPH::ShapeRefC {
    if (compound.children.data == nullptr || compound.children.size == 0) return nullptr;
    if (depth >= kMaxCompoundDepth) return nullptr;

    JPH::StaticCompoundShapeSettings settings;
    for (size_t index = 0; index < compound.children.size; ++index) {
        bool valid   = true;
        const bool matched = VisitCompoundChild<Box, Sphere, Capsule, Cylinder, Plane, TriangleMesh, MeshAsset, Compound>(
            compound.children[index], [&](const auto& child) {
                using T = std::remove_cvref_t<decltype(child)>;
                if constexpr (std::is_same_v<T, Placed<Compound>>) {
                    const JPH::ShapeRefC nested = MakeCompoundShape(cache, child.shape, depth + 1U);
                    if (!nested) {
                        valid = false;
                        return;
                    }
                    settings.AddShape(JPH::Vec3(child.position.x, child.position.y, child.position.z),
                                      JPH::Quat(child.rotation.x, child.rotation.y, child.rotation.z, child.rotation.w),
                                      nested);
                } else {
                    const JPH::ShapeRefC resolved = cache.Resolve(child.shape);
                    if (!resolved) {
                        valid = false;
                        return;
                    }
                    settings.AddShape(JPH::Vec3(child.position.x, child.position.y, child.position.z),
                                      JPH::Quat(child.rotation.x, child.rotation.y, child.rotation.z, child.rotation.w),
                                      resolved);
                }
            });
        if (!matched || !valid) return nullptr;
    }

    auto result = settings.Create();
    return result.IsValid() ? result.Get() : nullptr;
}

} // namespace

auto ShapeCache::Resolve(const Compound& shape) -> JPH::ShapeRefC {
    return MakeCompoundShape(*this, shape, 0U);
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
