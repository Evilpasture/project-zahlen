// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ArticulationSystem.hpp"
#include "Hierarchy.hpp"
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

    std::unique_ptr<ECS::EntityCommandBuffer> mainECB;
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

    // The culling counters are world data (see Components::CullingStatsComponent), so
    // the world owns them from the moment it exists rather than from whenever a scene
    // happens to be initialized. Scene resets clear the registry and re-seed them in
    // InitializeDefaultScene; GetCullingStats() is total either way.
    impl.registry.Create(Components::CullingStatsComponent {});

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

    _impl->visibleShadowEntities.clear();
    _impl->visibleEntities.clear();
    _impl->articulationSystem.reset();
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
    // The camera is world data: the main camera entity carries it. Same contract
    // the bundle used to state -- a world asked for a camera has one -- but
    // resolved from the registry instead of being bound into a service slot.
    //
    // Unlike the culling counters, this one keeps its assertion: "a scene with no
    // main camera yet" is a state a caller can be in legitimately, and RenderSystem
    // reports it as NoMainCamera before it gets here -- so asking for a Camera& is
    // an opt-in to the contract, not an assumption about someone else's timing.
    const Entity cameraEntity = _impl->registry.SingletonEntity<Components::MainCameraTagComponent>();
    auto         camera       = _impl->registry.Get<Components::CameraComponent>(cameraEntity);
    ZHLN::Assert(camera.has_value(), "World::GetCamera(): no main camera entity carrying a CameraComponent");
    return camera->camera;
}

auto World::GetMainECB() -> ECS::EntityCommandBuffer& {
    return *_impl->mainECB;
}

auto World::GetArticulationSystem() -> ArticulationSystem& {
    return *_impl->articulationSystem;
}

auto World::GetCullingStats() -> CullingStats& {
    // The culling pass publishes here: its counters are read by the overlay, the
    // crash dump and the render tests, so they are world data. The rest of the
    // culler is the pass's own state and stays on the pass.
    //
    // Total on purpose. World::Create() creates the singleton and a scene reset
    // re-seeds it in InitializeDefaultScene, but neither is a precondition a caller
    // can check: this used to assert on the missing singleton, and ZHLN::Assert is
    // only a panic in dev builds -- in a ship build it is [[assume(false)]] and the
    // dereference below it is undefined behaviour. Creating it on first access keeps
    // the promise the return type makes. A zeroed CullingStats is the honest value
    // for "no cull has run yet"; it is the same value the render tests assign to
    // reset the counters.
    return _impl->registry.GetOrEmplaceSingleton<Components::CullingStatsComponent>().stats;
}

auto World::GetVisibleEntities() -> JPH::Array<Entity>& {
    return _impl->visibleEntities;
}
auto World::GetVisibleShadowEntities() -> JPH::Array<Entity>& {
    return _impl->visibleShadowEntities;
}

}
