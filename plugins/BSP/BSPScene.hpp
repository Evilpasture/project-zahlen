// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// plugins/BSP/BSPScene.hpp
//
// The other half of "marshal down to native representation, let the
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

// Entity classes that map cleanly onto the scene model today. Everything else
// (triggers, choreo, logic entities...) is dropped with a count available via
// the returned scene's entity list; their gameplay semantics are not the
// importer's problem.
[[nodiscard]] auto DescribeScene(const BSPMap& map, std::string_view virtualPath, const MarshallOptions& options = {}) -> Scene::Scene;

} // namespace ZHLN::BSP
