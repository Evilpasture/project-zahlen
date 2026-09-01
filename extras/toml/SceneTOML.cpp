// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extras/toml/SceneTOML.cpp
//
// One function: parse, then hand the description to the core instantiator.
// All the interesting work is on either side of it -- the schema is the type
// in Zahlen/Scene.hpp, and the entity building is Scene::Instantiate() in
// src/engine/Scene.cpp.

#include <Zahlen/Scene.hpp>
#include <expected>
#include <string_view>
#include <toml/SceneTOML.hpp>
#include <toml/TOML.hpp>

namespace ZHLN::Scene {

auto InstantiateFromTOML(Engine& engine, std::string_view tomlText) -> std::expected<Instance, Error> {
    auto description = ReflectTOML::TryParse<Scene>(tomlText);
    if (!description) {
        return std::unexpected(description.error());
    }
    return Instantiate(engine, *description);
}

} // namespace ZHLN::Scene
