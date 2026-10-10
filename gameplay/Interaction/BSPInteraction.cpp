// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "BSPInteraction.hpp"
#include <BSP/BSPGeometry.hpp>
#include <BSP/BSPRead.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>

namespace ZHLN::Interaction {

namespace {

auto ParseFloat(std::string_view sv, float defaultVal = 0.0f) noexcept -> float {
    if (sv.empty()) {
        return defaultVal;
    }
    const std::string s(sv);
    char*             end = nullptr;
    const float       val = std::strtof(s.c_str(), &end);
    return (end != s.c_str()) ? val : defaultVal;
}

auto ParseUInt(std::string_view sv, uint32_t defaultVal = 0) noexcept -> uint32_t {
    if (sv.empty()) {
        return defaultVal;
    }
    const std::string s(sv);
    char*             end = nullptr;
    const auto        val = static_cast<uint32_t>(std::strtoul(s.c_str(), &end, 10));
    return (end != s.c_str()) ? val : defaultVal;
}

auto ParseMoveDir(std::string_view movedirStr, const BSP::ImportOptions& opts) -> JPH::Vec3 {
    if (movedirStr.empty()) {
        return ConvertDirection(opts, JPH::Vec3::sAxisX());
    }
    float             v[3] = {0.0f, 0.0f, 0.0f};
    const std::string s(movedirStr);
    const char*       cur = s.c_str();
    char*             end = nullptr;
    for (float& component: v) {
        component = std::strtof(cur, &end);
        if (end == cur) {
            break;
        }
        cur = end;
    }
    // Special Source conventions:
    // -1 0 0 => UP (+Z in Source Z-up)
    // -2 0 0 => DOWN (-Z in Source Z-up)
    if (std::abs(v[0] - (-1.0f)) < 0.01f && std::abs(v[1]) < 0.01f && std::abs(v[2]) < 0.01f) {
        return ConvertDirection(opts, JPH::Vec3(0.0f, 0.0f, 1.0f));
    }
    if (std::abs(v[0] - (-2.0f)) < 0.01f && std::abs(v[1]) < 0.01f && std::abs(v[2]) < 0.01f) {
        return ConvertDirection(opts, JPH::Vec3(0.0f, 0.0f, -1.0f));
    }
    const float     pitch = v[0] * (3.14159265358979323846f / 180.0f);
    const float     yaw   = v[1] * (3.14159265358979323846f / 180.0f);
    const float     cp    = std::cos(pitch);
    const JPH::Vec3 sourceDir(cp * std::cos(yaw), cp * std::sin(yaw), -std::sin(pitch));
    if (sourceDir.LengthSq() < 1e-4f) {
        return ConvertDirection(opts, JPH::Vec3(1.0f, 0.0f, 0.0f));
    }
    return ConvertDirection(opts, sourceDir.Normalized());
}

auto FindMatchingEntity(ECS::Registry& registry, std::string_view matchName, std::string_view modelSource) -> Entity {
    for (const Entity e: registry.GetEntitiesWith<Components::NameComponent>()) {
        const auto nameComp = registry.Get<Components::NameComponent>(e);
        if (nameComp && std::string_view(nameComp->name) == matchName) {
            return e;
        }
    }
    if (!modelSource.empty()) {
        for (const Entity e: registry.GetEntitiesWith<Components::SceneSourceComponent>()) {
            const auto srcComp = registry.Get<Components::SceneSourceComponent>(e);
            if (srcComp && std::string_view(srcComp->source) == modelSource) {
                return e;
            }
        }
    }
    return Entity::Null();
}

} // namespace

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
            // Check DoorComponent
            if (auto door = registry.Get<DoorComponent>(e)) {
                if ((door->flags & DoorFlags::Locked) != DoorFlags::None && input != "Unlock") {
                    Log("[Interaction] Door '{}' is locked", targetname);
                    continue;
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
                    door->speed = ParseFloat(parameter, 100.0f);
                }
            }

