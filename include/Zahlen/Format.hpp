// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Format.hpp
#pragma once
#include "../../src/detail/Reflection.hpp"
#include "Entity.hpp"
#include <Jolt/Jolt.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <format>

namespace ZHLN::Reflect {

// 1. Specialization for ZHLN::Entity
template <>
struct CustomFormatter<ZHLN::Entity> {
    static void format(const ZHLN::Entity& e, std::string& out) {
        out += std::format("Entity({}:{})", e.index, e.generation);
    }
};

// 2. Specialization for JPH::Vec3
template <>
struct CustomFormatter<JPH::Vec3> {
    static void format(const JPH::Vec3& v, std::string& out) {
        out += std::format("({}, {}, {})", v.GetX(), v.GetY(), v.GetZ());
    }
};

// 3. Specialization for JPH::DVec3
template <>
struct CustomFormatter<JPH::DVec3> {
    static void format(const JPH::DVec3& v, std::string& out) {
        out += std::format("({}, {}, {})", v.GetX(), v.GetY(), v.GetZ());
    }
};

// 4. Specialization for JPH::Quat
template <>
struct CustomFormatter<JPH::Quat> {
    static void format(const JPH::Quat& q, std::string& out) {
        out += std::format("({}, {}, {}, {})", q.GetX(), q.GetY(), q.GetZ(), q.GetW());
    }
};

} // namespace ZHLN::Reflect
