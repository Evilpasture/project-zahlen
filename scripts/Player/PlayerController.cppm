// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module;
#include <Jolt/Jolt.h>
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <Zahlen/Audio.hpp>
#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Types.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <optional>
#include <random>
#include <string>
#include <vector>

export module ZHLN.PlayerController;

import ZHLN.MathUtils;
import ZHLN.FPS;
import ZHLN.Rig;
import ZHLN.Actor;
import ZHLN.Weapons;
import ZHLN.CombatAudio;
import ZHLN.CombatFX;
import ZHLN.Pickups;
import ZHLN.EnemyAI;
import ZHLN.BlacksiteState;

export namespace ZHLN::PlayerController {

constexpr float PLAYER_EYE_OFFSET_Y = 0.55f;
constexpr float HIP_FOV             = 78.0f;
constexpr float SIGHT_HEIGHT        = 0.079f;
constexpr float SIGHT_Z             = 0.125f;
constexpr float ADS_SIGHT_Z         = -0.285f;
constexpr float BLAST_RANGE         = 17.0f;
constexpr float BLAST_FORCE         = 26.0f;
constexpr float BLAST_COOLDOWN      = 6.0f;

struct PlayerControllerComp {
    FPS::Spring3D     weaponSpring;
    FPS::Spring3D     swaySpring;
    FPS::Spring1D     pitchRecoil;
    FPS::Spring1D     yawRecoil;
    FPS::Spring1D     kickSpring;
    FPS::BobEvaluator bobber;
    FPS::SwaySolver   sway;

    float baseYaw    = -90.0f;
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

    Weapons::WeaponId currentWeapon = Weapons::WeaponId::Rifle;
    Weapons::WeaponId pendingWeapon = Weapons::WeaponId::Rifle;
    float             swapT         = 0.0f;

    struct AmmoState {
        int32_t mag     = 30;
        int32_t reserve = 210;
    };
    std::array<AmmoState, static_cast<size_t>(Weapons::WeaponId::Count)> ammo = {{
        {.mag = 30, .reserve = 210},  // Rifle
        {.mag = 7, .reserve = 70},    // Shotgun
        {.mag = 600, .reserve = 2400} // Minigun
    }};

    float   reloading    = 0.0f;
    int32_t shellsToLoad = 0;
    float   shellTimer   = 0.0f;
    float   fireCd       = 0.0f;
    int32_t shotsFired   = 0;
    float   ads          = 0.0f;
    float   spin         = 0.0f;
    float   barrelAngle  = 0.0f;

    float blastCd   = 0.0f;
    float blastTime = -99.0f;

    bool godMode      = false;
    bool infiniteAmmo = false;
    bool adsToggle    = false;

    float hurtFlash        = 0.0f;
    float hurtDir          = 0.0f;
    float hitMarkerTime    = -99.0f;
    float headMarkerTime   = -99.0f;
    float pierceMarkerTime = -99.0f;

