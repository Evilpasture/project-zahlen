/*
 * Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// extras/Scripting/ScriptValueTypes.hpp
//
// Teaches ScriptBinder the value types the engine's scripting surface actually
// passes around: Entity, and Jolt's Vec3/DVec3/Quat/Vec4.
//
// Why these need teaching at all is at ScriptValueTrait in ScriptBinder.hpp. The
// short version: left generic, a Vec3 crosses as an opaque BoxedObject, so a
// script could only hand back one it had already been given. Physics calls like
//   SetCharacterVelocity(Entity, Vec3Arg)
//   Raycast(RVec3Arg origin, Vec3Arg dir, float max, Entity ignore)
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
// Core/Reflection.hpp returns "" for every type. A guard written against
// TypeName therefore compares "" to "" and accepts anything on a build without
// reflection -- fail-open, silently. These names are the bare identifiers
// std::meta::identifier_of produces, so on a reflection build they match what a
// generic producer writes into BoxedObject::typeName, and on a stub build they
// do not match "" and the boxed path is refused instead of guessed at.

#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Math/DVec3.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Vec4.h>
#include <Zahlen/Entity.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <Scripting/ScriptBinder.hpp>
#include <string_view>

namespace ZHLN {
namespace {

/// Build a ScriptVal holding a fixed-length array of numbers.
inline auto NumberArray(std::initializer_list<double> values) -> ScriptVal {
    ScriptArray arr;
    arr.elements.reserve(values.size());
    for (double v: values) {
        arr.elements.push_back(v);
    }
    return arr;
}

/// Read exactly N numbers out of a script value.
///
/// Accepts a ScriptArray of N numbers, or a boxed object of the right C++ type
/// -- which is how a vector read off a component arrives when the script passes
/// it along without rewriting it as a literal.
///
/// The binder cannot check a pointer's dynamic type, so `scriptName` is the only
/// guard available. Without it a Quat handed where a Vec3 was wanted would be
/// reinterpreted rather than rejected, reading four floats out of a
/// three-float object.
template <size_t N, typename T, typename Unbox>
auto ReadNumbers(const ScriptVal& sval, std::string_view scriptName, Unbox unbox) -> std::expected<std::array<double, N>, Error> {
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
    } else if (const auto* obj = std::get_if<OwnedObject>(&sval)) {
        if (obj->typeName != scriptName) {
            return std::unexpected(ScriptError::TypeMismatch);
        }
        raw = obj->ptr.get();
    }
    if (raw != nullptr) {
        return unbox(*static_cast<const T*>(raw));
    }

    return std::unexpected(ScriptError::TypeMismatch);
}

} // namespace

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

    static auto From(const ScriptVal& sval) -> std::expected<Entity, Error> {
        if (const auto* d = std::get_if<double>(&sval)) {
            return Entity::Unpack(static_cast<uint64_t>(*d));
        }
        return std::unexpected(ScriptError::TypeMismatch);
    }
};

// ---------------------------------------------------------------------------
// JPH::Vec3 / JPH::DVec3 <-> {x, y, z}
// ---------------------------------------------------------------------------

template <>
struct ScriptValueTrait<JPH::Vec3> {
    static constexpr bool             specialized = true;
    static constexpr std::string_view scriptName  = "Vec3";

    static auto To(const JPH::Vec3& v) -> ScriptVal {
        return NumberArray({static_cast<double>(v.GetX()), static_cast<double>(v.GetY()), static_cast<double>(v.GetZ())});
    }

    static auto From(const ScriptVal& sval) -> std::expected<JPH::Vec3, Error> {
        auto nums = ReadNumbers<3, JPH::Vec3>(sval, scriptName, [](const JPH::Vec3& v) -> std::array<double, 3> {
            return {static_cast<double>(v.GetX()), static_cast<double>(v.GetY()), static_cast<double>(v.GetZ())};
        });
        if (!nums) {
            return std::unexpected(nums.error());
        }
        return JPH::Vec3(static_cast<float>((*nums)[0]), static_cast<float>((*nums)[1]), static_cast<float>((*nums)[2]));
    }
};

// RVec3 is DVec3 under JPH_DOUBLE_PRECISION and Vec3 otherwise (Real.h:17,29).
// Both are handled, so a physics origin crosses correctly in either build.
template <>
struct ScriptValueTrait<JPH::DVec3> {
    static constexpr bool             specialized = true;
    static constexpr std::string_view scriptName  = "DVec3";

    static auto To(const JPH::DVec3& v) -> ScriptVal {
        return NumberArray({v.GetX(), v.GetY(), v.GetZ()});
    }

    static auto From(const ScriptVal& sval) -> std::expected<JPH::DVec3, Error> {
        auto nums = ReadNumbers<3, JPH::DVec3>(sval, scriptName, [](const JPH::DVec3& v) -> std::array<double, 3> { return {v.GetX(), v.GetY(), v.GetZ()}; });
        if (!nums) {
            return std::unexpected(nums.error());
        }
        return JPH::DVec3((*nums)[0], (*nums)[1], (*nums)[2]);
    }
};

// ---------------------------------------------------------------------------
// JPH::Quat <-> {x, y, z, w}
// ---------------------------------------------------------------------------

template <>
struct ScriptValueTrait<JPH::Quat> {
    static constexpr bool             specialized = true;
    static constexpr std::string_view scriptName  = "Quat";

    static auto To(const JPH::Quat& q) -> ScriptVal {
        return NumberArray({static_cast<double>(q.GetX()), static_cast<double>(q.GetY()), static_cast<double>(q.GetZ()), static_cast<double>(q.GetW())});
    }

    static auto From(const ScriptVal& sval) -> std::expected<JPH::Quat, Error> {
        auto nums = ReadNumbers<4, JPH::Quat>(sval, scriptName, [](const JPH::Quat& q) -> std::array<double, 4> {
            return {static_cast<double>(q.GetX()), static_cast<double>(q.GetY()), static_cast<double>(q.GetZ()), static_cast<double>(q.GetW())};
        });
        if (!nums) {
            return std::unexpected(nums.error());
        }
        return JPH::Quat(static_cast<float>((*nums)[0]), static_cast<float>((*nums)[1]), static_cast<float>((*nums)[2]), static_cast<float>((*nums)[3]));
    }
};

// ---------------------------------------------------------------------------
// JPH::Vec4 <-> {x, y, z, w}
//
// Colours reach scripts this way: DrawLine's two endpoints, and
// Material.baseColorFactor.
// ---------------------------------------------------------------------------

template <>
struct ScriptValueTrait<JPH::Vec4> {
    static constexpr bool             specialized = true;
    static constexpr std::string_view scriptName  = "Vec4";

    static auto To(const JPH::Vec4& v) -> ScriptVal {
        return NumberArray({static_cast<double>(v.GetX()), static_cast<double>(v.GetY()), static_cast<double>(v.GetZ()), static_cast<double>(v.GetW())});
    }

    static auto From(const ScriptVal& sval) -> std::expected<JPH::Vec4, Error> {
        auto nums = ReadNumbers<4, JPH::Vec4>(sval, scriptName, [](const JPH::Vec4& v) -> std::array<double, 4> {
            return {static_cast<double>(v.GetX()), static_cast<double>(v.GetY()), static_cast<double>(v.GetZ()), static_cast<double>(v.GetW())};
        });
        if (!nums) {
            return std::unexpected(nums.error());
        }
        return JPH::Vec4(static_cast<float>((*nums)[0]), static_cast<float>((*nums)[1]), static_cast<float>((*nums)[2]), static_cast<float>((*nums)[3]));
    }
};

} // namespace ZHLN
