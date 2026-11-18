// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Reflection/Utilities.hpp>
#include <Zahlen/ECS/Entity.hpp>
#include <Zahlen/Math/Quat.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <format>

namespace ZHLN::Reflect {

template <>
struct CustomFormatter<ZHLN::Entity> {
    static void format(const ZHLN::Entity& e, std::string& out) {
        out += std::format("Entity({}:{})", e.index, e.generation);
    }
};

template <>
struct CustomFormatter<FVec3> {
    static void format(const FVec3& v, std::string& out) {
        out += std::format("({}, {}, {})", v.x, v.y, v.z);
    }
};

template <>
struct CustomFormatter<DVec3> {
    static void format(const DVec3& v, std::string& out) {
        out += std::format("({}, {}, {})", v.x, v.y, v.z);
    }
};

template <>
struct CustomFormatter<FVec4> {
    static void format(const FVec4& v, std::string& out) {
        out += std::format("({}, {}, {}, {})", v.x, v.y, v.z, v.w);
    }
};

template <>
struct CustomFormatter<DVec4> {
    static void format(const DVec4& v, std::string& out) {
        out += std::format("({}, {}, {}, {})", v.x, v.y, v.z, v.w);
    }
};

template <>
struct CustomFormatter<ZHLN::Quat> {
    static void format(const ZHLN::Quat& q, std::string& out) {
        out += std::format("({}, {}, {}, {})", q.x, q.y, q.z, q.w);
    }
};

} // namespace ZHLN::Reflect
