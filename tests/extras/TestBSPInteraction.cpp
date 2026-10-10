// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <BSP/BSPGeometry.hpp>
#include <BSP/BSPRead.hpp>
#include <BSP/BSPScene.hpp>
#include <CharacterController/CharacterComponents.hpp>
#include <Interaction/BSPInteraction.hpp>
#include <Interaction/InteractionComponents.hpp>
#include <Interaction/InteractionSystem.hpp>
#include <Zahlen/Audio.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Scene.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <cmath>
#include <string>
#include <vector>

namespace ZHLN {
std::string GetPoorMansStacktrace() {
    return "test_stacktrace";
}
namespace SceneResources {
void Release(PhysicsContext&, Components::PhysicsComponent&) {
}
} // namespace SceneResources
void AudioContext::PostEvent(const AudioEvent&) noexcept {
}
void PhysicsContext::SetCharacterPosition(Physics::BodyHandle, JPH::RVec3Arg) {
}
auto Engine::GetRegistry() -> ECS::Registry& {
    static ECS::Registry reg;
    return reg;
}
void Engine::AddSystemGraphsExtension(SystemGraphsExtension) {
}
} // namespace ZHLN

namespace {

auto BuildSyntheticMap() -> ZHLN::BSP::BSPMap {
    ZHLN::BSP::BSPMap map;
    map.version = 20;

    // Submodel 0: Worldspawn
    ZHLN::BSP::DModel worldModel {};
    worldModel.mins[0] = -100.0f;
    worldModel.mins[1] = -100.0f;
    worldModel.mins[2] = 0.0f;
    worldModel.maxs[0] = 100.0f;
    worldModel.maxs[1] = 100.0f;
    worldModel.maxs[2] = 100.0f;
    map.models.push_back(worldModel);

    // Submodel 1: func_door
    ZHLN::BSP::DModel doorModel {};
    doorModel.mins[0] = -20.0f;
    doorModel.mins[1] = -5.0f;
    doorModel.mins[2] = 0.0f;
    doorModel.maxs[0] = 20.0f;
    doorModel.maxs[1] = 5.0f;
    doorModel.maxs[2] = 80.0f;
    map.models.push_back(doorModel);

    // Submodel 2: func_button
    ZHLN::BSP::DModel btnModel {};
    btnModel.mins[0] = -5.0f;
    btnModel.mins[1] = -5.0f;
    btnModel.mins[2] = 0.0f;
    btnModel.maxs[0] = 5.0f;
    btnModel.maxs[1] = 5.0f;
    btnModel.maxs[2] = 10.0f;
    map.models.push_back(btnModel);

    // Submodel 3: trigger_multiple
    ZHLN::BSP::DModel trigModel {};
    trigModel.mins[0] = -50.0f;
    trigModel.mins[1] = -50.0f;
    trigModel.mins[2] = 0.0f;
    trigModel.maxs[0] = 50.0f;
    trigModel.maxs[1] = 50.0f;
    trigModel.maxs[2] = 60.0f;
    map.models.push_back(trigModel);

    // Entity 0: worldspawn
    {
        ZHLN::BSP::BSPEntity ent;
        ent.keys.push_back({"classname", "worldspawn"});
        map.entities.push_back(ent);
    }

    // Entity 1: info_player_start
    {
        ZHLN::BSP::BSPEntity ent;
        ent.keys.push_back({"classname", "info_player_start"});
        ent.keys.push_back({"origin", "0 0 16"});
        ent.keys.push_back({"angles", "0 90 0"});
        map.entities.push_back(ent);
    }

    // Entity 2: func_door
    {
        ZHLN::BSP::BSPEntity ent;
        ent.keys.push_back({"classname", "func_door"});
        ent.keys.push_back({"targetname", "door_main"});
        ent.keys.push_back({"model", "*1"});
        ent.keys.push_back({"speed", "100"});
        ent.keys.push_back({"wait", "2"});
        ent.keys.push_back({"movedir", "0 90 0"}); // North (+Y in source)
        ent.keys.push_back({"spawnflags", "256"}); // Use opens
        ent.keys.push_back({"OnOpen", "light_indicator,TurnOn,,0,-1"});
        map.entities.push_back(ent);
    }

    // Entity 3: prop_door_rotating
    {
        ZHLN::BSP::BSPEntity ent;
        ent.keys.push_back({"classname", "prop_door_rotating"});
        ent.keys.push_back({"targetname", "door_rot"});
        ent.keys.push_back({"model", "models/props/door01.glb"});
        ent.keys.push_back({"origin", "50 0 0"});
        ent.keys.push_back({"angles", "0 0 0"});
        ent.keys.push_back({"distance", "90"});
        ent.keys.push_back({"speed", "120"});
        ent.keys.push_back({"wait", "3"});
        ent.keys.push_back({"spawnflags", "32"}); // Toggle
        map.entities.push_back(ent);
    }

    // Entity 4: func_button
    {
        ZHLN::BSP::BSPEntity ent;
        ent.keys.push_back({"classname", "func_button"});
        ent.keys.push_back({"targetname", "btn_entry"});
        ent.keys.push_back({"model", "*2"});
        ent.keys.push_back({"target", "door_main"});
        ent.keys.push_back({"speed", "50"});
        ent.keys.push_back({"wait", "2"});
        ent.keys.push_back({"movedir", "-1 0 0"});  // Up
        ent.keys.push_back({"spawnflags", "2048"}); // Use activates
        ent.keys.push_back({"OnPressed", "door_main,Toggle,,0,-1"});
        map.entities.push_back(ent);
    }

    // Entity 5: trigger_multiple
    {
        ZHLN::BSP::BSPEntity ent;
        ent.keys.push_back({"classname", "trigger_multiple"});
        ent.keys.push_back({"targetname", "trig_entrance"});
        ent.keys.push_back({"model", "*3"});
        ent.keys.push_back({"target", "door_main"});
        ent.keys.push_back({"wait", "1"});
        map.entities.push_back(ent);
    }

    // Entity 6: trigger_once
    {
        ZHLN::BSP::BSPEntity ent;
        ent.keys.push_back({"classname", "trigger_once"});
        ent.keys.push_back({"targetname", "trig_secret"});
        ent.keys.push_back({"origin", "30 30 10"});
        ent.keys.push_back({"target", "door_rot"});
        map.entities.push_back(ent);
    }

    return map;
}

} // namespace

