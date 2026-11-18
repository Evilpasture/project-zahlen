// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// gameplay/Interaction/BSPInteraction.cpp

#include "BSPInteraction.hpp"
#include <BSP/BSPInteraction.hpp>
#include <Zahlen/Audio/AudioValues.hpp>
#include <Zahlen/COM/IAudioDevice.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/Core/COM/Chain.hpp>
#include <Zahlen/Core/COM/Ref.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math/Angles.hpp>
#include <Zahlen/Math/Quat.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <algorithm>
#include <cmath>
#include <string>

namespace ZHLN::Interaction {

namespace {

void PostBeep(COM::IAudioDevice* audio, float volume, float frequency, float duration) noexcept {
    if (audio != nullptr && audio->Initialized()) {
        (void)audio->PostEvent(COM::InChain(AudioEvent {
            .type     = AudioEventType::ProceduralBeep,
            .volume   = volume,
            .param1   = frequency,
            .duration = duration,
        }));
    }
}

constexpr float kDefaultDoorSpeed    = 100.0f;
constexpr float kDefaultDoorWait     = 4.0f;
constexpr float kDefaultDoorRotation = 90.0f;
constexpr float kDefaultButtonSpeed  = 100.0f;
constexpr float kDefaultButtonWait   = 3.0f;
constexpr float kDefaultButtonTravel = 0.05f;
constexpr float kDefaultTriggerWait  = 0.2f;
constexpr float kMinLinearTravel     = 0.5f;

[[nodiscard]] auto DoorFlagsFrom(const BSP::DoorFlags& decoded, bool rotating) noexcept -> DoorFlags {
    DoorFlags flags = rotating ? DoorFlags::Rotating : DoorFlags::None;
    if (decoded.startsOpen) flags |= DoorFlags::StartsOpen;
    if (decoded.toggle) flags |= DoorFlags::Toggle;
    if (decoded.useOpens) flags |= DoorFlags::UseOpens;
    if (decoded.touchOpens) flags |= DoorFlags::TouchOpens;
    if (decoded.locked) flags |= DoorFlags::Locked;
    if ((flags & (DoorFlags::UseOpens | DoorFlags::TouchOpens)) == DoorFlags::None) {
        flags |= DoorFlags::UseOpens;
    }
    return flags;
}

[[nodiscard]] auto ButtonFlagsFrom(const BSP::ButtonFlags& decoded) noexcept -> ButtonFlags {
    ButtonFlags flags = ButtonFlags::UseActivates;
    if (decoded.dontMove) flags |= ButtonFlags::DontMove;
    if (decoded.toggle) flags |= ButtonFlags::Toggle;
    if (decoded.touchActivates) flags |= ButtonFlags::TouchActivates;
    if (decoded.damageActivates) flags |= ButtonFlags::DamageActivates;
    if (decoded.useActivates) flags |= ButtonFlags::UseActivates;
    return flags;
}

[[nodiscard]] auto TriggerFlagsFrom(BSP::InteractiveKind kind) noexcept -> TriggerFlags {
    TriggerFlags flags = TriggerFlags::Active;
    if (kind == BSP::InteractiveKind::TriggerOnce) flags |= TriggerFlags::TriggerOnce;
    if (kind == BSP::InteractiveKind::TriggerTeleport) flags |= TriggerFlags::Teleports;
    return flags;
}

[[nodiscard]] auto HalfExtentsOf(const COM::IEntities& entities, Entity e) noexcept -> FVec3 {
    const auto* source = entities.Get<Components::SceneSourceComponent>(e);
    return source != nullptr ? FVec3 {source->halfExtents} : FVec3::Zero();
}

void AttachOutputs(COM::IEntities& entities, Entity e, const std::vector<BSP::EntityOutput>& outputs) {
    if (outputs.empty()) {
        return;
    }
    EntityConnectionComponent conn;
    for (const BSP::EntityOutput& decoded : outputs) {
        conn.AddOutput(EntityOutput {
            .event       = String64(decoded.key),
            .target      = String64(decoded.target),
            .input       = String64(decoded.input),
            .parameter   = String64(decoded.parameter),
            .delay       = decoded.delay,
            .timesToFire = decoded.timesToFire,
        });
    }
    if (conn.outputCount > 0) {
        (void)entities.Attach(e, conn);
    }
}

void AttachDoor(COM::IEntities& entities, const BSP::EntityRecord& record, const BSP::ImportOptions& options,
                bool hasTransform, FVec3 initialPos, Quat initialRot) {
    const bool      rotating = record.kind == BSP::InteractiveKind::DoorRotating;
    const DoorFlags flags    = DoorFlagsFrom(*record.door, rotating);

    const float speed = record.speed.value_or(kDefaultDoorSpeed);
    const float wait  = record.wait.value_or(kDefaultDoorWait);
    const float lip   = record.lip.value_or(0.0f);

    const FVec3 closedPos = hasTransform ? initialPos : FVec3::Zero();
    const Quat  closedRot = hasTransform ? initialRot : Quat::Identity();
    FVec3       openPos   = closedPos;
    Quat        openRot   = closedRot;
    FVec3       rotAxis   = FVec3::UnitY();
    float       rotAngle  = record.distance.value_or(kDefaultDoorRotation);
    float       speedNorm = 1.0f;

    if (rotating) {
        if (record.door->rotateBack) {
            rotAngle = -rotAngle;
        }
        const FVec3 sourceAxis =
            record.door->axisX ? FVec3::UnitX() : (record.door->axisY ? FVec3::UnitY() : FVec3::UnitZ());
        rotAxis   = BSP::ConvertDirection(options, sourceAxis);
        openRot   = closedRot * Quat::FromAxisAngle(rotAxis, DegF {rotAngle}.ToRadians());
        speedNorm = std::abs(speed / std::max(std::abs(rotAngle), 1.0f));
    } else {
        const FVec3 half           = HalfExtentsOf(entities, record.entity);
        const FVec3 moveDir        = record.moveDirection;
        const float extentAlongDir =
            std::abs(half.x * moveDir.x) + std::abs(half.y * moveDir.y) + std::abs(half.z * moveDir.z);
        const float travelDist = std::max(2.0f * extentAlongDir - lip * options.unitScale, kMinLinearTravel);
        openPos                = closedPos + moveDir * travelDist;
        speedNorm              = (speed * options.unitScale) / std::max(travelDist, 0.01f);
    }

    DoorState state    = DoorState::Closed;
    float     progress = 0.0f;
    if ((flags & DoorFlags::StartsOpen) != DoorFlags::None) {
        state    = DoorState::Open;
        progress = 1.0f;
        if (hasTransform) {
            (void)entities.Patch<Components::TransformComponent>(record.entity, [&](auto& transform) {
                if (rotating) {
                    transform.rotation = openRot;
                } else {
                    transform.position = openPos;
                }
            });
        }
    }

    (void)entities.Attach(record.entity, DoorComponent {
        .state          = state,
        .flags          = flags,
        .closedPosition = closedPos,
        .openPosition   = openPos,
        .closedRotation = closedRot,
        .openRotation   = openRot,
        .rotationAxis   = rotAxis,
        .rotationAngle  = rotAngle,
        .speed          = std::max(speedNorm, 0.05f),
        .progress       = progress,
        .wait           = wait,
        .waitTimer      = 0.0f,
        .damage         = record.damage.value_or(0.0f),
        .target         = String64(record.target),
        .targetname     = String64(record.targetName),
        .message        = String64(record.message),
    });
}

void AttachButton(COM::IEntities& entities, const BSP::EntityRecord& record, const BSP::ImportOptions& options,
                  bool hasTransform, FVec3 initialPos) {
    const ButtonFlags flags = ButtonFlagsFrom(*record.button);
    const float       speed = record.speed.value_or(kDefaultButtonSpeed);
    const float       wait  = record.wait.value_or(kDefaultButtonWait);

    const FVec3 unpressed = hasTransform ? initialPos : FVec3::Zero();
    const FVec3 half      = HalfExtentsOf(entities, record.entity);
    const FVec3 moveDir   = record.moveDirection;
    const float extent =
        std::abs(half.x * moveDir.x) + std::abs(half.y * moveDir.y) + std::abs(half.z * moveDir.z);
    float travel = kDefaultButtonTravel;
    if (record.lip.has_value()) {
        travel = std::max(2.0f * extent - (*record.lip) * options.unitScale, 0.01f);
    } else if (extent > 0.0f) {
        travel = std::min(2.0f * extent, kDefaultButtonTravel);
    }
    const FVec3 pressed = ((flags & ButtonFlags::DontMove) != ButtonFlags::None) ? unpressed : (unpressed + moveDir * travel);

    (void)entities.Attach(record.entity, ButtonComponent {
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
    });
}

void AttachTrigger(COM::IEntities& entities, const BSP::EntityRecord& record) {
    const float        wait   = record.wait.value_or(kDefaultTriggerWait);
    const TriggerFlags flags  = TriggerFlagsFrom(record.kind);
    const FVec3        half   = HalfExtentsOf(entities, record.entity);
    const float        radius = (half.LengthSquared() > 0.0f) ? half.Length() : 2.0f;

    (void)entities.Attach(record.entity, TriggerComponent {
        .radius      = radius,
        .flags       = flags,
        .halfExtents = half,
        .wait        = (record.kind == BSP::InteractiveKind::TriggerOnce) ? -1.0f : wait,
        .waitTimer   = 0.0f,
        .target      = String64(record.target),
        .targetname  = String64(record.targetName),
        .filterName  = String64(record.filterName),
    });
}

} // namespace

void FireEntityInput(COM::IEntities& entities, Entity entity, std::string_view input, std::string_view parameter,
                     COM::IAudioDevice* audio) {
    std::string_view label = "<unnamed>";
    if (const auto* nameComp = entities.Get<Components::NameComponent>(entity)) {
        label = std::string_view(nameComp->name);
    }

    if (auto* door = entities.Get<DoorComponent>(entity)) {
        if ((door->flags & DoorFlags::Locked) != DoorFlags::None && input != "Unlock") {
            Log("[Interaction] Door '{}' is locked", label);
            return;
        }
        if (input == "Open") {
            if (door->state == DoorState::Closed || door->state == DoorState::Closing) {
                door->state = DoorState::Opening;
                FireEntityOutputs(entities, entity, "OnStartOpening", audio);
            }
        } else if (input == "Close") {
            if (door->state == DoorState::Open || door->state == DoorState::Opening) {
                door->state = DoorState::Closing;
                FireEntityOutputs(entities, entity, "OnStartClosing", audio);
            }
        } else if (input == "Toggle") {
            if (door->state == DoorState::Closed || door->state == DoorState::Closing) {
                door->state = DoorState::Opening;
                FireEntityOutputs(entities, entity, "OnStartOpening", audio);
            } else {
                door->state = DoorState::Closing;
                FireEntityOutputs(entities, entity, "OnStartClosing", audio);
            }
        } else if (input == "Lock") {
            door->flags |= DoorFlags::Locked;
        } else if (input == "Unlock") {
            door->flags &= ~DoorFlags::Locked;
        } else if (input == "SetSpeed") {
            door->speed = BSP::ParseFloatOr(parameter, 100.0f);
        }
    }

    if (auto* button = entities.Get<ButtonComponent>(entity)) {
        if ((button->flags & ButtonFlags::Locked) != ButtonFlags::None && input != "Unlock") {
            Log("[Interaction] Button '{}' is locked", label);
            return;
        }
        if (input == "Press" || input == "Use" || input == "Toggle") {
            if (button->state == ButtonState::Off) {
                if ((button->flags & ButtonFlags::DontMove) != ButtonFlags::None) {
                    button->state     = ButtonState::Pressed;
                    button->progress  = 1.0f;
                    button->waitTimer = button->wait;
                    const uint32_t firedCount = FireEntityOutputs(entities, entity, "OnPressed", audio);
                    if (firedCount == 0 && !button->target.empty()) {
                        FireEntityInput(entities, std::string_view(button->target), "Toggle", "", audio);
                    }
                    FireEntityOutputs(entities, entity, "OnIn", audio);
                } else {
                    button->state = ButtonState::MovingIn;
                    const uint32_t firedCount = FireEntityOutputs(entities, entity, "OnPressed", audio);
                    if (firedCount == 0 && !button->target.empty()) {
                        FireEntityInput(entities, std::string_view(button->target), "Toggle", "", audio);
                    }
                }
                PostBeep(audio, 0.2f, 660.0f, 0.06f);
            } else if (input == "Toggle" && button->state == ButtonState::Pressed) {
                button->state = ((button->flags & ButtonFlags::DontMove) != ButtonFlags::None) ? ButtonState::Off : ButtonState::MovingOut;
                if (button->state == ButtonState::Off) {
                    button->progress = 0.0f;
                    FireEntityOutputs(entities, entity, "OnOut", audio);
                }
            }
        } else if (input == "Lock") {
            button->flags |= ButtonFlags::Locked;
        } else if (input == "Unlock") {
            button->flags &= ~ButtonFlags::Locked;
        }
    }

    if (auto* trigger = entities.Get<TriggerComponent>(entity)) {
        if (input == "Enable") {
            trigger->flags |= TriggerFlags::Active;
        } else if (input == "Disable") {
            trigger->flags &= ~TriggerFlags::Active;
        } else if (input == "Toggle") {
            trigger->flags ^= TriggerFlags::Active;
        }
    }

    if (auto* light = entities.Get<Components::LightComponent>(entity)) {
        if (input == "TurnOff") {
            light->intensity = 0.0f;
        } else if (input == "TurnOn") {
            light->intensity = BSP::ParseFloatOr(parameter, light->intensity > 0.0f ? light->intensity : 1.0f);
        } else if (input == "Toggle") {
            light->intensity = (light->intensity > 0.0f) ? 0.0f : 1.0f;
        } else if (input == "SetBrightness") {
            light->intensity = BSP::ParseFloatOr(parameter, light->intensity);
        }
    }

    if (const auto* usable = entities.Get<UsableComponent>(entity)) {
        if (input == "Use" && usable->scriptHash != 0) {
            Log("[Interaction] Entity '{}' dispatched script {:#X}", label, usable->scriptHash);
        }
    }
}

void FireEntityInput(COM::IEntities& entities, std::string_view targetname, std::string_view input,
                     std::string_view parameter, COM::IAudioDevice* audio) {
    if (targetname.empty()) {
        return;
    }
    for (const Entity e : entities.EntitiesWith<Components::NameComponent>()) {
        const auto* nameComp = entities.Get<Components::NameComponent>(e);
        if (nameComp == nullptr) {
            continue;
        }
        if (std::string_view(nameComp->name) == targetname || targetname == "*") {
            FireEntityInput(entities, e, input, parameter, audio);
        }
    }
}

uint32_t FireEntityOutputs(COM::IEntities& entities, Entity entity, std::string_view event, COM::IAudioDevice* audio) {
    auto* conn = entities.Get<EntityConnectionComponent>(entity);
    if (conn == nullptr) {
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
                --out.timesToFire;
            }
            // Snapshot strings before recursive dispatch in case dynamic pool reallocates.
            const String64 target    = out.target;
            const String64 input     = out.input;
            const String64 parameter = out.parameter;
            FireEntityInput(entities, std::string_view(target), std::string_view(input), std::string_view(parameter), audio);
            conn = entities.Get<EntityConnectionComponent>(entity);
            if (conn == nullptr) {
                return fired + 1;
            }
            ++fired;
        }
    }
    return fired;
}

