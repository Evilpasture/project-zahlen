// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Zahlen/Components.hpp"
#include "Zahlen/CreativeWorksFactory.hpp"
#include "Zahlen/Engine.hpp"
#include "Zahlen/Input.hpp"
#include "Zahlen/Log.hpp"
#include "Zahlen/Profiler.hpp"
#include "Zahlen/Render.hpp"
#include "Zahlen/Window.hpp"
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <cmath>

import std;
import ZHLN.MainMenu;
import ZHLN.Weapons;
import ZHLN.CombatFX;
import ZHLN.BlacksiteState;
import ZHLN.PlayerController;
import ZHLN.EnemyAI;

#if defined(_WIN32)
#define GAMEPLAY_API extern "C" __declspec(dllexport)
#else
#define GAMEPLAY_API extern "C" [[gnu::visibility("default")]]
#endif

namespace Game {

using namespace ZHLN;

void StartGame(Engine* engine) {
    auto& state = BlacksiteState::GetSceneState();
    auto& reg   = engine->GetRegistry();
    auto& pc    = engine->GetPhysicsContext();
    auto& rc    = engine->GetRenderContext();

    ZHLN::Log("[Blacksite] Initializing FPS Tactical Sandbox...");

    if (auto settings = reg.GetEntitiesWith<Components::GlobalSettingsTagComponent>(); !settings.empty()) {
        if (auto* pp = reg.Get<Components::PostProcessSettingsComponent>(settings[0])) {
            pp->giMode            = 1;
            pp->ambientExposure   = 12.0f;
            pp->enableSSR         = 1;
            pp->enableRTR         = 0;
            pp->vignetteIntensity = 1.15f;
            pp->vignettePower     = 1.6f;
        }
    }

    state.concreteMat = HashAssetID("concrete_mat_asset");
    state.metalMat    = HashAssetID("metal_mat_asset");
    state.barrierMat  = HashAssetID("barrier_mat_asset");
    state.crateMat    = HashAssetID("crate_mat_asset");
    state.sandbagMat  = HashAssetID("sandbag_mat_asset");
    state.enemyMat    = HashAssetID("enemy_mat_asset");

    rc.RegisterGPUMaterial(state.concreteMat, CreativeWorksFactory::CreateBasicMaterial(rc).value());
    rc.RegisterGPUMaterial(state.metalMat, CreativeWorksFactory::CreateBasicMaterial(rc).value());
    rc.RegisterGPUMaterial(state.barrierMat, CreativeWorksFactory::CreateBasicMaterial(rc).value());
    rc.RegisterGPUMaterial(state.crateMat, CreativeWorksFactory::CreateBasicMaterial(rc).value());
    rc.RegisterGPUMaterial(state.sandbagMat, CreativeWorksFactory::CreateBasicMaterial(rc).value());

    auto enemyMaterial               = CreativeWorksFactory::CreateBasicMaterial(rc).value();
    enemyMaterial.baseColorFactor[0] = 0.28f;
    enemyMaterial.baseColorFactor[1] = 0.33f;
    enemyMaterial.baseColorFactor[2] = 0.26f;
    rc.RegisterGPUMaterial(state.enemyMat, enemyMaterial);

    state.tracerMat   = CreativeWorksFactory::CreateBasicMaterial(rc, true, true, true).value();
    state.particleMat = CreativeWorksFactory::CreateBasicMaterial(rc, true, true, true).value();

    AssetID groundMeshAsset = HashAssetID("ground_floor_mesh");
    if (!rc.GetGPUMesh(groundMeshAsset).has_value()) {
        rc.RegisterGPUMesh(groundMeshAsset, CreativeWorksFactory::CreateBox(rc, JPH::Vec3(100.0f, 0.1f, 100.0f)));
    }

    state.floorPlane = reg.Create();
    reg.Add(state.floorPlane, Components::TransformComponent {.position = JPH::Vec3(0.0f, -0.1f, 0.0f)});
    reg.Add(
        state.floorPlane, Components::PhysicsComponent {Physics::CreateRigidBody(
                              pc, Physics::GetOrCreateShape(pc, Physics::ShapeType::Plane, 0.0f, 1.0f, 0.0f, 0.0f), JPH::RVec3::sZero(), JPH::Quat::sIdentity(),
                              JPH::EMotionType::Static, 0
                          )}
    );
    reg.Add(state.floorPlane, Components::MeshComponent {.meshAsset = groundMeshAsset, .materialAsset = state.concreteMat, .cullRadius = 250.0f});
    reg.Add(state.floorPlane, Components::PBRComponent {.roughness = 0.92f, .metallic = 0.05f});

    state.sunLight = reg.Create();
    reg.Add(state.sunLight, Components::TransformComponent {.rotation = JPH::Quat::sRotation(JPH::Vec3::sAxisX(), -0.78f)});
    reg.Add(
        state.sunLight, Components::LightComponent {
                            .type        = LightType::Sun,
                            .color       = JPH::Vec3(1.0f, 0.98f, 0.95f),
                            .intensity   = 150.0f,
                            .radius      = 0.5f,
                            .direction   = JPH::Vec3(0.4f, -0.8f, 0.4f).Normalized(),
                            .range       = 500.0f,
                            .shadowLayer = -1
                        }
    );

    // Build compound level environment
    BlacksiteState::AddBox(engine, JPH::Vec3(0.0f, 0.0f, -6.0f), JPH::Vec3(14.0f, 5.0f, 10.0f), state.concreteMat);
    BlacksiteState::AddBox(engine, JPH::Vec3(-3.0f, 5.0f, -6.0f), JPH::Vec3(8.0f, 0.4f, 10.0f), state.barrierMat, true);
    BlacksiteState::AddBox(engine, JPH::Vec3(9.0f, 0.0f, -10.0f), JPH::Vec3(4.0f, 2.6f, 4.0f), state.concreteMat);

    BlacksiteState::AddBox(engine, JPH::Vec3(-16.0f, 0.0f, 4.0f), JPH::Vec3(6.0f, 2.6f, 2.5f), state.metalMat);
    BlacksiteState::AddBox(engine, JPH::Vec3(-16.0f, 2.6f, 4.0f), JPH::Vec3(6.0f, 2.6f, 2.5f), state.metalMat, true);
    BlacksiteState::AddBox(engine, JPH::Vec3(-9.0f, 0.0f, 12.0f), JPH::Vec3(2.5f, 2.6f, 6.0f), state.metalMat);
    BlacksiteState::AddBox(engine, JPH::Vec3(14.0f, 0.0f, 8.0f), JPH::Vec3(6.0f, 2.6f, 2.5f), state.metalMat);
    BlacksiteState::AddBox(engine, JPH::Vec3(20.0f, 0.0f, -4.0f), JPH::Vec3(2.5f, 2.6f, 6.0f), state.metalMat);
    BlacksiteState::AddBox(engine, JPH::Vec3(-22.0f, 0.0f, -12.0f), JPH::Vec3(6.0f, 2.6f, 2.5f), state.metalMat);

    BlacksiteState::AddBox(engine, JPH::Vec3(-6.0f, 0.0f, 4.0f), JPH::Vec3(6.0f, 1.15f, 0.9f), state.barrierMat);
    BlacksiteState::AddBox(engine, JPH::Vec3(6.0f, 0.0f, 4.0f), JPH::Vec3(6.0f, 1.15f, 0.9f), state.barrierMat);
    BlacksiteState::AddBox(engine, JPH::Vec3(0.0f, 0.0f, 14.0f), JPH::Vec3(6.0f, 1.15f, 0.9f), state.barrierMat);

    JPH::Vec3 spawnPos(0.0f, 1.5f, 24.0f);
    state.playerEnt = reg.Create();
    reg.Add(state.playerEnt, Components::PlayerTagComponent {});
    reg.Add(state.playerEnt, Components::TransformComponent {.position = spawnPos});
    reg.Add(state.playerEnt, Components::MovementComponent {});
    reg.Add(state.playerEnt, Components::InputComponent {});
    reg.Add(state.playerEnt, Components::PhysicsComponent {Physics::CreateCharacter(pc, JPH::RVec3(spawnPos))});
    reg.Add(state.playerEnt, Components::PhysicsStateComponent {.currPosition = spawnPos, .prevPosition = spawnPos});

    auto& p     = reg.Add(state.playerEnt, PlayerController::PlayerControllerComp {});
    p.baseYaw   = -90.0f;
    p.basePitch = 0.0f;

    state.weaponEntity = Weapons::CreateWeaponModel(engine, p.currentWeapon, state.metalMat, state.crateMat);

    if (auto camEnts = reg.GetEntitiesWith<Components::MainCameraTagComponent>(); !camEnts.empty()) {
        Entity camEnt    = camEnts[0];
        auto*  targetCam = reg.Get<Components::TargetCameraComponent>(camEnt);
        if (!targetCam)
            targetCam = &reg.Add(camEnt, Components::TargetCameraComponent {});

        // C++26 strictly requires initialization exactly in the struct's declared order
        *targetCam = Components::TargetCameraComponent {
            .target          = state.playerEnt,
            .distance        = 0.0f,
            .targetDistance  = 0.0f,
            .yaw             = -90.0f,
            .pitch           = 0.0f,
            .targetOffset    = JPH::Vec3(0.0f, PlayerController::PLAYER_EYE_OFFSET_Y, 0.0f),
            .stiffness       = 0.0f,
            .smoothTargetPos = spawnPos
        };
    }

    for (int i = 0; i < 5; ++i) {
        float randAngle = (static_cast<float>(i) / 5.0f) * 6.283f;
        EnemyAI::SpawnEnemy(
            engine, JPH::Vec3(std::cos(randAngle) * 18.0f, 0.0f, std::sin(randAngle) * 18.0f),
            (i % 3 == 0) ? Weapons::WeaponId::Shotgun : Weapons::WeaponId::Rifle
        );
    }

    uint32_t fontIdx = 0;
    for (Entity uiEnt: reg.GetEntitiesWith<Components::UISettingsComponent>()) {
        if (auto* uiSettings = reg.Get<Components::UISettingsComponent>(uiEnt)) {
            fontIdx = uiSettings->defaultFontAtlasIdx;
            break;
        }
    }

    state.hudVitalsBg = reg.Create();
    reg.Add(
        state.hudVitalsBg, Components::UIRectComponent {
                               .x              = 24.0f,
                               .y              = -80.0f,
                               .width          = 200.0f,
                               .height         = 14.0f,
                               .anchorMinX     = 0.0f,
                               .anchorMinY     = 1.0f,
                               .anchorMaxX     = 0.0f,
                               .anchorMaxY     = 1.0f,
                               .hierarchyDepth = 10
                           }
    );
    reg.Add(state.hudVitalsBg, Components::UIPanelComponent {.color = JPH::Vec4(0.12f, 0.12f, 0.16f, 0.65f)});

    state.hudVitalsBar = reg.Create();
    reg.Add(
        state.hudVitalsBar,
        Components::UIRectComponent {.parentEntity = state.hudVitalsBg, .x = 2.0f, .y = 2.0f, .width = 196.0f, .height = 10.0f, .hierarchyDepth = 11}
    );
    reg.Add(state.hudVitalsBar, Components::UIPanelComponent {.color = JPH::Vec4(0.35f, 0.95f, 0.45f, 0.95f)});

    state.hudAmmoText = reg.Create();
    reg.Add(
        state.hudAmmoText, Components::UIRectComponent {
                               .x              = -240.0f,
                               .y              = -85.0f,
                               .width          = 200.0f,
                               .height         = 40.0f,
                               .anchorMinX     = 1.0f,
                               .anchorMinY     = 1.0f,
                               .anchorMaxX     = 1.0f,
                               .anchorMaxY     = 1.0f,
                               .hierarchyDepth = 10
                           }
    );
    reg.Add(
        state.hudAmmoText,
        Components::TextComponent {.text = String256("30 / 210"), .scale = 1.25f, .color = JPH::Vec4(0.95f, 0.95f, 0.95f, 0.95f), .fontIndex = fontIdx}
    );

    state.hudCrosshair = reg.Create();
    reg.Add(
        state.hudCrosshair, Components::UIRectComponent {
                                .x              = -6.0f,
                                .y              = -8.0f,
                                .width          = 20.0f,
                                .height         = 20.0f,
                                .anchorMinX     = 0.5f,
                                .anchorMinY     = 0.5f,
                                .anchorMaxX     = 0.5f,
                                .anchorMaxY     = 0.5f,
                                .hierarchyDepth = 15
                            }
    );
    reg.Add(
        state.hudCrosshair,
        Components::TextComponent {.text = String256("+"), .scale = 1.5f, .color = JPH::Vec4(0.43f, 1.00f, 0.70f, 0.85f), .fontIndex = fontIdx}
    );

    state.hudWaveText = reg.Create();
    reg.Add(
        state.hudWaveText, Components::UIRectComponent {
                               .x              = 24.0f,
                               .y              = 20.0f,
                               .width          = 320.0f,
                               .height         = 40.0f,
                               .anchorMinX     = 0.0f,
                               .anchorMinY     = 0.0f,
                               .anchorMaxX     = 0.0f,
                               .anchorMaxY     = 0.0f,
                               .hierarchyDepth = 10
                           }
    );
    reg.Add(
        state.hudWaveText, Components::TextComponent {
                               .text = String256("WAVE 01 - HOSTILES: 5"), .scale = 1.0f, .color = JPH::Vec4(0.55f, 0.82f, 1.00f, 0.85f), .fontIndex = fontIdx
                           }
    );

    for (size_t i = 0; i < 5; ++i) {
        Entity kfEnt = reg.Create();
        reg.Add(
            kfEnt, Components::UIRectComponent {
                       .x              = -280.0f,
                       .y              = 20.0f + static_cast<float>(i) * 22.0f,
                       .width          = 260.0f,
                       .height         = 20.0f,
                       .anchorMinX     = 1.0f,
                       .anchorMinY     = 0.0f,
                       .anchorMaxX     = 1.0f,
                       .anchorMaxY     = 0.0f,
                       .hierarchyDepth = 10
                   }
        );
        reg.Add(kfEnt, Components::TextComponent {.text = String256(""), .scale = 0.95f, .color = JPH::Vec4(0.4f, 0.95f, 0.7f, 0.9f), .fontIndex = fontIdx});
        state.hudKillFeedTexts[i] = kfEnt;
    }

    state.gameStarted = true;
}

void GameRulesSystem(ZHLN::Engine* engine, float /*dt*/) {
    auto& state = ZHLN::BlacksiteState::GetSceneState();
    auto& reg   = engine->GetRegistry();

    std::vector<ZHLN::Entity> corpses;
    uint32_t                  aliveCount = 0;

    for (auto ent: state.enemies) {
        if (!reg.IsAlive(ent))
            continue;
        if (auto* enemy = reg.Get<ZHLN::EnemyAI::EnemyController>(ent)) {
            if (enemy->behavior.alive)
                aliveCount++;
            else
                corpses.push_back(ent);
        }
    }

    uint32_t corpseBudget = (aliveCount > 60) ? 4 : (aliveCount > 30) ? 7 : 12;
    if (corpses.size() > corpseBudget) {
        size_t toRemove = corpses.size() - corpseBudget;
        for (size_t i = 0; i < toRemove; ++i)
            reg.Destroy(corpses[i]);
    }

    if (state.kills >= state.wave * 6) {
        state.wave++;
        ZHLN::BlacksiteState::PushKillFeed(std::format("WAVE {:02d} INBOUND", state.wave), false);
    }
}

void CameraEffectsSystem(ZHLN::Engine* engine, float /*dt*/) {
    auto& state = ZHLN::BlacksiteState::GetSceneState();
    auto& reg   = engine->GetRegistry();

    if (state.playerEnt == ZHLN::NullEntity || !reg.IsAlive(state.playerEnt))
        return;
    auto* p    = reg.Get<ZHLN::PlayerController::PlayerControllerComp>(state.playerEnt);
    auto* move = reg.Get<ZHLN::Components::MovementComponent>(state.playerEnt);
    if (!p || !move)
        return;

    auto camEnts = reg.GetEntitiesWith<ZHLN::Components::MainCameraTagComponent>();
    if (camEnts.empty())
        return;
    if (auto* targetCam = reg.Get<ZHLN::Components::TargetCameraComponent>(camEnts[0])) {
        targetCam->vignetteIntensity = (p->health < 40.0f && p->alive) ? 1.4f + 0.35f * std::sin(p->totalTime * 6.0f) : 1.15f;
        targetCam->vignettePower     = (p->health < 40.0f && p->alive) ? 2.0f : 1.6f;
        targetCam->targetFov         = move->isSprinting ? 55.0f : 45.0f;
    }
}

void HUDSyncSystem(ZHLN::Engine* engine, float /*dt*/) {
    auto& state = ZHLN::BlacksiteState::GetSceneState();
    auto& reg   = engine->GetRegistry();

    if (state.playerEnt == ZHLN::NullEntity || !reg.IsAlive(state.playerEnt))
        return;
    auto* p = reg.Get<ZHLN::PlayerController::PlayerControllerComp>(state.playerEnt);
    if (!p)
        return;

    if (state.hudVitalsBar != ZHLN::NullEntity && reg.IsAlive(state.hudVitalsBar)) {
        if (auto* rect = reg.Get<ZHLN::Components::UIRectComponent>(state.hudVitalsBar)) {
            rect->width = 196.0f * (std::max(0.0f, p->health) / 100.0f);
        }
        if (auto* panel = reg.Get<ZHLN::Components::UIPanelComponent>(state.hudVitalsBar)) {
            panel->color = p->godMode          ? JPH::Vec4(1.0f, 0.85f, 0.4f, 0.95f) :
                           (p->health < 35.0f) ? JPH::Vec4(0.95f, 0.25f, 0.25f, 0.95f) :
                                                 JPH::Vec4(0.35f, 0.95f, 0.45f, 0.95f);
        }
    }

    if (state.hudAmmoText != ZHLN::NullEntity && reg.IsAlive(state.hudAmmoText)) {
        if (auto* text = reg.Get<ZHLN::Components::TextComponent>(state.hudAmmoText)) {
            auto& ammoState = p->ammo[static_cast<size_t>(p->currentWeapon)];
            text->text.assign(p->infiniteAmmo ? std::format("{} / INF", ammoState.mag) : std::format("{} / {}", ammoState.mag, ammoState.reserve));
            text->color = (ammoState.mag == 0) ? JPH::Vec4(0.95f, 0.3f, 0.3f, 0.95f) : JPH::Vec4(0.95f, 0.95f, 0.95f, 0.95f);
        }
    }

    if (state.hudWaveText != ZHLN::NullEntity && reg.IsAlive(state.hudWaveText)) {
        if (auto* text = reg.Get<ZHLN::Components::TextComponent>(state.hudWaveText)) {
            text->text.assign(
                state.hordeMode ? std::format("HORDE TARGET: {} - KILLS: {}", state.hordeTarget, state.kills) :
                                  std::format("WAVE {:02d} - HOSTILES: {}", state.wave, state.enemies.size())
            );
        }
    }

    for (size_t i = 0; i < 5; ++i) {
        if (Entity kfEnt = state.hudKillFeedTexts[i]; kfEnt != NullEntity && reg.IsAlive(kfEnt)) {
            if (auto* text = reg.Get<ZHLN::Components::TextComponent>(kfEnt)) {
                text->text.assign((i < state.killFeed.size()) ? state.killFeed[i].text : "");
                if (i < state.killFeed.size())
                    text->color = state.killFeed[i].head ? JPH::Vec4(0.95f, 0.35f, 0.35f, 0.95f) : JPH::Vec4(0.4f, 0.95f, 0.7f, 0.9f);
            }
        }
    }

    if (state.hudCrosshair != ZHLN::NullEntity && reg.IsAlive(state.hudCrosshair)) {
        if (auto* text = reg.Get<ZHLN::Components::TextComponent>(state.hudCrosshair)) {
            text->text.assign((p->ads > 0.25f) ? "." : "+");
            text->color = (p->ads > 0.25f) ? JPH::Vec4(1.0f, 0.2f, 0.2f, 0.85f) : JPH::Vec4(0.43f, 1.00f, 0.70f, 0.85f);
            text->scale = (p->ads > 0.25f) ? 2.0f : 1.5f;
        }
    }
}

} // namespace Game

