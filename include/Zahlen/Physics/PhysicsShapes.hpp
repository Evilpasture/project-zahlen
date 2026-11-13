// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Math/Quat.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace ZHLN::Physics {

struct Box {};
struct Sphere {};
struct Capsule {};
struct Cylinder {};
struct Plane {};
struct Mesh {};
struct HeightField {};
struct Compound {};

template <typename Tag> struct Shape;

template <> struct Shape<Box> {
    using Tag = Box;
    FVec3 halfExtents {0.5f, 0.5f, 0.5f};
    constexpr Shape() noexcept = default;
    constexpr explicit Shape(FVec3 v) noexcept: halfExtents(v) {}
    constexpr Shape(float x, float y, float z) noexcept: halfExtents(x, y, z) {}
    constexpr auto operator==(const Shape&) const noexcept -> bool = default;
};
template <> struct Shape<Sphere> {
    using Tag = Sphere;
    float radius = 0.5f;
    constexpr Shape() noexcept = default;
    constexpr explicit Shape(float v) noexcept: radius(v) {}
    constexpr auto operator==(const Shape&) const noexcept -> bool = default;
};
template <> struct Shape<Capsule> {
    using Tag = Capsule;
    float halfHeight = 0.5f, radius = 0.25f;
    constexpr Shape() noexcept = default;
    constexpr Shape(float h, float r) noexcept: halfHeight(h), radius(r) {}
    constexpr auto operator==(const Shape&) const noexcept -> bool = default;
};
template <> struct Shape<Cylinder> {
    using Tag = Cylinder;
    float halfHeight = 0.5f, radius = 0.25f;
    constexpr Shape() noexcept = default;
    constexpr Shape(float h, float r) noexcept: halfHeight(h), radius(r) {}
    constexpr auto operator==(const Shape&) const noexcept -> bool = default;
};
template <> struct Shape<Plane> {
    using Tag = Plane;
    FVec3 normal {0.0f, 1.0f, 0.0f};
    float constant = 0.0f;
    constexpr Shape() noexcept = default;
    constexpr Shape(FVec3 n, float c) noexcept: normal(n), constant(c) {}
    constexpr auto operator==(const Shape&) const noexcept -> bool = default;
};
// Imported prefabs may own geometry; procedural shapes may instead borrow
// spans, which must remain valid until CreateRigidBody returns.
struct MeshGeometry {
    std::vector<FVec3> vertices;
    std::vector<uint32_t> indices;
};
template <> struct Shape<Mesh> {
    using Tag = Mesh;
    CollisionAssetID asset = CollisionAssetID::Invalid;
    std::shared_ptr<const MeshGeometry> storage {};
    std::span<const FVec3> vertices {};
    std::span<const uint32_t> indices {};
    Shape() noexcept = default;
    explicit Shape(CollisionAssetID id) noexcept: asset(id) {}
    Shape(std::span<const FVec3> v, std::span<const uint32_t> i) noexcept: vertices(v), indices(i) {}
    explicit Shape(std::shared_ptr<const MeshGeometry> data) noexcept: storage(std::move(data)) {
        if (storage) { vertices = storage->vertices; indices = storage->indices; }
    }
};
// The returned immutable snapshot pins asset data across cache clears/reloads.
using CollisionAssetResolver = std::shared_ptr<const MeshGeometry> (*)(void*, CollisionAssetID) noexcept;

template <> struct Shape<HeightField> {
    using Tag = HeightField;
    std::span<const float> heights {};
    uint32_t sampleCount = 0;
    float worldSize = 0.0f;
    constexpr Shape() noexcept = default;
    constexpr Shape(std::span<const float> h, uint32_t samples, float size) noexcept: heights(h), sampleCount(samples), worldSize(size) {}
};

using AnyPrimitiveShape = std::variant<Shape<Box>, Shape<Sphere>, Shape<Capsule>, Shape<Cylinder>, Shape<Plane>, Shape<Mesh>, Shape<HeightField>>;
template <> struct Shape<Compound> {
    using Tag = Compound;
    struct Child {
        FVec3 position {0, 0, 0};
        Quat rotation = Quat::Identity();
        AnyPrimitiveShape shape;
    };
    std::vector<Child> children;
    template <typename ChildTag>
        requires std::is_constructible_v<AnyPrimitiveShape, Shape<ChildTag>>
    auto Add(const Shape<ChildTag>& shape, FVec3 pos = {}, Quat rot = Quat::Identity()) -> Shape& {
        children.push_back(Child {.position = pos, .rotation = rot, .shape = shape});
        return *this;
    }
};
template <typename T> inline constexpr bool IsShape = false;
template <typename Tag> inline constexpr bool IsShape<Shape<Tag>> = true;
template <typename S> concept PhysicsShape = IsShape<std::remove_cvref_t<S>>;
using AnyShape = std::variant<Shape<Box>, Shape<Sphere>, Shape<Capsule>, Shape<Cylinder>, Shape<Plane>, Shape<Mesh>, Shape<HeightField>, Shape<Compound>>;

} // namespace ZHLN::Physics
