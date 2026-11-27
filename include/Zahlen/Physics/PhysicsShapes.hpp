// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The physics shape vocabulary: one plain wire struct per primitive, no *Desc
// suffix, no tag template, no runtime type enum. Every struct here is
// standard-layout and trivially copyable so it can travel a COM chain
// (LayoutHash + ChainPayload both accept it) and be passed by value across
// the shared-library boundary.

#include <Zahlen/Core/AssetID.hpp>
// COM::Span is this header's borrowed-storage spelling: fixed layout, no
// ownership, trivially copyable. Sanctioned in configure/check_com_boundary.py.
#include <Zahlen/Core/COM/Types.hpp>
#include <Zahlen/Math/Quat.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <vector>

namespace ZHLN::Physics {

// --- Primitive value shapes --------------------------------------------------

struct Box {
    FVec3 halfExtents {0.5f, 0.5f, 0.5f};
};

struct Sphere {
    float radius = 0.5f;
};

struct Capsule {
    float halfHeight = 0.5f;
    float radius     = 0.25f;
};

struct Cylinder {
    float halfHeight = 0.5f;
    float radius     = 0.25f;
};

struct Plane {
    FVec3 normal {0.0f, 1.0f, 0.0f};
    float constant = 0.0f;
};

// Overgrowth-style dual-shape character collider (lower lifter sphere + upper
// oval bumper). Character silhouettes only; see CharacterShape below.
struct DualShape {
    float lifterRadius   = 0.40F;
    float bumperRadiusXZ = 0.50F;
    float bumperRadiusY  = 0.70F;

    [[nodiscard]] constexpr auto GetLifterOffsetY() const noexcept -> float {
        return lifterRadius;
    }

    [[nodiscard]] inline auto GetBumperOffsetY() const noexcept -> float {
        const float ratio = lifterRadius / bumperRadiusXZ;
        if (ratio >= 1.0F) {
            return lifterRadius;
        }
        return lifterRadius + (bumperRadiusY * std::sqrt(1.0F - (ratio * ratio)));
    }
};

// Borrows `vertices` and `indices` only for the duration of body creation
// (ShapeCache::Resolve copies into Jolt); the spans must outlive that call.
struct TriangleMesh {
    COM::Span<const FVec3>    vertices {};
    COM::Span<const uint32_t> indices {};
};

// A pre-cached collision mesh resolved through CollisionAssetResolver at
// body-creation time; the cache keeps its own snapshot.
struct MeshAsset {
    CollisionAssetID id = CollisionAssetID::Invalid;
};

struct HeightField {
    COM::Span<const float> heights {};
    uint32_t               sampleCount = 0;
    float                  worldSize   = 0.0f;
};

// A compound child: the primitive plus its local placement.
template <typename S>
struct Placed {
    S     shape {};
    FVec3 position {};
    Quat  rotation = Quat::Identity();
};

// Typed compound: one span per primitive instead of a runtime kind enum.
// Spans borrow from the caller (a local vector, an OwnedCompound, a chain
// node's arrays) and must outlive the body-creation call.
struct Compound {
    COM::Span<const Placed<Box>>        boxes {};
    COM::Span<const Placed<Sphere>>     spheres {};
    COM::Span<const Placed<Capsule>>    capsules {};
    COM::Span<const Placed<Cylinder>>   cylinders {};
    COM::Span<const Placed<Plane>>      planes {};
    COM::Span<const Placed<TriangleMesh>>   meshes {};
    COM::Span<const Placed<MeshAsset>>  meshAssets {};
};

// --- Owned geometry (non-wire) -----------------------------------------------
// Imported prefabs own geometry; spans inside TriangleMesh/Compound point into
// these vectors, which must outlive the body-creation call.
struct MeshGeometry {
    std::vector<FVec3>    vertices;
    std::vector<uint32_t> indices;
};

// The returned immutable snapshot pins asset data across cache clears/reloads.
using CollisionAssetResolver = std::shared_ptr<const MeshGeometry> (*)(void*, CollisionAssetID) noexcept;

// Non-ABI helper: an owning builder for Compound views. It has std::vector
// members, so it is not itself a wire type -- keep one alive across the
// CreateBody call and borrow View().
class CompoundBuilder {
  public:
    auto Add(Box shape, FVec3 position = {}, Quat rotation = Quat::Identity()) -> CompoundBuilder& {
        m_boxes.push_back({shape, position, rotation});
        return *this;
    }
    auto Add(Sphere shape, FVec3 position = {}, Quat rotation = Quat::Identity()) -> CompoundBuilder& {
        m_spheres.push_back({shape, position, rotation});
        return *this;
    }
    auto Add(Capsule shape, FVec3 position = {}, Quat rotation = Quat::Identity()) -> CompoundBuilder& {
        m_capsules.push_back({shape, position, rotation});
        return *this;
    }
    auto Add(Cylinder shape, FVec3 position = {}, Quat rotation = Quat::Identity()) -> CompoundBuilder& {
        m_cylinders.push_back({shape, position, rotation});
        return *this;
    }
    auto Add(Plane shape, FVec3 position = {}, Quat rotation = Quat::Identity()) -> CompoundBuilder& {
        m_planes.push_back({shape, position, rotation});
        return *this;
    }
    auto Add(TriangleMesh shape, FVec3 position = {}, Quat rotation = Quat::Identity()) -> CompoundBuilder& {
        m_meshes.push_back({shape, position, rotation});
        return *this;
    }
    auto Add(MeshAsset shape, FVec3 position = {}, Quat rotation = Quat::Identity()) -> CompoundBuilder& {
        m_meshAssets.push_back({shape, position, rotation});
        return *this;
    }

