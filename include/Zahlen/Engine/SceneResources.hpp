// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/Engine/Capabilities/GraphicsDevice.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <type_traits>
#include <utility>

namespace ZHLN::SceneResources {

template <typename T>
inline constexpr bool OwnsExternalResource =
    std::is_same_v<T, Components::PhysicsComponent> || std::is_same_v<T, Components::RagdollComponent> ||
    std::is_same_v<T, Components::AudioSourceComponent> ||
    std::is_same_v<T, Components::OwnedMeshComponent> || std::is_same_v<T, Components::SkeletalMeshComponent>;

// Typed, explicit ownership operations for the components that hold external
// handles. These release the old resource before the registry erases or
// replaces its component; raw Registry::Remove/Add and ECB::AddComponent
// do not do so.
ZHLN_API void Release(PhysicsContext& physics, Components::PhysicsComponent& component);
ZHLN_API void Release(PhysicsContext& physics, Components::RagdollComponent& component);
ZHLN_API void Release(AudioContext& audio, Components::AudioSourceComponent& component);
ZHLN_API void Release(RenderContext& render, Components::OwnedMeshComponent& component);
ZHLN_API void Release(GraphicsDevice graphics, Components::OwnedMeshComponent& component);

ZHLN_API void Release(Engine& engine, Components::PhysicsComponent& component);
ZHLN_API void Release(Engine& engine, Components::RagdollComponent& component);
ZHLN_API void Release(Engine& engine, Components::AudioSourceComponent& component);
ZHLN_API void Release(Engine& engine, Components::OwnedMeshComponent& component);
ZHLN_API void Release(Engine& engine, Components::SkeletalMeshComponent& component);

// Engine-owned scenes go through Engine::Entities(), whose Attach/Detach call
// the overloads above with the engine as the release owner. There is no
// Engine& overload here: a public header must not reach the internal registry
// seam, and routing replacement through this namespace's Release is the only
// way a Jolt body, audio voice or GPU buffer cannot be orphaned.

// Standalone Registry scenes can use the same explicit operations when they
// own their PhysicsContext or RenderContext independently of an Engine.
template <typename T, typename Owner>
    requires requires(Owner& owner, T& component) { Release(owner, component); }
auto Detach(Owner& owner, ECS::Registry& registry, Entity entity) -> bool {
    if (auto component = registry.Get<T>(entity)) {
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
