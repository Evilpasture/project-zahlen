// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The physics shape vocabulary: one plain wire struct per primitive, no *Desc
// suffix, no tag template, no runtime type enum. Every struct here is
// standard-layout and trivially copyable so it can travel a COM chain
// (LayoutHash + ChainPayload both accept it) and be passed by value across
// the shared-library boundary. A Compound stores its children as chain-node
// stamps -- the same struct the boundary ships -- and FixedCompound is its
// zero-allocation compile-time sibling: children live in a tuple, CreateBody
// stamps a node array on the stack, and no vector is ever owned.

#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Core/COM/Chain.hpp>
// COM::Span is this header's borrowed-storage spelling: fixed layout, no
// ownership, trivially copyable. Sanctioned in configure/check_com_boundary.py.
#include <Zahlen/Core/COM/Types.hpp>
#include <Zahlen/Math/Quat.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <array>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <memory>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
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

// One node per child, stamped with the payload's own chain identity
// (ChainTypeID<Placed<S>> + LayoutHash<Placed<S>> + size). The span borrows
// from the caller (a local array, a builder's vectors, a chain node) and must
// outlive the body-creation call. Child order is the compound's SubShapeID
// order -- every consumer iterates this span once, in insertion order.
struct Compound {
    COM::Span<const COM::ChainInNode> children {};
};

// Every child kind a Compound may carry, as one variant vector: the single
// storage spelling for runtime-built compounds (CompoundBuilder,
// OwnedCompound). Value semantics -- a copy of the variant copies the
// Placed<T> payload, so appending never repoints an already-stamped node.
using CompoundChild = std::variant<Placed<Box>, Placed<Sphere>, Placed<Capsule>, Placed<Cylinder>, Placed<Plane>,
                                   Placed<TriangleMesh>, Placed<MeshAsset>, Placed<Compound>>;

// Stamp one payload as a Compound child node. ChainTypeID strips cv/ref
// internally, so T is the bare payload type either way.
template <typename T>
[[nodiscard]] auto StampChildNode(const T& value) noexcept -> COM::ChainInNode {
    return COM::ChainInNode {
        .sType      = COM::ChainTypeID<T>(),
        .layoutHash = Reflect::LayoutHash<T>(),
        .size       = static_cast<uint32_t>(sizeof(T)),
        .data       = &value,
    };
}

// Stamp-verified dispatch over one child node of a Compound: `fn` receives
// the Placed<S> payload for the first Shapes... alternative whose stamps the
// node carries (ChainTypeID, LayoutHash, size, non-null data). Returns false
// when no alternative matches -- a foreign or corrupt node, never a
// silent no-op. Consumers bound recursion with kMaxCompoundDepth.
template <typename... Shapes, typename Fn>
[[nodiscard]] auto VisitCompoundChild(const COM::ChainInNode& node, Fn&& fn) -> bool {
    bool matched = false;
    const auto tryOne = [&]<typename T>() {
        if (matched) return;
        if (node.sType == COM::ChainTypeID<T>() && node.layoutHash == Reflect::LayoutHash<T>() &&
            node.size == sizeof(T) && node.data != nullptr) {
            fn(*static_cast<const T*>(node.data));
            matched = true;
        }
    };
    (tryOne.template operator()<Placed<Shapes>>(), ...);
    return matched;
}

// Structural policy check for a Compound child list against the shape kinds a
// body role accepts: every child must carry a stamp matching one of Shapes...
// with in-range extents, nested compounds must recurse (bounded depth), and
// the list must be non-empty. Called at the CreateBody boundary -- the only
// place a role policy exists -- so ShapeCache and Jolt stay policy-free.
inline constexpr uint32_t kMaxCompoundDepth = 8U;

template <typename... Shapes>
[[nodiscard]] auto ValidateCompound(const Compound& compound, uint32_t depth = 0U) -> bool {
    if (compound.children.data == nullptr || compound.children.size == 0U) return false;
    if (depth >= kMaxCompoundDepth) return false;
    for (size_t index = 0; index < compound.children.size; ++index) {
        const COM::ChainInNode& node = compound.children[index];
        bool                    valid = true;
        const bool matched = VisitCompoundChild<Shapes...>(node, [&]<typename S>(const Placed<S>& child) {
            if constexpr (std::is_same_v<S, Box>) {
                valid = child.shape.halfExtents.x > 0.0F && child.shape.halfExtents.y > 0.0F &&
                        child.shape.halfExtents.z > 0.0F;
            } else if constexpr (std::is_same_v<S, Sphere>) {
                valid = child.shape.radius > 0.0F;
            } else if constexpr (std::is_same_v<S, Capsule> || std::is_same_v<S, Cylinder>) {
                valid = child.shape.halfHeight > 0.0F && child.shape.radius > 0.0F;
            } else if constexpr (std::is_same_v<S, Compound>) {
                valid = ValidateCompound<Shapes...>(child.shape, depth + 1U);
            }
            // Plane/TriangleMesh/MeshAsset children carry no local extents;
            // resolve-time validation covers meshes.
        });
        if (!matched || !valid) return false;
    }
    return true;
}

