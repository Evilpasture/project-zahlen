// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// gameplay/FallbackScene/FallbackScene.hpp
//
// The compiled-in diagnostic scene for a tool host with no scene document.
// Moved out of core: the scene content (emblem, ground, orbit light), the popup
// are application policy, not engine substrate. Core keeps the pieces
// this composes -- the Scene::Instantiate path, the frame/graph extension
// seams, the enableFallbackScene config flag and the teardown-hook list --
// and a host that wants none of it simply never calls Install.
#pragma once

#include <Zahlen/Entity.hpp>
#include <Zahlen/GameModule.hpp>

namespace ZHLN {

class Engine;

namespace Scene {
struct Scene;
} // namespace Scene

namespace FallbackScene {

// Compiled-in diagnostic scene for the scene host's no-document mode.
// Hosts that do not want it simply do not install this module.
class DefaultPreset {
  public:
    static void BuildFallbackScene(Engine& engine);

    // The fallback scene as a scene description -- the same data
    // BuildFallbackScene instantiates. Exposed so it can be inspected and
    // checked without a device.
    //
    // It is a compiled-in ZHLN::Scene::Scene, not a baked-in document: the
    // scene host displays it when no document was requested. It does not
    // require a text parser just to show this diagnostic scene.
    [[nodiscard]] static auto FallbackScene() noexcept -> const Scene::Scene&;
    static void               Update(Engine& engine, float dt);
    [[nodiscard]] static bool IsActive() noexcept;
    static void               ClearFallback() noexcept;

    // Drops the fallback state if @p engine is the engine that built it.
    //
    // The preset keeps entity handles (the emblem, the orbit light, the UI
    // widgets) in process-global storage, and nothing used to clear them when
    // an engine died: the next engine in the process inherited s_IsActive
    // together with handles naming entities in a registry that no longer
    // exists. Entity indices are handed out deterministically, so those
    // handles resolve against the new registry and the preset animates
    // whatever entity happens to sit at the same slot. Install subscribes a
    // teardown hook that calls this; the state is owner-scoped.
    static void ReleaseFor(const Engine* engine) noexcept;

  private:
    static inline bool           s_IsActive       = false;
    static inline const Engine*  s_Owner          = nullptr;

    // 3D Scene Entities
    static inline Entity s_CubeEntity = Entity::Null();
    static inline Entity s_PointLight = Entity::Null();

    // Native ECS UI Entities
    static inline Entity s_UIPopupBox = Entity::Null();
    static inline Entity s_BtnReload  = Entity::Null();
    static inline Entity s_BtnAnimate = Entity::Null();
    static inline Entity s_BtnQuit    = Entity::Null();

    static inline float s_AccumTime    = 0.0f;
    static inline bool  s_AnimateScene = true;
    static inline bool  s_PopupVisible = true;
};

// Composition-root entry point: installs the module Describe() returns, whose
// step list places "DefaultPreset" before the simulation graph and whose teardown hook releases
// the preset's parked entity handles. Without Install the frame never engages
// fallback detection and a boot failure leaves the empty default scene on
// screen.
// The module as a value: what the host installs, and what a host that wants to
// report a placement failure installs itself instead of calling Install.
auto Describe() -> Module;

void Install(Engine& engine);

} // namespace FallbackScene
} // namespace ZHLN