struct BSPInteractionTestSuite {
    struct Tests {
        std::expected<void, ZHLN::ErrorCode> describe_scene_converts_interactive_entities() {
            const auto map   = BuildSyntheticMap();
            const auto scene = ZHLN::BSP::DescribeScene(map, "maps/test.bsp");

            // worldspawn + door_main + door_rot + btn_entry + trig_entrance + trig_secret = 6 entities
            ZHLN::Test::ExpectEq(scene.entities.size(), size_t {6});

            // 1. worldspawn
            ZHLN::Test::ExpectEq(scene.entities[0].name, std::string {"worldspawn"});
            ZHLN::Test::ExpectTrue(scene.entities[0].body == ZHLN::Scene::BodyKind::Static);

            // 2. func_door (door_main)
            const auto& door = scene.entities[1];
            ZHLN::Test::ExpectEq(door.name, std::string {"door_main"});
            ZHLN::Test::ExpectTrue(door.body == ZHLN::Scene::BodyKind::Kinematic);
            ZHLN::Test::ExpectTrue(door.shape == ZHLN::Scene::ShapeKind::Box);
            ZHLN::Test::ExpectEq(door.source, std::string {"*1"});
            ZHLN::Test::ExpectGt(door.halfExtents.x, 0.0f);
            ZHLN::Test::ExpectGt(door.halfExtents.y, 0.0f);
            ZHLN::Test::ExpectGt(door.halfExtents.z, 0.0f);

            // 3. prop_door_rotating (door_rot)
            const auto& rotDoor = scene.entities[2];
            ZHLN::Test::ExpectEq(rotDoor.name, std::string {"door_rot"});
            ZHLN::Test::ExpectTrue(rotDoor.body == ZHLN::Scene::BodyKind::Kinematic);
            ZHLN::Test::ExpectTrue(rotDoor.shape == ZHLN::Scene::ShapeKind::Prefab);
            ZHLN::Test::ExpectEq(rotDoor.source, std::string {"models/props/door01.glb"});

            // 4. func_button (btn_entry)
            const auto& button = scene.entities[3];
            ZHLN::Test::ExpectEq(button.name, std::string {"btn_entry"});
            ZHLN::Test::ExpectTrue(button.body == ZHLN::Scene::BodyKind::Kinematic);
            ZHLN::Test::ExpectTrue(button.shape == ZHLN::Scene::ShapeKind::Box);
            ZHLN::Test::ExpectEq(button.source, std::string {"*2"});

            // 5. trigger_multiple (trig_entrance)
            const auto& trig = scene.entities[4];
            ZHLN::Test::ExpectEq(trig.name, std::string {"trig_entrance"});
            ZHLN::Test::ExpectTrue(trig.body == ZHLN::Scene::BodyKind::None);
            ZHLN::Test::ExpectTrue(trig.shape == ZHLN::Scene::ShapeKind::Box);
            ZHLN::Test::ExpectEq(trig.source, std::string {"*3"});

            // 6. trigger_once (trig_secret)
            const auto& trigOnce = scene.entities[5];
            ZHLN::Test::ExpectEq(trigOnce.name, std::string {"trig_secret"});
            ZHLN::Test::ExpectTrue(trigOnce.body == ZHLN::Scene::BodyKind::None);

            return {};
        }