// Stamp a variant child list into a caller-owned scratch vector; the returned
// Compound views both that scratch and the variant values, so both must
// outlive the body-creation call.
[[nodiscard]] inline auto StampCompoundNodes(std::span<const CompoundChild> children,
                                             std::vector<COM::ChainInNode>& scratch) -> Compound {
    scratch.clear();
    scratch.reserve(children.size());
    for (const CompoundChild& child : children) {
        std::visit([&scratch](const auto& payload) { scratch.push_back(StampChildNode(payload)); }, child);
    }
    return Compound {.children = {scratch.data(), scratch.size()}};
}

// --- Owned geometry (non-wire) -----------------------------------------------
// Imported prefabs own geometry; spans inside TriangleMesh/Compound point into
// these vectors, which must outlive the body-creation call.
struct MeshGeometry {
    std::vector<FVec3>    vertices;
    std::vector<uint32_t> indices;
};

// The returned immutable snapshot pins asset data across cache clears/reloads.
using CollisionAssetResolver = std::shared_ptr<const MeshGeometry> (*)(void*, CollisionAssetID) noexcept;

// Compile-time compound: the child list lives as a tuple, so CreateBody can
// stamp a ChainInNode array on the stack and hand Jolt a zero-allocation
// Compound. This is the fixed-geometry answer to CompoundBuilder -- no
// vectors to own at runtime. The children's body-kind policy is expressed by
// the DynamicShape/CharacterShape/StaticShape concepts below.
template <typename... Children>
struct FixedCompound {
    std::tuple<Placed<Children>...> children;

    [[nodiscard]] auto MakeNodes() const noexcept -> std::array<COM::ChainInNode, sizeof...(Children)> {
        return std::apply(
            [](const auto&... placed) {
                return std::array<COM::ChainInNode, sizeof...(Children)> {StampChildNode(placed)...};
            },
            children);
    }
};

template <typename... Children>
[[nodiscard]] auto MakeCompound(const Placed<Children>&... children) noexcept -> FixedCompound<Children...> {
    return {{children...}};
}

// Non-ABI helper: an owning builder for Compound views. It has a std::vector
// member, so it is not itself a wire type -- keep it alive across the
// CreateBody call and borrow View() into a caller-owned node scratch.
// Insertion order is the compound's SubShapeID order.
class CompoundBuilder {
  public:
    auto Add(Box shape, FVec3 position = {}, Quat rotation = Quat::Identity()) -> CompoundBuilder& {
        m_children.emplace_back(Placed<Box> {shape, position, rotation});
        return *this;
    }
    auto Add(Sphere shape, FVec3 position = {}, Quat rotation = Quat::Identity()) -> CompoundBuilder& {
        m_children.emplace_back(Placed<Sphere> {shape, position, rotation});
        return *this;
    }
    auto Add(Capsule shape, FVec3 position = {}, Quat rotation = Quat::Identity()) -> CompoundBuilder& {
        m_children.emplace_back(Placed<Capsule> {shape, position, rotation});
        return *this;
    }
    auto Add(Cylinder shape, FVec3 position = {}, Quat rotation = Quat::Identity()) -> CompoundBuilder& {
        m_children.emplace_back(Placed<Cylinder> {shape, position, rotation});
        return *this;
    }
    auto Add(Plane shape, FVec3 position = {}, Quat rotation = Quat::Identity()) -> CompoundBuilder& {
        m_children.emplace_back(Placed<Plane> {shape, position, rotation});
        return *this;
    }
    auto Add(TriangleMesh shape, FVec3 position = {}, Quat rotation = Quat::Identity()) -> CompoundBuilder& {
        m_children.emplace_back(Placed<TriangleMesh> {shape, position, rotation});
        return *this;
    }
    auto Add(MeshAsset shape, FVec3 position = {}, Quat rotation = Quat::Identity()) -> CompoundBuilder& {
        m_children.emplace_back(Placed<MeshAsset> {shape, position, rotation});
        return *this;
    }

    [[nodiscard]] auto View(std::vector<COM::ChainInNode>& scratch) const -> Compound {
        return StampCompoundNodes(m_children, scratch);
    }

  private:
    std::vector<CompoundChild> m_children;
};

// --- Shape concepts -----------------------------------------------------------
// The creation-time contracts: a static body accepts anything Jolt accepts on
// a static body, a moving body only convex-ish shapes, a character any convex
// primitive or compound. Concepts reject a bad combination at compile time on
// every creation call, raw chain or ergonomic wrapper.

template <typename T>
concept ConvexShape = std::same_as<std::remove_cvref_t<T>, Box> ||
                      std::same_as<std::remove_cvref_t<T>, Sphere> ||
                      std::same_as<std::remove_cvref_t<T>, Capsule> ||
                      std::same_as<std::remove_cvref_t<T>, Cylinder>;

