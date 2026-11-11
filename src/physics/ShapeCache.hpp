// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Core/Array.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Zahlen/Threading/Mutex.hpp>
#include <Zahlen/Core/Hash.hpp>
#include <Zahlen/Physics/Physics.hpp>
#include <cstdint>
#include <type_traits>
#include <unordered_map>

namespace ZHLN::Physics {

struct ShapeKey {
    uint32_t type = 0;
    float    p1   = 0.0f;
    float    p2   = 0.0f;
    float    p3   = 0.0f;
    float    p4   = 0.0f;

    [[nodiscard]] constexpr auto operator==(const ShapeKey&) const noexcept -> bool = default;
};

struct ShapeEntry {
    ShapeKey       key {};
    JPH::ShapeRefC shape;
};

struct MeshKey {
    uint64_t vertexHash, indexHash;
    uint32_t vertexCount, indexCount;
    [[nodiscard]] auto operator==(const MeshKey&) const noexcept -> bool = default;
};
struct MeshKeyHash {
    [[nodiscard]] auto operator()(const MeshKey& key) const noexcept -> size_t {
        auto h = static_cast<size_t>(key.vertexHash);
        ZHLN::HashCombine(h, static_cast<size_t>(key.indexHash));
        ZHLN::HashCombine(h, key.vertexCount);
        ZHLN::HashCombine(h, key.indexCount);
        return h;
    }
};

struct AssetShapeEntry {
    std::weak_ptr<const MeshGeometry> snapshot;
    JPH::ShapeRefC shape;
};

// Jolt shapes are immutable and reference counted; lookups and insertions
// take this mutex, but simulation steps do not.
struct alignas(64) ShapeCache {
    mutable ZHLN::Mutex    mutex {};
    JPH::Array<ShapeEntry> entries;
    std::unordered_map<MeshKey, JPH::ShapeRefC, MeshKeyHash> meshes;
    std::unordered_map<CollisionAssetID, AssetShapeEntry> assetMeshes;
    CollisionAssetResolver resolver = nullptr;
    void* resolverContext = nullptr;

    [[nodiscard]] auto GetOrCreate(ShapeType type, float p1, float p2, float p3, float p4) -> JPH::ShapeRefC;
    [[nodiscard]] auto Resolve(const AnyShape& shape) -> JPH::ShapeRefC;
    [[nodiscard]] auto Resolve(const AnyPrimitiveShape& shape) -> JPH::ShapeRefC;
    void               BindCollisionAssetResolver(CollisionAssetResolver lookup, void* context) noexcept;
    void               ClearMeshes() noexcept;
    void               Clear() noexcept;
};

static_assert(std::is_standard_layout_v<ShapeKey>);

} // namespace ZHLN::Physics