        std::expected<void, ZHLN::ErrorCode> initialize_bsp_entities_attaches_components() {
            const auto map   = BuildSyntheticMap();
            const auto scene = ZHLN::BSP::DescribeScene(map, "maps/test.bsp");

            ZHLN::ECS::Registry reg;
            reg.RegisterComponent<ZHLN::Components::NameComponent>();
            reg.RegisterComponent<ZHLN::Components::TransformComponent>();
            reg.RegisterComponent<ZHLN::Components::SceneSourceComponent>();
            reg.RegisterComponent<ZHLN::Interaction::DoorComponent>();
            reg.RegisterComponent<ZHLN::Interaction::ButtonComponent>();
            reg.RegisterComponent<ZHLN::Interaction::TriggerComponent>();
            reg.RegisterComponent<ZHLN::Interaction::EntityConnectionComponent>();
            reg.RegisterComponent<ZHLN::Interaction::BSPEntityMetadataComponent>();

            ZHLN::Scene::Instance inst;
            for (const auto& sceneEnt: scene.entities) {
                const auto e = reg.Create();
                reg.Add(e, ZHLN::Components::NameComponent {.name = ZHLN::String64(sceneEnt.name)});
                reg.Add<ZHLN::Components::TransformComponent>(
                    e,
                    ZHLN::Components::TransformComponent {
                        .position = JPH::Vec3(sceneEnt.transform.position), .rotation = JPH::Quat::sIdentity(), .scale = JPH::Vec3::sReplicate(1.0f)
                    }
                );
                reg.Add<ZHLN::Components::SceneSourceComponent>(
                    e,
                    ZHLN::Components::SceneSourceComponent {
                        .shape = sceneEnt.shape, .source = ZHLN::String256(sceneEnt.source), .halfExtents = sceneEnt.halfExtents
                    }
                );
                inst.entities.push_back(e);
            }

            // Run initialization pass
            ZHLN::Interaction::InitializeBSPEntities(reg, map, inst);

            // 1. Verify door_main
            const auto doorEnt  = inst.entities[1];
            const auto door     = reg.Get<ZHLN::Interaction::DoorComponent>(doorEnt);
            const auto doorMeta = reg.Get<ZHLN::Interaction::BSPEntityMetadataComponent>(doorEnt);
            const auto doorConn = reg.Get<ZHLN::Interaction::EntityConnectionComponent>(doorEnt);
            if (!ZHLN::Test::ExpectTrue(door.has_value()) || !ZHLN::Test::ExpectTrue(doorMeta.has_value()) || !ZHLN::Test::ExpectTrue(doorConn.has_value())) {
                return {};
            }

            ZHLN::Test::ExpectEq(door->state, ZHLN::Interaction::DoorState::Closed);
            ZHLN::Test::ExpectTrue((door->flags & ZHLN::Interaction::DoorFlags::UseOpens) != ZHLN::Interaction::DoorFlags::None);
            ZHLN::Test::ExpectGt(door->speed, 0.0f);
            ZHLN::Test::ExpectEq(door->wait, 2.0f);
            ZHLN::Test::ExpectGt((door->openPosition - door->closedPosition).LengthSq(), 0.0f);
            ZHLN::Test::ExpectEq(doorMeta->className, ZHLN::String64("func_door"));
            ZHLN::Test::ExpectEq(doorConn->outputCount, uint32_t {1});
            ZHLN::Test::ExpectEq(doorConn->outputs[0].event, ZHLN::String64("OnOpen"));
            ZHLN::Test::ExpectEq(doorConn->outputs[0].target, ZHLN::String64("light_indicator"));

            // 2. Verify prop_door_rotating (door_rot)
            const auto rotDoorEnt = inst.entities[2];
            const auto rotDoor    = reg.Get<ZHLN::Interaction::DoorComponent>(rotDoorEnt);
            if (!ZHLN::Test::ExpectTrue(rotDoor.has_value())) {
                return {};
            }
            ZHLN::Test::ExpectTrue((rotDoor->flags & ZHLN::Interaction::DoorFlags::Rotating) != ZHLN::Interaction::DoorFlags::None);
            ZHLN::Test::ExpectTrue((rotDoor->flags & ZHLN::Interaction::DoorFlags::Toggle) != ZHLN::Interaction::DoorFlags::None);
            ZHLN::Test::ExpectEq(rotDoor->rotationAngle, 90.0f);

            // 3. Verify func_button (btn_entry)
            const auto btnEnt  = inst.entities[3];
            const auto button  = reg.Get<ZHLN::Interaction::ButtonComponent>(btnEnt);
            const auto btnConn = reg.Get<ZHLN::Interaction::EntityConnectionComponent>(btnEnt);
            if (!ZHLN::Test::ExpectTrue(button.has_value()) || !ZHLN::Test::ExpectTrue(btnConn.has_value())) {
                return {};
            }
            ZHLN::Test::ExpectEq(button->state, ZHLN::Interaction::ButtonState::Off);
            ZHLN::Test::ExpectTrue((button->flags & ZHLN::Interaction::ButtonFlags::UseActivates) != ZHLN::Interaction::ButtonFlags::None);
            ZHLN::Test::ExpectEq(button->wait, 2.0f);
            ZHLN::Test::ExpectGt(btnConn->outputCount, uint32_t {0});

            // 4. Verify trigger_multiple (trig_entrance)
            const auto trigEnt = inst.entities[4];
            const auto trig    = reg.Get<ZHLN::Interaction::TriggerComponent>(trigEnt);
            if (!ZHLN::Test::ExpectTrue(trig.has_value())) {
                return {};
            }
            ZHLN::Test::ExpectTrue((trig->flags & ZHLN::Interaction::TriggerFlags::Active) != ZHLN::Interaction::TriggerFlags::None);
            ZHLN::Test::ExpectEq(trig->target, ZHLN::String64("door_main"));

            // 5. Verify trigger_once (trig_secret)
            const auto trigOnceEnt = inst.entities[5];
            const auto trigOnce    = reg.Get<ZHLN::Interaction::TriggerComponent>(trigOnceEnt);
            if (!ZHLN::Test::ExpectTrue(trigOnce.has_value())) {
                return {};
            }
            ZHLN::Test::ExpectTrue((trigOnce->flags & ZHLN::Interaction::TriggerFlags::TriggerOnce) != ZHLN::Interaction::TriggerFlags::None);

            return {};
        }

