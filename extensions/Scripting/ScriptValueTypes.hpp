/*
 * Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// extensions/Scripting/ScriptValueTypes.hpp
//
// Teaches ScriptBinder the value types the engine's scripting surface actually
// passes around: Entity and Zahlen's FVec3/DVec3/Quat/FVec4.
//
// Why these need teaching at all is at ScriptValueTrait in ScriptBinder.hpp. The
// short version: left generic, a FVec3 crosses as an opaque BoxedObject, so a
// script could only hand back one it had already been given. Physics calls like
//   SetCharacterVelocity(Entity, FVec3)
//   Raycast(DVec3 origin, FVec3 dir, float max, Physics::BodyID ignore)
// would be uncallable from Lua, which is most of the reason the
// ZHLN_DispatchCommand table still exists.
//
// Representation
// --------------
// Entity crosses as a number -- its packed 64-bit id. Scripts already spell
// entities that way throughout the Fennel sources, and it keeps the common case
// (`:AddImpulse(player, {0, 8, 0})`) free of wrapper objects.
//
// Vectors and quaternions cross as arrays of numbers, so Lua writes `{0, 5, 0}`
// rather than constructing a cdata struct. Reading is symmetric. The cost is
// that field identity is positional: `v[1]` is x by index, not by name. For a
// three-float vector that is a fair trade, and it is what the zero-copy buffer
// views already do when they hand a component's floats to Lua as a flat array.
//
// A BoxedObject or OwnedObject naming the right type is still accepted on the
// way in, so a vector read off a component can be passed straight back without
// the script unpacking it into a literal first.
//
// On type names
// -------------
// Each trait declares its own `scriptName` rather than asking
// Reflect::TypeName<T>(), and the reason is that the no-reflection stub in
// Core/Reflection/Core.hpp returns "" for every type. A guard written against
// TypeName therefore compares "" to "" and accepts anything on a build without
// reflection -- fail-open, silently. These names are the bare identifiers
// std::meta::identifier_of produces, so on a reflection build they match what a
// generic producer writes into BoxedObject::typeName, and on a stub build they
// do not match "" and the boxed path is refused instead of guessed at.

#pragma once

#include <Zahlen/Math/Angles.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Math/Quat.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Physics/Jolt.hpp>
#include <Zahlen/ECS/Entity.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <Scripting/ScriptBinder.hpp>
#include <string_view>

namespace ZHLN {
namespace {

// Build a ScriptVal holding a fixed-length array of numbers.
inline auto NumberArray(std::initializer_list<double> values) -> ScriptVal {
    ScriptArray arr;
    arr.elements.reserve(values.size());
    for (double v: values) {
        arr.elements.push_back(v);
    }
    return arr;
}

// Read exactly N numbers out of a script value.
//
// Accepts a ScriptArray of N numbers, or a boxed object of the right C++ type
// -- which is how a vector read off a component arrives when the script passes
// it along without rewriting it as a literal.
//
// The binder cannot check a pointer's dynamic type, so `scriptName` is the only
// guard available. Without it a Quat handed where a FVec3 was wanted would be
// reinterpreted rather than rejected, reading four floats out of a
// three-float object.
template <size_t N, typename T, typename Unbox>
auto ReadNumbers(const ScriptVal& sval, std::string_view scriptName, Unbox unbox) -> std::expected<std::array<double, N>, ErrorCode> {
    if (const auto* arr = std::get_if<ScriptArray>(&sval)) {
        if (arr->elements.size() != N) {
            return std::unexpected(ScriptError::ArityMismatch);
        }
        std::array<double, N> out {};
        for (size_t i = 0; i < N; ++i) {
            const auto* d = std::get_if<double>(&arr->elements[i]);
            if (d == nullptr) {
                return std::unexpected(ScriptError::TypeMismatch);
            }
            out[i] = *d;
        }
        return out;
    }

    const void* raw = nullptr;
    if (const auto* obj = std::get_if<BoxedObject>(&sval)) {
        if (obj->typeName != scriptName) {
            return std::unexpected(ScriptError::TypeMismatch);
        }
        raw = obj->rawPtr;
    }
    if (raw != nullptr) {
        return unbox(*static_cast<const T*>(raw));
    }

    return std::unexpected(ScriptError::TypeMismatch);
}

} // namespace

// Camera degree fields remain numbers on the scripting boundary, rather than
// becoming boxed objects just because their native storage has a unit.
template <>
struct ScriptValueTrait<DegF> {
    static constexpr bool specialized = true;

    static auto To(DegF angle) -> ScriptVal {
        return static_cast<double>(angle.value);
    }

    static auto From(const ScriptVal& sval) -> std::expected<DegF, ErrorCode> {
        if (const auto* d = std::get_if<double>(&sval)) {
            return DegF{static_cast<float>(*d)};
        }
        return std::unexpected(ScriptError::TypeMismatch);
    }
};

// ---------------------------------------------------------------------------
// Entity <-> packed id
// ---------------------------------------------------------------------------

template <>
struct ScriptValueTrait<Entity> {
    static constexpr bool             specialized = true;
    static constexpr std::string_view scriptName  = "Entity";

    static auto To(const Entity& e) -> ScriptVal {
        return static_cast<double>(e.Pack());
    }

    static auto From(const ScriptVal& sval) -> std::expected<Entity, ErrorCode> {
        if (const auto* d = std::get_if<double>(&sval)) {
            return Entity::Unpack(static_cast<uint64_t>(*d));
        }
        return std::unexpected(ScriptError::TypeMismatch);
    }
};

// ---------------------------------------------------------------------------
// FVec3 / Vec3<-> {x, y, z}
// ---------------------------------------------------------------------------

template <>
struct ScriptValueTrait<FVec3> {
    static constexpr bool             specialized = true;
    static constexpr std::string_view scriptName  = "FVec3";

    static auto To(const FVec3& v) -> ScriptVal {
        return NumberArray({static_cast<double>(v.x), static_cast<double>(v.y), static_cast<double>(v.z)});
    }

    static auto From(const ScriptVal& sval) -> std::expected<FVec3, ErrorCode> {
        auto nums = ReadNumbers<3, FVec3>(sval, scriptName, [](const FVec3& v) -> std::array<double, 3> {
            return {static_cast<double>(v.x), static_cast<double>(v.y), static_cast<double>(v.z)};
        });
        if (!nums) {
            return std::unexpected(nums.error());
        }
        return FVec3{static_cast<float>((*nums)[0]), static_cast<float>((*nums)[1]), static_cast<float>((*nums)[2])};
    }
};

// DVec3 is DVec3 under JPH_DOUBLE_PRECISION and FVec3 otherwise (Real.h:17,29).
// Both are handled, so a physics origin crosses correctly in either build.
template <>
struct ScriptValueTrait<DVec3> {
    static constexpr bool             specialized = true;
    static constexpr std::string_view scriptName  = "DVec3";

    static auto To(const DVec3& v) -> ScriptVal {
        return NumberArray({v.x, v.y, v.z});
    }

    static auto From(const ScriptVal& sval) -> std::expected<DVec3, ErrorCode> {
        auto nums = ReadNumbers<3, DVec3>(sval, scriptName, [](const DVec3& v) -> std::array<double, 3> { return {v.x, v.y, v.z}; });
        if (!nums) {
            return std::unexpected(nums.error());
        }
        return DVec3{(*nums)[0], (*nums)[1], (*nums)[2]};
    }
};

// ---------------------------------------------------------------------------
// ZHLN::Quat <-> {x, y, z, w}
// ---------------------------------------------------------------------------

template <>
struct ScriptValueTrait<ZHLN::Quat> {
    static constexpr bool             specialized = true;
    static constexpr std::string_view scriptName  = "Quat";

    static auto To(const ZHLN::Quat& q) -> ScriptVal {
        return NumberArray({static_cast<double>(q.x), static_cast<double>(q.y), static_cast<double>(q.z), static_cast<double>(q.w)});
    }

    static auto From(const ScriptVal& sval) -> std::expected<ZHLN::Quat, ErrorCode> {
        auto nums = ReadNumbers<4, ZHLN::Quat>(sval, scriptName, [](const ZHLN::Quat& q) -> std::array<double, 4> {
            return {static_cast<double>(q.x), static_cast<double>(q.y), static_cast<double>(q.z), static_cast<double>(q.w)};
        });
        if (!nums) {
            return std::unexpected(nums.error());
        }
        return ZHLN::Quat{static_cast<float>((*nums)[0]), static_cast<float>((*nums)[1]), static_cast<float>((*nums)[2]), static_cast<float>((*nums)[3])};
    }
};

// ---------------------------------------------------------------------------
// Vec4<-> {x, y, z, w}
//
// Colours reach scripts this way: DrawLine's two endpoints, and
// Material.baseColorFactor.
// ---------------------------------------------------------------------------

template <>
struct ScriptValueTrait<FVec4> {
    static constexpr bool             specialized = true;
    static constexpr std::string_view scriptName  = "FVec4";

    static auto To(const FVec4& v) -> ScriptVal {
        return NumberArray({static_cast<double>(v.x), static_cast<double>(v.y), static_cast<double>(v.z), static_cast<double>(v.w)});
    }

    static auto From(const ScriptVal& sval) -> std::expected<FVec4, ErrorCode> {
        auto nums = ReadNumbers<4, FVec4>(sval, scriptName, [](const FVec4& v) -> std::array<double, 4> {
            return {static_cast<double>(v.x), static_cast<double>(v.y), static_cast<double>(v.z), static_cast<double>(v.w)};
        });
        if (!nums) {
            return std::unexpected(nums.error());
        }
        return FVec4{static_cast<float>((*nums)[0]), static_cast<float>((*nums)[1]), static_cast<float>((*nums)[2]), static_cast<float>((*nums)[3])};
    }
};

} // namespace ZHLN
