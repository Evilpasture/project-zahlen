// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// plugins/lua/SceneLua.hpp
//
// Bridges Zahlen's math and Scene descriptor types onto ReflectLua.
// Remember include-governance: plugins/ always includes <Zahlen/Physics/Jolt.hpp>,
// never <Jolt/Jolt.h> directly.

#include <lua/ReflectLua.hpp>

#include <Zahlen/Physics/Jolt.hpp>
#include <Zahlen/Math/ValueTypes.hpp>
#include <Zahlen/Engine/Scene.hpp>

namespace ZHLN::ReflectLua {

template <>
struct LuaVector<JPH::Float2> : std::true_type {};

template <>
struct LuaVector<JPH::Float3> : std::true_type {};

template <>
struct LuaVector<JPH::Float4> : std::true_type {};

// Native math values must retain the same sequence/named-table Lua contract as
// Jolt's storage vectors. Their fields are public and reflection can visit them.
template <>
struct LuaVector<ZHLN::Float2> : std::true_type {};

template <>
struct LuaVector<ZHLN::Float3> : std::true_type {};

template <>
struct LuaVector<ZHLN::Float4> : std::true_type {};

template <>
struct LuaVector<ZHLN::Double3> : std::true_type {};

namespace TemplatedDetail {

    template <typename Scalar>
    struct Vec3TablePatch {
        Scalar x = Scalar(0);
        Scalar y = Scalar(0);
        Scalar z = Scalar(0);
    };

    template <typename Scalar>
    struct Vec4TablePatch {
        Scalar x = Scalar(0);
        Scalar y = Scalar(0);
        Scalar z = Scalar(0);
        Scalar w = Scalar(1);
    };

} // namespace TemplatedDetail

template <>
struct LuaValueTrait<JPH::Vec3> {
    static constexpr bool specialized = true;

    static auto Read(Value reader) -> std::expected<JPH::Vec3, ErrorCode> {
        size_t seqLen = 0;
        if (reader.IsSequence(seqLen)) {
            if (seqLen != 3u) {
                return std::unexpected(LuaError::OutOfRange);
            }
            std::array<float, 3> comps {0.0f, 0.0f, 0.0f};
            if (auto res = TemplatedDetail::ReadInto(comps, reader); !res) {
                return std::unexpected(res.error());
            }
            return JPH::Vec3(comps[0], comps[1], comps[2]);
        }
        TemplatedDetail::Vec3TablePatch<float> named {};
        if (auto res = TemplatedDetail::PatchObject(named, reader); !res) {
            return std::unexpected(res.error());
        }
        return JPH::Vec3(named.x, named.y, named.z);
    }

    static void Push(Builder& builder, const JPH::Vec3& v) {
        builder.BeginTable(3, 0);
        builder.PushDouble(static_cast<double>(v.GetX()));
        builder.SetElementTop(0);
        builder.PushDouble(static_cast<double>(v.GetY()));
        builder.SetElementTop(1);
        builder.PushDouble(static_cast<double>(v.GetZ()));
        builder.SetElementTop(2);
    }
};

template <>
struct LuaValueTrait<JPH::Vec4> {
    static constexpr bool specialized = true;

    static auto Read(Value reader) -> std::expected<JPH::Vec4, ErrorCode> {
        size_t seqLen = 0;
        if (reader.IsSequence(seqLen)) {
            if (seqLen != 4u) {
                return std::unexpected(LuaError::OutOfRange);
            }
            std::array<float, 4> comps {0.0f, 0.0f, 0.0f, 1.0f};
            if (auto res = TemplatedDetail::ReadInto(comps, reader); !res) {
                return std::unexpected(res.error());
            }
            return JPH::Vec4(comps[0], comps[1], comps[2], comps[3]);
        }
        TemplatedDetail::Vec4TablePatch<float> named {};
        if (auto res = TemplatedDetail::PatchObject(named, reader); !res) {
            return std::unexpected(res.error());
        }
        return JPH::Vec4(named.x, named.y, named.z, named.w);
    }

    static void Push(Builder& builder, const JPH::Vec4& v) {
        builder.BeginTable(4, 0);
        builder.PushDouble(static_cast<double>(v.GetX()));
        builder.SetElementTop(0);
        builder.PushDouble(static_cast<double>(v.GetY()));
        builder.SetElementTop(1);
        builder.PushDouble(static_cast<double>(v.GetZ()));
        builder.SetElementTop(2);
        builder.PushDouble(static_cast<double>(v.GetW()));
        builder.SetElementTop(3);
    }
};

template <>
struct LuaValueTrait<JPH::Quat> {
    static constexpr bool specialized = true;

    static auto Read(Value reader) -> std::expected<JPH::Quat, ErrorCode> {
        size_t seqLen = 0;
        if (reader.IsSequence(seqLen)) {
            if (seqLen != 4u) {
                return std::unexpected(LuaError::OutOfRange);
            }
            std::array<float, 4> comps {0.0f, 0.0f, 0.0f, 1.0f};
            if (auto res = TemplatedDetail::ReadInto(comps, reader); !res) {
                return std::unexpected(res.error());
            }
            return JPH::Quat(comps[0], comps[1], comps[2], comps[3]);
        }
        TemplatedDetail::Vec4TablePatch<float> named {};
        if (auto res = TemplatedDetail::PatchObject(named, reader); !res) {
            return std::unexpected(res.error());
        }
        return JPH::Quat(named.x, named.y, named.z, named.w);
    }

    static void Push(Builder& builder, const JPH::Quat& q) {
        builder.BeginTable(4, 0);
        builder.PushDouble(static_cast<double>(q.GetX()));
        builder.SetElementTop(0);
        builder.PushDouble(static_cast<double>(q.GetY()));
        builder.SetElementTop(1);
        builder.PushDouble(static_cast<double>(q.GetZ()));
        builder.SetElementTop(2);
        builder.PushDouble(static_cast<double>(q.GetW()));
        builder.SetElementTop(3);
    }
};

} // namespace ZHLN::ReflectLua
