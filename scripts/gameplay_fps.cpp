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
#include "Zahlen/Profiler.hpp"
#include "Zahlen/Render.hpp"
#include "Zahlen/Window.hpp"
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

import std;

#if defined(_WIN32)
#define GAMEPLAY_API extern "C" __declspec(dllexport)
#else
#define GAMEPLAY_API extern "C" [[gnu::visibility("default")]]
#endif

namespace Game {

using namespace ZHLN;

// ============================================================================
// CONSTANTS & CONFIGURATIONS
// ============================================================================
constexpr float HIP_FOV      = 78.0f;
constexpr float ADS_FOV      = 42.0f;
constexpr float SIGHT_HEIGHT = 0.079f;
constexpr float SIGHT_Z      = 0.125f;
constexpr float ADS_SIGHT_Z  = -0.285f;

// ============================================================================
// PROCEDURAL AUDIO SYNTHESIZERS
// ============================================================================

inline void PlaySound_Shoot(Engine* engine, float dist) {
    float atten = std::max(0.08f, 1.0f - dist / 55.0f);
    auto& audio = engine->GetAudioContext();
    audio.PlayProceduralBeep(180.0f, 0.16f, 0.35f * atten);
    audio.PlayProceduralBeep(1400.0f, 0.14f, 0.50f * atten);
}

inline void PlaySound_Impact(Engine* engine, float dist) {
    float atten = std::max(0.05f, 1.0f - dist / 45.0f);
    engine->GetAudioContext().PlayProceduralBeep(2600.0f, 0.07f, 0.25f * atten);
}

inline void PlaySound_Flesh(Engine* engine, float dist) {
    float atten = std::max(0.05f, 1.0f - dist / 45.0f);
    engine->GetAudioContext().PlayProceduralBeep(420.0f, 0.10f, 0.40f * atten);
}

inline void PlaySound_Hitmark(Engine* engine) {
    engine->GetAudioContext().PlayProceduralBeep(1500.0f, 0.05f, 0.14f);
}

inline void PlaySound_Kill(Engine* engine) {
    engine->GetAudioContext().PlayProceduralBeep(700.0f, 0.18f, 0.18f);
}

inline void PlaySound_Hurt(Engine* engine) {
    auto& audio = engine->GetAudioContext();
    audio.PlayProceduralBeep(120.0f, 0.30f, 0.30f);
    audio.PlayProceduralBeep(260.0f, 0.25f, 0.50f);
}

inline void PlaySound_Step(Engine* engine) {
    engine->GetAudioContext().PlayProceduralBeep(900.0f, 0.06f, 0.09f);
}

inline void PlaySound_Empty(Engine* engine) {
    engine->GetAudioContext().PlayProceduralBeep(4200.0f, 0.03f, 0.20f);
}

inline void PlaySound_Reload(Engine* engine) {
    auto& audio = engine->GetAudioContext();
    audio.PlayProceduralBeep(3000.0f, 0.06f, 0.30f);
    audio.PlayProceduralBeep(1800.0f, 0.08f, 0.30f);
    audio.PlayProceduralBeep(2600.0f, 0.06f, 0.35f);
}

// ============================================================================
// PARTICLE & FX STRUCTURES
// ============================================================================

struct VisualParticle {
    JPH::Vec3 position;
    JPH::Vec3 velocity;
    JPH::Vec4 color;
    float     size;
    float     life;
    float     maxLife;
    float     drag;
    float     gravity;
};

struct BulletTracer {
    JPH::Vec3 start;
    JPH::Vec3 direction;
    float     speed;
    float     length;
    float     totalDistance;
    float     traveled;
    JPH::Vec3 color;
};

struct AnimInput {
    float speed;
    float crouch;
    float aimYaw;
    float aimPitch;
    float aiming;
};

// ============================================================================
// PROCEDURAL WEAPON STANCE & ANIMATOR
// ============================================================================

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

// ============================================================================
// ECS COMPONENT ABSTRACTIONS
// ============================================================================

struct GameplayComponents {
    struct PlayerController {
        FPS::Spring3D     weaponSpring;
        FPS::Spring3D     swaySpring;
        FPS::Spring1D     pitchRecoil;
        FPS::Spring1D     yawRecoil;
        FPS::Spring1D     kickSpring;
        FPS::BobEvaluator bobber;
        FPS::SwaySolver   sway;

        float baseYaw    = -90.0f; // Base FPS view angles
        float basePitch  = 0.0f;
        float totalTime  = 0.0f;
        float bobPhase   = 0.0f;
        float bobAmt     = 0.0f;
        float landDip    = 0.0f;
        float landVel    = 0.0f;
        float stepPhase  = 0.0f;
        float lastHeight = 1.5f;

        float health    = 100.0f;
        float maxHealth = 100.0f;
        bool  alive     = true;

        int32_t mag        = 30;
        int32_t magSize    = 30;
        int32_t reserve    = 210;
        float   reloading  = 0.0f;
        float   fireCd     = 0.0f;
        int32_t shotsFired = 0;
        float   ads        = 0.0f;

        float hurtFlash      = 0.0f;
        float hurtDir        = 0.0f;
        float hitMarkerTime  = 0.0f;
        float headMarkerTime = 0.0f;
    };

    struct EnemyController {
        Actor::StandardActor behavior;
        SoldierAnimator      anim;
        std::vector<Entity>  limbEntities;
        Entity               weaponEntity = NullEntity;
        float                phase        = 0.0f;
        float                breath       = 0.0f;
        float                recoil       = 0.0f;
        float                recoilVel    = 0.0f;
        float                reloadT      = -1.0f;
        float                lowReady     = 1.0f;
    };
};

struct BlacksiteSceneState {
    MainMenu mainMenu;
    bool     gameStarted = false;

    Entity playerEnt    = NullEntity;
    Entity weaponEntity = NullEntity;
    Entity floorPlane   = NullEntity;
    Entity sunLight     = NullEntity;

    MaterialID concreteMat = 0;
    MaterialID crateMat    = 0;
    MaterialID barrierMat  = 0;
    MaterialID sandbagMat  = 0;
    MaterialID metalMat    = 0;
    MaterialID enemyMat    = 0;

    Material tracerMat;
    Material particleMat;

    Entity hudVitalsBg  = NullEntity;
    Entity hudVitalsBar = NullEntity;
    Entity hudAmmoText  = NullEntity;
    Entity hudCrosshair = NullEntity;
    Entity hudWaveText  = NullEntity;

