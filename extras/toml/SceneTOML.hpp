// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// extras/toml/SceneTOML.hpp
//
// Jolt vector bindings for the reflection-driven TOML layer.
//
// JPH::Float2/3/4 are plain structs to C++ and coordinates to a reader, so
// this header opts them into array form: `position = [x, y, z]` instead of a
// `[position.x]` table. That is what lets a core ZHLN::Scene::Scene -- whose
// fields are Jolt storage types -- read and write as a document:
//
//     #include <Zahlen/Scene.hpp>
//     #include <toml/TOML.hpp>
//     #include <toml/SceneTOML.hpp>
//
//     const auto scene = ReflectTOML::TryParse<ZHLN::Scene::Scene>(text);
//     const auto built = ZHLN::Scene::Instantiate(engine, *scene);
//
// The bindings live here, not in toml/TOML.hpp (that header stays free of any
// dependency on Jolt) and not in Zahlen/Scene.hpp (the core scene model stays
// free of any dependency on the document format), and they must be visible
// wherever ReflectTOML::TryParse / SerializeTOML is instantiated over one of
// these vector types, or the field serialises as a table of members instead
// of `[x, y, z]`.
//
// TOML rather than JSON because scene files are hand-edited and reviewed:
// comments survive, [[entities]] reads as a list of things rather than a
// bracket forest, and `position = [0.0, 8.0, 0.0]` diffs one line at a time.

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Float2.h>
#include <Jolt/Math/Float3.h>
#include <Jolt/Math/Float4.h>
#include <toml/TOML.hpp>
#include <type_traits>

namespace ZHLN::ReflectTOML {

// Jolt's storage vectors are structs to C++ and coordinates to a reader, so
// they are written and parsed as `[x, y, z]` rather than a table of members.
// Declared here rather than in toml/TOML.hpp to keep that header free of any
// dependency on Jolt, and rather than in Zahlen/Scene.hpp to keep the core
// scene model free of any dependency on the document format.
template <>
struct TOMLVector<JPH::Float2> : std::true_type {};
template <>
struct TOMLVector<JPH::Float3> : std::true_type {};
template <>
struct TOMLVector<JPH::Float4> : std::true_type {};

} // namespace ZHLN::ReflectTOML