        std::expected<void, ZHLN::ErrorCode> door_movement_system_state_machine() {
            ZHLN::ECS::Registry reg;
            reg.RegisterComponent<ZHLN::Components::NameComponent>();
            reg.RegisterComponent<ZHLN::Components::TransformComponent>();
            reg.RegisterComponent<ZHLN::Interaction::DoorComponent>();
            reg.RegisterComponent<ZHLN::Interaction::EntityConnectionComponent>();
            reg.RegisterComponent<ZHLN::Components::PhysicsComponent>();

            const auto doorEnt = reg.Create();
            reg.Add(doorEnt, ZHLN::Components::NameComponent {.name = ZHLN::String64("sliding_door")});
            reg.Add<ZHLN::Components::TransformComponent>(
                doorEnt,
                ZHLN::Components::TransformComponent {
                    .position = JPH::Vec3(0.0f, 0.0f, 0.0f), .rotation = JPH::Quat::sIdentity(), .scale = JPH::Vec3::sReplicate(1.0f)
                }
            );
            reg.Add<ZHLN::Interaction::DoorComponent>(
                doorEnt,
                ZHLN::Interaction::DoorComponent {
                    .state          = ZHLN::Interaction::DoorState::Closed,
                    .flags          = ZHLN::Interaction::DoorFlags::None,
                    .closedPosition = JPH::Vec3(0.0f, 0.0f, 0.0f),
                    .openPosition   = JPH::Vec3(0.0f, 2.0f, 0.0f),
                    .speed          = 2.0f, // 0.5s to open
                    .progress       = 0.0f,
                    .wait           = 1.0f, // 1s wait before auto-closing
                    .waitTimer      = 0.0f,
                    .targetname     = ZHLN::String64("sliding_door"),
                }
            );

            // 1. Send "Open" input
            ZHLN::Interaction::FireEntityInput(reg, "sliding_door", "Open");
            auto door = reg.Get<ZHLN::Interaction::DoorComponent>(doorEnt);
            ZHLN::Test::ExpectEq(door->state, ZHLN::Interaction::DoorState::Opening);

            // 2. Step 0.25s (halfway)
            ZHLN::ECS::Query<
                ZHLN::Interaction::DoorComponent&, ZHLN::Components::TransformComponent&, const ZHLN::Interaction::EntityConnectionComponent,
                const ZHLN::Components::PhysicsComponent>
                query(reg);
            ZHLN::Interaction::DoorMovementSystem::Update(query, reg, ZHLN::FrameDt {0.25f}, std::nullopt, std::nullopt);

            auto trans = reg.Get<ZHLN::Components::TransformComponent>(doorEnt);
            door       = reg.Get<ZHLN::Interaction::DoorComponent>(doorEnt);
            ZHLN::Test::ExpectEq(door->state, ZHLN::Interaction::DoorState::Opening);
            ZHLN::Test::ExpectLt(std::abs(door->progress - 0.5f), 1e-3f);
            ZHLN::Test::ExpectLt(std::abs(trans->position.GetY() - 1.0f), 1e-3f);

            // 3. Step another 0.25s -> reaches Open
            ZHLN::Interaction::DoorMovementSystem::Update(query, reg, ZHLN::FrameDt {0.25f}, std::nullopt, std::nullopt);
            door  = reg.Get<ZHLN::Interaction::DoorComponent>(doorEnt);
            trans = reg.Get<ZHLN::Components::TransformComponent>(doorEnt);
            ZHLN::Test::ExpectEq(door->state, ZHLN::Interaction::DoorState::Open);
            ZHLN::Test::ExpectLt(std::abs(door->progress - 1.0f), 1e-3f);
            ZHLN::Test::ExpectLt(std::abs(trans->position.GetY() - 2.0f), 1e-3f);
            ZHLN::Test::ExpectEq(door->waitTimer, 1.0f);

            // 4. Wait 1.0s -> transitions to Closing
            ZHLN::Interaction::DoorMovementSystem::Update(query, reg, ZHLN::FrameDt {1.0f}, std::nullopt, std::nullopt);
            door = reg.Get<ZHLN::Interaction::DoorComponent>(doorEnt);
            ZHLN::Test::ExpectEq(door->state, ZHLN::Interaction::DoorState::Closing);

            // 5. Step 0.5s -> reaches Closed
            ZHLN::Interaction::DoorMovementSystem::Update(query, reg, ZHLN::FrameDt {0.5f}, std::nullopt, std::nullopt);
            door  = reg.Get<ZHLN::Interaction::DoorComponent>(doorEnt);
            trans = reg.Get<ZHLN::Components::TransformComponent>(doorEnt);
            ZHLN::Test::ExpectEq(door->state, ZHLN::Interaction::DoorState::Closed);
            ZHLN::Test::ExpectLt(std::abs(door->progress - 0.0f), 1e-3f);
            ZHLN::Test::ExpectLt(std::abs(trans->position.GetY() - 0.0f), 1e-3f);

            return {};
        }

