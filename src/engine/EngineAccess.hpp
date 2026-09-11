// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Types.hpp>
#include <optional>

namespace ZHLN {

class Engine;
class NativeScriptModule;
struct EngineConfig;
struct FontAtlas;

/// Narrow friend of Engine: frame steps and scene setup may read engine-owned
/// module/config/atlas state without putting those types on the public API.
class EngineFrameStepAccess {
  public:
    [[nodiscard]] static auto NativeGameplayModule(Engine& engine) -> NativeScriptModule&;
    [[nodiscard]] static auto Config(Engine& engine) -> const EngineConfig&;
    [[nodiscard]] static auto PersistentFontAtlas(Engine& engine) -> std::optional<FontAtlas>&;
};

} // namespace ZHLN
