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

#include <Jolt/Math/Float2.h>
#include <Scene/Scene.hpp>
#include <Zahlen/Math/Matrix.hpp>
#include <Zahlen/Math/Vec2.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Physics/Jolt.hpp>
#include <lua/LuaValueTypes.hpp>
#include <lua/ReflectLua.hpp>

namespace ZHLN::ReflectLua {

template <>
struct LuaVector<JPH::Float2> : std::true_type {};

template <>
struct LuaVector<JPH::Float3> : std::true_type {};

template <>
struct LuaVector<JPH::Float4> : std::true_type {};

} // namespace ZHLN::ReflectLua