    float       pickupFlash     = 0.0f;
    std::string pickupFlashText = "";
};

void ProcessPlayerWeaponFire(Engine* engine, PlayerControllerComp& p) {
    auto&       state     = BlacksiteState::GetSceneState();
    const auto& def       = Weapons::GetWeaponDef(p.currentWeapon);
    auto&       ammoState = p.ammo[static_cast<size_t>(p.currentWeapon)];

    if (p.spin < 1.0f && def.spinUp > 0.0f)
        return;

    if (ammoState.mag <= 0 || p.reloading > 0.0f || p.fireCd > 0.0f) {
        if (ammoState.mag <= 0 && p.reloading <= 0.0f && p.fireCd <= 0.0f) {
            CombatAudio::PlayEmpty(engine);
            p.fireCd = 0.25f;
        }
        return;
    }

    if (!p.godMode && !p.infiniteAmmo)
        ammoState.mag--;
    p.shotsFired++;
    p.fireCd = def.fireRate;

    float adsScale = 1.0f - p.ads * def.adsTighten;
    float growth   = std::min(1.0f, static_cast<float>(p.shotsFired) / 9.0f);
    p.pitchRecoil.ApplyImpulse(def.recoilPitch * (0.75f + growth * 0.45f) * adsScale);
    p.yawRecoil.ApplyImpulse((((std::rand() % 100) / 100.0f) - 0.45f) * def.recoilYaw * (0.8f + growth * 0.7f) * adsScale);
    p.kickSpring.ApplyImpulse(def.kick);

    auto& pc  = engine->GetPhysicsContext();
    auto& reg = engine->GetRegistry();
    auto& cam = engine->GetCamera();

    JPH::Vec3 origin      = cam.position;
    JPH::Vec3 muzzleWorld = origin;
    if (state.weaponEntity != NullEntity && reg.IsAlive(state.weaponEntity)) {
        if (auto* wTrans = reg.Get<Components::TransformComponent>(state.weaponEntity)) {
            muzzleWorld = wTrans->position + (wTrans->rotation * JPH::Vec3(0.0f, 0.012f, 0.66f));
        }
    }

    float     yawRad   = JPH::DegreesToRadians(cam.yaw);
    float     pitchRad = JPH::DegreesToRadians(cam.pitch);
    JPH::Vec3 baseDir(JPH::Cos(yawRad) * JPH::Cos(pitchRad), JPH::Sin(pitchRad), JPH::Sin(yawRad) * JPH::Cos(pitchRad));

    std::random_device                    rd;
    std::mt19937                          gen(rd());
    std::uniform_real_distribution<float> dis(-1.0f, 1.0f);

    float moving      = (p.bobAmt * 0.028f) * (def.pellets > 1 ? 0.45f : 1.0f);
    float totalSpread = (def.baseSpread + moving + std::min(1.0f, p.shotsFired / 10.0f) * 0.02f) * (1.0f - p.ads * def.adsTighten);

    Entity ignorePhys = NullEntity;
    if (state.playerEnt != NullEntity && reg.IsAlive(state.playerEnt)) {
        if (auto* phys = reg.Get<Components::PhysicsComponent>(state.playerEnt)) {
            ignorePhys = phys->physicsHandle;
        }
    }

    bool anyHit    = false;
    bool anyPierce = false;

    struct HitRecord {
        float                         t        = 220.0f;
        Entity                        victim   = NullEntity;
        std::optional<Actor::BodyHit> bodyHit  = std::nullopt;
        bool                          isGround = false;
        JPH::Vec3                     point    = JPH::Vec3::sZero();
        JPH::Vec3                     normal   = JPH::Vec3::sZero();
    };

    for (uint32_t pel = 0; pel < def.pellets; ++pel) {
        float cone = totalSpread;
        if (def.pellets > 1) {
            float r = (pel == 0) ? 0.05f : (pel <= 3) ? 0.45f : 1.0f;
            cone += def.patternSpread * (1.0f - p.ads * def.adsTighten) * r;
        }

        JPH::Vec3 dir = baseDir.Normalized() + JPH::Vec3(dis(gen) * cone, dis(gen) * cone, dis(gen) * cone);
        dir           = dir.Normalized();

        float                  maxT = def.range;
        std::vector<HitRecord> hits;

        auto worldHit = Physics::Raycast(pc, JPH::RVec3(origin), dir, maxT, ignorePhys);
        if (worldHit.hasHit) {
            hits.push_back(
                {.t        = static_cast<float>(worldHit.fraction) * maxT,
                 .victim   = NullEntity,
                 .bodyHit  = std::nullopt,
                 .isGround = false,
                 .point    = JPH::Vec3(worldHit.position),
                 .normal   = worldHit.normal}
            );
        }

        for (Entity enemyEnt: state.enemies) {
            if (!reg.IsAlive(enemyEnt))
                continue;
            auto* enemy = reg.Get<EnemyAI::EnemyController>(enemyEnt);
            if (enemy) {
                auto bHit = enemy->behavior.Raycast(origin, dir, maxT);
                if (bHit) {
                    hits.push_back({.t = bHit->t, .victim = enemyEnt, .bodyHit = bHit, .isGround = false, .point = bHit->point, .normal = bHit->normal});
                }
            }
        }

        std::sort(hits.begin(), hits.end(), [](const HitRecord& a, const HitRecord& b) { return a.t < b.t; });

        float    pen           = def.penetration;
        float    dmgScale      = 1.0f;
        uint32_t bodiesPierced = 0;
        float    endT          = maxT;

        for (const auto& hit: hits) {
            if (hit.t > endT)
                break;

            if (hit.victim != NullEntity && hit.bodyHit) {
                auto* enemy = reg.Get<EnemyAI::EnemyController>(hit.victim);
                if (enemy && enemy->behavior.alive) {
                    Actor::ActorContext dummyCtx;
                    dummyCtx.fx.spawnImpact = [&state](JPH::Vec3Arg pt, JPH::Vec3Arg n, uint32_t type) {
                        CombatFX::SpawnImpactParticles(state.particles, pt, n, type);
                    };
                    dummyCtx.onKilled = [&](bool hs) {
                        state.kills++;
                        state.score += hs ? 250 : 100;
                        if (hs)
                            state.headshots++;
                        CombatAudio::PlayKill(engine);
                        BlacksiteState::PushKillFeed(hs ? "HEADSHOT — HOSTILE DOWN" : "HOSTILE DOWN", hs);
                    };

                    float falloff = Weapons::FalloffAt(def, hit.t);
                    float dmg     = def.damage * falloff * dmgScale;

                    enemy->behavior.Damage(dmg, *hit.bodyHit, dir, dummyCtx, true);

                    anyHit          = true;
                    p.hitMarkerTime = p.totalTime;
                    if (hit.bodyHit->zone == 0)
                        p.headMarkerTime = p.totalTime;
                    if (bodiesPierced > 0) {
                        anyPierce          = true;
                        p.pierceMarkerTime = p.totalTime;
                    }

                    bodiesPierced++;
                    if (pen < 0.55f || bodiesPierced >= def.maxBodyPierces || def.bodyPenRetain <= 0.0f) {
                        endT = hit.t;
                        break;
                    }
                    pen -= 0.55f;
                    dmgScale *= def.bodyPenRetain;
                }
            } else {
                CombatFX::SpawnImpactParticles(state.particles, hit.point, hit.normal, 0);
                CombatAudio::PlayImpact(engine, hit.t);
                endT = hit.t;
                break;
            }
        }

        JPH::Vec3 endPoint = origin + dir * endT;

        CombatFX::BulletTracer tracer;
        tracer.start         = muzzleWorld;
        tracer.direction     = (endPoint - muzzleWorld).Normalized();
        tracer.speed         = 320.0f;
        tracer.length        = 3.2f;
        tracer.totalDistance = (endPoint - muzzleWorld).Length();
        tracer.traveled      = 0.0f;
        tracer.color         = JPH::Vec3(1.0f, 0.81f, 0.44f);
        state.tracers.push_back(tracer);
    }

    if (def.pellets > 1)
        CombatAudio::PlayShotgun(engine, 0.0f);
    else if (def.spinUp > 0.0f)
        CombatAudio::PlayMinigun(engine);
    else
        CombatAudio::PlayShoot(engine, 0.0f);

    if (anyPierce)
        CombatAudio::PlayPierce(engine);
    if (anyHit)
        CombatAudio::PlayHitmark(engine);
}

void ProcessKineticBlast(Engine* engine, PlayerControllerComp& p) {
    if (!p.alive)
        return;
    if (p.blastCd > 0.0f) {
        CombatAudio::PlayEmpty(engine);
        return;
    }

    p.blastCd   = BLAST_COOLDOWN;
    p.blastTime = p.totalTime;

    auto& state = BlacksiteState::GetSceneState();
    auto& cam   = engine->GetCamera();

    float     yawRad   = JPH::DegreesToRadians(cam.yaw);
    float     pitchRad = JPH::DegreesToRadians(cam.pitch);
    JPH::Vec3 aimDir   = JPH::Vec3(JPH::Cos(yawRad) * JPH::Cos(pitchRad), JPH::Sin(pitchRad), JPH::Sin(yawRad) * JPH::Cos(pitchRad)).Normalized();

    JPH::Vec3 origin   = cam.position + aimDir * 0.9f;
    float     cosAngle = JPH::Cos(JPH::DegreesToRadians(62.0f));

    uint32_t stunnedCount = 0;

    for (Entity enemyEnt: state.enemies) {
        if (!engine->GetRegistry().IsAlive(enemyEnt))
            continue;
        auto* enemy = engine->GetRegistry().Get<EnemyAI::EnemyController>(enemyEnt);
        if (!enemy)
            continue;

        JPH::Vec3 targetPos = enemy->behavior.position + JPH::Vec3(0, 1.2f, 0);
        JPH::Vec3 toTarget  = targetPos - origin;
        float     dist      = toTarget.Length();

        if (dist > BLAST_RANGE || dist < 0.01f)
            continue;
        JPH::Vec3 dirToTarget = toTarget / dist;
        if (dirToTarget.Dot(aimDir) < cosAngle)
            continue;

        float     falloff   = std::pow(1.0f - dist / BLAST_RANGE, 0.7f);
        JPH::Vec3 launchDir = (aimDir * 0.55f + dirToTarget * 0.45f).Normalized();
        launchDir.SetY(launchDir.GetY() + 0.55f);

        JPH::Vec3 impulse = launchDir.Normalized() * (BLAST_FORCE * (0.45f + 0.55f * falloff));

        if (enemy->behavior.alive) {
            Actor::BodyHit      hit {.t = dist, .point = targetPos, .normal = -aimDir, .joint = Rig::Joint::Chest, .mult = 1.0f, .zone = 1};
            Actor::ActorContext dummyCtx;
            enemy->behavior.Damage(20.0f * falloff, hit, launchDir, dummyCtx, true);
            enemy->behavior.state = Actor::AIState::Suppressed;
            enemy->behavior.ragdoll.ApplyImpulse(static_cast<uint32_t>(Rig::Joint::Chest), impulse);
            stunnedCount++;
        }
    }

    CombatFX::KineticShockwave wave {.position = origin, .direction = aimDir, .radius = BLAST_RANGE * 0.62f, .life = 0.55f, .maxLife = 0.55f};
    state.shockwaves.push_back(wave);

    p.pitchRecoil.ApplyImpulse(2.6f);
    p.kickSpring.ApplyImpulse(4.5f);
    CombatAudio::PlayBlast(engine);

    if (stunnedCount > 0) {
        BlacksiteState::PushKillFeed(std::format("{} HOSTILES STUNNED", stunnedCount), false);
        state.score += stunnedCount * 25;
    }
}

void PlayerInputSystem(Engine* engine, [[maybe_unused]] float dt) {
    auto&       state = BlacksiteState::GetSceneState();
    auto&       reg   = engine->GetRegistry();
    const auto& input = engine->GetInput();

    if (state.playerEnt == NullEntity || !reg.IsAlive(state.playerEnt))
        return;

    auto* p    = reg.Get<PlayerControllerComp>(state.playerEnt);
    auto* move = reg.Get<Components::MovementComponent>(state.playerEnt);
    if (!p || !move)
        return;

    if (!state.mainMenu.IsActive()) {
        const float sensitivity = 0.15f;
        p->baseYaw += input.GetMouse().deltaX * sensitivity;
        p->basePitch = std::clamp(p->basePitch - (input.GetMouse().deltaY * sensitivity), -89.0f, 89.0f);
    }

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
    if (input.IsKeyDown(KeyCode::Space))
        move->jumpRequested = true;
}

void PlayerUpdateTick(Engine* engine, float dt) {
    auto&       state = BlacksiteState::GetSceneState();
    auto&       reg   = engine->GetRegistry();
    const auto& input = engine->GetInput();
    auto&       cam   = engine->GetCamera();

    if (state.playerEnt == NullEntity)
        return;

    auto* p    = reg.Get<PlayerControllerComp>(state.playerEnt);
    auto* move = reg.Get<Components::MovementComponent>(state.playerEnt);
    if (!p || !move)
        return;

    p->totalTime += dt;
    const auto& def = Weapons::GetWeaponDef(p->currentWeapon);

    if (p->swapT > 0.0f) {
        float prev = p->swapT;
        p->swapT -= dt;
        if (prev > 0.275f && p->swapT <= 0.275f) {
            p->currentWeapon = p->pendingWeapon;
            if (state.weaponEntity != NullEntity && reg.IsAlive(state.weaponEntity)) {
                reg.Destroy(state.weaponEntity);
            }
            state.weaponEntity = Weapons::CreateWeaponModel(engine, p->currentWeapon, state.metalMat, state.crateMat);
        }
        if (p->swapT <= 0.0f)
            p->swapT = 0.0f;
    }

    if (input.IsKeyDown(KeyCode::R)) {
        if (p->reloading <= 0.0f && p->swapT <= 0.0f) {
            auto& ammoState = p->ammo[static_cast<size_t>(p->currentWeapon)];
            if (ammoState.mag < def.magSize && ammoState.reserve > 0) {
                if (def.shellReload > 0.0f) {
                    p->shellsToLoad = std::min(def.magSize - ammoState.mag, ammoState.reserve);
                    p->shellTimer   = 0.35f;
                    p->reloading    = 0.35f + p->shellsToLoad * def.shellReload + 0.3f;
                } else {
                    p->reloading = def.reloadTime;
                }
                CombatAudio::PlayReload(engine);
            }
        }
    }

    if (def.spinUp > 0.0f) {
        bool  wantSpin = input.IsMouseButtonDown(KeyCode::LButton) || input.IsMouseButtonDown(KeyCode::RButton);
        float rate     = wantSpin ? dt / def.spinUp : -dt / (def.spinUp * 1.5f);
        p->spin        = MathUtils::Clamp(p->spin + rate, 0.0f, 1.0f);
        p->barrelAngle += p->spin * 46.0f * dt;
    } else {
        p->spin = 0.0f;
    }

    p->fireCd = std::max(0.0f, p->fireCd - dt);
    if (p->reloading > 0.0f) {
        p->reloading -= dt;
        auto& ammoState = p->ammo[static_cast<size_t>(p->currentWeapon)];

        if (def.shellReload > 0.0f) {
            p->shellTimer -= dt;
            if (p->shellTimer <= 0.0f && p->shellsToLoad > 0 && ammoState.reserve > 0 && ammoState.mag < def.magSize) {
                ammoState.mag++;
                if (!p->infiniteAmmo)
                    ammoState.reserve--;
                p->shellsToLoad--;
                p->shellTimer = def.shellReload;
                CombatAudio::PlayStep(engine);
            }
        } else if (p->reloading <= 0.0f) {
            int32_t need     = def.magSize - ammoState.mag;
            int32_t transfer = std::min(need, ammoState.reserve);
            ammoState.mag += transfer;
            if (!p->infiniteAmmo)
                ammoState.reserve -= transfer;
        }
    }

    p->hurtFlash = std::max(0.0f, p->hurtFlash - dt * 1.4f);
    p->pitchRecoil.Update(dt);
    p->yawRecoil.Update(dt);
    p->kickSpring.Update(dt);

    bool canAds = !def.noAds && p->reloading <= 0.0f && p->swapT <= 0.0f;
    p->ads      = MathUtils::Damp(p->ads, (canAds && input.IsMouseButtonDown(KeyCode::RButton)) ? 1.0f : 0.0f, 14.0f, dt);

    float speedSq     = move->inputX * move->inputX + move->inputZ * move->inputZ;
    float planarSpeed = (speedSq > 0.01f) ? (move->isSprinting ? 7.4f : 5.1f) : 0.0f;
    p->bobAmt         = MathUtils::Damp(p->bobAmt, move->isGrounded ? planarSpeed / 6.0f : 0.0f, 8.0f, dt);
    p->bobPhase += planarSpeed * dt * (move->isSprinting ? 2.1f : 1.7f);

    p->stepPhase += planarSpeed * dt * 0.55f;
    if (p->stepPhase > 1.0f) {
        p->stepPhase -= 1.0f;
        if (move->isGrounded)
            CombatAudio::PlayStep(engine);
    }

    p->landVel += (-160.0f * p->landDip - 18.0f * p->landVel) * dt;
    p->landDip += p->landVel * dt;

    float height = reg.Get<Components::TransformComponent>(state.playerEnt)->position.GetY();
    if (move->isGrounded && p->lastHeight - height > 1.5f) {
        p->landVel = -(p->lastHeight - height) * 3.5f;
    }
    p->lastHeight = height;

    float bobScale = 1.0f - p->ads * 0.9f;
    float bobY     = std::sin(p->bobPhase * 2.0f * JPH::JPH_PI) * 0.035f * p->bobAmt * bobScale;
    float bobX     = std::cos(p->bobPhase * JPH::JPH_PI) * 0.045f * p->bobAmt * bobScale;

    cam.yaw   = p->baseYaw + p->yawRecoil.value * 0.35f;
    cam.pitch = std::clamp(p->basePitch + p->pitchRecoil.value, -89.0f, 89.0f);
    cam.fov   = MathUtils::Lerp(HIP_FOV, def.adsFov, p->ads);

    JPH::Vec3 playerPos = JPH::Vec3::sZero();
    if (auto* trans = reg.Get<Components::TransformComponent>(state.playerEnt)) {
        playerPos = trans->position;
    }

    JPH::Vec3 mutablePos = playerPos;
    mutablePos.SetY(mutablePos.GetY() + PLAYER_EYE_OFFSET_Y + bobY - p->landDip);
    mutablePos.SetX(mutablePos.GetX() + bobX * 0.4f);
    cam.position = mutablePos;

    auto camEnts = reg.GetEntitiesWith<Components::MainCameraTagComponent>();
    if (!camEnts.empty()) {
        if (auto* targetCam = reg.Get<Components::TargetCameraComponent>(camEnts[0])) {
            targetCam->distance        = 0.0f;
            targetCam->targetDistance  = 0.0f;
            targetCam->yaw             = cam.yaw;
            targetCam->pitch           = cam.pitch;
            targetCam->targetOffset    = JPH::Vec3(0.0f, PLAYER_EYE_OFFSET_Y + bobY - p->landDip, 0.0f);
            targetCam->smoothTargetPos = playerPos;
        }
    }

    if (state.weaponEntity != NullEntity && reg.IsAlive(state.weaponEntity)) {
        if (auto* wTrans = reg.Get<Components::TransformComponent>(state.weaponEntity)) {
            float free   = 1.0f - p->ads;
            float freeSq = free * free;

            p->sway.Update(dt, input.GetMouse().deltaX * 0.002f, input.GetMouse().deltaY * 0.002f);

            float pitchRad = JPH::DegreesToRadians(cam.pitch);
            float yawRad   = JPH::DegreesToRadians(cam.yaw);

            JPH::Vec3 forward = JPH::Vec3(std::cos(yawRad) * std::cos(pitchRad), std::sin(pitchRad), std::sin(yawRad) * std::cos(pitchRad)).Normalized();
            JPH::Vec3 worldUp(0.0f, 1.0f, 0.0f);
            JPH::Vec3 right    = worldUp.Cross(forward).Normalized();
            JPH::Vec3 actualUp = forward.Cross(right).Normalized();

            JPH::Vec3 hipBase(-0.14f, 0.05f, 0.38f);
            JPH::Vec3 adsBase(0.0f, -SIGHT_HEIGHT, 0.285f);
            JPH::Vec3 base = MathUtils::Lerp(hipBase, adsBase, p->ads);

            float offsetX = base.GetX() + (p->sway.currentSwayX + std::sin(p->bobPhase * 2.0f * JPH::JPH_PI) * 0.012f * p->bobAmt) * freeSq;
            float offsetY = base.GetY() + (p->sway.currentSwayY + std::abs(std::cos(p->bobPhase * JPH::JPH_PI)) * 0.012f * p->bobAmt) * freeSq;
            float offsetZ = base.GetZ() - p->kickSpring.value * 0.04f;

            if (p->swapT > 0.0f) {
                float swapDip = std::sin(MathUtils::Clamp(1.0f - std::abs(p->swapT - 0.275f) / 0.275f, 0.0f, 1.0f) * (JPH::JPH_PI * 0.5f));
                offsetY -= swapDip * 0.34f;
                offsetZ -= swapDip * 0.10f;
            }

            JPH::Mat44 basis = JPH::Mat44::sIdentity();
            basis.SetColumn3(0, right);
            basis.SetColumn3(1, actualUp);
            basis.SetColumn3(2, forward);

            JPH::Quat camRot  = basis.GetQuaternion().Normalized();
            JPH::Quat kickRot = MathUtils::EulerYXZ(-p->kickSpring.value * 0.55f, p->kickSpring.value * 0.12f, p->kickSpring.value * 0.2f);

            wTrans->position = cam.position + right * offsetX + actualUp * offsetY + forward * offsetZ;
            wTrans->rotation = (camRot * kickRot).Normalized();
        }
    }

    if (input.IsMouseButtonDown(KeyCode::LButton))
        ProcessPlayerWeaponFire(engine, *p);
    else
        p->shotsFired = std::max(0, static_cast<int>(p->shotsFired - dt * 6.0f));

    if (input.IsKeyDown(KeyCode::E))
        ProcessKineticBlast(engine, *p);

    Pickups::UpdatePickupsSystem(engine, *p, state.pickups, state.playerEnt, dt);
}

} // namespace ZHLN::PlayerController
