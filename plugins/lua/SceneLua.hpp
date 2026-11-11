// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Math/Quat.hpp>
#include <Zahlen/Math/Vec3.hpp>
// plugins/lua/SceneLua.hpp
//
// Bridges Zahlen's math and Scene descriptor types onto ReflectLua.
// Remember include-governance: plugins/ always includes <Zahlen/Physics/Jolt.hpp>,
// never <Jolt/Jolt.h> directly.

#include <lua/ReflectLua.hpp>

#include <Zahlen/Physics/Jolt.hpp>
#include <Jolt/Math/Float2.h>
#include <Zahlen/Math/Vec2.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Math/Matrix.hpp>
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
struct LuaVector<ZHLN::FVec2> : std::true_type {};

template <>
struct LuaVector<ZHLN::FVec3> : std::true_type {};

template <>
struct LuaVector<ZHLN::FVec4> : std::true_type {};

template <>
struct LuaVector<ZHLN::DVec3> : std::true_type {};

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

// Matrices cross Lua as a flat, column-major sequence. Require every element:
// silently accepting a partial transform would replace its omitted lanes with zero.
template <std::floating_point T, size_t Rows, size_t Cols, size_t Alignment>
struct LuaValueTrait<ZHLN::Matrix<T, Rows, Cols, Alignment>> {
    static constexpr bool specialized = true;
    using Mat = ZHLN::Matrix<T, Rows, Cols, Alignment>;

    static auto Read(Value reader) -> std::expected<Mat, ErrorCode> {
        size_t length = 0;
        if (!reader.IsSequence(length)) return std::unexpected(LuaError::TypeMismatch);
        if (length != Rows * Cols) return std::unexpected(LuaError::OutOfRange);
        Mat value {};
        if (auto result = TemplatedDetail::ReadInto(value.elements, reader); !result) return std::unexpected(result.error());
        return value;
    }

    static void Push(Builder& builder, const Mat& value) {
        builder.BeginTable(static_cast<int>(Rows * Cols), 0);
        for (size_t i = 0; i < Rows * Cols; ++i) {
            builder.PushDouble(static_cast<double>(value.elements[i]));
            builder.SetElementTop(i);
        }
    }
};

template <>
struct LuaValueTrait<FVec3> {
    static constexpr bool specialized = true;

    static auto Read(Value reader) -> std::expected<FVec3, ErrorCode> {
        size_t seqLen = 0;
        if (reader.IsSequence(seqLen)) {
            if (seqLen != 3u) {
                return std::unexpected(LuaError::OutOfRange);
            }
            std::array<float, 3> comps {0.0f, 0.0f, 0.0f};
            if (auto res = TemplatedDetail::ReadInto(comps, reader); !res) {
                return std::unexpected(res.error());
            }
            return FVec3(comps[0], comps[1], comps[2]);
        }
        TemplatedDetail::Vec3TablePatch<float> named {};
        if (auto res = TemplatedDetail::PatchObject(named, reader); !res) {
            return std::unexpected(res.error());
        }
        return FVec3(named.x, named.y, named.z);
    }

    static void Push(Builder& builder, const FVec3& v) {
        builder.BeginTable(3, 0);
        builder.PushDouble(static_cast<double>(v.x));
        builder.SetElementTop(0);
        builder.PushDouble(static_cast<double>(v.y));
        builder.SetElementTop(1);
        builder.PushDouble(static_cast<double>(v.z));
        builder.SetElementTop(2);
    }
};

template <>
struct LuaValueTrait<FVec4> {
    static constexpr bool specialized = true;

    static auto Read(Value reader) -> std::expected<FVec4, ErrorCode> {
        size_t seqLen = 0;
        if (reader.IsSequence(seqLen)) {
            if (seqLen != 4u) {
                return std::unexpected(LuaError::OutOfRange);
            }
            std::array<float, 4> comps {0.0f, 0.0f, 0.0f, 1.0f};
            if (auto res = TemplatedDetail::ReadInto(comps, reader); !res) {
                return std::unexpected(res.error());
            }
            return FVec4(comps[0], comps[1], comps[2], comps[3]);
        }
        TemplatedDetail::Vec4TablePatch<float> named {};
        if (auto res = TemplatedDetail::PatchObject(named, reader); !res) {
            return std::unexpected(res.error());
        }
        return FVec4(named.x, named.y, named.z, named.w);
    }

    static void Push(Builder& builder, const FVec4& v) {
        builder.BeginTable(4, 0);
        builder.PushDouble(static_cast<double>(v.x));
        builder.SetElementTop(0);
        builder.PushDouble(static_cast<double>(v.y));
        builder.SetElementTop(1);
        builder.PushDouble(static_cast<double>(v.z));
        builder.SetElementTop(2);
        builder.PushDouble(static_cast<double>(v.w));
        builder.SetElementTop(3);
    }
};

template <>
struct LuaValueTrait<ZHLN::Quat> {
    static constexpr bool specialized = true;

    static auto Read(Value reader) -> std::expected<ZHLN::Quat, ErrorCode> {
        size_t seqLen = 0;
        if (reader.IsSequence(seqLen)) {
            if (seqLen != 4u) {
                return std::unexpected(LuaError::OutOfRange);
            }
            std::array<float, 4> comps {0.0f, 0.0f, 0.0f, 1.0f};
            if (auto res = TemplatedDetail::ReadInto(comps, reader); !res) {
                return std::unexpected(res.error());
            }
            return ZHLN::Quat(comps[0], comps[1], comps[2], comps[3]);
        }
        TemplatedDetail::Vec4TablePatch<float> named {};
        if (auto res = TemplatedDetail::PatchObject(named, reader); !res) {
            return std::unexpected(res.error());
        }
        return ZHLN::Quat(named.x, named.y, named.z, named.w);
    }

    static void Push(Builder& builder, const ZHLN::Quat& q) {
        builder.BeginTable(4, 0);
        builder.PushDouble(static_cast<double>(q.x));
        builder.SetElementTop(0);
        builder.PushDouble(static_cast<double>(q.y));
        builder.SetElementTop(1);
        builder.PushDouble(static_cast<double>(q.z));
        builder.SetElementTop(2);
        builder.PushDouble(static_cast<double>(q.w));
        builder.SetElementTop(3);
    }
};

} // namespace ZHLN::ReflectLua
