// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Jolt/Jolt.h>
#include <Zahlen/Camera.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/Config.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Error.hpp>
#include <expected>
#include <memory>

namespace ZHLN {

class PhysicsContext;
class CullingSystem;
class ArticulationSystem;
struct CullingStats;

namespace ECS {
class Registry;
class SystemGraph;
class EntityCommandBuffer;
}

class ZHLN_API World {
  public:
    static auto Create(const PhysicsConfig& physicsConfig) -> std::expected<std::unique_ptr<World>, ErrorCode>;
    ~World();

    World(const World&)                    = delete;
    auto operator=(const World&) -> World& = delete;

    auto GetRegistry() -> ECS::Registry&;
    [[nodiscard]] auto GetRegistry() const -> const ECS::Registry&;
    auto GetPhysics() -> PhysicsContext&;
    auto GetCamera() -> Camera&;

    auto GetUpdateGraph() -> ECS::SystemGraph&;
    auto GetRenderGraph() -> ECS::SystemGraph&;
    auto GetMainECB() -> ECS::EntityCommandBuffer&;

    auto GetCullingSystem() -> CullingSystem&;
    auto GetArticulationSystem() -> ArticulationSystem&;
    auto GetCullingStats() -> CullingStats&;

    auto GetVisibleEntities() -> JPH::Array<Entity>&;
    auto GetVisibleShadowEntities() -> JPH::Array<Entity>&;

  private:
    World() = default;

    struct Impl;
    std::unique_ptr<Impl> _impl;
};

}
