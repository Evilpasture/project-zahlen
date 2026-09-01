// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// extras/toml/SceneTOML.hpp
//
// The text half of the scene format.
//
// Zahlen/Scene.hpp owns the scene as data -- the structs, their defaults, and
// Scene::Instantiate() -- and deliberately knows nothing about how a scene is
// spelled on disk, so the core engine never depends on a document parser. This
// header is the parser's half of that deal: it binds the core description to
// the reflection-driven TOML layer in extras/toml/TOML.hpp.
//
//     const auto scene = ZHLN::ReflectTOML::TryParse<ZHLN::Scene::Scene>(text);
//     const auto built = ZHLN::Scene::Instantiate(engine, *scene);
//
//     // ... or both steps at once:
//     const auto built = ZHLN::Scene::InstantiateFromTOML(engine, text);
//
// Including this header -- not toml/TOML.hpp alone -- is what makes a scene
// readable and writable as a document: the TOMLVector specialisations at the
// bottom have to be visible wherever those templates are instantiated, or a
// JPH::Float3 serialises as a table of members instead of `[x, y, z]`.
//
// TOML rather than JSON because these files are hand-edited and reviewed:
// comments survive, [[entities]] reads as a list of things rather than a
// bracket forest, and `position = [0.0, 8.0, 0.0]` diffs one line at a time.

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Float2.h>
#include <Jolt/Math/Float3.h>
#include <Jolt/Math/Float4.h>
#include <Zahlen/Error.hpp>
#include <Zahlen/Scene.hpp>
#include <expected>
#include <string_view>
#include <toml/TOML.hpp>
#include <type_traits>

namespace ZHLN::Scene {

/// Reads a scene document and instantiates it in one step. The document half
/// lives here rather than next to Instantiate() so that a build without this
/// extra can still construct scenes from C++ -- which is exactly what the
/// engine's own fallback preset does.
[[nodiscard]] auto InstantiateFromTOML(Engine& engine, std::string_view tomlText) -> std::expected<Instance, Error>;

} // namespace ZHLN::Scene

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