GAMEPLAY_API ZHLN::GameplayStatus NativeGameplayUpdate(ZHLN::Engine* engine, float dt) {
    if (!engine)
        return ZHLN::GameplayStatus::Error;

    ZHLN_PROFILE_SCOPE("ECS System: Native Gameplay Update");

    static bool wasTabDown = false;
    bool        isTabDown  = engine->GetInput().IsKeyDown(ZHLN::KeyCode::Tab) || engine->GetInput().IsKeyDown(ZHLN::KeyCode::Escape);

    auto& state = ZHLN::BlacksiteState::GetSceneState();

    if (!state.gameStarted) {
        state.mainMenu.Build(
            engine,
            ZHLN::MenuConfig {
                .cameraPosition = JPH::Vec3(0.0f, 1.5f, 12.0f),
                .cameraYaw      = -90.0f,
                .cameraPitch    = 0.0f,
                .buttons        = {
                    ZHLN::MenuButtonDesc {
                        .text = "DEPLOY",
                        .onClick =
                            [](ZHLN::Engine* eng) {
                                eng->GetWindow().CaptureMouse(true);
                                Game::StartGame(eng);
                                ZHLN::BlacksiteState::GetSceneState().mainMenu.Destroy(eng);
                            },
                        .textX = 55.0f,
                        .textY = 25.0f
                    },
                    ZHLN::MenuButtonDesc {.text = "QUIT", .onClick = [](ZHLN::Engine* eng) { eng->GetWindow().Close(); }, .textX = 80.0f, .textY = 25.0f}
                }
            }
        );
    } else if (isTabDown && !wasTabDown) {
        if (state.mainMenu.IsActive()) {
            engine->GetWindow().CaptureMouse(true);
            state.mainMenu.Destroy(engine);
        } else {
            engine->GetWindow().CaptureMouse(false);
            state.mainMenu.Build(
                engine,
                ZHLN::MenuConfig {
                    .cameraPosition = engine->GetCamera().position,
                    .cameraYaw      = engine->GetCamera().yaw,
                    .cameraPitch    = engine->GetCamera().pitch,
                    .buttons        = {
                        ZHLN::MenuButtonDesc {
                            .text = "RESUME",
                            .onClick =
                                [](ZHLN::Engine* eng) {
                                    eng->GetWindow().CaptureMouse(true);
                                    ZHLN::BlacksiteState::GetSceneState().mainMenu.Destroy(eng);
                                },
                            .textX = 55.0f,
                            .textY = 25.0f
                        },
                        ZHLN::MenuButtonDesc {.text = "QUIT", .onClick = [](ZHLN::Engine* eng) { eng->GetWindow().Close(); }, .textX = 80.0f, .textY = 25.0f}
                    }
                }
            );
        }
    }
    wasTabDown = isTabDown;

    if (state.mainMenu.IsActive()) {
        state.mainMenu.Update(engine, dt);
        return ZHLN::GameplayStatus::OK;
    }

    if (state.gameStarted) {
        ZHLN::PlayerController::PlayerInputSystem(engine, dt);
        ZHLN::PlayerController::PlayerUpdateTick(engine, dt);
        ZHLN::EnemyAI::EnemyAISystem(engine, dt);
        ZHLN::CombatFX::ProcessRenderTick(engine, dt, state.tracers, state.shockwaves, state.particles, state.tracerMat, state.particleMat);

        Game::GameRulesSystem(engine, dt);
        Game::CameraEffectsSystem(engine, dt);
        Game::HUDSyncSystem(engine, dt);
    }

    return ZHLN::GameplayStatus::OK;
}