            // Check ButtonComponent
            if (auto button = registry.Get<ButtonComponent>(e)) {
                if ((button->flags & ButtonFlags::Locked) != ButtonFlags::None && input != "Unlock") {
                    Log("[Interaction] Button '{}' is locked", targetname);
                    continue;
                }

                if (input == "Press") {
                    if (button->state == ButtonState::Off) {
                        button->state     = ((button->flags & ButtonFlags::DontMove) != ButtonFlags::None) ? ButtonState::Pressed : ButtonState::MovingIn;
                        button->waitTimer = button->wait;
                        if ((button->flags & ButtonFlags::DontMove) != ButtonFlags::None) {
                            button->progress = 1.0f;
                            FireEntityOutputs(registry, e, "OnPressed", audio);
                            if (!button->target.empty()) {
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
                    Log("[Interaction] Script triggered on {}: {:#X}", targetname, usable->scriptHash);
                }
            }
        }
    }
}

void FireEntityOutputs(ECS::Registry& registry, Entity entity, std::string_view event, ZHLN::Optional<AudioContext&> audio) {
    auto conn = registry.Get<EntityConnectionComponent>(entity);
    if (!conn) {
        return;
    }

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
        }
    }
}

void InitializeBSPEntities(ECS::Registry& registry, const BSP::BSPMap& map, const Scene::Instance& /*instance*/, const BSP::ImportOptions& options) {
    size_t interactiveIndex = 0;

    for (const BSP::BSPEntity& entity: map.entities) {
        const std::string_view className = entity.Find("classname");
        if (className.empty()) {
            continue;
        }

        const bool isDoor =
            (className == "func_door" || className == "func_door_rotating" || className == "prop_door_rotating" || className == "momentary_door");
        const bool isButton  = (className == "func_button" || className == "func_rot_button" || className == "momentary_rot_button");
        const bool isTrigger = className.starts_with("trigger_");

        if (!isDoor && !isButton && !isTrigger) {
            continue;
        }

        const auto        targetName = entity.Find("targetname");
        const std::string matchName  = targetName.empty() ? (std::string(className) + "_" + std::to_string(interactiveIndex++)) : std::string(targetName);
        const std::string modelStr   = std::string(entity.Find("model"));

        const Entity e = FindMatchingEntity(registry, matchName, modelStr);
        if (e == Entity::Null()) {
            continue;
        }

        const auto spawnflags = ParseUInt(entity.Find("spawnflags"));

        // 1. Assign BSPEntityMetadataComponent
        registry.Add<BSPEntityMetadataComponent>(
            e,
            BSPEntityMetadataComponent {
                .className  = String64(className),
                .targetName = String64(targetName),
                .model      = String64(modelStr),
                .spawnFlags = spawnflags,
            }
        );

        // 2. Parse Connections & Outputs
        EntityConnectionComponent conn;
        for (const auto& [key, val]: entity.keys) {
            if (key.starts_with("On") && !val.empty()) {
                EntityOutput out;
                out.event = String64(key);

                const size_t c1 = val.find(',');
                if (c1 != std::string::npos) {
                    out.target      = String64(val.substr(0, c1));
                    const size_t c2 = val.find(',', c1 + 1);
                    if (c2 != std::string::npos) {
                        out.input       = String64(val.substr(c1 + 1, c2 - c1 - 1));
                        const size_t c3 = val.find(',', c2 + 1);
                        if (c3 != std::string::npos) {
                            out.parameter   = String64(val.substr(c2 + 1, c3 - c2 - 1));
                            const size_t c4 = val.find(',', c3 + 1);
                            if (c4 != std::string::npos) {
                                out.delay       = ParseFloat(val.substr(c3 + 1, c4 - c3 - 1));
                                out.timesToFire = static_cast<int32_t>(ParseUInt(val.substr(c4 + 1)));
                            } else {
                                out.delay = ParseFloat(val.substr(c3 + 1));
                            }
                        } else {
                            out.parameter = String64(val.substr(c2 + 1));
                        }
                    } else {
                        out.input = String64(val.substr(c1 + 1));
                    }
                } else {
                    out.target = String64(val);
                    out.input  = String64("Toggle");
                }
                conn.AddOutput(out);
            }
        }

        const std::string_view targetVal = entity.Find("target");
        if (!targetVal.empty()) {
            EntityOutput out;
            out.target = String64(targetVal);
            out.input  = String64("Toggle");
            if (isButton) {
                out.event = String64("OnPressed");
            } else if (isTrigger) {
                out.event = String64("OnTrigger");
            } else if (isDoor) {
                out.event = String64("OnOpen");
            }
            conn.AddOutput(out);
        }

        if (conn.outputCount > 0) {
            registry.Add(e, std::move(conn));
        }

        auto trans = registry.Get<Components::TransformComponent>(e);

        // 3. Attach DoorComponent
        if (isDoor) {
            const float speedVal = ParseFloat(entity.Find("speed"), 100.0f);
            const float waitVal  = ParseFloat(entity.Find("wait"), 4.0f);
            const float lipVal   = ParseFloat(entity.Find("lip"), 0.0f);

            const bool isRotating = (className == "func_door_rotating" || className == "prop_door_rotating");
            DoorFlags  doorFlags  = isRotating ? DoorFlags::Rotating : DoorFlags::None;

            if ((spawnflags & 1) != 0) {
                doorFlags |= DoorFlags::StartsOpen;
            }
            if ((spawnflags & 32) != 0) {
                doorFlags |= DoorFlags::Toggle;
            }
            if ((spawnflags & 256) != 0 || (spawnflags & 512) != 0) {
                doorFlags |= DoorFlags::UseOpens;
            }
            if ((spawnflags & 1024) != 0) {
                doorFlags |= DoorFlags::TouchOpens;
            }
            if ((spawnflags & 2048) != 0) {
                doorFlags |= DoorFlags::Locked;
            }
            if ((doorFlags & (DoorFlags::UseOpens | DoorFlags::TouchOpens)) == DoorFlags::None) {
                doorFlags |= DoorFlags::UseOpens;
            }

            JPH::Vec3 closedPos = trans ? trans->position : JPH::Vec3::sZero();
            JPH::Vec3 openPos   = closedPos;
            JPH::Quat closedRot = trans ? trans->rotation : JPH::Quat::sIdentity();
            JPH::Quat openRot   = closedRot;
            JPH::Vec3 rotAxis   = JPH::Vec3::sAxisY();
            float     rotAngle  = ParseFloat(entity.Find("distance"), 90.0f);
            float     speedNorm = 1.0f;

            if (isRotating) {
                if ((spawnflags & 2) != 0) {
                    rotAngle = -rotAngle;
                }

                const JPH::Vec3 srcAxis = ((spawnflags & 64) != 0) ? JPH::Vec3::sAxisX() :
                                                                     (((spawnflags & 128) != 0) ? JPH::Vec3::sAxisY() : JPH::Vec3::sAxisZ());
                rotAxis                 = ConvertDirection(options, srcAxis);
                openRot                 = closedRot * JPH::Quat::sRotation(rotAxis, JPH::DegreesToRadians(rotAngle));
                speedNorm               = std::abs(speedVal / std::max(std::abs(rotAngle), 1.0f));
            } else {
                const JPH::Vec3 moveDir        = ParseMoveDir(entity.Find("movedir"), options);
                auto            srcComp        = registry.Get<Components::SceneSourceComponent>(e);
                const JPH::Vec3 half           = srcComp ? JPH::Vec3(srcComp->halfExtents) : JPH::Vec3(0.5f, 0.5f, 0.5f);
                const float     extentAlongDir = std::abs(half.GetX() * moveDir.GetX()) + std::abs(half.GetY() * moveDir.GetY()) +
                                             std::abs(half.GetZ() * moveDir.GetZ());
                const float travelDist = std::max(2.0f * extentAlongDir - lipVal * options.unitScale, 0.5f);
                openPos                = closedPos + moveDir * travelDist;
                speedNorm              = (speedVal * options.unitScale) / std::max(travelDist, 0.01f);
            }

            DoorState state    = DoorState::Closed;
            float     progress = 0.0f;
            if ((doorFlags & DoorFlags::StartsOpen) != DoorFlags::None) {
                state    = DoorState::Open;
                progress = 1.0f;
                if (trans) {
                    if (isRotating) {
                        trans->rotation = openRot;
                    } else {
                        trans->position = openPos;
                    }
                }
            }

            registry.Add<DoorComponent>(
                e,
                DoorComponent {
                    .state          = state,
                    .flags          = doorFlags,
                    .closedPosition = closedPos,
                    .openPosition   = openPos,
                    .closedRotation = closedRot,
                    .openRotation   = openRot,
                    .rotationAxis   = rotAxis,
                    .rotationAngle  = rotAngle,
                    .speed          = std::max(speedNorm, 0.1f),
                    .progress       = progress,
                    .wait           = waitVal,
                    .waitTimer      = (state == DoorState::Open) ? waitVal : 0.0f,
                    .damage         = ParseFloat(entity.Find("dmg"), 0.0f),
                    .target         = String64(entity.Find("target")),
                    .targetname     = String64(targetName),
                    .message        = String64(entity.Find("message")),
                }
            );
        } else if (isButton) {
            const float speedVal = ParseFloat(entity.Find("speed"), 100.0f);
            const float waitVal  = ParseFloat(entity.Find("wait"), 3.0f);
            const float lipVal   = ParseFloat(entity.Find("lip"), 0.0f);

            ButtonFlags btnFlags = ButtonFlags::UseActivates;
            if ((spawnflags & 1) != 0) {
                btnFlags |= ButtonFlags::DontMove;
            }
            if ((spawnflags & 32) != 0) {
                btnFlags |= ButtonFlags::Toggle;
            }
            if ((spawnflags & 512) != 0) {
                btnFlags |= ButtonFlags::TouchActivates;
            }
            if ((spawnflags & 1024) != 0) {
                btnFlags |= ButtonFlags::DamageActivates;
            }
            if ((spawnflags & 2048) != 0) {
                btnFlags |= ButtonFlags::UseActivates;
            }

            const JPH::Vec3 moveDir   = ParseMoveDir(entity.Find("movedir"), options);
            const JPH::Vec3 unpressed = trans ? trans->position : JPH::Vec3::sZero();
            const float     travel    = (lipVal > 0.0f) ? (lipVal * options.unitScale) : 0.05f;
            const JPH::Vec3 pressed   = ((btnFlags & ButtonFlags::DontMove) != ButtonFlags::None) ? unpressed : (unpressed + moveDir * travel);

            registry.Add<ButtonComponent>(
                e,
                ButtonComponent {
                    .state             = ButtonState::Off,
                    .flags             = btnFlags,
                    .unpressedPosition = unpressed,
                    .pressedPosition   = pressed,
                    .speed             = (travel > 0.0f) ? std::max((speedVal * options.unitScale) / travel, 1.0f) : 2.0f,
                    .progress          = 0.0f,
                    .wait              = waitVal,
                    .waitTimer         = 0.0f,
                    .target            = String64(entity.Find("target")),
                    .targetname        = String64(targetName),
                }
            );
        } else if (isTrigger) {
            const float waitVal = ParseFloat(entity.Find("wait"), 0.2f);

            TriggerFlags trigFlags = TriggerFlags::Active;
            if (className == "trigger_once") {
                trigFlags |= TriggerFlags::TriggerOnce;
            }

            auto            srcComp = registry.Get<Components::SceneSourceComponent>(e);
            const JPH::Vec3 half    = srcComp ? JPH::Vec3(srcComp->halfExtents) : JPH::Vec3::sZero();
            const float     radius  = (half.LengthSq() > 0.0f) ? half.Length() : 2.0f;

            registry.Add<TriggerComponent>(
                e,
                TriggerComponent {
                    .radius      = radius,
                    .flags       = trigFlags,
                    .halfExtents = half,
                    .wait        = (className == "trigger_once") ? -1.0f : waitVal,
                    .waitTimer   = 0.0f,
                    .target      = String64(entity.Find("target")),
                    .targetname  = String64(targetName),
                    .filterName  = String64(entity.Find("filtername")),
                }
            );
        }
    }
}

} // namespace ZHLN::Interaction
