// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#include "NativeSceneDocument.hpp"
#include "SceneDocument.hpp"
#include <Scene/NativeScene.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/Log.hpp>
namespace ZHLN::Tools {
auto LoadSceneDocument(Engine& engine, const std::filesystem::path& path) -> bool {
    const auto parsed = ReadSceneDocument(path);
    if (!parsed) {
        LogError("[Scene] document '{}': {}", path.string(), parsed.error());
        return false;
    }
    engine.ClearScene();
    const auto instance = Scene::Instantiate(engine, *parsed);
    if (!instance) {
        engine.ClearScene();
        LogError("[Scene] instantiate '{}': {}", path.string(), instance.error());
        return false;
    }
    return true;
}
} // namespace ZHLN::Tools
