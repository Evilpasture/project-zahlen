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

    auto settingsEntities = reg.GetEntitiesWith<Components::GlobalSettingsTagComponent>();
    if (!settingsEntities.empty()) {
        if (auto* pp = reg.Get<Components::PostProcessSettingsComponent>(settingsEntities[0])) {
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
        Mesh groundBox = CreativeWorksFactory::CreateBox(rc, JPH::Vec3(100.0f, 0.1f, 100.0f));
        rc.RegisterGPUMesh(groundMeshAsset, groundBox);
    }

    auto groundShape = Physics::GetOrCreateShape(pc, Physics::ShapeType::Plane, 0.0f, 1.0f, 0.0f, 0.0f);
    state.floorPlane = reg.Create();
    reg.Add(state.floorPlane, Components::TransformComponent {.position = JPH::Vec3(0.0f, -0.1f, 0.0f)});
    reg.Add(
        state.floorPlane,
        Components::PhysicsComponent {Physics::CreateRigidBody(pc, groundShape, JPH::RVec3::sZero(), JPH::Quat::sIdentity(), JPH::EMotionType::Static, 0)}
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
                            .direction   = JPH::Vec3(0.4f, 0.8f, 0.4f).Normalized(),
                            .range       = 500.0f,
                            .shadowLayer = -1
                        }
    );

    BlacksiteState::AddBox(engine, JPH::Vec3(0.0f, 0.0f, -6.0f), JPH::Vec3(14.0f, 5.0f, 10.0f), state.concreteMat);
    BlacksiteState::AddBox(engine, JPH::Vec3(-3.0f, 5.0f, -6.0f), JPH::Vec3(8.0f, 0.4f, 10.0f), state.barrierMat, true);
    BlacksiteState::AddBox(engine, JPH::Vec3(9.0f, 0.0f, -10.0f), JPH::Vec3(4.0f, 2.6f, 4.0f), state.concreteMat);

    BlacksiteState::AddBox(engine, JPH::Vec3(-16.0f, 0.0f, 4.0f), JPH::Vec3(6.0f, 2.6f, 2.5f), state.metalMat);
    BlacksiteState::AddBox(engine, JPH::Vec3(-16.0f, 2.6f, 4.0f), JPH::Vec3(6.0f, 2.6f, 2.5f), state.metalMat, true);
    BlacksiteState::AddBox(engine, JPH::Vec3(-9.0f, 0.0f, 12.0f), JPH::Vec3(2.5f, 2.6f, 6.0f), state.metalMat);
    BlacksiteState::AddBox(engine, JPH::Vec3(14.0f, 0.0f, 8.0f), JPH::Vec3(6.0f, 2.6f, 2.5f), state.metalMat);
    BlacksiteState::AddBox(engine, JPH::Vec3(20.0f, 0.0f, -4.0f), JPH::Vec3(2.5f, 2.6f, 6.0f), state.metalMat);
    BlacksiteState::AddBox(engine, JPH::Vec3(-22.0f, 0.0f, -12.0f), JPH::Vec3(6.0f, 2.6f, 2.5f), state.metalMat);

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

    auto camEnts = reg.GetEntitiesWith<Components::MainCameraTagComponent>();
    if (!camEnts.empty()) {
        Entity camEnt    = camEnts[0];
        auto*  targetCam = reg.Get<Components::TargetCameraComponent>(camEnt);
        if (!targetCam) {
            targetCam = &reg.Add(camEnt, Components::TargetCameraComponent {});
        }
        targetCam->target          = state.playerEnt;
        targetCam->distance        = 0.0f;
        targetCam->targetDistance  = 0.0f;
        targetCam->yaw             = -90.0f;
        targetCam->pitch           = 0.0f;
        targetCam->stiffness       = 0.0f;
        targetCam->targetOffset    = JPH::Vec3(0.0f, PlayerController::PLAYER_EYE_OFFSET_Y, 0.0f);
        targetCam->smoothTargetPos = spawnPos;
    }

    for (int i = 0; i < 5; ++i) {
        float             randAngle = (static_cast<float>(i) / 5.0f) * 6.283f;
        Weapons::WeaponId wId       = (i % 3 == 0) ? Weapons::WeaponId::Shotgun : Weapons::WeaponId::Rifle;
        EnemyAI::SpawnEnemy(engine, JPH::Vec3(std::cos(randAngle) * 18.0f, 0.0f, std::sin(randAngle) * 18.0f), wId);
    }

    uint32_t fontIdx = 0;
    for (Entity uiEnt: reg.GetEntitiesWith<Components::UISettingsComponent>()) {
        if (auto* uiSettings = reg.Get<Components::UISettingsComponent>(uiEnt)) {
            fontIdx = uiSettings->defaultFontAtlasIdx;
            break;
        }
    }

    state.hudVitalsBg     = reg.Create();
    auto& bgRect          = reg.Add(state.hudVitalsBg, Components::UIRectComponent {});
    bgRect.anchorMinX     = 0.0f;
    bgRect.anchorMaxX     = 0.0f;
    bgRect.anchorMinY     = 1.0f;
    bgRect.anchorMaxY     = 1.0f;
    bgRect.x              = 24.0f;
    bgRect.y              = -80.0f;
    bgRect.width          = 200.0f;
    bgRect.height         = 14.0f;
    bgRect.hierarchyDepth = 10;

    auto& bgPanel = reg.Add(state.hudVitalsBg, Components::UIPanelComponent {});
    bgPanel.color = JPH::Vec4(0.12f, 0.12f, 0.16f, 0.65f);

    state.hudVitalsBar     = reg.Create();
    auto& barRect          = reg.Add(state.hudVitalsBar, Components::UIRectComponent {});
    barRect.parentEntity   = state.hudVitalsBg;
    barRect.x              = 2.0f;
    barRect.y              = 2.0f;
    barRect.width          = 196.0f;
    barRect.height         = 10.0f;
    barRect.hierarchyDepth = 11;

    auto& barPanel = reg.Add(state.hudVitalsBar, Components::UIPanelComponent {});
    barPanel.color = JPH::Vec4(0.35f, 0.95f, 0.45f, 0.95f);

    state.hudAmmoText       = reg.Create();
    auto& ammoRect          = reg.Add(state.hudAmmoText, Components::UIRectComponent {});
    ammoRect.anchorMinX     = 1.0f;
    ammoRect.anchorMaxX     = 1.0f;
    ammoRect.anchorMinY     = 1.0f;
    ammoRect.anchorMaxY     = 1.0f;
    ammoRect.x              = -240.0f;
    ammoRect.y              = -85.0f;
    ammoRect.width          = 200.0f;
    ammoRect.height         = 40.0f;
    ammoRect.hierarchyDepth = 10;

    auto& ammoText = reg.Add(state.hudAmmoText, Components::TextComponent {});
    ammoText.text.assign("30 / 210");
    ammoText.scale     = 1.25f;
    ammoText.fontIndex = fontIdx;
    ammoText.color     = JPH::Vec4(0.95f, 0.95f, 0.95f, 0.95f);

    state.hudCrosshair    = reg.Create();
    auto& chRect          = reg.Add(state.hudCrosshair, Components::UIRectComponent {});
    chRect.anchorMinX     = 0.5f;
    chRect.anchorMaxX     = 0.5f;
    chRect.anchorMinY     = 0.5f;
    chRect.anchorMaxY     = 0.5f;
    chRect.x              = -6.0f;
    chRect.y              = -8.0f;
    chRect.width          = 20.0f;
    chRect.height         = 20.0f;
    chRect.hierarchyDepth = 15;

    auto& chText = reg.Add(state.hudCrosshair, Components::TextComponent {});
    chText.text.assign("+");
    chText.scale     = 1.5f;
    chText.fontIndex = fontIdx;
    chText.color     = JPH::Vec4(0.43f, 1.00f, 0.70f, 0.85f);

    state.hudWaveText       = reg.Create();
    auto& waveRect          = reg.Add(state.hudWaveText, Components::UIRectComponent {});
    waveRect.anchorMinX     = 0.0f;
    waveRect.anchorMaxX     = 0.0f;
    waveRect.anchorMinY     = 0.0f;
    waveRect.anchorMaxY     = 0.0f;
    waveRect.x              = 24.0f;
    waveRect.y              = 20.0f;
    waveRect.width          = 250.0f;
    waveRect.height         = 80.0f;
    waveRect.hierarchyDepth = 10;

    auto& waveText = reg.Add(state.hudWaveText, Components::TextComponent {});
    waveText.text.assign("WAVE 01");
    waveText.scale     = 1.0f;
    waveText.fontIndex = fontIdx;
    waveText.color     = JPH::Vec4(0.55f, 0.82f, 1.00f, 0.85f);

    state.gameStarted = true;
}

} // namespace Game

