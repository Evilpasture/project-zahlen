// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/World.cpp
#include "ArticulationSystem.hpp"
#include "CullingSystem.hpp"
#include "EngineGlobals.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/World.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/ecs/EntityCommandBuffer.hpp>
#include <Zahlen/ecs/SystemGraph.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <new>

namespace ZHLN {

// World bootstrap errors

enum class WorldInitError : uint8_t {
    PhysicsInitializationFailed ZHLN_ANNOTATION(ZHLN::Description<"Physics initialization failed"> {}) = 1,
    WorldAllocationFailed       ZHLN_ANNOTATION(ZHLN::Description<"World instance allocation failed"> {}),
};

struct World::Impl {
    // Member order encodes the teardown order (reverse of declaration): the
    // registry dies before the physics world, matching the original engine
    // sequence where Registry::Clear() ran before PhysicsContext destruction.
    std::unique_ptr<PhysicsContext> physicsContext;
    ECS::Registry                   registry;
    Camera                          mainCamera;

    std::unique_ptr<ECS::SystemGraph>         updateGraph;
    std::unique_ptr<ECS::SystemGraph>         renderGraph;
    std::unique_ptr<ECS::EntityCommandBuffer> mainECB;
    std::unique_ptr<CullingSystem>            cullingSystem;
    std::unique_ptr<ArticulationSystem>       articulationSystem;

    JPH::Array<Entity> visibleEntities;
    JPH::Array<Entity> visibleShadowEntities;

    bool joltAcquired = false;
};

auto World::Create(const PhysicsConfig& physicsConfig) -> std::expected<std::unique_ptr<World>, ErrorCode> {
    auto instance = std::unique_ptr<World>(new (std::nothrow) World());
    if (!instance) {
        return std::unexpected(WorldInitError::WorldAllocationFailed);
    }

    instance->_impl = std::make_unique<Impl>();
    auto& impl      = *instance->_impl;

    // Singleton InputStateComponent must exist before the first event pump
    // writes into it (the window callbacks target this very registry).
    impl.registry.Create(Components::InputStateComponent {});

    AcquireJoltRegistration();
    impl.joltAcquired = true;

    impl.physicsContext      = std::make_unique<PhysicsContext>(physicsConfig);
    impl.updateGraph         = std::make_unique<ECS::SystemGraph>();
    impl.renderGraph         = std::make_unique<ECS::SystemGraph>();
    impl.mainECB             = std::make_unique<ECS::EntityCommandBuffer>(impl.registry);
    impl.cullingSystem       = std::make_unique<CullingSystem>();
    impl.articulationSystem  = std::make_unique<ArticulationSystem>();

    return instance;
}

World::~World() {
    if (_impl == nullptr) {
        return;
    }

    // Reverse declaration order: visibility buffers, systems, command buffer,
    // graphs, camera, registry, then the physics world, then Jolt itself.
    _impl->visibleShadowEntities.clear();
    _impl->visibleEntities.clear();
    _impl->articulationSystem.reset();
    _impl->cullingSystem.reset();
    _impl->mainECB.reset();
    _impl->renderGraph.reset();
    _impl->updateGraph.reset();
    _impl->registry.Clear();
    _impl->physicsContext.reset();

    if (_impl->joltAcquired) {
        ReleaseJoltRegistration();
    }
}

auto World::GetRegistry() -> ECS::Registry& {
    return _impl->registry;
}
auto World::GetRegistry() const -> const ECS::Registry& {
    return _impl->registry;
}
auto World::GetPhysics() -> PhysicsContext& {
    return *_impl->physicsContext;
}
auto World::GetCamera() -> Camera& {
    return _impl->mainCamera;
}

auto World::GetUpdateGraph() -> ECS::SystemGraph& {
    return *_impl->updateGraph;
}
auto World::GetRenderGraph() -> ECS::SystemGraph& {
    return *_impl->renderGraph;
}
auto World::GetMainECB() -> ECS::EntityCommandBuffer& {
    return *_impl->mainECB;
}

auto World::GetCullingSystem() -> CullingSystem& {
    return *_impl->cullingSystem;
}
auto World::GetArticulationSystem() -> ArticulationSystem& {
    return *_impl->articulationSystem;
}

auto World::GetVisibleEntities() -> JPH::Array<Entity>& {
    return _impl->visibleEntities;
}
auto World::GetVisibleShadowEntities() -> JPH::Array<Entity>& {
    return _impl->visibleShadowEntities;
}

} // namespace ZHLN
