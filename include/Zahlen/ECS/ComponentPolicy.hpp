// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Zahlen/ECS/Components.hpp>
#include <type_traits>
namespace ZHLN::SceneResources {
// Shared data policy: replacing these owners requires engine-side retirement.
// Hosts may inspect them, but reflective edits must not overwrite handles.
template <typename T>
inline constexpr bool OwnsExternalResource = std::is_same_v<T, Components::PhysicsComponent> || std::is_same_v<T, Components::RagdollComponent> ||
                                             std::is_same_v<T, Components::AudioSourceComponent> || std::is_same_v<T, Components::OwnedMeshComponent> ||
                                             std::is_same_v<T, Components::SkeletalMeshComponent>;
template <typename T>
inline constexpr bool ReadOnlyReflective =
    OwnsExternalResource<T> || std::is_same_v<T, Components::AnimatorComponent> || std::is_same_v<T, Components::CharacterMotorStateComponent>;
} // namespace ZHLN::SceneResources
