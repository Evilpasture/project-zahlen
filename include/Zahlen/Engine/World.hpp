// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Math/Camera.hpp>
#include <Zahlen/Core/Optional.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/Config.hpp>
#include <Zahlen/ECS/Entity.hpp>
#include <Zahlen/Error.hpp>
#include <expected>
#include <memory>
#include <span>

namespace ZHLN {

class VisibilityList;
class PhysicsContext;
class InputContext;
class ArticulationSystem;
struct CullingStats;

namespace ECS {
class Registry;
template <typename Services>
class SystemGraph;
class EntityCommandBuffer;
class EventBus;
}

class ZHLN_API World {
  public:
    // Engine-owned Worlds opt into marking ECB destruction for scene cleanup;
    // standalone data-only Worlds retain immediate ECB playback by default.
    static auto Create(const PhysicsConfig& physicsConfig, bool deferECBDestroy = false) -> std::expected<std::unique_ptr<World>, ErrorCode>;
    ~World();

    World(const World&)                    = delete;
    auto operator=(const World&) -> World& = delete;

    auto GetPhysics() -> PhysicsContext&;
    // No camera accessor here. The main camera is a component on the entity
    // carrying MainCameraTagComponent. Engine callers resolve it through the
    // entity capability; scheduled systems declare the components they read.

    auto GetMainECB() -> ECS::EntityCommandBuffer&;
    auto GetEventBus() noexcept -> ECS::EventBus&;
    [[nodiscard]] auto GetEventBus() const noexcept -> const ECS::EventBus&;
    auto GetInput() noexcept -> InputContext&;
    [[nodiscard]] auto GetInput() const noexcept -> const InputContext&;

    auto GetArticulationSystem() -> ArticulationSystem&;
    auto GetCullingStats() noexcept -> CullingStats&;
    [[nodiscard]] auto GetCullingStats() const noexcept -> const CullingStats&;

    // Read-only views onto the engine-owned visibility lists. No container type
    // crosses this boundary, and no caller can clear or resize engine state.
    [[nodiscard]] auto GetVisibleEntities() const noexcept -> std::span<const Entity>;
    [[nodiscard]] auto GetVisibleShadowEntities() const noexcept -> std::span<const Entity>;

  private:
    // Engine is the only owner of the render services, and the only code that
    // needs the *writable* producer. These are deliberately not public: a
    // caller gets a std::span<const Entity>, never a container to mutate.
    friend class Engine;
    auto GetRegistry() -> ECS::Registry&;
    [[nodiscard]] auto GetRegistry() const -> const ECS::Registry&;
    auto VisibleEntitiesForServices() noexcept -> VisibilityList&;
    auto VisibleShadowEntitiesForServices() noexcept -> VisibilityList&;

    World() = default;

    struct Impl;
    std::unique_ptr<Impl> _impl;
};

}
