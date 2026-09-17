// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extras/Interaction/InteractionComponents.hpp
#pragma once

#include <Zahlen/Core/String.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Types.hpp> // EnableEnumFlags + the flag operators
#include <array>
#include <cstddef>
#include <cstdint>

namespace ZHLN::Interaction {

/// Identity of a carryable item. `id` is the gameplay hash scripts and the
/// pickup log line refer to; `icon` names the inventory icon asset.
struct ItemBaseComponent {
    String64 name;
    uint32_t id = 0;
    String64 icon;
};

/// Marks an item entity as picked up. InteractionSystem sets it when the item
/// enters a container; rendering/physics are stripped off at the same moment.
struct PickupComponent {
    uint32_t isPickedUp = 0;
};

/// An interactable that dispatches a script by hash when used.
struct UsableComponent {
    uint64_t scriptHash = 0;
};

/// Fixed-capacity inventory. The slot count is part of the gameplay rules
/// (16 slots), not engine substrate.
struct ContainerComponent {
    static constexpr size_t       MAX_SLOTS = 16;
    std::array<Entity, MAX_SLOTS> slots     = {Entity::Null(), Entity::Null(), Entity::Null(), Entity::Null(), Entity::Null(), Entity::Null(), Entity::Null(), Entity::Null(),
                                               Entity::Null(), Entity::Null(), Entity::Null(), Entity::Null(), Entity::Null(), Entity::Null(), Entity::Null(), Entity::Null()};
    uint32_t                      count     = 0;
    uint32_t                      _padding  = 0;
};

/// Proximity gate: InteractionSystem measures player distance against `radius`
/// and tracks presence through `flags`. `Active` gates the trigger, and
/// `PlayerInside` is the presence edge the system maintains; the remaining bits
/// are gameplay configuration.
enum class TriggerFlags : uint32_t {
    None         = 0,
    Active       = 1u << 0,
    PlayerInside = 1u << 1,
    TriggerOnce  = 1u << 2,
    RequiresItem = 1u << 3,
};

struct TriggerComponent {
    float        radius = 2.0f;
    TriggerFlags flags  = TriggerFlags::Active;
};

} // namespace ZHLN::Interaction

template <>
inline constexpr bool ZHLN::EnableEnumFlags<ZHLN::Interaction::TriggerFlags> = true;
