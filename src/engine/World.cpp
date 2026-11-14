// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ArticulationSystem.hpp"
#include "Hierarchy.hpp"
#include "EngineGlobals.hpp"
#include <Zahlen/Core/Profiler.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Engine/EngineServices.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Engine/World.hpp>
#include <Zahlen/Engine/VisibilityList.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <Zahlen/ECS/EntityCommandBuffer.hpp>
#include <Zahlen/ECS/EventBus.hpp>
#include <Zahlen/ECS/SystemGraph.hpp>
#include <Zahlen/Physics/Physics.hpp>
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
    ECS::EventBus                   eventBus;

    std::unique_ptr<ECS::EntityCommandBuffer> mainECB;
    std::unique_ptr<ArticulationSystem>       articulationSystem;

    VisibilityList visibleEntities;
    VisibilityList visibleShadowEntities;
    CullingStats   cullingStats {};
    DebugSettings  debugSettings {};

    bool joltAcquired = false;
};

auto World::Create(const PhysicsConfig& physicsConfig, bool deferECBDestroy) -> std::expected<std::unique_ptr<World>, ErrorCode> {
    auto instance = std::unique_ptr<World>(new (std::nothrow) World());
    if (!instance) {
        return std::unexpected(WorldInitError::WorldAllocationFailed);
    }

    instance->_impl = std::make_unique<Impl>();
    auto& impl      = *instance->_impl;

    impl.registry.RegisterAllComponentsIn<Components>();

    AcquireJoltRegistration();
    impl.joltAcquired = true;

    impl.physicsContext      = std::make_unique<PhysicsContext>(physicsConfig);
    impl.mainECB             = std::make_unique<ECS::EntityCommandBuffer>(impl.registry, deferECBDestroy ? &MarkPendingDestroy : nullptr);
    impl.articulationSystem  = std::make_unique<ArticulationSystem>();

    return instance;
}

World::~World() {
    if (_impl == nullptr) {
        return;
    }

    _impl->visibleShadowEntities.Clear();
    _impl->visibleEntities.Clear();
    _impl->articulationSystem.reset();
    _impl->mainECB.reset();

    // A standalone World can still contain physics owners. Release the bulk
    // handles before clearing their components and tearing down Jolt.
    auto& registry = _impl->registry;
    if (!registry.GetEntitiesWith<Components::RagdollComponent>().empty()) {
        std::vector<Physics::RagdollID> ragdolls;
        for (auto& component: registry.GetRawArray<Components::RagdollComponent>()) {
            ragdolls.push_back(std::exchange(component.ragdollID, Physics::RagdollID::Invalid));
        }
        _impl->physicsContext->DestroyRagdolls(ragdolls);
    }
    if (!registry.GetEntitiesWith<Components::PhysicsComponent>().empty()) {
        std::vector<Physics::BodyID> bodies;
        for (auto& component: registry.GetRawArray<Components::PhysicsComponent>()) {
            bodies.push_back(std::exchange(component.physicsID, Physics::BodyID::Null()));
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
auto World::GetMainECB() -> ECS::EntityCommandBuffer& {
    return *_impl->mainECB;
}
auto World::GetEventBus() noexcept -> ECS::EventBus& {
    return _impl->eventBus;
}
auto World::GetEventBus() const noexcept -> const ECS::EventBus& {
    return _impl->eventBus;
}

auto World::GetArticulationSystem() -> ArticulationSystem& {
    return *_impl->articulationSystem;
}

auto World::GetCullingStats() noexcept -> CullingStats& {
    return _impl->cullingStats;
}

auto World::GetCullingStats() const noexcept -> const CullingStats& {
    return _impl->cullingStats;
}

auto World::GetDebugSettings() noexcept -> DebugSettings& {
    return _impl->debugSettings;
}

auto World::GetDebugSettings() const noexcept -> const DebugSettings& {
    return _impl->debugSettings;
}

auto World::VisibleEntitiesForServices() noexcept -> VisibilityList& {
    return _impl->visibleEntities;
}
auto World::VisibleShadowEntitiesForServices() noexcept -> VisibilityList& {
    return _impl->visibleShadowEntities;
}

auto World::GetVisibleEntities() const noexcept -> std::span<const Entity> {
    return _impl->visibleEntities.AsSpan();
}
auto World::GetVisibleShadowEntities() const noexcept -> std::span<const Entity> {
    return _impl->visibleShadowEntities.AsSpan();
}

}