// Compound children are checked against the child kinds a body kind accepts:
// dynamic, kinematic and character bodies admit convex primitives and nested
// compounds only, while static bodies additionally admit planes and
// mesh-backed shapes. The role policy itself is enforced by ValidateCompound
// at the CreateBody boundary; these concepts gate the type.
template <typename T>
concept DynamicCompoundChild = ConvexShape<T> || std::same_as<std::remove_cvref_t<T>, Compound>;

template <typename T>
concept StaticCompoundChild =
    DynamicCompoundChild<T> || std::same_as<std::remove_cvref_t<T>, Plane> ||
    std::same_as<std::remove_cvref_t<T>, TriangleMesh> || std::same_as<std::remove_cvref_t<T>, MeshAsset>;

// FixedCompound<Ts...> is accepted as a body shape exactly when every child
// is acceptable -- the compile-time half of the same policy.
template <typename T>
struct FixedCompoundTraits {
    static constexpr bool dynamicChildren = false;
    static constexpr bool staticChildren  = false;
};

template <typename... Children>
struct FixedCompoundTraits<FixedCompound<Children...>> {
    static constexpr bool dynamicChildren = (DynamicCompoundChild<Children> && ...);
    static constexpr bool staticChildren  = (StaticCompoundChild<Children> && ...);
};

template <typename T>
struct IsFixedCompound : std::false_type {};

template <typename... Children>
struct IsFixedCompound<FixedCompound<Children...>> : std::true_type {};

template <typename T>
inline constexpr bool kIsFixedCompound = IsFixedCompound<std::remove_cvref_t<T>>::value;

template <typename T>
concept DynamicShape =
    ConvexShape<T> || std::same_as<std::remove_cvref_t<T>, Compound> ||
    (kIsFixedCompound<T> && FixedCompoundTraits<std::remove_cvref_t<T>>::dynamicChildren);

template <typename T>
concept CharacterShape =
    ConvexShape<T> || std::same_as<std::remove_cvref_t<T>, Compound> ||
    (kIsFixedCompound<T> && FixedCompoundTraits<std::remove_cvref_t<T>>::dynamicChildren);

template <typename T>
concept StaticShape =
    ConvexShape<T> ||
    std::same_as<std::remove_cvref_t<T>, Plane> ||
    std::same_as<std::remove_cvref_t<T>, TriangleMesh> || std::same_as<std::remove_cvref_t<T>, MeshAsset> ||
    std::same_as<std::remove_cvref_t<T>, HeightField> || std::same_as<std::remove_cvref_t<T>, Compound> ||
    (kIsFixedCompound<T> && FixedCompoundTraits<std::remove_cvref_t<T>>::staticChildren);

static_assert(StaticShape<Box> && DynamicShape<Sphere> && CharacterShape<Capsule> && CharacterShape<Box> &&
              CharacterShape<Compound>);
static_assert(!DynamicShape<Plane> && !DynamicShape<TriangleMesh> && !DynamicShape<HeightField>);
static_assert(!CharacterShape<Plane> && !CharacterShape<TriangleMesh>);

// FixedCompound follows the child policy at compile time: convex children are
// accepted by every body kind, a Plane child only by static bodies.
using ConvexPair = decltype(MakeCompound(Placed<Sphere> {Sphere {0.5F}}, Placed<Capsule> {Capsule {1.0F, 0.3F}}));
using PlanePair  = decltype(MakeCompound(Placed<Sphere> {Sphere {0.5F}}, Placed<Plane> {Plane {}}));
static_assert(DynamicShape<ConvexPair> && CharacterShape<ConvexPair> && StaticShape<ConvexPair>);
static_assert(!DynamicShape<PlanePair> && !CharacterShape<PlanePair> && StaticShape<PlanePair>);

// Wire safety: every shape struct must travel a COM chain unchanged.
static_assert(std::is_standard_layout_v<Box> && std::is_trivially_copyable_v<Box>);
static_assert(std::is_standard_layout_v<Sphere> && std::is_trivially_copyable_v<Sphere>);
static_assert(std::is_standard_layout_v<Capsule> && std::is_trivially_copyable_v<Capsule>);
static_assert(std::is_standard_layout_v<Cylinder> && std::is_trivially_copyable_v<Cylinder>);
static_assert(std::is_standard_layout_v<Plane> && std::is_trivially_copyable_v<Plane>);
static_assert(std::is_standard_layout_v<TriangleMesh> && std::is_trivially_copyable_v<TriangleMesh>);
static_assert(std::is_standard_layout_v<MeshAsset> && std::is_trivially_copyable_v<MeshAsset>);
static_assert(std::is_standard_layout_v<HeightField> && std::is_trivially_copyable_v<HeightField>);
static_assert(std::is_standard_layout_v<Compound> && std::is_trivially_copyable_v<Compound>);
static_assert(std::is_trivially_copyable_v<decltype(MakeCompound(Placed<Sphere> {Sphere {}}))>);

} // namespace ZHLN::Physics