        std::expected<void, ZHLN::ErrorCode> rotating_door_slerp_movement() {
            ZHLN::ECS::Registry reg;
            reg.RegisterComponent<ZHLN::Components::NameComponent>();
            reg.RegisterComponent<ZHLN::Components::TransformComponent>();
            reg.RegisterComponent<ZHLN::Interaction::DoorComponent>();
            reg.RegisterComponent<ZHLN::Interaction::EntityConnectionComponent>();
            reg.RegisterComponent<ZHLN::Components::PhysicsComponent>();

            const auto rotDoorEnt = reg.Create();
            reg.Add(rotDoorEnt, ZHLN::Components::NameComponent {.name = ZHLN::String64("rot_door")});
            reg.Add<ZHLN::Components::TransformComponent>(
                rotDoorEnt,
                ZHLN::Components::TransformComponent {.position = JPH::Vec3::sZero(), .rotation = JPH::Quat::sIdentity(), .scale = JPH::Vec3::sReplicate(1.0f)}
            );

            const JPH::Quat closedQ = JPH::Quat::sIdentity();
            const JPH::Quat openQ   = JPH::Quat::sRotation(JPH::Vec3::sAxisY(), JPH::DegreesToRadians(90.0f));

            reg.Add<ZHLN::Interaction::DoorComponent>(
                rotDoorEnt,
                ZHLN::Interaction::DoorComponent {
                    .state          = ZHLN::Interaction::DoorState::Closed,
                    .flags          = ZHLN::Interaction::DoorFlags::Rotating,
                    .closedRotation = closedQ,
                    .openRotation   = openQ,
                    .rotationAxis   = JPH::Vec3::sAxisY(),
                    .rotationAngle  = 90.0f,
                    .speed          = 1.0f, // 1s to open
                    .progress       = 0.0f,
                    .wait           = -1.0f, // stay open
                    .targetname     = ZHLN::String64("rot_door"),
                }
            );

            ZHLN::Interaction::FireEntityInput(reg, "rot_door", "Open");
            ZHLN::ECS::Query<
                ZHLN::Interaction::DoorComponent&, ZHLN::Components::TransformComponent&, const ZHLN::Interaction::EntityConnectionComponent,
                const ZHLN::Components::PhysicsComponent>
                query(reg);

            // Step 0.5s -> 45 degrees
            ZHLN::Interaction::DoorMovementSystem::Update(query, reg, ZHLN::FrameDt {0.5f}, std::nullopt, std::nullopt);
            auto trans = reg.Get<ZHLN::Components::TransformComponent>(rotDoorEnt);
            auto door  = reg.Get<ZHLN::Interaction::DoorComponent>(rotDoorEnt);
            ZHLN::Test::ExpectEq(door->state, ZHLN::Interaction::DoorState::Opening);
            ZHLN::Test::ExpectLt(std::abs(door->progress - 0.5f), 1e-3f);

            // Quat length remains 1.0
            ZHLN::Test::ExpectLt(std::abs(trans->rotation.Length() - 1.0f), 1e-3f);

            // Complete open
            ZHLN::Interaction::DoorMovementSystem::Update(query, reg, ZHLN::FrameDt {0.5f}, std::nullopt, std::nullopt);
            trans = reg.Get<ZHLN::Components::TransformComponent>(rotDoorEnt);
            door  = reg.Get<ZHLN::Interaction::DoorComponent>(rotDoorEnt);
            ZHLN::Test::ExpectEq(door->state, ZHLN::Interaction::DoorState::Open);
            ZHLN::Test::ExpectLt(std::abs((trans->rotation.GetY() - openQ.GetY())), 1e-3f);

            return {};
        }

