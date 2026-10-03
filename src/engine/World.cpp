// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ArticulationSystem.hpp"
#include "Hierarchy.hpp"
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
#include <utility>
#include <vector>

namespace ZHLN {


enum class WorldInitError : uint8_t {
    PhysicsInitializationFailed ZHLN_ANNOTATION(ZHLN::Description<"Physics initialization failed"> {}) = 1,
    WorldAllocationFailed       ZHLN_ANNOTATION(ZHLN::Description<"World instance allocation failed"> {}),
};

struct World::Impl {
    std::unique_ptr<PhysicsContext> physicsContext;
    ECS::Registry                   registry;
    Camera                          mainCamera;

    std::unique_ptr<ECS::EntityCommandBuffer> mainECB;
    std::unique_ptr<CullingSystem>            cullingSystem;
    std::unique_ptr<ArticulationSystem>       articulationSystem;

    JPH::Array<Entity> visibleEntities;
    JPH::Array<Entity> visibleShadowEntities;

    bool joltAcquired = false;
};

auto World::Create(const PhysicsConfig& physicsConfig, bool deferECBDestroy) -> std::expected<std::unique_ptr<World>, ErrorCode> {
    auto instance = std::unique_ptr<World>(new (std::nothrow) World());
    if (!instance) {
        return std::unexpected(WorldInitError::WorldAllocationFailed);
    }

    instance->_impl = std::make_unique<Impl>();
    auto& impl      = *instance->_impl;

    impl.registry.Create(Components::InputStateComponent {});

    AcquireJoltRegistration();
    impl.joltAcquired = true;

    impl.physicsContext      = std::make_unique<PhysicsContext>(physicsConfig);
    impl.mainECB             = std::make_unique<ECS::EntityCommandBuffer>(impl.registry, deferECBDestroy ? &MarkPendingDestroy : nullptr);
    impl.cullingSystem       = std::make_unique<CullingSystem>();
    impl.articulationSystem  = std::make_unique<ArticulationSystem>();

    return instance;
}

World::~World() {
    if (_impl == nullptr) {
        return;
    }

    _impl->visibleShadowEntities.clear();
    _impl->visibleEntities.clear();
    _impl->articulationSystem.reset();
    _impl->cullingSystem.reset();
    _impl->mainECB.reset();

    // A standalone World can still contain physics owners. Release the bulk
    // handles before clearing their components and tearing down Jolt.
    auto& registry = _impl->registry;
    if (!registry.GetEntitiesWith<Components::RagdollComponent>().empty()) {
        std::vector<Physics::RagdollHandle> ragdolls;
        for (auto& component: registry.GetRawArray<Components::RagdollComponent>()) {
            ragdolls.push_back(std::exchange(component.ragdollHandle, Physics::RagdollHandle::Invalid));
        }
        _impl->physicsContext->DestroyRagdolls(ragdolls);
    }
    if (!registry.GetEntitiesWith<Components::PhysicsComponent>().empty()) {
        std::vector<Physics::BodyHandle> bodies;
        for (auto& component: registry.GetRawArray<Components::PhysicsComponent>()) {
            bodies.push_back(std::exchange(component.physicsHandle, Physics::BodyHandle::Null()));
        }
        _impl->physicsContext->DestroyBodies(bodies);
    }
    registry.Clear();
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

auto World::GetMainECB() -> ECS::EntityCommandBuffer& {
    return *_impl->mainECB;
}

auto World::GetCullingSystem() -> CullingSystem& {
    return *_impl->cullingSystem;
}
auto World::GetArticulationSystem() -> ArticulationSystem& {
    return *_impl->articulationSystem;
}

auto World::GetCullingStats() -> CullingStats& {
    return _impl->cullingSystem->Stats();
}

auto World::GetVisibleEntities() -> JPH::Array<Entity>& {
    return _impl->visibleEntities;
}
auto World::GetVisibleShadowEntities() -> JPH::Array<Entity>& {
    return _impl->visibleShadowEntities;
}

}
