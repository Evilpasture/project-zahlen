// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Scene/Scene.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/Core/COM/Ref.hpp>
#include <Zahlen/ECS/Entity.hpp>
namespace ZHLN {
class HostUI;
namespace FallbackScene {
// Host-owned scene/popup state; no globals, module, registry or teardown hook.
// The host explicitly drives Update and DrawUI and clears it before retiring
// its process/task scope. Geometry remains engine scene-owned, not preset-owned.
class DefaultPreset {
  public:
    DefaultPreset()                                                                 = default;
    DefaultPreset(const DefaultPreset&)                                             = delete;
    auto operator=(const DefaultPreset&) -> DefaultPreset&                          = delete;
    DefaultPreset(DefaultPreset&&) noexcept                                         = default;
    auto                      operator=(DefaultPreset&&) noexcept -> DefaultPreset& = default;
    void                      BuildFallbackScene(COM::IEngine&, Scene::InstantiateOptions options = {});
    [[nodiscard]] static auto FallbackScene() noexcept -> const Scene::Document&;
    void                      Update(COM::IEngine&, float dt);
    [[nodiscard]] auto        DrawUI(COM::IEngine&, HostUI&, float dt) -> COM::Result<void>;
    [[nodiscard]] bool        IsActive() const noexcept;
    void                      ClearFallback() noexcept;

  private:
    bool                   _active = false;
    COM::Ref<COM::IEngine> _owner;
    Entity                 _emblem     = Entity::Null();
    Entity                 _pointLight = Entity::Null();
    float                  _time       = 0;
    bool                   _animate    = true;
    bool                   _popup      = true;
};
} // namespace FallbackScene
} // namespace ZHLN