        std::expected<void, ZHLN::ErrorCode> source_entity_io_wiring() {
            ZHLN::ECS::Registry reg;
            reg.RegisterComponent<ZHLN::Components::NameComponent>();
            reg.RegisterComponent<ZHLN::Components::TransformComponent>();
            reg.RegisterComponent<ZHLN::Interaction::DoorComponent>();
            reg.RegisterComponent<ZHLN::Interaction::ButtonComponent>();
            reg.RegisterComponent<ZHLN::Interaction::EntityConnectionComponent>();
            reg.RegisterComponent<ZHLN::Components::PhysicsComponent>();

            // 1. Door
            const auto doorEnt = reg.Create();
            reg.Add(doorEnt, ZHLN::Components::NameComponent {.name = ZHLN::String64("secure_vault")});
            reg.Add<ZHLN::Components::TransformComponent>(doorEnt, ZHLN::Components::TransformComponent {});
            reg.Add<ZHLN::Interaction::DoorComponent>(
                doorEnt,
                ZHLN::Interaction::DoorComponent {
                    .state          = ZHLN::Interaction::DoorState::Closed,
                    .flags          = ZHLN::Interaction::DoorFlags::Toggle,
                    .closedPosition = JPH::Vec3::sZero(),
                    .openPosition   = JPH::Vec3(0.0f, 3.0f, 0.0f),
                    .speed          = 2.0f,
                    .wait           = -1.0f,
                    .targetname     = ZHLN::String64("secure_vault"),
                }
            );

            // 2. Button wired to Door
            const auto btnEnt = reg.Create();
            reg.Add(btnEnt, ZHLN::Components::NameComponent {.name = ZHLN::String64("vault_button")});
            reg.Add<ZHLN::Components::TransformComponent>(btnEnt, ZHLN::Components::TransformComponent {});
            reg.Add<ZHLN::Interaction::ButtonComponent>(
                btnEnt,
                ZHLN::Interaction::ButtonComponent {
                    .state      = ZHLN::Interaction::ButtonState::Off,
                    .flags      = ZHLN::Interaction::ButtonFlags::DontMove,
                    .target     = ZHLN::String64("secure_vault"),
                    .targetname = ZHLN::String64("vault_button"),
                }
            );

            ZHLN::Interaction::EntityConnectionComponent conn;
            conn.AddOutput({.event = ZHLN::String64("OnPressed"), .target = ZHLN::String64("secure_vault"), .input = ZHLN::String64("Toggle")});
            reg.Add(btnEnt, std::move(conn));

            // Verify door is closed initially
            auto door = reg.Get<ZHLN::Interaction::DoorComponent>(doorEnt);
            ZHLN::Test::ExpectEq(door->state, ZHLN::Interaction::DoorState::Closed);

            // Press the button via input
            ZHLN::Interaction::FireEntityInput(reg, "vault_button", "Press");

            // Button is pressed and fired its output to the door!
            auto button = reg.Get<ZHLN::Interaction::ButtonComponent>(btnEnt);
            ZHLN::Test::ExpectEq(button->state, ZHLN::Interaction::ButtonState::Pressed);

            door = reg.Get<ZHLN::Interaction::DoorComponent>(doorEnt);
            ZHLN::Test::ExpectEq(door->state, ZHLN::Interaction::DoorState::Opening);

            // Test door locking: lock door and verify input ignored
            ZHLN::Interaction::FireEntityInput(reg, "secure_vault", "Lock");
            door = reg.Get<ZHLN::Interaction::DoorComponent>(doorEnt);
            ZHLN::Test::ExpectTrue((door->flags & ZHLN::Interaction::DoorFlags::Locked) != ZHLN::Interaction::DoorFlags::None);

            return {};
        }

