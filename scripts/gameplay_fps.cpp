// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Zahlen/Audio.hpp"
#include "Zahlen/Components.hpp"
#include "Zahlen/CreativeWorksFactory.hpp"
#include "Zahlen/CreativeWorksManager.hpp"
#include "Zahlen/Engine.hpp"
#include "Zahlen/Input.hpp"
#include "Zahlen/Log.hpp"
#include "Zahlen/Math3D.hpp"
#include "Zahlen/Profiler.hpp" // Added to resolve ZHLN_PROFILE_SCOPE
#include "Zahlen/Window.hpp"   // Added to resolve complete type of Window
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/physics/Physics.hpp>

// Zahlen C++26 Module Imports
import ZHLN.MathUtils;
import ZHLN.Rig;
import ZHLN.FPS;
import ZHLN.Ragdoll;
import ZHLN.Animator;
import ZHLN.Actor;
import ZHLN.MainMenu;

#if defined(_WIN32)
#define GAMEPLAY_API extern "C" __declspec(dllexport)
#else
#define GAMEPLAY_API extern "C" [[gnu::visibility("default")]]
#endif

namespace Game {

using namespace ZHLN;

/**
 * @brief Standardized input structure for driving humanoid procedural animations.
 */
struct AnimInput {
    float speed;
    float crouch;
    float aimYaw;
    float aimPitch;
    float aiming;
};

/**
 * @brief Procedural Weapon Stance & IK State Tracker.
 */
class SoldierAnimator {
  public:
    float phase     = 0.0f;
    float breath    = 0.0f;
    float recoil    = 0.0f;
    float recoilVel = 0.0f;
    float reloadT   = -1.0f;
    float lowReady  = 1.0f;

    JPH::Vec3 weaponPos   = JPH::Vec3::sZero();
    JPH::Quat weaponQuat  = JPH::Quat::sIdentity();
    JPH::Vec3 muzzleWorld = JPH::Vec3::sZero();
    JPH::Vec3 aimDir      = JPH::Vec3::sAxisZ();

    void Fire(float power = 1.0f) {
        recoilVel += 3.6f * power;
    }
    void StartReload() {
        reloadT = 0.0f;
    }

    void Update(float dt, const AnimInput& in) {
        recoilVel += (-260.0f * recoil - 22.0f * recoilVel) * dt;
        recoil += recoilVel * dt;
        breath += dt;
        if (reloadT >= 0.0f) {
            reloadT += dt / 2.2f;
            if (reloadT >= 1.0f)
                reloadT = -1.0f;
        }
        lowReady = MathUtils::Damp(lowReady, 1.0f - in.aiming, 6.0f, dt);

        float stride = in.speed > 3.2f ? 1.85f : 1.25f;
        phase += (in.speed / stride) * dt;

        float pitch     = MathUtils::Clamp(in.aimPitch, -1.1f, 0.9f);
        weaponQuat      = MathUtils::EulerYXZ(pitch + lowReady * 0.62f, in.aimYaw, 0.0f);
        JPH::Quat kickQ = MathUtils::EulerYXZ(-recoil * 0.55f, recoil * 0.12f, recoil * 0.2f);
        weaponQuat      = (weaponQuat * kickQ).Normalized();

        JPH::Vec3 offset(-0.085f - lowReady * 0.02f, 0.22f - lowReady * 0.14f, 0.27f - lowReady * 0.02f - recoil * 0.05f);
        weaponPos = JPH::Vec3(0.0f, 1.4f, 0.0f) + weaponQuat * offset;

        muzzleWorld = weaponPos + weaponQuat * JPH::Vec3(0, 0.012f, 0.66f);
        aimDir      = weaponQuat * JPH::Vec3(0, 0, 1);
    }
};

/**
 * @brief Simple components to represent FPS properties in the ECS registry.
 */
struct GameplayComponents {
    struct PlayerController {
        FPS::Spring3D     weaponSpring;
        FPS::BobEvaluator cameraBob;
        FPS::SwaySolver   sway;
        float             totalTime = 0.0f;
    };

