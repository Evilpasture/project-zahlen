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
#include <Zahlen/Math/Angles.hpp>
#include <Zahlen/Math/Quat.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <BSP/BSPInteraction.hpp>
#include <Zahlen/Audio/Audio.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/ECS/SystemGraph.hpp>
#include <Zahlen/Engine/Capabilities/Entities.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <numbers>
#include <cmath>
#include <string>

namespace ZHLN::Interaction {

// --- Dispatch ---------------------------------------------------------------

void FireEntityInput(
    DispatchQuery                 query,
    Entity                        entity,
    std::string_view              input,
    std::string_view              parameter,
    ZHLN::Optional<AudioContext&> audio
) {
    const Entity e = entity;
    std::string_view label = "<unnamed>";
    if (const auto nameComp = query.Get<Components::NameComponent>(e)) {
        label = std::string_view(nameComp->name);
    }

    {
        {
            // Check DoorComponent
            if (auto door = query.Get<DoorComponent>(e)) {
                if ((door->flags & DoorFlags::Locked) != DoorFlags::None && input != "Unlock") {
                    Log("[Interaction] Door '{}' is locked", label);
                    return;
                }

                if (input == "Open") {
                    if (door->state == DoorState::Closed || door->state == DoorState::Closing) {
                        door->state = DoorState::Opening;
                        FireEntityOutputs(query, e, "OnStartOpening", audio);
                    }
                } else if (input == "Close") {
                    if (door->state == DoorState::Open || door->state == DoorState::Opening) {
                        door->state = DoorState::Closing;
                        FireEntityOutputs(query, e, "OnStartClosing", audio);
                    }
                } else if (input == "Toggle") {
                    if (door->state == DoorState::Closed || door->state == DoorState::Closing) {
                        door->state = DoorState::Opening;
                        FireEntityOutputs(query, e, "OnStartOpening", audio);
                    } else {
                        door->state = DoorState::Closing;
                        FireEntityOutputs(query, e, "OnStartClosing", audio);
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
            if (auto button = query.Get<ButtonComponent>(e)) {
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
                            const uint32_t firedCount = FireEntityOutputs(query, e, "OnPressed", audio);
                            if (firedCount == 0 && !button->target.empty()) {
                                FireEntityInput(query, std::string_view(button->target), "Toggle", "", audio);
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
            if (auto trigger = query.Get<TriggerComponent>(e)) {
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
            if (auto light = query.Get<Components::LightComponent>(e)) {
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
            if (auto usable = query.Get<UsableComponent>(e)) {
                if (usable->scriptHash != 0) {
                    Log("[Interaction] Script triggered on {}: {:#X}", label, usable->scriptHash);
                }
            }
        }
    }
}

void FireEntityInput(
    DispatchQuery                 query,
    std::string_view              targetname,
    std::string_view              input,
    std::string_view              parameter,
    ZHLN::Optional<AudioContext&> audio
) {
    if (targetname.empty()) {
        return;
    }

    for (const Entity e: query.GetEntitiesWith<Components::NameComponent>()) {
        const auto nameComp = query.Get<Components::NameComponent>(e);
        if (!nameComp) {
            continue;
        }

        if (std::string_view(nameComp->name) == targetname || targetname == "*") {
            FireEntityInput(query, e, input, parameter, audio);
        }
    }
}

uint32_t FireEntityOutputs(DispatchQuery query, Entity entity, std::string_view event, ZHLN::Optional<AudioContext&> audio) {
    auto conn = query.Get<EntityConnectionComponent>(entity);
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

            FireEntityInput(query, std::string_view(out.target), std::string_view(out.input), std::string_view(out.parameter), audio);
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

struct BSPResolutionRequest {
    const BSP::BSPMap*        map     = nullptr;
    const BSP::ImportOptions* options = nullptr;
    std::vector<BSP::EntityRecord>* out = nullptr;
};

void CollectInteractiveEntityRecords(BSP::ResolutionQuery query, ECS::ResMut<ECS::ExtensionServices> ext) {
    if (auto req = ext->Get<BSPResolutionRequest>(); req && req->map && req->options && req->out) {
        *req->out = BSP::DescribeInteractiveEntities(query, *req->map, *req->options);
    }
}
template <typename... Cs>
void SpawnEntity(ECS::Registry& registry, Cs&&... cs) {
    registry.Create(std::forward<Cs>(cs)...);
}
template <typename... Cs>
void SpawnEntity(Entities& entities, Cs&&... cs) {
    entities.Spawn(std::forward<Cs>(cs)...);
}
template <typename C>
void AttachComponent(ECS::Registry& registry, Entity e, C&& c) {
    registry.Add(e, std::forward<C>(c));
}
template <typename C>
void AttachComponent(Entities& entities, Entity e, C&& c) {
    entities.Attach(e, std::forward<C>(c));
}

template <typename World>
[[nodiscard]] auto HalfExtentsOf(World& registry, Entity e) -> FVec3 {
    const auto source = registry.template Get<Components::SceneSourceComponent>(e);
    return source ? FVec3{source->halfExtents} : FVec3::Zero();
}

template <typename World>
void AttachOutputs(World& registry, Entity e, const std::vector<BSP::EntityOutput>& outputs) {
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
        AttachComponent(registry, e, std::move(conn));
    }
}

template <typename World>
void AttachDoor(
    World& registry, const BSP::EntityRecord& record, const BSP::ImportOptions& options, bool hasTransform, FVec3 initialPos, Quat initialRot
) {
    const bool       rotating = record.kind == BSP::InteractiveKind::DoorRotating;
    const DoorFlags  flags    = DoorFlagsFrom(*record.door, rotating);

    const float speed    = record.speed.value_or(kDefaultDoorSpeed);
    const float wait     = record.wait.value_or(kDefaultDoorWait);
    const float lip      = record.lip.value_or(0.0f);

    FVec3 closedPos = hasTransform ? initialPos : FVec3::Zero();
    Quat  closedRot = hasTransform ? initialRot : Quat::Identity();
    FVec3       openPos   = closedPos;
    Quat       openRot   = closedRot;
    FVec3       rotAxis   = FVec3::UnitY();
    float           rotAngle  = record.distance.value_or(kDefaultDoorRotation);
    float           speedNorm = 1.0f;

    if (rotating) {
        if (record.door->rotateBack) {
            rotAngle = -rotAngle;
        }
        const FVec3 sourceAxis = record.door->axisX ? FVec3::UnitX() : (record.door->axisY ? FVec3::UnitY() : FVec3::UnitZ());
        rotAxis                    = BSP::ConvertDirection(options, sourceAxis);
        openRot                    = closedRot * Quat::FromAxisAngle(rotAxis, DegF{rotAngle}.ToRadians());
        speedNorm                  = std::abs(speed / std::max(std::abs(rotAngle), 1.0f));
    } else {
        // Travel is the brush's own extent along the direction it moves, minus
        // the lip; a door that has to open past its frame does not go further
        // than its own thickness.
        const FVec3 half           = HalfExtentsOf(registry, record.entity);
        const FVec3 moveDir        = record.moveDirection;
        const float     extentAlongDir = std::abs(half.x * moveDir.x) + std::abs(half.y * moveDir.y) + std::abs(half.z * moveDir.z);
        const float     travelDist     = std::max(2.0f * extentAlongDir - lip * options.unitScale, kMinLinearTravel);
        openPos                        = closedPos + moveDir * travelDist;
        speedNorm                      = (speed * options.unitScale) / std::max(travelDist, 0.01f);
    }

    DoorState state    = DoorState::Closed;
    float     progress = 0.0f;
    if ((flags & DoorFlags::StartsOpen) != DoorFlags::None) {
        state    = DoorState::Open;
        progress = 1.0f;
        if (hasTransform) {
            // The instance is authoritative for where the geometry is: a door
            // that starts open has to *be* open, not animate open on the first
            // frame.
            registry.template Patch<Components::TransformComponent>(record.entity, [&](auto& transform) {
                if (rotating) {
                    transform.rotation = openRot;
                } else {
                    transform.position = openPos;
                }
            });
        }
    }

    AttachComponent(
        registry,
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

template <typename World>
void AttachButton(World& registry, const BSP::EntityRecord& record, const BSP::ImportOptions& options, bool hasTransform, FVec3 initialPos) {
    const ButtonFlags flags = ButtonFlagsFrom(*record.button);

    const float speed = record.speed.value_or(kDefaultButtonSpeed);
    const float wait  = record.wait.value_or(kDefaultButtonWait);
    const float lip   = record.lip.value_or(0.0f);

    FVec3 unpressed = hasTransform ? initialPos : FVec3::Zero();
    const float     travel    = (lip > 0.0f) ? (lip * options.unitScale) : kDefaultButtonTravel;
    const FVec3 pressed =
        ((flags & ButtonFlags::DontMove) != ButtonFlags::None) ? unpressed : (unpressed + record.moveDirection * travel);

    AttachComponent(
        registry,
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

template <typename World>
void AttachTrigger(World& registry, const BSP::EntityRecord& record) {
    const float     wait  = record.wait.value_or(kDefaultTriggerWait);
    const TriggerFlags flags = TriggerFlagsFrom(record.kind);

    const FVec3 half   = HalfExtentsOf(registry, record.entity);
    const float     radius = (half.LengthSquared() > 0.0f) ? half.Length() : 2.0f;

    AttachComponent(
        registry,
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

template <typename World>
void InitializeBSPEntitiesImpl(World& registry, const std::vector<BSP::EntityRecord>& records, const BSP::ImportOptions& options) {
    for (const BSP::EntityRecord& record: records) {
        // A teleport landing point is pure logic and has nothing in the scene:
        // DescribeScene keeps info_* classes out of the spawned entities, so
        // this layer creates the lightweight entity the trigger resolves its
        // `target` against. The quaternion is already the conjugated Source
        // rotation, which TeleportPlayer reads off the transform's +X axis.
        if (record.kind == BSP::InteractiveKind::TeleportDestination) {
            SpawnEntity(
                registry,
                Components::NameComponent {.name = String64(record.targetName)},
                Components::TransformComponent {
                    .position = record.origin.value_or(FVec3::Zero()),
                    .rotation = record.orientation,
                    .scale    = FVec3::Splat(1.0f)
                },
                TeleportDestinationComponent {}
            );
            continue;
        }

        const Entity e = record.entity;
        if (e == Entity::Null()) {
            continue;
        }

        AttachComponent(
            registry,
            e,
            BSPEntityMetadataComponent {
                .className  = String64(record.className),
                .targetName = String64(record.targetName),
                .model      = String64(record.model),
                .spawnFlags = record.spawnFlags,
            }
        );

        AttachOutputs(registry, e, record.outputs);

        const auto transformView = registry.template Get<Components::TransformComponent>(e);
        const bool hasTransform  = static_cast<bool>(transformView);
        const FVec3 initialPos   = hasTransform ? transformView->position : FVec3::Zero();
        const Quat  initialRot   = hasTransform ? transformView->rotation : Quat::Identity();

        switch (record.kind) {
            case BSP::InteractiveKind::Door:
            case BSP::InteractiveKind::DoorRotating: AttachDoor(registry, record, options, hasTransform, initialPos, initialRot); break;
            case BSP::InteractiveKind::Button:
            case BSP::InteractiveKind::ButtonRotating: AttachButton(registry, record, options, hasTransform, initialPos); break;
            case BSP::InteractiveKind::Trigger:
            case BSP::InteractiveKind::TriggerOnce:
            case BSP::InteractiveKind::TriggerTeleport: AttachTrigger(registry, record); break;
            case BSP::InteractiveKind::TeleportDestination:
            case BSP::InteractiveKind::None: break;
        }
    }
}

} // namespace

void InitializeBSPEntities(ECS::Registry& registry, const BSP::BSPMap& map, const Scene::Instance& /*instance*/, const BSP::ImportOptions& options) {
    const auto records = BSP::DescribeInteractiveEntities(BSP::ResolutionQuery {registry}, map, options);
    InitializeBSPEntitiesImpl(registry, records, options);
}

void InitializeBSPEntities(Engine& engine, const BSP::BSPMap& map, const Scene::Instance& /*instance*/, const BSP::ImportOptions& options) {
    std::vector<BSP::EntityRecord> records;
    engine.EmplaceService<BSPResolutionRequest>(BSPResolutionRequest {.map = &map, .options = &options, .out = &records});
    engine.GetUpdateGraph().InvokeSequential<&CollectInteractiveEntityRecords>(Frame {});
    auto entities = engine.Entities();
    InitializeBSPEntitiesImpl(entities, records, options);
}

} // namespace ZHLN::Interaction