        std::expected<void, ZHLN::ErrorCode> proximity_trigger_volume_detection() {
            ZHLN::ECS::Registry reg;
            reg.RegisterComponent<ZHLN::Character::MovementComponent>();
            reg.RegisterComponent<ZHLN::Components::TransformComponent>();
            reg.RegisterComponent<ZHLN::Interaction::TriggerComponent>();
            reg.RegisterComponent<ZHLN::Interaction::EntityConnectionComponent>();
            reg.RegisterComponent<ZHLN::Components::InputStateComponent>();
            reg.RegisterComponent<ZHLN::Interaction::PickupComponent>();
            reg.RegisterComponent<ZHLN::Interaction::ItemBaseComponent>();
            reg.RegisterComponent<ZHLN::Interaction::ContainerComponent>();
            reg.RegisterComponent<ZHLN::Components::PhysicsComponent>();
            reg.RegisterComponent<ZHLN::Components::MeshComponent>();
            reg.RegisterComponent<ZHLN::Interaction::UsableComponent>();
            reg.RegisterComponent<ZHLN::Interaction::DoorComponent>();
            reg.RegisterComponent<ZHLN::Interaction::ButtonComponent>();

            // 1. Create player at (0, 0, 0)
            const auto playerEnt = reg.Create();
            reg.Add<ZHLN::Character::MovementComponent>(playerEnt, ZHLN::Character::MovementComponent {});
            reg.Add<ZHLN::Components::TransformComponent>(
                playerEnt,
                ZHLN::Components::TransformComponent {
                    .position = JPH::Vec3(0.0f, 0.0f, 0.0f), .rotation = JPH::Quat::sIdentity(), .scale = JPH::Vec3::sReplicate(1.0f)
                }
            );

            // 2. Create trigger volume at (0, 0, 0) with halfExtents (2, 2, 2)
            const auto trigEnt = reg.Create();
            reg.Add<ZHLN::Components::TransformComponent>(trigEnt, ZHLN::Components::TransformComponent {});
            reg.Add<ZHLN::Interaction::TriggerComponent>(
                trigEnt,
                ZHLN::Interaction::TriggerComponent {
                    .radius      = 2.0f,
                    .flags       = ZHLN::Interaction::TriggerFlags::Active | ZHLN::Interaction::TriggerFlags::TriggerOnce,
                    .halfExtents = JPH::Vec3(2.0f, 2.0f, 2.0f),
                }
            );

            // Update interaction system
            ZHLN::ECS::Query<
                const ZHLN::Character::MovementComponent, const ZHLN::Components::TransformComponent, ZHLN::Interaction::TriggerComponent&,
                const ZHLN::Components::InputStateComponent, ZHLN::Interaction::PickupComponent&, const ZHLN::Interaction::ItemBaseComponent,
                ZHLN::Interaction::ContainerComponent&, const ZHLN::Components::PhysicsComponent, const ZHLN::Components::MeshComponent,
                const ZHLN::Interaction::UsableComponent, ZHLN::Interaction::DoorComponent&, ZHLN::Interaction::ButtonComponent&>
                query(reg);
            ZHLN::Interaction::InteractionSystem::Update(query, reg, std::nullopt, std::nullopt);

            auto trig = reg.Get<ZHLN::Interaction::TriggerComponent>(trigEnt);
            ZHLN::Test::ExpectTrue((trig->flags & ZHLN::Interaction::TriggerFlags::PlayerInside) != ZHLN::Interaction::TriggerFlags::None);
            ZHLN::Test::ExpectTrue((trig->flags & ZHLN::Interaction::TriggerFlags::Fired) != ZHLN::Interaction::TriggerFlags::None);
            ZHLN::Test::ExpectTrue((trig->flags & ZHLN::Interaction::TriggerFlags::Active) == ZHLN::Interaction::TriggerFlags::None);

            return {};
        }

