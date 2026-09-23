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

namespace ECS {
class Registry;
class SystemGraph;
class EntityCommandBuffer;
} // namespace ECS

// One simulation instance: the ECS registry, the physics world, the main
// camera, the visibility/rig systems and the hazard-analysed update/render
// graphs.
//
// A World owns no hardware. It can be stepped headless for logic/physics
// tests, and several worlds can share one Kernel (GPU, windows, audio) --
// e.g. a game world beside an editor preview. Engine composes the default
// pair; see Engine.hpp.
class ZHLN_API World {
  public:
    // Acquires the process-wide Jolt registration and builds the physics
    // context, the registry (including the input-state singleton the event
    // pump writes into) and the empty system graphs.
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

    auto GetVisibleEntities() -> JPH::Array<Entity>&;
    auto GetVisibleShadowEntities() -> JPH::Array<Entity>&;

  private:
    World() = default;

    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace ZHLN