    struct EnemyController {
        Actor::StandardActor behavior;
        SoldierAnimator      anim; // Locally bound animator
        std::vector<Entity>  visualParts;
    };
};

struct BlacksiteSceneState {
    MainMenu mainMenu;
    bool     gameStarted = false;
    Entity   playerEnt   = NullEntity;
    Entity   floorPlane  = NullEntity;
    Entity   sunLight    = NullEntity;

    std::vector<Entity> enemies;
    std::vector<Entity> playerParts;
};

static BlacksiteSceneState g_State;

// ============================================================================
// DYNAMIC PROCEDURAL GENERATION & SPAWNING
// ============================================================================

void SpawnEnemy(Engine* engine, JPH::Vec3Arg position) {
    auto& reg = engine->GetRegistry();

    Entity enemyEnt = reg.Create();
    reg.Add(enemyEnt, Components::TransformComponent {.position = position});
    reg.Add(enemyEnt, Components::NameComponent {.name = String64("TacticalSoldier")});

    auto& enemy = reg.Add(enemyEnt, GameplayComponents::EnemyController {});
    enemy.behavior.SetPosition(position);

    // Load animated model
    CreativeWorksFactory::SpawnParams params;
    params.isAnimated    = true;
    params.createPhysics = false;

    enemy.visualParts.resize(32);
    uint32_t count = CreativeWorksFactory::InstantiatePrefab(*engine, "murderdrones/Uzi.glb", params, enemy.visualParts.data(), 32);
    enemy.visualParts.resize(count);

    if (!enemy.visualParts.empty()) {
        Entity meshRoot = enemy.visualParts[0];
        reg.Add(meshRoot, Components::HierarchyComponent {.parent = enemyEnt});
    }

    g_State.enemies.push_back(enemyEnt);
    ZHLN::Log("[Blacksite] Spawned procedural hostile at ({:.1f}, {:.1f})", position.GetX(), position.GetZ());
}

void StartGame(Engine* engine) {
    auto& reg = engine->GetRegistry();
    auto& pc  = engine->GetPhysicsContext();

    ZHLN::Log("[Blacksite] Initializing FPS Tactical Sandbox...");

    // Setup global lighting & post processing
    auto settingsEntities = reg.GetEntitiesWith<Components::GlobalSettingsTagComponent>();
    if (!settingsEntities.empty()) {
        if (auto* pp = reg.Get<Components::PostProcessSettingsComponent>(settingsEntities[0])) {
            pp->giMode            = 1; // SSAO
            pp->ambientExposure   = 12.0f;
            pp->enableSSR         = 1;
            pp->enableRTR         = 0;
            pp->vignetteIntensity = 1.15f;
            pp->vignettePower     = 1.6f;
        }
    }

    // 1. Spawning static physical ground
    auto groundShape   = Physics::GetOrCreateShape(pc, Physics::ShapeType::Plane, 0.0f, 1.0f, 0.0f, 0.0f);
    g_State.floorPlane = reg.Create();
    reg.Add(g_State.floorPlane, Components::TransformComponent {});
    reg.Add(
        g_State.floorPlane,
        Components::PhysicsComponent {Physics::CreateRigidBody(pc, groundShape, JPH::RVec3::sZero(), JPH::Quat::sIdentity(), JPH::EMotionType::Static, 0)}
    );

    // 2. Setup Sun Directional Light
    g_State.sunLight = reg.Create();
    reg.Add(g_State.sunLight, Components::TransformComponent {.rotation = JPH::Quat::sRotation(JPH::Vec3::sAxisX(), -0.78f)});
    reg.Add(
        g_State.sunLight, Components::LightComponent {
                              .type        = LightType::Sun,
                              .color       = JPH::Vec3(1.0f, 0.98f, 0.95f),
                              .intensity   = 150.0f,
                              .radius      = 0.5f,
                              .direction   = JPH::Vec3(0.0f, -0.707f, -0.707f),
                              .range       = 500.0f,
                              .shadowLayer = -1
                          }
    );

    // 3. Setup Player Capsule
    JPH::Vec3 spawnPos(0.0f, 1.5f, 5.0f);
    g_State.playerEnt = reg.Create();
    reg.Add(g_State.playerEnt, Components::PlayerTagComponent {});
    reg.Add(g_State.playerEnt, Components::TransformComponent {.position = spawnPos});
    reg.Add(g_State.playerEnt, Components::MovementComponent {});
    reg.Add(g_State.playerEnt, Components::InputComponent {});
    reg.Add(g_State.playerEnt, Components::PhysicsComponent {Physics::CreateCharacter(pc, JPH::RVec3(spawnPos))});
    reg.Add(g_State.playerEnt, Components::PhysicsStateComponent {.currPosition = spawnPos, .prevPosition = spawnPos});
    reg.Add(g_State.playerEnt, GameplayComponents::PlayerController {});

    auto camEnts = reg.GetEntitiesWith<Components::MainCameraTagComponent>();
    if (!camEnts.empty()) {
        Entity camEnt    = camEnts[0];
        auto*  targetCam = reg.Get<Components::TargetCameraComponent>(camEnt);
        if (!targetCam) {
            targetCam = &reg.Add(camEnt, Components::TargetCameraComponent {});
        }
        targetCam->target         = g_State.playerEnt;
        targetCam->distance       = 4.5f;
        targetCam->targetDistance = 4.5f;
        targetCam->yaw            = -90.0f;
        targetCam->pitch          = -10.0f;
        targetCam->stiffness      = 15.0f;
        targetCam->targetOffset   = JPH::Vec3(0.0f, 1.3f, 0.0f);
    }

    // 4. Spawn Hostile Actors
    SpawnEnemy(engine, JPH::Vec3(-4.0f, 0.0f, -10.0f));
    SpawnEnemy(engine, JPH::Vec3(4.0f, 0.0f, -12.0f));
    SpawnEnemy(engine, JPH::Vec3(0.0f, 0.0f, -18.0f));

    g_State.gameStarted = true;
}

// ============================================================================
// SYSTEM DISPATCHERS
// ============================================================================

void PlayerInputSystem(Engine* engine, [[maybe_unused]] float dt) {
    auto&       reg   = engine->GetRegistry();
    const auto& input = engine->GetInput();
    const auto& cam   = engine->GetCamera();

    for (Entity e: reg.GetEntitiesWith<Components::MovementComponent>()) {
        auto* move = reg.Get<Components::MovementComponent>(e);
        if (!move)
            continue;

        float yawRad   = JPH::DegreesToRadians(cam.yaw);
        float forwardX = std::cos(yawRad), forwardZ = std::sin(yawRad);
        float rightX = -std::sin(yawRad), rightZ = std::cos(yawRad);

        float moveX = 0.0f, moveZ = 0.0f;
        if (input.IsKeyDown(KeyCode::W)) {
            moveX += forwardX;
            moveZ += forwardZ;
        }
        if (input.IsKeyDown(KeyCode::S)) {
            moveX -= forwardX;
            moveZ -= forwardZ;
        }
        if (input.IsKeyDown(KeyCode::A)) {
            moveX -= rightX;
            moveZ -= rightZ;
        }
        if (input.IsKeyDown(KeyCode::D)) {
            moveX += rightX;
            moveZ += rightZ;
        }

        float len = std::sqrt(moveX * moveX + moveZ * moveZ);
        if (len > 0.01f) {
            move->inputX = moveX / len;
            move->inputZ = moveZ / len;
        } else {
            move->inputX = 0.0f;
            move->inputZ = 0.0f;
        }

        move->isSprinting = input.IsKeyDown(KeyCode::LShift) && (len > 0.01f);
        if (input.IsKeyDown(KeyCode::Space)) {
            move->jumpRequested = true;
        }
    }
}

void EnemyAISystem(Engine* engine, float dt) {
    auto& reg = engine->GetRegistry();
    auto& pc  = engine->GetPhysicsContext();

    for (Entity e: reg.GetEntitiesWith<GameplayComponents::EnemyController>()) {
        auto& enemy = *reg.Get<GameplayComponents::EnemyController>(e);
        if (!enemy.behavior.alive) {
            enemy.behavior.Update(dt, *(Actor::ActorContext*) nullptr); // Solely updates Verlet simulation
            continue;
        }

        // Build fully-decoupled Actor Context at frame boundaries
        Actor::ActorContext ctx;
        ctx.playerPos   = reg.Get<Components::TransformComponent>(g_State.playerEnt)->position;
        ctx.playerAlive = true;
        ctx.time        = static_cast<float>(engine->GetCurrentFrame()) * 0.0166f;
        ctx.floorY      = 0.0f;

        ctx.world.pointBlocked = []([[maybe_unused]] JPH::Vec3Arg pos, [[maybe_unused]] float radius) { return false; };
        ctx.world.lineOfSight  = [](JPH::Vec3Arg, JPH::Vec3Arg) { return true; };
        ctx.world.raycastWorld = [&](JPH::Vec3Arg origin, JPH::Vec3Arg direction, float maxDistance) -> std::optional<Actor::BodyHit> {
            // Corrected const-safety violation and bypassed redundant round-trip by passing e directly as ignore
            auto hit = Physics::Raycast(pc, JPH::RVec3(origin), direction, maxDistance, e);
            if (hit.hasHit) {
                return Actor::BodyHit {
                    .t      = hit.fraction * maxDistance,
                    .point  = JPH::Vec3(hit.position),
                    .normal = hit.normal,
                    .joint  = Rig::Joint::Hips,
                    .mult   = 1.0f,
                    .zone   = 2 // Limb
                };
            }
            return std::nullopt;
        };

        ctx.fx.playBeep = [&](float freq, float dur, float vol) { engine->GetAudioContext().PlayProceduralBeep(freq, dur, vol); };

        // Modular Stance & Animation Driver Hooks
        ctx.updateAnimation = [&enemy](
                                  float frameDt, [[maybe_unused]] float speed, [[maybe_unused]] float crouch, [[maybe_unused]] float aimYaw,
                                  [[maybe_unused]] float aimPitch, [[maybe_unused]] float aiming
                              ) -> Actor::WeaponStance {
            AnimInput ai = {.speed = speed, .crouch = crouch, .aimYaw = aimYaw, .aimPitch = aimPitch, .aiming = aiming};
            enemy.anim.Update(frameDt, ai);

            return Actor::WeaponStance {
                .position = enemy.anim.weaponPos, .rotation = enemy.anim.weaponQuat, .muzzleWorld = enemy.anim.muzzleWorld, .aimDir = enemy.anim.aimDir
            };
        };

        ctx.fx.fireWeapon = [&enemy]() { enemy.anim.Fire(1.0f); };

        ctx.fx.reloadWeapon = [&enemy]() { enemy.anim.StartReload(); };

        enemy.behavior.Update(dt, ctx);
    }
}

} // namespace Game