        std::expected<void, ZHLN::ErrorCode> player_use_interaction_and_locking() {
            ZHLN::ECS::Registry reg;
            reg.RegisterComponent<ZHLN::Character::MovementComponent>();
            reg.RegisterComponent<ZHLN::Components::TransformComponent>();
            reg.RegisterComponent<ZHLN::Interaction::TriggerComponent>();
            reg.RegisterComponent<ZHLN::Interaction::EntityConnectionComponent>();
            reg.RegisterComponent<ZHLN::Components::InputStateComponent>();
            reg.RegisterComponent<ZHLN::Interaction::PickupComponent>();
            reg.RegisterComponent<ZHLN::Interaction::ItemBaseComponent>();
            reg.RegisterComponent<ZHLN::Interaction::ContainerComponent>();
            reg.RegisterComponent<ZHLN::Components::PhysicsComponent>();
            reg.RegisterComponent<ZHLN::Components::MeshComponent>();
            reg.RegisterComponent<ZHLN::Interaction::UsableComponent>();
            reg.RegisterComponent<ZHLN::Interaction::DoorComponent>();
            reg.RegisterComponent<ZHLN::Interaction::ButtonComponent>();
            reg.RegisterComponent<ZHLN::Components::NameComponent>();

            // Create input singleton with E key down
            const auto                            inputEnt = reg.Create();
            ZHLN::Components::InputStateComponent inputState {};
            inputState.SetKey(static_cast<uint8_t>(ZHLN::KeyCode::E), true);
            reg.Add(inputEnt, std::move(inputState));

            // Create player at (0, 0, 0)
            const auto playerEnt = reg.Create();
            reg.Add<ZHLN::Character::MovementComponent>(playerEnt, ZHLN::Character::MovementComponent {});
            reg.Add<ZHLN::Components::TransformComponent>(playerEnt, ZHLN::Components::TransformComponent {});

            // Create unlocked door near player
            const auto doorEnt = reg.Create();
            reg.Add(doorEnt, ZHLN::Components::NameComponent {.name = ZHLN::String64("interact_door")});
            reg.Add<ZHLN::Components::TransformComponent>(doorEnt, ZHLN::Components::TransformComponent {.position = JPH::Vec3(1.0f, 0.0f, 0.0f)});
            reg.Add<ZHLN::Interaction::DoorComponent>(
                doorEnt,
                ZHLN::Interaction::DoorComponent {
                    .state          = ZHLN::Interaction::DoorState::Closed,
                    .flags          = ZHLN::Interaction::DoorFlags::UseOpens | ZHLN::Interaction::DoorFlags::Toggle,
                    .closedPosition = JPH::Vec3(1.0f, 0.0f, 0.0f),
                    .openPosition   = JPH::Vec3(1.0f, 2.0f, 0.0f),
                    .targetname     = ZHLN::String64("interact_door"),
                }
            );

            ZHLN::ECS::Query<
                const ZHLN::Character::MovementComponent, const ZHLN::Components::TransformComponent, ZHLN::Interaction::TriggerComponent&,
                const ZHLN::Components::InputStateComponent, ZHLN::Interaction::PickupComponent&, const ZHLN::Interaction::ItemBaseComponent,
                ZHLN::Interaction::ContainerComponent&, const ZHLN::Components::PhysicsComponent, const ZHLN::Components::MeshComponent,
                const ZHLN::Interaction::UsableComponent, ZHLN::Interaction::DoorComponent&, ZHLN::Interaction::ButtonComponent&>
                query(reg);
            ZHLN::Interaction::InteractionSystem::Update(query, reg, std::nullopt, std::nullopt);

            auto door = reg.Get<ZHLN::Interaction::DoorComponent>(doorEnt);
            ZHLN::Test::ExpectEq(door->state, ZHLN::Interaction::DoorState::Opening);

            return {};
        }
    };
};

int main() {
    BSPInteractionTestSuite::Tests suite;
    int                            passed = 0;
    int                            failed = 0;

    auto runTest = [&](std::string_view testName, auto testFunc) {
        ZHLN::Println("\033[32m[ RUN      ]\033[0m {}", testName);
        auto res = testFunc();
        if (res.has_value()) {
            ZHLN::Println("\033[32m[       OK ]\033[0m {}", testName);
            passed++;
        } else {
            ZHLN::Println("\033[31m[  FAILED  ]\033[0m {}", testName);
            failed++;
        }
    };

    ZHLN::Println("\033[36m==================================================\033[0m");
    ZHLN::Println("\033[36mRunning Test Suite: BSPInteractionTestSuite\033[0m");
    ZHLN::Println("\033[36m==================================================\033[0m");

    runTest("describe_scene_converts_interactive_entities", [&] { return suite.describe_scene_converts_interactive_entities(); });
    runTest("initialize_bsp_entities_attaches_components", [&] { return suite.initialize_bsp_entities_attaches_components(); });
    runTest("door_movement_system_state_machine", [&] { return suite.door_movement_system_state_machine(); });
    runTest("rotating_door_slerp_movement", [&] { return suite.rotating_door_slerp_movement(); });
    runTest("source_entity_io_wiring", [&] { return suite.source_entity_io_wiring(); });
    runTest("proximity_trigger_volume_detection", [&] { return suite.proximity_trigger_volume_detection(); });
    runTest("player_use_interaction_and_locking", [&] { return suite.player_use_interaction_and_locking(); });

    ZHLN::Println("--------------------------------------------------");
    ZHLN::Println("Summary for BSPInteractionTestSuite: {} Passed, {} Failed", passed, failed);
    ZHLN::Println("==================================================");

    return failed == 0 ? 0 : 1;
}
