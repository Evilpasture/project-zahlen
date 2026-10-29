// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "SceneDocument.hpp"
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Scene.hpp>
#include <toml/SceneTOML.hpp>
#include <toml/TOML.hpp>
#if defined(ZHLN_SCENE_DOC_HAS_LUA)
#include <lua/ReflectLua.hpp>
#include <lua/SceneLua.hpp>
#endif
#include <fstream>
#include <iterator>
#include <string>

namespace ZHLN::Tools {

[[nodiscard]] auto LoadSceneDocument(Engine& engine, const std::filesystem::path& path) -> bool {
    const std::string ext = path.extension().string();
    if (ext == ".toml") {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            LogError("[Scene] could not open TOML scene '{}'", path.string());
            return false;
        }
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto parsed = ReflectTOML::Parse<Scene::Scene>(text);
        if (!parsed) {
            LogError("[Scene] failed to parse TOML scene '{}': {}", path.string(), parsed.error());
            return false;
        }
        engine.ClearScene();
        // Scene cleanup removes the camera and settings singletons. Rebuild
        // them and their per-scene graphs before instantiating the document.
        if (!engine.InitializeDefaultScene()) {
            LogError("[Scene] could not initialize scene before '{}'", path.string());
            return false;
        }
        auto inst = Scene::Instantiate(engine, *parsed);
        if (!inst) {
            LogError("[Scene] failed to instantiate TOML scene '{}': {}", path.string(), inst.error());
            return false;
        }
        Log("[Scene] instantiated '{}' ({} entities, {} lights)", path.string(), inst->entities.size(), inst->lights.size());
        return true;
    }
#if defined(ZHLN_SCENE_DOC_HAS_LUA)
    if (ext == ".lua") {
        auto parsed = ReflectLua::EvaluateFile<Scene::Scene>(path);
        if (!parsed) {
            LogError("[Scene] failed to evaluate Lua scene '{}': {}", path.string(), parsed.error());
            return false;
        }
        engine.ClearScene();
        // Scene cleanup removes the camera and settings singletons. Rebuild
        // them and their per-scene graphs before instantiating the document.
        if (!engine.InitializeDefaultScene()) {
            LogError("[Scene] could not initialize scene before '{}'", path.string());
            return false;
        }
        auto inst = Scene::Instantiate(engine, *parsed);
        if (!inst) {
            LogError("[Scene] failed to instantiate Lua scene '{}': {}", path.string(), inst.error());
            return false;
        }
        Log("[Scene] instantiated '{}' ({} entities, {} lights)", path.string(), inst->entities.size(), inst->lights.size());
        return true;
    }
#endif
    LogError("[Scene] unsupported or disabled scene format for '{}'", path.string());
    return false;
}

} // namespace ZHLN::Tools
