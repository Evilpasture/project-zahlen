// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// gameplay/Interaction/InteractionComponents.hpp
#pragma once

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <Zahlen/Core/EnumFlags.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Entity.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ZHLN::Interaction {

using ZHLN::operator|;
using ZHLN::operator&;
using ZHLN::operator^;
using ZHLN::operator~;
using ZHLN::operator|=;
using ZHLN::operator&=;
using ZHLN::operator^=;

// Identity of a carryable item. `id` is the gameplay hash scripts and the
// pickup log line refer to; `icon` names the inventory icon asset.
struct ItemBaseComponent {
    String64 name;
    uint32_t id = 0;
    String64 icon;
};

// Marks an item entity as picked up. InteractionSystem sets it when the item
// enters a container; rendering/physics are stripped off at the same moment.
struct PickupComponent {
    uint32_t isPickedUp = 0;
};

// An interactable that dispatches a script by hash when used.
struct UsableComponent {
    uint64_t scriptHash = 0;
};

// Fixed-capacity inventory. The slot count is part of the gameplay rules
// (16 slots), not engine substrate.
struct ContainerComponent {
    static constexpr size_t       MAX_SLOTS = 16;
    std::array<Entity, MAX_SLOTS> slots     = {Entity::Null(), Entity::Null(), Entity::Null(), Entity::Null(), Entity::Null(), Entity::Null(),
                                               Entity::Null(), Entity::Null(), Entity::Null(), Entity::Null(), Entity::Null(), Entity::Null(),
                                               Entity::Null(), Entity::Null(), Entity::Null(), Entity::Null()};
    uint32_t                      count     = 0;
    uint32_t                      _padding  = 0;
};

// Proximity gate: InteractionSystem measures player distance against `radius`
// or `halfExtents` (AABB) and tracks presence through `flags`.
enum class TriggerFlags : uint32_t {
    None         = 0,
    Active       = 1u << 0,
    PlayerInside = 1u << 1,
    TriggerOnce  = 1u << 2,
    RequiresItem = 1u << 3,
    TouchOpens   = 1u << 4,
    UseOpens     = 1u << 5,
    Fired        = 1u << 6,
};

struct TriggerComponent {
    float        radius      = 2.0f;
    TriggerFlags flags       = TriggerFlags::Active;
    JPH::Vec3    halfExtents = JPH::Vec3::sZero(); // If > 0, evaluated as an AABB trigger volume
    float        wait        = 0.2f;
    float        waitTimer   = 0.0f;

    String64 target;
    String64 targetname;
    String64 filterName;
    uint64_t scriptHash = 0;
};

// --- Door Component & States ---
enum class DoorState : uint8_t { Closed, Opening, Open, Closing };

enum class DoorFlags : uint32_t {
    None       = 0,
    StartsOpen = 1u << 0,
    Toggle     = 1u << 1,
    UseOpens   = 1u << 2,
    TouchOpens = 1u << 3,
    Locked     = 1u << 4,
    OneWay     = 1u << 5,
    Rotating   = 1u << 6,
};

struct DoorComponent {
    DoorState state = DoorState::Closed;
    DoorFlags flags = DoorFlags::None;

    // Linear motion (func_door)
    JPH::Vec3 closedPosition = JPH::Vec3::sZero();
    JPH::Vec3 openPosition   = JPH::Vec3::sZero();

    // Angular motion (func_door_rotating, prop_door_rotating)
    JPH::Quat closedRotation = JPH::Quat::sIdentity();
    JPH::Quat openRotation   = JPH::Quat::sIdentity();
    JPH::Vec3 rotationAxis   = JPH::Vec3::sAxisY();
    float     rotationAngle  = 90.0f; // degrees

    float speed     = 2.0f; // travel progress speed (1.0 / duration)
    float progress  = 0.0f; // 0.0 (closed) -> 1.0 (open)
    float wait      = 4.0f; // return delay in seconds (-1 = stay open)
    float waitTimer = 0.0f;
    float damage    = 0.0f; // damage when blocked

    String64 target;     // targetname of entity to fire on open/close
    String64 targetname; // this door's targetname
    String64 message;    // lock/interaction message
    uint64_t scriptHash = 0;
};

// --- Button Component & States ---
enum class ButtonState : uint8_t { Off, MovingIn, Pressed, MovingOut };

enum class ButtonFlags : uint32_t {
    None            = 0,
    DontMove        = 1u << 0,
    Toggle          = 1u << 1,
    TouchActivates  = 1u << 2,
    UseActivates    = 1u << 3,
    DamageActivates = 1u << 4,
    Locked          = 1u << 5,
};

struct ButtonComponent {
    ButtonState state = ButtonState::Off;
    ButtonFlags flags = ButtonFlags::UseActivates;

    JPH::Vec3 unpressedPosition = JPH::Vec3::sZero();
    JPH::Vec3 pressedPosition   = JPH::Vec3::sZero();

    float speed     = 2.0f;
    float progress  = 0.0f; // 0.0 (unpressed) -> 1.0 (pressed)
    float wait      = 3.0f; // reset delay in seconds (-1 = stay pressed)
    float waitTimer = 0.0f;

    String64 target;     // targetname of entity to trigger
    String64 targetname; // this button's targetname
    uint64_t scriptHash = 0;
};

// --- Source Entity I/O Connections ---
struct EntityOutput {
    String64 event;     // "OnPressed", "OnTrigger", "OnOpen", "OnClose", "OnIn", "OnOut", etc.
    String64 target;    // target entity's targetname
    String64 input;     // action/input: "Open", "Close", "Toggle", "Lock", "Unlock", "Press", etc.
    String64 parameter; // parameter string
    float    delay       = 0.0f;
    int32_t  timesToFire = -1; // -1 = infinite, 1 = once
};

struct EntityConnectionComponent {
    static constexpr size_t               MAX_OUTPUTS = 16;
    std::array<EntityOutput, MAX_OUTPUTS> outputs {};
    uint32_t                              outputCount = 0;

    void AddOutput(const EntityOutput& out) noexcept {
        if (outputCount < MAX_OUTPUTS) {
            outputs[outputCount++] = out;
        }
    }
};

// Raw metadata preserved from BSPEntity lump
struct BSPEntityMetadataComponent {
    String64 className;
    String64 targetName;
    String64 model;
    uint32_t spawnFlags = 0;
};

} // namespace ZHLN::Interaction

template <>
inline constexpr bool ZHLN::EnableEnumFlags<ZHLN::Interaction::TriggerFlags> = true;
template <>
inline constexpr bool ZHLN::EnableEnumFlags<ZHLN::Interaction::DoorFlags> = true;
template <>
inline constexpr bool ZHLN::EnableEnumFlags<ZHLN::Interaction::ButtonFlags> = true;
