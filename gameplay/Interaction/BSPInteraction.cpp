// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// gameplay/Interaction/BSPInteraction.cpp
//
// Two halves of the same idea: the runtime dispatch that runs a door's inputs,
// and the translation pass that builds the components it dispatches on.
//
// The decoding moved to plugins/BSP -- see the header for why the split is on
// this line. What is left is the part that is about *this game*: the defaults
// a mapper did not specify, the flag combinations that mean "usable", and the
// geometry of how far a door travels.

#include "BSPInteraction.hpp"
#include <BSP/BSPInteraction.hpp>
#include <Zahlen/Audio.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <cmath>
#include <string>

namespace ZHLN::Interaction {

// --- Dispatch ---------------------------------------------------------------

void FireEntityInput(
    ECS::Registry&                registry,
    Entity                        entity,
    std::string_view              input,
    std::string_view              parameter,
    ZHLN::Optional<AudioContext&> audio
) {
    const Entity e = entity;
    std::string_view label = "<unnamed>";
    if (const auto nameComp = registry.Get<Components::NameComponent>(e)) {
        label = std::string_view(nameComp->name);
    }

    {
        {
            // Check DoorComponent
            if (auto door = registry.Get<DoorComponent>(e)) {
                if ((door->flags & DoorFlags::Locked) != DoorFlags::None && input != "Unlock") {
                    Log("[Interaction] Door '{}' is locked", label);
                    return;
                }

                if (input == "Open") {
                    if (door->state == DoorState::Closed || door->state == DoorState::Closing) {
                        door->state = DoorState::Opening;
                        FireEntityOutputs(registry, e, "OnStartOpening", audio);
                    }
                } else if (input == "Close") {
                    if (door->state == DoorState::Open || door->state == DoorState::Opening) {
                        door->state = DoorState::Closing;
                        FireEntityOutputs(registry, e, "OnStartClosing", audio);
                    }
                } else if (input == "Toggle") {
                    if (door->state == DoorState::Closed || door->state == DoorState::Closing) {
                        door->state = DoorState::Opening;
                        FireEntityOutputs(registry, e, "OnStartOpening", audio);
                    } else {
                        door->state = DoorState::Closing;
                        FireEntityOutputs(registry, e, "OnStartClosing", audio);
                    }
                } else if (input == "Lock") {
                    door->flags |= DoorFlags::Locked;
                } else if (input == "Unlock") {
                    door->flags &= ~DoorFlags::Locked;
                } else if (input == "SetSpeed") {
                    door->speed = BSP::ParseFloatOr(parameter, 100.0f);
                }
            }

            // Check ButtonComponent
            if (auto button = registry.Get<ButtonComponent>(e)) {
                if ((button->flags & ButtonFlags::Locked) != ButtonFlags::None && input != "Unlock") {
                    Log("[Interaction] Button '{}' is locked", label);
                    return;
                }

                if (input == "Press") {
                    if (button->state == ButtonState::Off) {
                        button->state     = ((button->flags & ButtonFlags::DontMove) != ButtonFlags::None) ? ButtonState::Pressed : ButtonState::MovingIn;
                        button->waitTimer = button->wait;
                        if ((button->flags & ButtonFlags::DontMove) != ButtonFlags::None) {
                            button->progress          = 1.0f;
                            const uint32_t firedCount = FireEntityOutputs(registry, e, "OnPressed", audio);
                            if (firedCount == 0 && !button->target.empty()) {
                                FireEntityInput(registry, std::string_view(button->target), "Toggle", "", audio);
                            }
                        }
                    }
                } else if (input == "Lock") {
                    button->flags |= ButtonFlags::Locked;
                } else if (input == "Unlock") {
                    button->flags &= ~ButtonFlags::Locked;
                }
            }

            // Check TriggerComponent
            if (auto trigger = registry.Get<TriggerComponent>(e)) {
                if (input == "Enable") {
                    trigger->flags |= TriggerFlags::Active;
                } else if (input == "Disable") {
                    trigger->flags &= ~TriggerFlags::Active;
                } else if (input == "Toggle") {
                    if ((trigger->flags & TriggerFlags::Active) != TriggerFlags::None) {
                        trigger->flags &= ~TriggerFlags::Active;
                    } else {
                        trigger->flags |= TriggerFlags::Active;
                    }
                }
            }

            // Check LightComponent
            if (auto light = registry.Get<Components::LightComponent>(e)) {
                if (input == "TurnOn") {
                    if (light->intensity <= 0.0f) {
                        light->intensity = 100.0f;
                    }
                } else if (input == "TurnOff") {
                    light->intensity = 0.0f;
                } else if (input == "Toggle") {
                    light->intensity = (light->intensity > 0.0f) ? 0.0f : 100.0f;
                }
            }

            // Check UsableComponent
            if (auto usable = registry.Get<UsableComponent>(e)) {
                if (usable->scriptHash != 0) {
                    Log("[Interaction] Script triggered on {}: {:#X}", label, usable->scriptHash);
                }
            }
        }
    }
}

void FireEntityInput(
    ECS::Registry&                registry,
    std::string_view              targetname,
    std::string_view              input,
    std::string_view              parameter,
    ZHLN::Optional<AudioContext&> audio
) {
    if (targetname.empty()) {
        return;
    }

    for (const Entity e: registry.GetEntitiesWith<Components::NameComponent>()) {
        const auto nameComp = registry.Get<Components::NameComponent>(e);
        if (!nameComp) {
            continue;
        }

        if (std::string_view(nameComp->name) == targetname || targetname == "*") {
            FireEntityInput(registry, e, input, parameter, audio);
        }
    }
}

uint32_t FireEntityOutputs(ECS::Registry& registry, Entity entity, std::string_view event, ZHLN::Optional<AudioContext&> audio) {
    auto conn = registry.Get<EntityConnectionComponent>(entity);
    if (!conn) {
        return 0;
    }

    uint32_t fired = 0;
    for (uint32_t i = 0; i < conn->outputCount; ++i) {
        auto& out = conn->outputs[i];
        if (std::string_view(out.event) == event) {
            if (out.timesToFire == 0) {
                continue;
            }
            if (out.timesToFire > 0) {
                out.timesToFire--;
            }

            FireEntityInput(registry, std::string_view(out.target), std::string_view(out.input), std::string_view(out.parameter), audio);
            ++fired;
        }
    }
    return fired;
}

// --- The translation pass ---------------------------------------------------

namespace {

// This game's defaults for a mapper's omitted fields. Source-shaped numbers,
// but they are *rules* -- what a door does when the map does not say -- and
// they belong to the layer that owns the behaviour.
constexpr float kDefaultDoorSpeed     = 100.0f; // units per second
constexpr float kDefaultDoorWait      = 4.0f;   // seconds open before closing
constexpr float kDefaultDoorRotation  = 90.0f;  // degrees of angular travel
constexpr float kDefaultButtonSpeed   = 100.0f;
constexpr float kDefaultButtonWait    = 3.0f;
constexpr float kDefaultButtonTravel  = 0.05f;  // metres, when the map sets no lip
constexpr float kDefaultTriggerWait   = 0.2f;   // re-arm delay
constexpr float kMinLinearTravel      = 0.5f;   // metres, so a zero-extent brush still moves

[[nodiscard]] auto DoorFlagsFrom(const BSP::DoorFlags& decoded, bool rotating) noexcept -> DoorFlags {
    DoorFlags flags = rotating ? DoorFlags::Rotating : DoorFlags::None;
    if (decoded.startsOpen) {
        flags |= DoorFlags::StartsOpen;
    }
    if (decoded.toggle) {
        flags |= DoorFlags::Toggle;
    }
    if (decoded.useOpens) {
        flags |= DoorFlags::UseOpens;
    }
    if (decoded.touchOpens) {
        flags |= DoorFlags::TouchOpens;
    }
    if (decoded.locked) {
        flags |= DoorFlags::Locked;
    }
    // A door nobody can use is a wall: unless the map asked for touch
    // activation, +use opens it.
    if ((flags & (DoorFlags::UseOpens | DoorFlags::TouchOpens)) == DoorFlags::None) {
        flags |= DoorFlags::UseOpens;
    }
    return flags;
}

[[nodiscard]] auto ButtonFlagsFrom(const BSP::ButtonFlags& decoded) noexcept -> ButtonFlags {
    // Unlike a door, a button starts out usable and the flags add to that.
    ButtonFlags flags = ButtonFlags::UseActivates;
    if (decoded.dontMove) {
        flags |= ButtonFlags::DontMove;
    }
    if (decoded.toggle) {
        flags |= ButtonFlags::Toggle;
    }
    if (decoded.touchActivates) {
        flags |= ButtonFlags::TouchActivates;
    }
    if (decoded.damageActivates) {
        flags |= ButtonFlags::DamageActivates;
    }
    if (decoded.useActivates) {
        flags |= ButtonFlags::UseActivates;
    }
    return flags;
}

[[nodiscard]] auto TriggerFlagsFrom(BSP::InteractiveKind kind) noexcept -> TriggerFlags {
    TriggerFlags flags = TriggerFlags::Active;
    if (kind == BSP::InteractiveKind::TriggerOnce) {
        flags |= TriggerFlags::TriggerOnce;
    }
    if (kind == BSP::InteractiveKind::TriggerTeleport) {
        flags |= TriggerFlags::Teleports;
    }
    return flags;
}

[[nodiscard]] auto HalfExtentsOf(ECS::Registry& registry, Entity e) -> JPH::Vec3 {
    const auto source = registry.Get<Components::SceneSourceComponent>(e);
    return source ? JPH::Vec3(source->halfExtents) : JPH::Vec3::sZero();
}

void AttachOutputs(ECS::Registry& registry, Entity e, const std::vector<BSP::EntityOutput>& outputs) {
    if (outputs.empty()) {
        return;
    }
    EntityConnectionComponent conn;
    for (const BSP::EntityOutput& decoded: outputs) {
        // The one conversion this pass makes: decoded strings into the fixed
        // size names the runtime dispatch compares against.
        conn.AddOutput(
            EntityOutput {
                .event       = String64(decoded.key),
                .target      = String64(decoded.target),
                .input       = String64(decoded.input),
                .parameter   = String64(decoded.parameter),
                .delay       = decoded.delay,
                .timesToFire = decoded.timesToFire,
            }
        );
    }
    if (conn.outputCount > 0) {
        registry.Add(e, std::move(conn));
    }
}

void AttachDoor(
    ECS::Registry& registry, const BSP::EntityRecord& record, const BSP::ImportOptions& options, Components::TransformComponent* transform
) {
    const bool       rotating = record.kind == BSP::InteractiveKind::DoorRotating;
    const DoorFlags  flags    = DoorFlagsFrom(*record.door, rotating);

    const float speed    = record.speed.value_or(kDefaultDoorSpeed);
    const float wait     = record.wait.value_or(kDefaultDoorWait);
    const float lip      = record.lip.value_or(0.0f);

    const JPH::Vec3 closedPos = transform ? transform->position : JPH::Vec3::sZero();
    JPH::Vec3       openPos   = closedPos;
    const JPH::Quat closedRot = transform ? transform->rotation : JPH::Quat::sIdentity();
    JPH::Quat       openRot   = closedRot;
    JPH::Vec3       rotAxis   = JPH::Vec3::sAxisY();
    float           rotAngle  = record.distance.value_or(kDefaultDoorRotation);
    float           speedNorm = 1.0f;

    if (rotating) {
        if (record.door->rotateBack) {
            rotAngle = -rotAngle;
        }
        const JPH::Vec3 sourceAxis = record.door->axisX ? JPH::Vec3::sAxisX() : (record.door->axisY ? JPH::Vec3::sAxisY() : JPH::Vec3::sAxisZ());
        rotAxis                    = BSP::ConvertDirection(options, sourceAxis);
        openRot                    = closedRot * JPH::Quat::sRotation(rotAxis, JPH::DegreesToRadians(rotAngle));
        speedNorm                  = std::abs(speed / std::max(std::abs(rotAngle), 1.0f));
    } else {
        // Travel is the brush's own extent along the direction it moves, minus
        // the lip; a door that has to open past its frame does not go further
        // than its own thickness.
        const JPH::Vec3 half           = HalfExtentsOf(registry, record.entity);
        const JPH::Vec3 moveDir        = record.moveDirection;
        const float     extentAlongDir = std::abs(half.GetX() * moveDir.GetX()) + std::abs(half.GetY() * moveDir.GetY()) + std::abs(half.GetZ() * moveDir.GetZ());
        const float     travelDist     = std::max(2.0f * extentAlongDir - lip * options.unitScale, kMinLinearTravel);
        openPos                        = closedPos + moveDir * travelDist;
        speedNorm                      = (speed * options.unitScale) / std::max(travelDist, 0.01f);
    }

    DoorState state    = DoorState::Closed;
    float     progress = 0.0f;
    if ((flags & DoorFlags::StartsOpen) != DoorFlags::None) {
        state    = DoorState::Open;
        progress = 1.0f;
        if (transform != nullptr) {
            // The instance is authoritative for where the geometry is: a door
            // that starts open has to *be* open, not animate open on the first
            // frame.
            if (rotating) {
                transform->rotation = openRot;
            } else {
                transform->position = openPos;
            }
        }
    }

    registry.Add<DoorComponent>(
        record.entity,
        DoorComponent {
            .state          = state,
            .flags          = flags,
            .closedPosition = closedPos,
            .openPosition   = openPos,
            .closedRotation = closedRot,
            .openRotation   = openRot,
            .rotationAxis   = rotAxis,
            .rotationAngle  = rotAngle,
            .speed          = std::max(speedNorm, 0.1f),
            .progress       = progress,
            .wait           = wait,
            .waitTimer      = (state == DoorState::Open) ? wait : 0.0f,
            .damage         = record.damage.value_or(0.0f),
            .target         = String64(record.target),
            .targetname     = String64(record.targetName),
            .message        = String64(record.message),
        }
    );
}

void AttachButton(ECS::Registry& registry, const BSP::EntityRecord& record, const BSP::ImportOptions& options, Components::TransformComponent* transform) {
    const ButtonFlags flags = ButtonFlagsFrom(*record.button);

    const float speed = record.speed.value_or(kDefaultButtonSpeed);
    const float wait  = record.wait.value_or(kDefaultButtonWait);
    const float lip   = record.lip.value_or(0.0f);

    const JPH::Vec3 unpressed = transform ? transform->position : JPH::Vec3::sZero();
    const float     travel    = (lip > 0.0f) ? (lip * options.unitScale) : kDefaultButtonTravel;
    const JPH::Vec3 pressed =
        ((flags & ButtonFlags::DontMove) != ButtonFlags::None) ? unpressed : (unpressed + record.moveDirection * travel);

    registry.Add<ButtonComponent>(
        record.entity,
        ButtonComponent {
            .state             = ButtonState::Off,
            .flags             = flags,
            .unpressedPosition = unpressed,
            .pressedPosition   = pressed,
            .speed             = (travel > 0.0f) ? std::max((speed * options.unitScale) / travel, 1.0f) : 2.0f,
            .progress          = 0.0f,
            .wait              = wait,
            .waitTimer         = 0.0f,
            .target            = String64(record.target),
            .targetname        = String64(record.targetName),
        }
    );
}

void AttachTrigger(ECS::Registry& registry, const BSP::EntityRecord& record, Components::TransformComponent* /*transform*/) {
    const float     wait  = record.wait.value_or(kDefaultTriggerWait);
    const TriggerFlags flags = TriggerFlagsFrom(record.kind);

    const JPH::Vec3 half   = HalfExtentsOf(registry, record.entity);
    const float     radius = (half.LengthSq() > 0.0f) ? half.Length() : 2.0f;

    registry.Add<TriggerComponent>(
        record.entity,
        TriggerComponent {
            .radius      = radius,
            .flags       = flags,
            .halfExtents = half,
            // A trigger_once is spent rather than re-armed, which the -1 wait
            // says without a second flag.
            .wait        = (record.kind == BSP::InteractiveKind::TriggerOnce) ? -1.0f : wait,
            .waitTimer   = 0.0f,
            .target      = String64(record.target),
            .targetname  = String64(record.targetName),
            .filterName  = String64(record.filterName),
        }
    );
}

} // namespace

void InitializeBSPEntities(ECS::Registry& registry, const BSP::BSPMap& map, const Scene::Instance& /*instance*/, const BSP::ImportOptions& options) {
    for (const BSP::EntityRecord& record: BSP::DescribeInteractiveEntities(registry, map, options)) {
        // A teleport landing point is pure logic and has nothing in the scene:
        // DescribeScene keeps info_* classes out of the spawned entities, so
        // this layer creates the lightweight entity the trigger resolves its
        // `target` against. The quaternion is already the conjugated Source
        // rotation, which TeleportPlayer reads off the transform's +X axis.
        if (record.kind == BSP::InteractiveKind::TeleportDestination) {
            registry.Create(
                Components::NameComponent {.name = String64(record.targetName)},
                Components::TransformComponent {
                    .position = record.origin.value_or(JPH::Vec3::sZero()),
                    .rotation = record.orientation,
                    .scale    = JPH::Vec3::sReplicate(1.0f)
                },
                TeleportDestinationComponent {}
            );
            continue;
        }

        const Entity e = record.entity;
        if (e == Entity::Null()) {
            continue;
        }

        registry.Add<BSPEntityMetadataComponent>(
            e,
            BSPEntityMetadataComponent {
                .className  = String64(record.className),
                .targetName = String64(record.targetName),
                .model      = String64(record.model),
                .spawnFlags = record.spawnFlags,
            }
        );

        AttachOutputs(registry, e, record.outputs);

        // The instantiator owns the transform. This pass reads it, except in
        // the "starts open" case, where the geometry has to actually start open
        // rather than animate open on the first frame -- hence a pointer and not
        // the borrowed optional Registry::Get hands back.
        const auto transformView = registry.Get<Components::TransformComponent>(e);
        auto*      transform     = transformView ? transformView.operator->() : nullptr;

        switch (record.kind) {
            case BSP::InteractiveKind::Door:
            case BSP::InteractiveKind::DoorRotating: AttachDoor(registry, record, options, transform); break;
            case BSP::InteractiveKind::Button:
            case BSP::InteractiveKind::ButtonRotating: AttachButton(registry, record, options, transform); break;
            case BSP::InteractiveKind::Trigger:
            case BSP::InteractiveKind::TriggerOnce:
            case BSP::InteractiveKind::TriggerTeleport: AttachTrigger(registry, record, transform); break;
            case BSP::InteractiveKind::TeleportDestination:
            case BSP::InteractiveKind::None: break;
        }
    }
}

} // namespace ZHLN::Interaction