GAMEPLAY_API ZHLN::GameplayStatus NativeGameplayUpdate(ZHLN::Engine* engine, float dt) {
    if (!engine) {
        return ZHLN::GameplayStatus::Error;
    }

    ZHLN_PROFILE_SCOPE("ECS System: Native Gameplay Update");

    static bool wasTabDown = false;
    bool        isTabDown  = engine->GetInput().IsKeyDown(ZHLN::KeyCode::Tab) || engine->GetInput().IsKeyDown(ZHLN::KeyCode::Escape);

    auto& state = ZHLN::BlacksiteState::GetSceneState();

    if (!state.gameStarted) {
        ZHLN::MenuConfig cfg;
        cfg.titleLogoPrefab = "";
        cfg.themeMusicPath  = "";
        cfg.cameraPosition  = JPH::Vec3(0.0f, 1.5f, 12.0f);
        cfg.cameraYaw       = -90.0f;
        cfg.cameraPitch     = 0.0f;

        cfg.buttons.push_back(
            {.text = "DEPLOY",
             .onClick =
                 [](ZHLN::Engine* eng) {
                     eng->GetWindow().CaptureMouse(true);
                     Game::StartGame(eng);
                     ZHLN::BlacksiteState::GetSceneState().mainMenu.Destroy(eng);
                 },
             .textX = 55.0f,
             .textY = 25.0f}
        );

        cfg.buttons.push_back({.text = "QUIT", .onClick = [](ZHLN::Engine* eng) { eng->GetWindow().Close(); }, .textX = 80.0f, .textY = 25.0f});

        state.mainMenu.Build(engine, cfg);
    } else if (isTabDown && !wasTabDown) {
        if (state.mainMenu.IsActive()) {
            engine->GetWindow().CaptureMouse(true);
            state.mainMenu.Destroy(engine);
        } else {
            engine->GetWindow().CaptureMouse(false);
            ZHLN::MenuConfig cfg;
            cfg.cameraPosition = engine->GetCamera().position;
            cfg.cameraYaw      = engine->GetCamera().yaw;
            cfg.cameraPitch    = engine->GetCamera().pitch;

            cfg.buttons.push_back(
                {.text = "RESUME",
                 .onClick =
                     [](ZHLN::Engine* eng) {
                         eng->GetWindow().CaptureMouse(true);
                         ZHLN::BlacksiteState::GetSceneState().mainMenu.Destroy(eng);
                     },
                 .textX = 55.0f,
                 .textY = 25.0f}
            );

            cfg.buttons.push_back({.text = "QUIT", .onClick = [](ZHLN::Engine* eng) { eng->GetWindow().Close(); }, .textX = 80.0f, .textY = 25.0f});

            state.mainMenu.Build(engine, cfg);
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
    }

    return ZHLN::GameplayStatus::OK;
}
