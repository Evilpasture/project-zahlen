// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <type_traits>
#include <utility>

namespace ZHLN::SceneResources {

template <typename T>
inline constexpr bool OwnsExternalResource =
    std::is_same_v<T, Components::PhysicsComponent> || std::is_same_v<T, Components::RagdollComponent> ||
    std::is_same_v<T, Components::AudioSourceComponent> || std::is_same_v<T, Components::LoopSynthComponent> ||
    std::is_same_v<T, Components::OwnedMeshComponent> || std::is_same_v<T, Components::ParticleEmitterComponent> ||
    std::is_same_v<T, Components::MeshParticleEmitterComponent> || std::is_same_v<T, Components::SkeletalMeshComponent>;

// Typed, explicit ownership operations for the components that hold external
// handles. These release the old resource before the registry erases or
// replaces its component; raw Registry::Remove/Add and ECB::AddComponent
// do not do so.
ZHLN_API void Release(PhysicsContext& physics, Components::PhysicsComponent& component);
ZHLN_API void Release(PhysicsContext& physics, Components::RagdollComponent& component);
ZHLN_API void Release(RenderContext& render, Components::OwnedMeshComponent& component);

ZHLN_API void Release(Engine& engine, Components::PhysicsComponent& component);
ZHLN_API void Release(Engine& engine, Components::RagdollComponent& component);
ZHLN_API void Release(Engine& engine, Components::AudioSourceComponent& component);
ZHLN_API void Release(Engine& engine, Components::LoopSynthComponent& component);
ZHLN_API void Release(Engine& engine, Components::OwnedMeshComponent& component);
ZHLN_API void Release(Engine& engine, Components::ParticleEmitterComponent& component);
ZHLN_API void Release(Engine& engine, Components::MeshParticleEmitterComponent& component);
ZHLN_API void Release(Engine& engine, Components::SkeletalMeshComponent& component);

template <typename T>
    requires requires(Engine& engine, T& component) { Release(engine, component); }
auto Detach(Engine& engine, Entity entity) -> bool {
    auto& registry = engine.GetRegistry();
    if (auto* component = registry.Get<T>(entity)) {
        Release(engine, *component);
        registry.Remove<T>(entity);
        return true;
    }
    return false;
}

template <typename T>
    requires requires(Engine& engine, T& component) { Release(engine, component); }
auto Attach(Engine& engine, Entity entity, T component) -> T& {
    Detach<T>(engine, entity);
    return engine.GetRegistry().Add(entity, std::move(component));
}

// Standalone Registry scenes can use the same explicit operations when they
// own their PhysicsContext or RenderContext independently of an Engine.
template <typename T, typename Owner>
    requires requires(Owner& owner, T& component) { Release(owner, component); }
auto Detach(Owner& owner, ECS::Registry& registry, Entity entity) -> bool {
    if (auto* component = registry.Get<T>(entity)) {
        Release(owner, *component);
        registry.Remove<T>(entity);
        return true;
    }
    return false;
}

template <typename T, typename Owner>
    requires requires(Owner& owner, T& component) { Release(owner, component); }
auto Attach(Owner& owner, ECS::Registry& registry, Entity entity, T component) -> T& {
    Detach<T>(owner, registry, entity);
    return registry.Add(entity, std::move(component));
}

} // namespace ZHLN::SceneResources
