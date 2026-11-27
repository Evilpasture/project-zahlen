// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <type_traits>

namespace ZHLN::Physics {

// Naming taxonomy for the four concepts that used to share the word "Handle":
//
//   1. inert numeric key         -> *ID   (BodyID, RagdollID)
//   2. non-owning domain facade  -> a noun (Simulation, GraphicsDevice)
//   3. scoped begin/end token    -> *Scope (FrameScope, SessionScope)
//   4. owning allocation         -> std::unique_ptr<T> / *Instance
//
// A physics slot is a key, not a body: it owns nothing and has no methods that
// act on the simulation. Hand it to the PhysicsContext/Simulation capability.
//
// Physics slots are not ECS entities. The 64-bit packing is shared with Jolt
// user data, but the distinct type prevents accidental cross-service use.
struct BodyID {
    uint32_t index;
    uint32_t generation;

    [[nodiscard]] constexpr auto Pack() const noexcept -> uint64_t {
        return (static_cast<uint64_t>(generation) << 32) | index;
    }
    [[nodiscard]] static constexpr auto Unpack(uint64_t raw) noexcept -> BodyID {
        return {.index = static_cast<uint32_t>(raw), .generation = static_cast<uint32_t>(raw >> 32)};
    }
    [[nodiscard]] static constexpr auto Null() noexcept -> BodyID {
        return {.index = UINT32_MAX, .generation = UINT32_MAX};
    }
    constexpr auto operator==(const BodyID&) const noexcept -> bool = default;
};

// A ragdoll uses a separate generational pool from rigid bodies.
enum class RagdollID : uint64_t { Invalid = UINT64_MAX };

// Constraints use their own generational pool, packed like BodyID.
struct ConstraintID {
    uint32_t index;
    uint32_t generation;

    [[nodiscard]] constexpr auto Pack() const noexcept -> uint64_t {
        return (static_cast<uint64_t>(generation) << 32) | index;
    }
    constexpr auto operator==(const ConstraintID&) const noexcept -> bool = default;
};

// The constraint payload structs live in PhysicsValues.hpp; they are named
// here only as tags so the ID vocabulary stays one header below payloads.
struct FixedConstraint;
struct PointConstraint;
struct DistanceConstraint;
struct HingeConstraint;
struct SliderConstraint;
struct ConeConstraint;

// A constraint ID that remembers which constraint kind it creates: handing a
// HingeConstraintID to SetTargetPosition is a compile error, not a silent
// no-op. Implicitly converts to ConstraintID for slot storage.
template <typename Kind>
struct TypedConstraintID {
    uint32_t index      = UINT32_MAX;
    uint32_t generation = UINT32_MAX;

    [[nodiscard]] constexpr auto Pack() const noexcept -> uint64_t {
        return (static_cast<uint64_t>(generation) << 32) | index;
    }
    [[nodiscard]] static constexpr auto Null() noexcept -> TypedConstraintID {
        return {};
    }
    [[nodiscard]] static constexpr auto From(ConstraintID id) noexcept -> TypedConstraintID {
        return {.index = id.index, .generation = id.generation};
    }
    constexpr operator ConstraintID() const noexcept {
        return {.index = index, .generation = generation};
    }
    constexpr auto operator==(const TypedConstraintID&) const noexcept -> bool = default;
};

using FixedConstraintID    = TypedConstraintID<FixedConstraint>;
using PointConstraintID    = TypedConstraintID<PointConstraint>;
using DistanceConstraintID = TypedConstraintID<DistanceConstraint>;
using HingeConstraintID    = TypedConstraintID<HingeConstraint>;
using SliderConstraintID   = TypedConstraintID<SliderConstraint>;
using ConeConstraintID     = TypedConstraintID<ConeConstraint>;

static_assert(sizeof(BodyID) == 8 && std::is_trivially_default_constructible_v<BodyID> && std::is_trivially_copyable_v<BodyID>);
static_assert(sizeof(RagdollID) == 8 && std::is_trivially_copyable_v<RagdollID>);
static_assert(std::is_trivially_default_constructible_v<ConstraintID> && std::is_trivially_copyable_v<ConstraintID>);
static_assert(sizeof(TypedConstraintID<FixedConstraint>) == 8 && std::is_standard_layout_v<TypedConstraintID<FixedConstraint>> &&
              std::is_trivially_copyable_v<TypedConstraintID<FixedConstraint>>);

}
