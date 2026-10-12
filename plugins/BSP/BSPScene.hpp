// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// plugins/BSP/BSPScene.hpp
//
// The other half of "import down to native representation, let the
// instantiator do the rest": this turns the entity lump into a plain
// ZHLN::Scene::Scene description -- the same document the reflection TOML
// layer parses -- and ZHLN::Scene::Instantiate does all the spawning. World
// geometry arrives as a single SceneEntity::source pointing at the .bsp
// virtual path, which the prefab cache resolves once the importer adapter has
// filled it.

#include "BSPGeometry.hpp"
#include "BSPRead.hpp"
#include <Zahlen/Scene.hpp>
#include <string_view>

namespace ZHLN::BSP {

// Entity classes mapped onto the scene model: world geometry, lights, player
// camera start, static/dynamic props, as well as interactive entities
// (func_door, func_button, trigger_*, prop_door_rotating) with their kinematic
// bodies, bounding extents, and prefab sources retained for ECS initialization.
[[nodiscard]] auto DescribeScene(const BSPMap& map, std::string_view virtualPath, const ImportOptions& options = {}) -> Scene::Scene;

// Converts a Source forward direction (and roll) into the engine's Euler
// triplet: pitch about +X, yaw about +Y, roll about +Z, with +X as the model
// forward axis. Exposed so the heading conversion is testable against Jolt's
// own quaternion builder rather than a second copy of the formula.
[[nodiscard]] auto EngineEulerAnglesFromSourceAngles(JPH::Vec3 sourceAnglesDegrees, const ImportOptions& options) -> JPH::Vec3;

} // namespace ZHLN::BSP