void InitializeBSPEntities(COM::IEntities& entities, const BSP::Map& map, const Scene::Instance& /*instance*/,
                           const BSP::ImportOptions& options) {
    const auto records = BSP::DescribeInteractiveEntities(entities, map, options);
    for (const BSP::EntityRecord& record : records) {
        if (record.kind == BSP::InteractiveKind::TeleportDestination) {
            (void)entities.Spawn(
                Components::NameComponent {.name = String64(record.targetName)},
                Components::TransformComponent {
                    .position = record.origin.value_or(FVec3::Zero()),
                    .rotation = record.orientation,
                    .scale    = FVec3::Splat(1.0f),
                },
                TeleportDestinationComponent {});
            continue;
        }

        const Entity e = record.entity;
        if (e == Entity::Null()) {
            continue;
        }

        (void)entities.Attach(e, BSPEntityMetadataComponent {
            .className  = String64(record.className),
            .targetName = String64(record.targetName),
            .model      = String64(record.model),
            .spawnFlags = record.spawnFlags,
        });

        AttachOutputs(entities, e, record.outputs);

        const auto* transformView = entities.Get<Components::TransformComponent>(e);
        const bool  hasTransform  = transformView != nullptr;
        const FVec3 initialPos    = hasTransform ? transformView->position : FVec3::Zero();
        const Quat  initialRot    = hasTransform ? transformView->rotation : Quat::Identity();

        switch (record.kind) {
            case BSP::InteractiveKind::Door:
            case BSP::InteractiveKind::DoorRotating:
                AttachDoor(entities, record, options, hasTransform, initialPos, initialRot);
                break;
            case BSP::InteractiveKind::Button:
            case BSP::InteractiveKind::ButtonRotating:
                AttachButton(entities, record, options, hasTransform, initialPos);
                break;
            case BSP::InteractiveKind::Trigger:
            case BSP::InteractiveKind::TriggerOnce:
            case BSP::InteractiveKind::TriggerTeleport:
                AttachTrigger(entities, record);
                break;
            case BSP::InteractiveKind::TeleportDestination:
            case BSP::InteractiveKind::None:
                break;
        }
    }
}

void InitializeBSPEntities(COM::IEngine& engine, const BSP::Map& map, const Scene::Instance& instance,
                           const BSP::ImportOptions& options) {
    if (auto* entities = engine.Entities()) {
        InitializeBSPEntities(*entities, map, instance, options);
    }
}

} // namespace ZHLN::Interaction