    [[nodiscard]] auto View() const noexcept -> Compound {
        return Compound {
            .boxes      = m_boxes,
            .spheres    = m_spheres,
            .capsules   = m_capsules,
            .cylinders  = m_cylinders,
            .planes     = m_planes,
            .meshes     = m_meshes,
            .meshAssets = m_meshAssets,
        };
    }

  private:
    std::vector<Placed<Box>>          m_boxes;
    std::vector<Placed<Sphere>>       m_spheres;
    std::vector<Placed<Capsule>>      m_capsules;
    std::vector<Placed<Cylinder>>     m_cylinders;
    std::vector<Placed<Plane>>        m_planes;
    std::vector<Placed<TriangleMesh>> m_meshes;
    std::vector<Placed<MeshAsset>>    m_meshAssets;
};

// --- Shape concepts -----------------------------------------------------------
// The creation-time contracts: a static body accepts anything Jolt accepts on
// a static body, a moving body only convex-ish shapes, a character only its
// two silhouettes. Concepts reject a bad combination at compile time on every
// creation call, raw chain or ergonomic wrapper.

template <typename T>
concept ConvexShape = std::same_as<std::remove_cvref_t<T>, Box> ||
                      std::same_as<std::remove_cvref_t<T>, Sphere> ||
                      std::same_as<std::remove_cvref_t<T>, Capsule> ||
                      std::same_as<std::remove_cvref_t<T>, Cylinder>;

// Dynamic compounds are legal (TestCOMBoundary spawns one); mesh children
// inside a moving compound are a runtime shape-creation failure, not a type
// error -- the concept keeps the common case honest without modelling Jolt's
// whole child matrix.
template <typename T>
concept DynamicShape = ConvexShape<T> || std::same_as<std::remove_cvref_t<T>, Compound>;

template <typename T>
concept StaticShape = ConvexShape<T> ||
                      std::same_as<std::remove_cvref_t<T>, Plane> ||
                      std::same_as<std::remove_cvref_t<T>, TriangleMesh> ||
                      std::same_as<std::remove_cvref_t<T>, MeshAsset> ||
                      std::same_as<std::remove_cvref_t<T>, HeightField> ||
                      std::same_as<std::remove_cvref_t<T>, Compound>;

template <typename T>
concept CharacterShape = std::same_as<std::remove_cvref_t<T>, DualShape> ||
                         std::same_as<std::remove_cvref_t<T>, Capsule>;

static_assert(StaticShape<Box> && DynamicShape<Sphere> && CharacterShape<DualShape> && CharacterShape<Capsule>);
static_assert(!DynamicShape<Plane> && !DynamicShape<TriangleMesh> && !DynamicShape<HeightField>);
static_assert(!StaticShape<DualShape> && !CharacterShape<Box>);

// Wire safety: every shape struct must travel a COM chain unchanged.
static_assert(std::is_standard_layout_v<Box> && std::is_trivially_copyable_v<Box>);
static_assert(std::is_standard_layout_v<Sphere> && std::is_trivially_copyable_v<Sphere>);
static_assert(std::is_standard_layout_v<Capsule> && std::is_trivially_copyable_v<Capsule>);
static_assert(std::is_standard_layout_v<Cylinder> && std::is_trivially_copyable_v<Cylinder>);
static_assert(std::is_standard_layout_v<Plane> && std::is_trivially_copyable_v<Plane>);
static_assert(std::is_standard_layout_v<DualShape> && std::is_trivially_copyable_v<DualShape>);
static_assert(std::is_standard_layout_v<TriangleMesh> && std::is_trivially_copyable_v<TriangleMesh>);
static_assert(std::is_standard_layout_v<MeshAsset> && std::is_trivially_copyable_v<MeshAsset>);
static_assert(std::is_standard_layout_v<HeightField> && std::is_trivially_copyable_v<HeightField>);
static_assert(std::is_standard_layout_v<Compound> && std::is_trivially_copyable_v<Compound>);

} // namespace ZHLN::Physics