GAMEPLAY_API ZHLN::GameplayStatus NativeGameplayUpdate(ZHLN::Engine* engine, float dt) {
    if (!engine) {
        return ZHLN::GameplayStatus::Error;
    }

    ZHLN_PROFILE_SCOPE("ECS System: Native Gameplay Update");

    if (!Game::g_State.gameStarted) {
        ZHLN::MenuConfig cfg;
        cfg.titleLogoPrefab = "TADCLogo.glb";
        cfg.logoPosition    = JPH::RVec3(0.0f, 0.0f, -5.0f);
        cfg.themeMusicPath  = "resources/assets/audio/theme.mp3";
        cfg.cameraPosition  = JPH::Vec3(0.0f, 1.5f, 12.0f);
        cfg.cameraYaw       = -90.0f;
        cfg.cameraPitch     = 0.0f;

        cfg.buttons.push_back(
            {.text = "START GAME",
             .onClick =
                 [](ZHLN::Engine* eng) {
                     Game::StartGame(eng);
                     Game::g_State.mainMenu.Destroy(eng);
                 },
             .textX = 55.0f,
             .textY = 25.0f}
        );

        cfg.buttons.push_back({.text = "QUIT", .onClick = [](ZHLN::Engine* eng) { eng->GetWindow().Close(); }, .textX = 80.0f, .textY = 25.0f});

        Game::g_State.mainMenu.Build(engine, cfg);
    }

    if (Game::g_State.mainMenu.IsActive()) {
        Game::g_State.mainMenu.Update(engine, dt);
        return ZHLN::GameplayStatus::OK;
    }

    if (Game::g_State.gameStarted) {
        Game::PlayerInputSystem(engine, dt);
        Game::EnemyAISystem(engine, dt);
    }

    return ZHLN::GameplayStatus::OK;
}
