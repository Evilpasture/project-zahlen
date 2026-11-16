// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Transitional native callers only. Not a dependency of zahlen_scene.
#include "Scene.hpp"
namespace ZHLN {
class Engine;
class Entities;
} // namespace ZHLN
namespace ZHLN::Scene {
[[nodiscard]] auto Instantiate(Engine&, const Scene&) -> std::expected<Instance, ErrorCode>;
[[nodiscard]] auto Extract(Engine&) -> Scene;
[[nodiscard]] auto Extract(const Camera&, const GraphicsSettings&, const Entities&, MaterialLookup = {}) -> Scene;
} // namespace ZHLN::Scene