    std::vector<Entity>         worldEntities;
    std::vector<Entity>         enemies;
    std::vector<VisualParticle> particles;
    std::vector<BulletTracer>   tracers;

    uint32_t score     = 0;
    uint32_t kills     = 0;
    uint32_t headshots = 0;
    uint32_t wave      = 1;
    float    waveTimer = 0.0f;
};

static BlacksiteSceneState g_State;

// ============================================================================
// LEVEL DESIGN & PROCEDURAL COLLIDER POPULATION
// ============================================================================

void AddBox(Engine* engine, JPH::Vec3 pos, JPH::Vec3 size, MaterialID mat, bool solid = true) {
    auto& reg = engine->GetRegistry();
    auto& pc  = engine->GetPhysicsContext();

    Entity e = reg.Create();
    reg.Add(e, Components::TransformComponent {.position = pos, .rotation = JPH::Quat::sIdentity(), .scale = size});
    reg.Add(e, Components::NameComponent {.name = String64("StaticCover")});

    AssetID unitBoxAsset = HashAssetID("unit_box");
    reg.Add(e, Components::MeshComponent {.meshAsset = unitBoxAsset, .materialAsset = mat, .cullRadius = size.Length() * 0.5f});
    reg.Add(e, Components::PBRComponent {.roughness = 0.86f, .metallic = 0.02f});

    if (solid) {
        auto boxShape = Physics::GetOrCreateShape(pc, Physics::ShapeType::Box, size.GetX() * 0.5f, size.GetY() * 0.5f, size.GetZ() * 0.5f);
        reg.Add(e, Components::PhysicsComponent {Physics::CreateRigidBody(pc, boxShape, JPH::RVec3(pos), JPH::Quat::sIdentity(), JPH::EMotionType::Static, 0)});
    }

    g_State.worldEntities.push_back(e);
}

Entity CreateProceduralRifle(Engine* engine, MaterialID mat) {
    auto& reg = engine->GetRegistry();
    auto& rc  = engine->GetRenderContext();

    Entity weaponRoot = reg.Create();
    reg.Add(weaponRoot, Components::TransformComponent {});
    reg.Add(weaponRoot, Components::NameComponent {.name = String64("ProceduralRifle")});

    AssetID unitBoxAsset = HashAssetID("unit_box");
    if (!rc.GetGPUMesh(unitBoxAsset).has_value()) {
        Mesh box = CreativeWorksFactory::CreateBox(rc, JPH::Vec3(0.5f, 0.5f, 0.5f));
        rc.RegisterGPUMesh(unitBoxAsset, box);
    }

    auto AddPart = [&](JPH::Vec3 pos, JPH::Vec3 size, JPH::Quat rot = JPH::Quat::sIdentity()) {
        Entity part = reg.Create();
        reg.Add(part, Components::TransformComponent {.position = pos, .rotation = rot, .scale = size});
        reg.Add(part, Components::MeshComponent {.meshAsset = unitBoxAsset, .materialAsset = mat, .cullRadius = 2.0f});
        reg.Add(part, Components::HierarchyComponent {.parent = weaponRoot});
        reg.Add(part, Components::NameComponent {.name = String64("RiflePart")});
    };

    float scale = 0.85f;
    AddPart(JPH::Vec3(0.000f, 0.000f, 0.060f) * scale, JPH::Vec3(0.058f, 0.088f, 0.460f) * scale);
    AddPart(JPH::Vec3(0.000f, 0.010f, 0.420f) * scale, JPH::Vec3(0.034f, 0.034f, 0.340f) * scale);
    AddPart(JPH::Vec3(0.000f, 0.010f, 0.620f) * scale, JPH::Vec3(0.044f, 0.044f, 0.060f) * scale);
    AddPart(JPH::Vec3(0.000f, -0.110f, 0.020f) * scale, JPH::Vec3(0.048f, 0.160f, 0.088f) * scale, JPH::Quat::sRotation(JPH::Vec3::sAxisX(), 0.25f));
    AddPart(JPH::Vec3(0.000f, -0.080f, -0.300f) * scale, JPH::Vec3(0.044f, 0.100f, 0.070f) * scale);
    AddPart(JPH::Vec3(0.000f, 0.004f, -0.240f) * scale, JPH::Vec3(0.048f, 0.082f, 0.240f) * scale);
    AddPart(JPH::Vec3(0.000f, 0.050f, 0.125f) * scale, JPH::Vec3(0.026f, 0.038f, 0.055f) * scale);

    return weaponRoot;
}

// ============================================================================
// PARTICLE GENERATOR
// ============================================================================

void SpawnImpactParticles(const JPH::Vec3& point, const JPH::Vec3& normal, uint32_t materialType) {
    std::mt19937                          gen(std::random_device {}());
    std::uniform_real_distribution<float> randomDist(-1.0f, 1.0f);

    if (materialType == 1) { // Blood
        JPH::Vec4 bloodColor(0.61f, 0.11f, 0.11f, 0.95f);
        for (int i = 0; i < 12; ++i) {
            VisualParticle p;
            p.position = point;
            p.velocity = normal * (1.5f + (randomDist(gen) + 1.0f) * 1.75f) +
                         JPH::Vec3(randomDist(gen) * 1.3f, (randomDist(gen) + 1.0f) * 0.9f, randomDist(gen) * 1.3f);
            p.color    = bloodColor;
            p.size     = 0.055f + (randomDist(gen) + 1.0f) * 0.025f;
            p.life     = 0.55f + (randomDist(gen) + 1.0f) * 0.2f;
            p.maxLife  = p.life;
            p.drag     = 1.2f;
            p.gravity  = -11.0f;
            g_State.particles.push_back(p);
        }
    } else { // Sparks
        JPH::Vec4 sparkColor(1.0f, 0.81f, 0.56f, 1.0f);
        int       count = (materialType == 2) ? 12 : 7;
        for (int i = 0; i < count; ++i) {
            VisualParticle p;
            p.position = point;
            p.velocity = normal * (2.0f + (randomDist(gen) + 1.0f) * 2.5f) +
                         JPH::Vec3(randomDist(gen) * 2.0f, (randomDist(gen) + 1.0f) * 1.25f, randomDist(gen) * 2.0f);
            p.color    = sparkColor;
            p.size     = 0.03f + (randomDist(gen) + 1.0f) * 0.015f;
            p.life     = 0.25f + (randomDist(gen) + 1.0f) * 0.15f;
            p.maxLife  = p.life;
            p.drag     = 1.5f;
            p.gravity  = -14.0f;
            g_State.particles.push_back(p);
        }
    }
}

// ============================================================================
// WEAPON FIRE LOGIC
// ============================================================================

void ProcessPlayerWeaponFire(Engine* engine, GameplayComponents::PlayerController& p) {
    if (p.mag <= 0 || p.reloading > 0.0f || p.fireCd > 0.0f) {
        if (p.mag <= 0 && p.reloading <= 0.0f && p.fireCd <= 0.0f) {
            PlaySound_Empty(engine);
            p.fireCd = 0.25f;
        }
        return;
    }

    p.mag--;
    p.shotsFired++;
    p.fireCd = 0.086f;

    float adsScale = 1.0f - p.ads * 0.35f;
    float growth   = std::min(1.0f, static_cast<float>(p.shotsFired) / 9.0f);
    p.pitchRecoil.ApplyImpulse((0.55f + growth * 0.5f) * adsScale);
    p.yawRecoil.ApplyImpulse((((std::rand() % 100) / 100.0f) - 0.45f) * (0.5f + growth * 0.9f) * adsScale);
    p.kickSpring.ApplyImpulse(1.9f);

    auto& rc  = engine->GetRenderContext();
    auto& pc  = engine->GetPhysicsContext();
    auto& reg = engine->GetRegistry();
    auto& cam = engine->GetCamera();

    JPH::Vec3 origin = cam.position;

    float     spread   = (0.005f + (p.bobAmt * 0.028f) + std::min(1.0f, p.shotsFired / 10.0f) * 0.02f) * (1.0f - p.ads * 0.8f);
    float     yawRad   = JPH::DegreesToRadians(cam.yaw);
    float     pitchRad = JPH::DegreesToRadians(cam.pitch);
    JPH::Vec3 baseDir(JPH::Cos(yawRad) * JPH::Cos(pitchRad), JPH::Sin(pitchRad), JPH::Sin(yawRad) * JPH::Cos(pitchRad));

    std::random_device                    rd;
    std::mt19937                          gen(rd());
    std::uniform_real_distribution<float> dis(-1.0f, 1.0f);

    JPH::Vec3 dir = baseDir.Normalized() + JPH::Vec3(dis(gen) * spread, dis(gen) * spread, dis(gen) * spread);
    dir           = dir.Normalized();

    float                         bestT  = 220.0f;
    Entity                        victim = NullEntity;
    std::optional<Actor::BodyHit> bestHit;

    Entity ignorePhys = NullEntity;
    if (g_State.playerEnt != NullEntity && reg.IsAlive(g_State.playerEnt)) {
        if (auto* phys = reg.Get<Components::PhysicsComponent>(g_State.playerEnt)) {
            ignorePhys = phys->physicsHandle;
        }
    }

    // 1. Raycast World static colliders
    auto worldHit = Physics::Raycast(pc, JPH::RVec3(origin), dir, bestT, ignorePhys);
    if (worldHit.hasHit) {
        bestT = static_cast<float>(worldHit.fraction) * bestT;
    }

    // 2. Raycast Dynamic Ragdoll Skeletons
    for (Entity enemyEnt: g_State.enemies) {
        if (!reg.IsAlive(enemyEnt))
            continue;
        auto* enemy = reg.Get<GameplayComponents::EnemyController>(enemyEnt);
        if (enemy) {
            auto hit = enemy->behavior.Raycast(origin, dir, bestT);
            if (hit && hit->t < bestT) {
                bestT   = hit->t;
                victim  = enemyEnt;
                bestHit = hit;
            }
        }
    }

    JPH::Vec3 endPoint = origin + dir * bestT;

    // 3. Emit volumetric projectile tracers
    BulletTracer tracer;
    tracer.start         = cam.position;
    tracer.direction     = (endPoint - cam.position).Normalized();
    tracer.speed         = 320.0f;
    tracer.length        = 3.2f;
    tracer.totalDistance = (endPoint - cam.position).Length();
    tracer.traveled      = 0.0f;
    tracer.color         = JPH::Vec3(1.0f, 0.81f, 0.44f);
    g_State.tracers.push_back(tracer);

    PlaySound_Shoot(engine, 0.0f);

    if (victim != NullEntity && bestHit) {
        auto* enemy = reg.Get<GameplayComponents::EnemyController>(victim);
        if (enemy && enemy->behavior.alive) {
            Actor::ActorContext dummyCtx;
            dummyCtx.fx.spawnImpact = [](JPH::Vec3Arg p, JPH::Vec3Arg n, uint32_t type) { SpawnImpactParticles(p, n, type); };
            dummyCtx.onKilled       = [&](bool hs) {
                g_State.kills++;
                g_State.score += hs ? 250 : 100;
                if (hs)
                    g_State.headshots++;
                PlaySound_Kill(engine);
            };

            enemy->behavior.Damage(30.0f, *bestHit, dir, dummyCtx, true);

            p.hitMarkerTime = p.totalTime;
            if (bestHit->zone == 0) { // Headshot
                p.headMarkerTime = p.totalTime;
            }
            PlaySound_Hitmark(engine);
            PlaySound_Flesh(engine, bestT);
        }
    } else if (worldHit.hasHit) {
        SpawnImpactParticles(JPH::Vec3(worldHit.position), worldHit.normal, 0);
        PlaySound_Impact(engine, bestT);
    }
}

// ============================================================================
// GAME INITIALIZATION & SPAWN LOOPS
// ============================================================================

void SpawnEnemy(Engine* engine, JPH::Vec3Arg position) {
    auto& reg = engine->GetRegistry();

    Entity enemyEnt = reg.Create();
    reg.Add(enemyEnt, Components::TransformComponent {.position = position});
    reg.Add(enemyEnt, Components::NameComponent {.name = String64("TacticalSoldier")});

    auto& enemy = reg.Add(enemyEnt, GameplayComponents::EnemyController {});
    enemy.behavior.SetPosition(position);
    enemy.behavior.health = 100.0f + g_State.wave * 6.0f;

    AssetID unitBoxAsset = HashAssetID("unit_box");

    // Spawn 11 Procedural Limb Entities Driven by Rig::HIT_CAPSULES
    enemy.limbEntities.reserve(Rig::HIT_CAPSULES.size());
    for (size_t i = 0; i < Rig::HIT_CAPSULES.size(); ++i) {
        Entity limb = reg.Create();
        reg.Add(limb, Components::TransformComponent {});
        reg.Add(limb, Components::MeshComponent {.meshAsset = unitBoxAsset, .materialAsset = g_State.enemyMat, .cullRadius = 2.0f});
        reg.Add(limb, Components::NameComponent {.name = String64("ProceduralLimb")});
        enemy.limbEntities.push_back(limb);
    }

    enemy.weaponEntity = CreateProceduralRifle(engine, g_State.metalMat);
    g_State.enemies.push_back(enemyEnt);
}

void StartGame(Engine* engine) {
    auto& reg = engine->GetRegistry();
    auto& pc  = engine->GetPhysicsContext();
    auto& rc  = engine->GetRenderContext();

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

    g_State.concreteMat = HashAssetID("concrete_mat_asset");
    g_State.metalMat    = HashAssetID("metal_mat_asset");
    g_State.barrierMat  = HashAssetID("barrier_mat_asset");
    g_State.crateMat    = HashAssetID("crate_mat_asset");
    g_State.sandbagMat  = HashAssetID("sandbag_mat_asset");
    g_State.enemyMat    = HashAssetID("enemy_mat_asset");

    rc.RegisterGPUMaterial(g_State.concreteMat, CreativeWorksFactory::CreateBasicMaterial(rc).value());
    rc.RegisterGPUMaterial(g_State.metalMat, CreativeWorksFactory::CreateBasicMaterial(rc).value());
    rc.RegisterGPUMaterial(g_State.barrierMat, CreativeWorksFactory::CreateBasicMaterial(rc).value());
    rc.RegisterGPUMaterial(g_State.crateMat, CreativeWorksFactory::CreateBasicMaterial(rc).value());
    rc.RegisterGPUMaterial(g_State.sandbagMat, CreativeWorksFactory::CreateBasicMaterial(rc).value());

    auto enemyMaterial               = CreativeWorksFactory::CreateBasicMaterial(rc).value();
    enemyMaterial.baseColorFactor[0] = 0.28f;
    enemyMaterial.baseColorFactor[1] = 0.33f;
    enemyMaterial.baseColorFactor[2] = 0.26f; // Tactical Olive
    rc.RegisterGPUMaterial(g_State.enemyMat, enemyMaterial);

    g_State.tracerMat   = CreativeWorksFactory::CreateBasicMaterial(rc, true, true, true).value();
    g_State.particleMat = CreativeWorksFactory::CreateBasicMaterial(rc, true, true, true).value();

    auto groundShape   = Physics::GetOrCreateShape(pc, Physics::ShapeType::Plane, 0.0f, 1.0f, 0.0f, 0.0f);
    g_State.floorPlane = reg.Create();
    reg.Add(g_State.floorPlane, Components::TransformComponent {});
    reg.Add(
        g_State.floorPlane,
        Components::PhysicsComponent {Physics::CreateRigidBody(pc, groundShape, JPH::RVec3::sZero(), JPH::Quat::sIdentity(), JPH::EMotionType::Static, 0)}
    );

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

    AddBox(engine, JPH::Vec3(0.0f, 0.0f, -6.0f), JPH::Vec3(14.0f, 5.0f, 10.0f), g_State.concreteMat);
    AddBox(engine, JPH::Vec3(-3.0f, 5.0f, -6.0f), JPH::Vec3(8.0f, 0.4f, 10.0f), g_State.barrierMat, true);
    AddBox(engine, JPH::Vec3(9.0f, 0.0f, -10.0f), JPH::Vec3(4.0f, 2.6f, 4.0f), g_State.concreteMat);

    AddBox(engine, JPH::Vec3(-16.0f, 0.0f, 4.0f), JPH::Vec3(6.0f, 2.6f, 2.5f), g_State.metalMat);
    AddBox(engine, JPH::Vec3(-16.0f, 2.6f, 4.0f), JPH::Vec3(6.0f, 2.6f, 2.5f), g_State.metalMat, true);
    AddBox(engine, JPH::Vec3(-9.0f, 0.0f, 12.0f), JPH::Vec3(2.5f, 2.6f, 6.0f), g_State.metalMat);
    AddBox(engine, JPH::Vec3(14.0f, 0.0f, 8.0f), JPH::Vec3(6.0f, 2.6f, 2.5f), g_State.metalMat);
    AddBox(engine, JPH::Vec3(20.0f, 0.0f, -4.0f), JPH::Vec3(2.5f, 2.6f, 6.0f), g_State.metalMat);
    AddBox(engine, JPH::Vec3(-22.0f, 0.0f, -12.0f), JPH::Vec3(6.0f, 2.6f, 2.5f), g_State.metalMat);

    JPH::Vec3 spawnPos(0.0f, 1.5f, 24.0f);
    g_State.playerEnt = reg.Create();
    reg.Add(g_State.playerEnt, Components::PlayerTagComponent {});
    reg.Add(g_State.playerEnt, Components::TransformComponent {.position = spawnPos});
    reg.Add(g_State.playerEnt, Components::MovementComponent {});
    reg.Add(g_State.playerEnt, Components::InputComponent {});
    reg.Add(g_State.playerEnt, Components::PhysicsComponent {Physics::CreateCharacter(pc, JPH::RVec3(spawnPos))});
    reg.Add(g_State.playerEnt, Components::PhysicsStateComponent {.currPosition = spawnPos, .prevPosition = spawnPos});

    auto& p     = reg.Add(g_State.playerEnt, GameplayComponents::PlayerController {});
    p.baseYaw   = -90.0f;
    p.basePitch = 0.0f;

    g_State.weaponEntity = CreateProceduralRifle(engine, g_State.metalMat);

    auto camEnts = reg.GetEntitiesWith<Components::MainCameraTagComponent>();
    if (!camEnts.empty()) {
        Entity camEnt    = camEnts[0];
        auto*  targetCam = reg.Get<Components::TargetCameraComponent>(camEnt);
        if (!targetCam) {
            targetCam = &reg.Add(camEnt, Components::TargetCameraComponent {});
        }
        targetCam->target         = g_State.playerEnt;
        targetCam->distance       = 0.0f;
        targetCam->targetDistance = 0.0f;
        targetCam->yaw            = -90.0f;
        targetCam->pitch          = 0.0f;
        targetCam->stiffness      = 0.0f;
        targetCam->targetOffset   = JPH::Vec3(0.0f, 1.62f, 0.0f);
    }

    for (int i = 0; i < 5; ++i) {
        float randAngle = (static_cast<float>(i) / 5.0f) * 6.283f;
        SpawnEnemy(engine, JPH::Vec3(std::cos(randAngle) * 18.0f, 0.0f, std::sin(randAngle) * 18.0f));
    }

    uint32_t fontIdx = 0;
    for (Entity uiEnt: reg.GetEntitiesWith<Components::UISettingsComponent>()) {
        if (auto* uiSettings = reg.Get<Components::UISettingsComponent>(uiEnt)) {
            fontIdx = uiSettings->defaultFontAtlasIdx;
            break;
        }
    }

    g_State.hudVitalsBg   = reg.Create();
    auto& bgRect          = reg.Add(g_State.hudVitalsBg, Components::UIRectComponent {});
    bgRect.anchorMinX     = 0.0f;
    bgRect.anchorMaxX     = 0.0f;
    bgRect.anchorMinY     = 1.0f;
    bgRect.anchorMaxY     = 1.0f;
    bgRect.x              = 24.0f;
    bgRect.y              = -80.0f;
    bgRect.width          = 200.0f;
    bgRect.height         = 14.0f;
    bgRect.hierarchyDepth = 10;

    auto& bgPanel = reg.Add(g_State.hudVitalsBg, Components::UIPanelComponent {});
    bgPanel.color = JPH::Vec4(0.12f, 0.12f, 0.16f, 0.65f);

    g_State.hudVitalsBar   = reg.Create();
    auto& barRect          = reg.Add(g_State.hudVitalsBar, Components::UIRectComponent {});
    barRect.parentEntity   = g_State.hudVitalsBg;
    barRect.x              = 2.0f;
    barRect.y              = 2.0f;
    barRect.width          = 196.0f;
    barRect.height         = 10.0f;
    barRect.hierarchyDepth = 11;

    auto& barPanel = reg.Add(g_State.hudVitalsBar, Components::UIPanelComponent {});
    barPanel.color = JPH::Vec4(0.35f, 0.95f, 0.45f, 0.95f);

    g_State.hudAmmoText     = reg.Create();
    auto& ammoRect          = reg.Add(g_State.hudAmmoText, Components::UIRectComponent {});
    ammoRect.anchorMinX     = 1.0f;
    ammoRect.anchorMaxX     = 1.0f;
    ammoRect.anchorMinY     = 1.0f;
    ammoRect.anchorMaxY     = 1.0f;
    ammoRect.x              = -240.0f;
    ammoRect.y              = -85.0f;
    ammoRect.width          = 200.0f;
    ammoRect.height         = 40.0f;
    ammoRect.hierarchyDepth = 10;

    auto& ammoText = reg.Add(g_State.hudAmmoText, Components::TextComponent {});
    ammoText.text.assign("30 / 210");
    ammoText.scale     = 1.25f;
    ammoText.fontIndex = fontIdx;
    ammoText.color     = JPH::Vec4(0.95f, 0.95f, 0.95f, 0.95f);

    g_State.hudCrosshair  = reg.Create();
    auto& chRect          = reg.Add(g_State.hudCrosshair, Components::UIRectComponent {});
    chRect.anchorMinX     = 0.5f;
    chRect.anchorMaxX     = 0.5f;
    chRect.anchorMinY     = 0.5f;
    chRect.anchorMaxY     = 0.5f;
    chRect.x              = -6.0f;
    chRect.y              = -8.0f;
    chRect.width          = 20.0f;
    chRect.height         = 20.0f;
    chRect.hierarchyDepth = 15;

    auto& chText = reg.Add(g_State.hudCrosshair, Components::TextComponent {});
    chText.text.assign("+");
    chText.scale     = 1.5f;
    chText.fontIndex = fontIdx;
    chText.color     = JPH::Vec4(0.43f, 1.00f, 0.70f, 0.85f);

    g_State.hudWaveText     = reg.Create();
    auto& waveRect          = reg.Add(g_State.hudWaveText, Components::UIRectComponent {});
    waveRect.anchorMinX     = 0.0f;
    waveRect.anchorMaxX     = 0.0f;
    waveRect.anchorMinY     = 0.0f;
    waveRect.anchorMaxY     = 0.0f;
    waveRect.x              = 24.0f;
    waveRect.y              = 20.0f;
    waveRect.width          = 250.0f;
    waveRect.height         = 80.0f;
    waveRect.hierarchyDepth = 10;

    auto& waveText = reg.Add(g_State.hudWaveText, Components::TextComponent {});
    waveText.text.assign("WAVE 01");
    waveText.scale     = 1.0f;
    waveText.fontIndex = fontIdx;
    waveText.color     = JPH::Vec4(0.55f, 0.82f, 1.00f, 0.85f);

    g_State.gameStarted = true;
}

// ============================================================================
// SYSTEM UPDATE TICK RUNNERS
// ============================================================================

void PlayerInputSystem(Engine* engine, [[maybe_unused]] float dt) {
    auto&       reg   = engine->GetRegistry();
    const auto& input = engine->GetInput();
    auto&       cam   = engine->GetCamera();

    if (g_State.playerEnt == NullEntity || !reg.IsAlive(g_State.playerEnt)) {
        return;
    }

    auto* p    = reg.Get<GameplayComponents::PlayerController>(g_State.playerEnt);
    auto* move = reg.Get<Components::MovementComponent>(g_State.playerEnt);
    if (!p || !move) {
        return;
    }

    // 1. Process Mouse Look (Only when Main Menu is NOT active)
    if (!g_State.mainMenu.IsActive()) {
        const float sensitivity = 0.15f;
        float       mouseDeltaX = input.GetMouse().deltaX;
        float       mouseDeltaY = input.GetMouse().deltaY;

        p->baseYaw += mouseDeltaX * sensitivity;
        p->basePitch = std::clamp(p->basePitch - (mouseDeltaY * sensitivity), -89.0f, 89.0f);
    }

    // 2. Process WASD Movement
    float yawRad   = JPH::DegreesToRadians(p->baseYaw);
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

void PlayerUpdateTick(Engine* engine, float dt) {
    auto&       reg   = engine->GetRegistry();
    const auto& input = engine->GetInput();
    auto&       cam   = engine->GetCamera();

    if (g_State.playerEnt == NullEntity) {
        return;
    }

    auto* p    = reg.Get<GameplayComponents::PlayerController>(g_State.playerEnt);
    auto* move = reg.Get<Components::MovementComponent>(g_State.playerEnt);
    if (!p || !move) {
        return;
    }

    p->totalTime += dt;

    p->fireCd = std::max(0.0f, p->fireCd - dt);
    if (p->reloading > 0.0f) {
        p->reloading -= dt;
        if (p->reloading <= 0.0f) {
            int32_t need     = p->magSize - p->mag;
            int32_t transfer = std::min(need, p->reserve);
            p->mag += transfer;
            p->reserve -= transfer;
        }
    }

    p->hurtFlash = std::max(0.0f, p->hurtFlash - dt * 1.4f);

    p->pitchRecoil.Update(dt);
    p->yawRecoil.Update(dt);
    p->kickSpring.Update(dt);

    p->ads = MathUtils::Damp(p->ads, input.IsMouseButtonDown(KeyCode::RButton) ? 1.0f : 0.0f, 14.0f, dt);

    float speedSq     = move->inputX * move->inputX + move->inputZ * move->inputZ;
    float planarSpeed = (speedSq > 0.01f) ? (move->isSprinting ? 7.4f : 5.1f) : 0.0f;
    p->bobAmt         = MathUtils::Damp(p->bobAmt, move->isGrounded ? planarSpeed / 6.0f : 0.0f, 8.0f, dt);
    p->bobPhase += planarSpeed * dt * (move->isSprinting ? 2.1f : 1.7f);

    p->stepPhase += planarSpeed * dt * 0.55f;
    if (p->stepPhase > 1.0f) {
        p->stepPhase -= 1.0f;
        if (move->isGrounded) {
            PlaySound_Step(engine);
        }
    }

    p->landVel += (-160.0f * p->landDip - 18.0f * p->landVel) * dt;
    p->landDip += p->landVel * dt;

    float height = reg.Get<Components::TransformComponent>(g_State.playerEnt)->position.GetY();
    if (move->isGrounded && p->lastHeight - height > 1.5f) {
        p->landVel = -(p->lastHeight - height) * 3.5f;
    }
    p->lastHeight = height;

    float bobScale = 1.0f - p->ads * 0.9f;
    float bobY     = std::sin(p->bobPhase * 2.0f * JPH::JPH_PI) * 0.035f * p->bobAmt * bobScale;
    float bobX     = std::cos(p->bobPhase * JPH::JPH_PI) * 0.045f * p->bobAmt * bobScale;

    // Combine base look angles with recoil kick offsets
    cam.yaw   = p->baseYaw + p->yawRecoil.value * 0.35f;
    cam.pitch = std::clamp(p->basePitch + p->pitchRecoil.value, -89.0f, 89.0f);
    cam.fov   = MathUtils::Lerp(HIP_FOV, ADS_FOV, p->ads);

    JPH::Vec3 playerPos = JPH::Vec3::sZero();
    if (auto* trans = reg.Get<Components::TransformComponent>(g_State.playerEnt)) {
        playerPos = trans->position;
    }

    JPH::Vec3 mutablePos = playerPos;
    mutablePos.SetY(mutablePos.GetY() + 1.62f + bobY - p->landDip);
    mutablePos.SetX(mutablePos.GetX() + bobX * 0.4f);
    cam.position = mutablePos;

    // Sync orientation back to TargetCameraComponent
    auto camEnts = reg.GetEntitiesWith<Components::MainCameraTagComponent>();
    if (!camEnts.empty()) {
        if (auto* targetCam = reg.Get<Components::TargetCameraComponent>(camEnts[0])) {
            targetCam->yaw             = cam.yaw;
            targetCam->pitch           = cam.pitch;
            targetCam->smoothTargetPos = playerPos;
        }
    }

    if (g_State.weaponEntity != NullEntity && reg.IsAlive(g_State.weaponEntity)) {
        if (auto* wTrans = reg.Get<Components::TransformComponent>(g_State.weaponEntity)) {
            float free   = 1.0f - p->ads;
            float freeSq = free * free;

            float mouseSwayX = input.GetMouse().deltaX;
            float mouseSwayY = input.GetMouse().deltaY;

            p->sway.Update(dt, mouseSwayX * 0.002f, mouseSwayY * 0.002f);

            JPH::Vec3 hipBase(0.145f, -0.135f, -0.33f);
            JPH::Vec3 adsBase(0.0f, -SIGHT_HEIGHT * 0.85f, ADS_SIGHT_Z + SIGHT_Z * 0.85f);
            JPH::Vec3 base = MathUtils::Lerp(hipBase, adsBase, p->ads);

            base.SetX(base.GetX() + (p->sway.currentSwayX + std::sin(p->bobPhase * 2.0f * JPH::JPH_PI) * 0.012f * p->bobAmt) * freeSq);
            base.SetY(base.GetY() + (p->sway.currentSwayY + std::abs(std::cos(p->bobPhase * JPH::JPH_PI)) * 0.012f * p->bobAmt) * freeSq);
            base.SetZ(base.GetZ() + p->kickSpring.value * 0.04f);

            wTrans->position = base;

            JPH::Quat rotation = MathUtils::EulerYXZ(cam.pitch + (1.0f - p->ads) * 0.62f, cam.yaw, 0.0f);
            JPH::Quat kickRot  = MathUtils::EulerYXZ(-p->kickSpring.value * 0.55f, p->kickSpring.value * 0.12f, p->kickSpring.value * 0.2f);
            wTrans->rotation   = (rotation * kickRot).Normalized();
        }
    }

    if (input.IsMouseButtonDown(KeyCode::LButton)) {
        ProcessPlayerWeaponFire(engine, *p);
    } else {
        p->shotsFired = std::max(0, static_cast<int>(p->shotsFired - dt * 6.0f));
    }

    if (input.IsKeyDown(KeyCode::R) && p->reloading <= 0.0f && p->mag < p->magSize && p->reserve > 0) {
        p->reloading = 2.1f;
        PlaySound_Reload(engine);
    }

    if (g_State.hudVitalsBar != NullEntity && reg.IsAlive(g_State.hudVitalsBar)) {
        if (auto* barRect = reg.Get<Components::UIRectComponent>(g_State.hudVitalsBar)) {
            float hpPct    = std::max(0.0f, p->health / p->maxHealth);
            barRect->width = 196.0f * hpPct;
        }
        if (auto* barPanel = reg.Get<Components::UIPanelComponent>(g_State.hudVitalsBar)) {
            if (p->health < 35.0f) {
                barPanel->color = JPH::Vec4(0.92f, 0.15f, 0.15f, 0.95f);
            } else {
                barPanel->color = JPH::Vec4(0.35f, 0.95f, 0.45f, 0.95f);
            }
        }
    }

    if (g_State.hudAmmoText != NullEntity && reg.IsAlive(g_State.hudAmmoText)) {
        if (auto* ammoText = reg.Get<Components::TextComponent>(g_State.hudAmmoText)) {
            ammoText->text.assign(std::format("{:02d} / {}", p->mag, p->reserve));
        }
    }

    if (g_State.hudCrosshair != NullEntity && reg.IsAlive(g_State.hudCrosshair)) {
        if (auto* chText = reg.Get<Components::TextComponent>(g_State.hudCrosshair)) {
            float alpha   = std::clamp(1.0f - p->ads, 0.0f, 1.0f);
            chText->color = JPH::Vec4(0.43f, 1.00f, 0.70f, alpha * 0.85f);

            float hitMarkerT = p->totalTime - p->hitMarkerTime;
            if (hitMarkerT < 0.18f) {
                chText->text.assign("x");
                if (p->totalTime - p->headMarkerTime < 0.25f) {
                    chText->color = JPH::Vec4(1.00f, 0.30f, 0.30f, 0.95f);
                } else {
                    chText->color = JPH::Vec4(1.00f, 1.00f, 1.00f, 0.95f);
                }
            } else {
                chText->text.assign("+");
            }
        }
    }

    if (g_State.hudWaveText != NullEntity && reg.IsAlive(g_State.hudWaveText)) {
        if (auto* waveText = reg.Get<Components::TextComponent>(g_State.hudWaveText)) {
            waveText->text.assign(std::format("WAVE {:02d} | HOSTILES: {}", g_State.wave, g_State.enemies.size()));
        }
    }
}

void EnemyAISystem(Engine* engine, float dt) {
    auto& reg = engine->GetRegistry();
    auto& pc  = engine->GetPhysicsContext();

    for (Entity e: g_State.enemies) {
        auto* enemyPtr = reg.Get<GameplayComponents::EnemyController>(e);
        if (!enemyPtr)
            continue;
        auto& enemy = *enemyPtr;

        Actor::ActorContext ctx;
        ctx.playerPos   = (g_State.playerEnt != NullEntity && reg.IsAlive(g_State.playerEnt) && reg.Get<Components::TransformComponent>(g_State.playerEnt)) ?
                              reg.Get<Components::TransformComponent>(g_State.playerEnt)->position :
                              JPH::Vec3::sZero();
        ctx.playerAlive = (g_State.playerEnt != NullEntity && reg.IsAlive(g_State.playerEnt));
        ctx.time        = static_cast<float>(engine->GetCurrentFrame()) * 0.0166f;
        ctx.floorY      = 0.0f;

        ctx.world.pointBlocked = []([[maybe_unused]] JPH::Vec3Arg pos, [[maybe_unused]] float radius) { return false; };
        ctx.world.lineOfSight  = [](JPH::Vec3Arg, JPH::Vec3Arg) { return true; };

        Entity ignorePhys = NullEntity;
        if (auto* phys = reg.Get<Components::PhysicsComponent>(e)) {
            ignorePhys = phys->physicsHandle;
        }

        ctx.world.raycastWorld = [&](JPH::Vec3Arg origin, JPH::Vec3Arg direction, float maxDistance) -> std::optional<Actor::BodyHit> {
            auto hit = Physics::Raycast(pc, JPH::RVec3(origin), direction, maxDistance, ignorePhys);
            if (hit.hasHit) {
                return Actor::BodyHit {
                    .t = hit.fraction * maxDistance, .point = JPH::Vec3(hit.position), .normal = hit.normal, .joint = Rig::Joint::Hips, .mult = 1.0f, .zone = 2
                };
            }
            return std::nullopt;
        };

        ctx.fx.playBeep = [&](float freq, float dur, float vol) { engine->GetAudioContext().PlayProceduralBeep(freq, dur, vol); };

        ctx.updateAnimation = [&enemy](float frameDt, float speed, float crouch, float aimYaw, float aimPitch, float aiming) -> Actor::WeaponStance {
            AnimInput ai = {.speed = speed, .crouch = crouch, .aimYaw = aimYaw, .aimPitch = aimPitch, .aiming = aiming};
            enemy.anim.Update(frameDt, ai);

            return Actor::WeaponStance {
                .position = enemy.anim.weaponPos, .rotation = enemy.anim.weaponQuat, .muzzleWorld = enemy.anim.muzzleWorld, .aimDir = enemy.anim.aimDir
            };
        };

        ctx.fx.fireWeapon   = [&enemy]() { enemy.anim.Fire(1.0f); };
        ctx.fx.reloadWeapon = [&enemy]() { enemy.anim.StartReload(); };

        enemy.behavior.Update(dt, ctx);

        // Update 11 Procedural Humanoid Body Part Transforms
        if (enemy.limbEntities.size() == Rig::HIT_CAPSULES.size()) {
            for (size_t c = 0; c < Rig::HIT_CAPSULES.size(); ++c) {
                const auto& cap     = Rig::HIT_CAPSULES[c];
                Entity      limbEnt = enemy.limbEntities[c];

                JPH::Vec3 posA = enemy.behavior.boneWorldPositions[static_cast<size_t>(cap.a)];
                JPH::Vec3 posB = enemy.behavior.boneWorldPositions[static_cast<size_t>(cap.b)];

                JPH::Vec3 delta = posB - posA;
                float     len   = delta.Length();
                JPH::Vec3 dir   = (len > 1e-4f) ? delta / len : JPH::Vec3::sAxisY();
                JPH::Vec3 mid   = (posA + posB) * 0.5f;

                JPH::Quat rot = JPH::Quat::sFromTo(JPH::Vec3::sAxisY(), dir);
                JPH::Vec3 scale(cap.r * 2.0f, std::max(len, 0.05f), cap.r * 2.0f);

                if (auto* trans = reg.Get<Components::TransformComponent>(limbEnt)) {
                    trans->position = mid;
                    trans->rotation = rot;
                    trans->scale    = scale;
                }
            }
        }
    }
}

void ProcessRenderTick(Engine* engine, float dt) {
    auto& rc  = engine->GetRenderContext();
    auto& cam = engine->GetCamera();

    AssetID unitBoxAsset = HashAssetID("unit_box");
    if (!rc.GetGPUMesh(unitBoxAsset).has_value()) {
        rc.RegisterGPUMesh(unitBoxAsset, CreativeWorksFactory::CreateBox(rc, JPH::Vec3(0.5f, 0.5f, 0.5f)));
    }

    // 1. Process CPU Tracer Lines (Reusing g_State.tracerMat - Zero Allocations!)
    for (auto it = g_State.tracers.begin(); it != g_State.tracers.end();) {
        it->traveled += it->speed * dt;
        if (it->traveled - it->length > it->totalDistance) {
            it = g_State.tracers.erase(it);
        } else {
            float     head      = std::min(it->traveled, it->totalDistance);
            float     tail      = std::max(0.0f, it->traveled - it->length);
            JPH::Vec3 headPoint = it->start + it->direction * head;
            JPH::Vec3 tailPoint = it->start + it->direction * tail;

            float     length   = (headPoint - tailPoint).Length();
            JPH::Vec3 midPoint = (headPoint + tailPoint) * 0.5f;

            if (length > 0.01f) {
                JPH::Mat44 transform =
                    Math::CreateTransform(midPoint, JPH::Quat::sFromTo(JPH::Vec3::sAxisZ(), it->direction), JPH::Vec3(0.015f, 0.015f, length));

                DrawParams params;
                params.transform        = transform;
                params.prevTransform    = transform;
                params.cullRadius       = length;
                params.flags            = DrawFlags::VisibleInMain | DrawFlags::ExcludeFromTLAS;
                params.colorOverride    = {it->color.GetX(), it->color.GetY(), it->color.GetZ(), 1.0f};
                params.emissiveOverride = {it->color.GetX() * 25.0f, it->color.GetY() * 25.0f, it->color.GetZ() * 25.0f, 1.0f};

                Renderer::Draw(rc, g_State.tracerMat, *rc.GetGPUMesh(unitBoxAsset), params);
            }
            ++it;
        }
    }

    // 2. Process CPU Blood & Spark Billboard Particles (Reusing g_State.particleMat - Zero Allocations!)
    JPH::Mat44 invView = cam.GetViewMatrix().Inversed();
    JPH::Vec3  right   = invView.GetColumn3(0).Normalized();
    JPH::Vec3  up      = invView.GetColumn3(1).Normalized();
    JPH::Vec3  back    = invView.GetColumn3(2).Normalized();

    JPH::Mat44 billboardMat(JPH::Vec4(right, 0.0f), JPH::Vec4(back, 0.0f), JPH::Vec4(-up, 0.0f), JPH::Vec4(0.0f, 0.0f, 0.0f, 1.0f));
    JPH::Quat  billboardRot = billboardMat.GetQuaternion().Normalized();

    AssetID particleMesh = HashAssetID("procedural_particle_mesh");
    if (!rc.GetGPUMesh(particleMesh).has_value()) {
        rc.RegisterGPUMesh(particleMesh, CreativeWorksFactory::CreatePlane(rc, 0.5f));
    }

    for (auto it = g_State.particles.begin(); it != g_State.particles.end();) {
        it->life -= dt;
        if (it->life <= 0.0f) {
            it = g_State.particles.erase(it);
        } else {
            it->velocity.SetY(it->velocity.GetY() + it->gravity * dt);
            it->velocity *= std::max(0.0f, 1.0f - it->drag * dt);
            it->position += it->velocity * dt;

            if (it->position.GetY() < 0.02f) {
                it->position.SetY(0.02f);
                it->velocity.SetY(it->velocity.GetY() * -0.25f);
                it->velocity.SetX(it->velocity.GetX() * 0.6f);
                it->velocity.SetZ(it->velocity.GetZ() * 0.6f);
            }

            float      t         = it->life / it->maxLife;
            float      size      = it->size * (0.4f + t * 0.8f);
            JPH::Mat44 transform = Math::CreateTransform(it->position, billboardRot, JPH::Vec3::sReplicate(size));

            DrawParams params;
            params.transform        = transform;
            params.prevTransform    = transform;
            params.cullRadius       = size;
            params.flags            = DrawFlags::VisibleInMain | DrawFlags::ExcludeFromTLAS;
            params.colorOverride    = {it->color.GetX(), it->color.GetY(), it->color.GetZ(), std::min(1.0f, t * 1.6f)};
            params.emissiveOverride = {it->color.GetX() * 15.0f, it->color.GetY() * 15.0f, it->color.GetZ() * 15.0f, 1.0f};

            Renderer::Draw(rc, g_State.particleMat, *rc.GetGPUMesh(particleMesh), params);
            ++it;
        }
    }
}

} // namespace Game

GAMEPLAY_API ZHLN::GameplayStatus NativeGameplayUpdate(ZHLN::Engine* engine, float dt) {
    if (!engine) {
        return ZHLN::GameplayStatus::Error;
    }

    ZHLN_PROFILE_SCOPE("ECS System: Native Gameplay Update");

    static bool wasTabDown = false;
    bool        isTabDown  = engine->GetInput().IsKeyDown(ZHLN::KeyCode::Tab) || engine->GetInput().IsKeyDown(ZHLN::KeyCode::Escape);

    if (!Game::g_State.gameStarted) {
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
                     Game::g_State.mainMenu.Destroy(eng);
                 },
             .textX = 55.0f,
             .textY = 25.0f}
        );

        cfg.buttons.push_back({.text = "QUIT", .onClick = [](ZHLN::Engine* eng) { eng->GetWindow().Close(); }, .textX = 80.0f, .textY = 25.0f});

        Game::g_State.mainMenu.Build(engine, cfg);
    } else if (isTabDown && !wasTabDown) {
        if (Game::g_State.mainMenu.IsActive()) {
            engine->GetWindow().CaptureMouse(true);
            Game::g_State.mainMenu.Destroy(engine);
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
                         Game::g_State.mainMenu.Destroy(eng);
                     },
                 .textX = 55.0f,
                 .textY = 25.0f}
            );

            cfg.buttons.push_back({.text = "QUIT", .onClick = [](ZHLN::Engine* eng) { eng->GetWindow().Close(); }, .textX = 80.0f, .textY = 25.0f});

            Game::g_State.mainMenu.Build(engine, cfg);
        }
    }
    wasTabDown = isTabDown;

    if (Game::g_State.mainMenu.IsActive()) {
        Game::g_State.mainMenu.Update(engine, dt);
        return ZHLN::GameplayStatus::OK;
    }

    if (Game::g_State.gameStarted) {
        Game::PlayerInputSystem(engine, dt);
        Game::PlayerUpdateTick(engine, dt);
        Game::EnemyAISystem(engine, dt);
        Game::ProcessRenderTick(engine, dt);
    }

    return ZHLN::GameplayStatus::OK;
}
