// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#include <print>
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
        auto parsedDoc = ReflectTOML::ParseWithDiagnostics<Scene::Document>(text);
        if (!parsedDoc) {
            const auto& failure = parsedDoc.error();
            if (!failure.detail.empty()) {
                if (failure.line > 0) {
                    std::println(stderr, "[SceneDocument] '{}' line {}: {} ({})", path.string(), failure.line, failure.detail, failure.code);
                } else {
                    // Schema/type mismatch from ParseObject: no line, but
                    // the field path and reason are in detail.
                    std::println(stderr, "[SceneDocument] '{}': {} ({})", path.string(), failure.detail, failure.code);
                }
            }
            return std::unexpected(failure.code);
        }
        return std::move(*parsedDoc);
    }
#if defined(ZHLN_SCENE_DOC_HAS_LUA)
    if (ext == ".lua") {
        std::string detail;
        auto        doc = ReflectLua::EvaluateFile<Scene::Document>(path, {}, &detail);
        if (!doc && !detail.empty()) {
            std::println(stderr, "[SceneDocument] '{}': {}", path.string(), detail);
        }
        return doc;
    }
#endif
    return std::unexpected(SceneDocumentError::UnsupportedFormat);
}
auto LoadSceneDocument(COM::IEngine& engine, const std::filesystem::path& path, Scene::InstantiateOptions options) -> bool {
    const auto parsed = ReadSceneDocument(path);
    if (!parsed) {
        ZHLN::Log<ZHLN::LogTag::Error>("document '{}': {}", path.string(), parsed.error());
        return false;
    }
    if (const auto valid = Scene::Validate(*engine.Assets(), *parsed); !valid) {
        ZHLN::Log<ZHLN::LogTag::Error>("document dependencies '{}': {}", path.string(), valid.error());
        return false;
    }
    engine.ClearScene();
    const auto instance = Scene::Instantiate(engine, *parsed, options);
    if (!instance) {
        engine.ClearScene();
        ZHLN::Log<ZHLN::LogTag::Error>("instantiate '{}': {}", path.string(), instance.error());
        return false;
    }
    return true;
}
}
