// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Scene/Scene.hpp>
#include <expected>
#include <filesystem>
namespace ZHLN::Tools {
enum class SceneDocumentError : uint8_t { ReadFailed = 1, UnsupportedFormat };
// Pure parsing: no engine, scene clear, imports or GPU allocation. Hosts can
// prepare every prefab source through their chosen format capability first.
[[nodiscard]] auto ReadSceneDocument(const std::filesystem::path&) -> std::expected<Scene::Document, ErrorCode>;
// Explicit replacement convenience. Assets must already be host-prepared.
[[nodiscard]] auto LoadSceneDocument(COM::IEngine&, const std::filesystem::path&, Scene::InstantiateOptions options = {}) -> bool;
}
