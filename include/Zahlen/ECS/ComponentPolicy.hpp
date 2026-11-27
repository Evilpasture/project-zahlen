// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Zahlen/ECS/Components.hpp>
#include <type_traits>
namespace ZHLN::SceneResources {
template <typename T>
inline constexpr bool IsPhysicsBodyComponent =
    std::is_same_v<T, Components::StaticBodyComponent> ||
    std::is_same_v<T, Components::KinematicBodyComponent> ||
    std::is_same_v<T, Components::DynamicBodyComponent> ||
    std::is_same_v<T, Components::CharacterBodyComponent>;

// Shared data policy: replacing these owners requires engine-side retirement.
// Hosts may inspect them, but reflective edits must not overwrite handles.
template <typename T>
inline constexpr bool OwnsExternalResource = IsPhysicsBodyComponent<T> || std::is_same_v<T, Components::RagdollComponent> ||
                                             std::is_same_v<T, Components::AudioSourceComponent> || std::is_same_v<T, Components::OwnedMeshComponent> ||
                                             std::is_same_v<T, Components::SkeletalMeshComponent>;
template <typename T>
inline constexpr bool ReadOnlyReflective = OwnsExternalResource<T> || std::is_same_v<T, Components::CharacterMotorStateComponent>;
} // namespace ZHLN::SceneResources

namespace ZHLN {
template <typename Reader>
[[nodiscard]] constexpr auto EntityPhysicsBodyID(const Reader& reader, Entity entity) noexcept -> Physics::BodyID {
    if (const auto c = reader.template Get<Components::DynamicBodyComponent>(entity)) return c->bodyID;
    if (const auto c = reader.template Get<Components::StaticBodyComponent>(entity)) return c->bodyID;
    if (const auto c = reader.template Get<Components::KinematicBodyComponent>(entity)) return c->bodyID;
    if (const auto c = reader.template Get<Components::CharacterBodyComponent>(entity)) return c->bodyID;
    return Physics::BodyID::Null();
}

template <typename Reader>
[[nodiscard]] constexpr auto HasPhysicsBodyComponent(const Reader& reader, Entity entity) noexcept -> bool {
    return static_cast<bool>(reader.template Get<Components::DynamicBodyComponent>(entity)) ||
           static_cast<bool>(reader.template Get<Components::StaticBodyComponent>(entity)) ||
           static_cast<bool>(reader.template Get<Components::KinematicBodyComponent>(entity)) ||
           static_cast<bool>(reader.template Get<Components::CharacterBodyComponent>(entity));
}
} // namespace ZHLN
