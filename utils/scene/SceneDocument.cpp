// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#include "SceneDocument.hpp"
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/Log.hpp>
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
auto ReadSceneDocument(const std::filesystem::path& path) -> std::expected<Scene::Document, ErrorCode> {
    const auto ext = path.extension().string();
    if (ext == ".toml") {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return std::unexpected(SceneDocumentError::ReadFailed);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (in.bad())
            return std::unexpected(SceneDocumentError::ReadFailed);
        return ReflectTOML::Parse<Scene::Document>(text);
    }
#if defined(ZHLN_SCENE_DOC_HAS_LUA)
    if (ext == ".lua")
        return ReflectLua::EvaluateFile<Scene::Document>(path);
#endif
    return std::unexpected(SceneDocumentError::UnsupportedFormat);
}
auto LoadSceneDocument(COM::IEngine& engine, const std::filesystem::path& path, Scene::InstantiateOptions options) -> bool {
    const auto parsed = ReadSceneDocument(path);
    if (!parsed) {
        LogError("[Scene] document '{}': {}", path.string(), parsed.error());
        return false;
    }
    if (const auto valid = Scene::Validate(*engine.Assets(), *parsed); !valid) {
        LogError("[Scene] document dependencies '{}': {}", path.string(), valid.error());
        return false;
    }
    engine.ClearScene();
    const auto instance = Scene::Instantiate(engine, *parsed, options);
    if (!instance) {
        engine.ClearScene();
        LogError("[Scene] instantiate '{}': {}", path.string(), instance.error());
        return false;
    }
    return true;
}
}
