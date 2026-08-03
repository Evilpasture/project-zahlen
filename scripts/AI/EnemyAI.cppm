// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module;
#include <Jolt/Jolt.h>
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <Zahlen/Audio.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Types.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

export module ZHLN.EnemyAI;

import ZHLN.MathUtils;
import ZHLN.Rig;
import ZHLN.Actor;
import ZHLN.Animator;
import ZHLN.Soldier;
import ZHLN.Weapons;
import ZHLN.Pickups;
import ZHLN.BlacksiteState; // <--- MUST BE BlacksiteState (DO NOT import ZHLN.BlacksiteLevel!)

export namespace ZHLN::EnemyAI {

struct AnimInput {
    float speed    = 0.0f;
    float crouch   = 0.0f;
    float aimYaw   = 0.0f;
    float aimPitch = 0.0f;
    float aiming   = 0.0f;
};

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

struct EnemyController {
    Actor::StandardActor behavior;
    SoldierAnimator      anim;
    std::vector<Entity>  limbEntities;
    Entity               weaponEntity = NullEntity;
    Weapons::WeaponId    weaponId     = Weapons::WeaponId::Rifle;
    float                phase        = 0.0f;
    float                breath       = 0.0f;
    float                recoil       = 0.0f;
    float                recoilVel    = 0.0f;
    float                reloadT      = -1.0f;
    float                lowReady     = 1.0f;
};

void SpawnEnemy(Engine* engine, JPH::Vec3Arg position, Weapons::WeaponId weaponId = Weapons::WeaponId::Rifle) {
    auto& state = BlacksiteState::GetSceneState();
    auto& reg   = engine->GetRegistry();
    auto& pc    = engine->GetPhysicsContext();
    auto& rc    = engine->GetRenderContext();

    AssetID soldierMeshAsset = Soldier::GetOrGenerateSoldierMeshAsset(rc);

    static uint32_t s_NextJointOffset = 0;
    uint32_t        myJointOffset     = s_NextJointOffset;
    s_NextJointOffset += Rig::JointCount;

    Entity enemyEnt = reg.Create();
    reg.Add(enemyEnt, Components::TransformComponent {.position = position});
    reg.Add(enemyEnt, Components::NameComponent {.name = String64("TacticalSoldier")});
    reg.Add(
        enemyEnt, Components::MeshComponent {
                      .meshAsset = soldierMeshAsset, .materialAsset = state.enemyMat, .cullRadius = 2.5f, .jointOffset = myJointOffset, .isSkinned = true
                  }
    );

    Entity physChar = Physics::CreateCharacter(pc, JPH::RVec3(position));
    reg.Add(enemyEnt, Components::PhysicsComponent {physChar});
    reg.Add(enemyEnt, Components::PhysicsStateComponent {.currPosition = position, .prevPosition = position});

    auto& enemy    = reg.Add(enemyEnt, EnemyController {});
    enemy.weaponId = weaponId;
    enemy.behavior.SetPosition(position);
    enemy.behavior.health = 100.0f + state.wave * 6.0f;

    enemy.weaponEntity = Weapons::CreateWeaponModel(engine, weaponId, state.metalMat, state.crateMat);
    state.enemies.push_back(enemyEnt);
}

void EnemyAISystem(Engine* engine, float dt) {
    // FIX: Changed from BlacksiteLevel to BlacksiteState
    auto& state = BlacksiteState::GetSceneState();
    auto& reg   = engine->GetRegistry();
    auto& pc    = engine->GetPhysicsContext();
    auto& rc    = engine->GetRenderContext();

    static std::array<JPH::Vec3, Rig::JointCount> s_BindWorldPos = []() {
        std::array<JPH::Vec3, Rig::JointCount> W;
        for (uint32_t i = 0; i < Rig::JointCount; ++i) {
            Rig::Joint j      = static_cast<Rig::Joint>(i);
            JPH::Vec3  local  = Rig::GetBindPosition(j);
            int32_t    parent = Rig::GetParentIndex(j);
            W[i]              = (parent >= 0) ? W[parent] + local : local;
        }
        return W;
    }();

    static std::array<JPH::Mat44, Rig::JointCount> s_InvBindMatrices = []() {
        std::array<JPH::Mat44, Rig::JointCount> invBind;
        for (uint32_t i = 0; i < Rig::JointCount; ++i) {
            invBind[i] = JPH::Mat44::sTranslation(-s_BindWorldPos[i]);
        }
        return invBind;
    }();

    static const std::array<int32_t, Rig::JointCount> s_JointChild = []() {
        std::array<int32_t, Rig::JointCount> childs;
        childs.fill(-1);
        childs[static_cast<size_t>(Rig::Joint::Hips)]      = static_cast<int32_t>(Rig::Joint::Spine);
        childs[static_cast<size_t>(Rig::Joint::Spine)]     = static_cast<int32_t>(Rig::Joint::Chest);
        childs[static_cast<size_t>(Rig::Joint::Chest)]     = static_cast<int32_t>(Rig::Joint::Neck);
        childs[static_cast<size_t>(Rig::Joint::Neck)]      = static_cast<int32_t>(Rig::Joint::Head);
        childs[static_cast<size_t>(Rig::Joint::Head)]      = static_cast<int32_t>(Rig::Joint::HeadEnd);
        childs[static_cast<size_t>(Rig::Joint::ClavicleL)] = static_cast<int32_t>(Rig::Joint::UpperArmL);
        childs[static_cast<size_t>(Rig::Joint::UpperArmL)] = static_cast<int32_t>(Rig::Joint::ForearmL);
        childs[static_cast<size_t>(Rig::Joint::ForearmL)]  = static_cast<int32_t>(Rig::Joint::HandL);
        childs[static_cast<size_t>(Rig::Joint::HandL)]     = static_cast<int32_t>(Rig::Joint::HandEndL);
        childs[static_cast<size_t>(Rig::Joint::ClavicleR)] = static_cast<int32_t>(Rig::Joint::UpperArmR);
        childs[static_cast<size_t>(Rig::Joint::UpperArmR)] = static_cast<int32_t>(Rig::Joint::ForearmR);
        childs[static_cast<size_t>(Rig::Joint::ForearmR)]  = static_cast<int32_t>(Rig::Joint::HandR);
        childs[static_cast<size_t>(Rig::Joint::HandR)]     = static_cast<int32_t>(Rig::Joint::HandEndR);
        childs[static_cast<size_t>(Rig::Joint::ThighL)]    = static_cast<int32_t>(Rig::Joint::ShinL);
        childs[static_cast<size_t>(Rig::Joint::ShinL)]     = static_cast<int32_t>(Rig::Joint::FootL);
        childs[static_cast<size_t>(Rig::Joint::FootL)]     = static_cast<int32_t>(Rig::Joint::ToeL);
        childs[static_cast<size_t>(Rig::Joint::ThighR)]    = static_cast<int32_t>(Rig::Joint::ShinR);
        childs[static_cast<size_t>(Rig::Joint::ShinR)]     = static_cast<int32_t>(Rig::Joint::FootR);
        childs[static_cast<size_t>(Rig::Joint::FootR)]     = static_cast<int32_t>(Rig::Joint::ToeR);
        return childs;
    }();

    for (auto it = state.enemies.begin(); it != state.enemies.end();) {
        Entity enemyEnt = *it;
        if (!reg.IsAlive(enemyEnt)) {
            it = state.enemies.erase(it);
            continue;
        }

        auto* enemyPtr = reg.Get<EnemyController>(enemyEnt);
        if (!enemyPtr) {
            ++it;
            continue;
        }
        auto& enemy = *enemyPtr;

        Actor::ActorContext ctx;
        ctx.playerPos   = (state.playerEnt != NullEntity && reg.IsAlive(state.playerEnt) && reg.Get<Components::TransformComponent>(state.playerEnt)) ?
                              reg.Get<Components::TransformComponent>(state.playerEnt)->position :
                              JPH::Vec3::sZero();
        ctx.playerAlive = (state.playerEnt != NullEntity && reg.IsAlive(state.playerEnt));
        ctx.time        = static_cast<float>(engine->GetCurrentFrame()) * 0.0166f;
        ctx.floorY      = 0.0f;

        ctx.world.pointBlocked = [&](JPH::Vec3Arg pos, float radius) {
            JPH::Array<Entity> results;
            Physics::OverlapSphere(pc, JPH::RVec3(pos), radius, results);
            return !results.empty();
        };

        ctx.world.lineOfSight = [](JPH::Vec3Arg, JPH::Vec3Arg) { return true; };

        Entity ignorePhys = NullEntity;
        if (auto* phys = reg.Get<Components::PhysicsComponent>(enemyEnt)) {
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

            JPH::Vec3 worldWeaponPos = enemy.behavior.position + enemy.anim.weaponPos;
            JPH::Vec3 worldMuzzle    = enemy.behavior.position + enemy.anim.muzzleWorld;

            return Actor::WeaponStance {.position = worldWeaponPos, .rotation = enemy.anim.weaponQuat, .muzzleWorld = worldMuzzle, .aimDir = enemy.anim.aimDir};
        };

        ctx.fx.fireWeapon   = [&enemy]() { enemy.anim.Fire(1.0f); };
        ctx.fx.reloadWeapon = [&enemy]() { enemy.anim.StartReload(); };

        enemy.behavior.Update(dt, ctx);

        auto* phys = reg.Get<Components::PhysicsComponent>(enemyEnt);

        if (enemy.behavior.alive) {
            if (phys && phys->physicsHandle != NullEntity) {
                Physics::SetCharacterVelocity(pc, phys->physicsHandle, enemy.behavior.velocity);
            }

            if (auto* stateComp = reg.Get<Components::PhysicsStateComponent>(enemyEnt)) {
                float feetY             = stateComp->currPosition.GetY() - 0.80f;
                enemy.behavior.position = JPH::Vec3(stateComp->currPosition.GetX(), std::max(0.0f, feetY), stateComp->currPosition.GetZ());
            }
        } else {
            if (reg.Get<Components::PhysicsComponent>(enemyEnt) != nullptr) {
                reg.Remove<Components::PhysicsComponent>(enemyEnt);
            }
        }

        if (auto* eTrans = reg.Get<Components::TransformComponent>(enemyEnt)) {
            eTrans->position = enemy.behavior.position;
            eTrans->rotation = JPH::Quat::sRotation(JPH::Vec3::sAxisY(), enemy.behavior.yaw);
        }

        if (enemy.behavior.alive) {
            float speed  = enemy.behavior.speed;
            float stride = (speed > 3.2f) ? 1.85f : 1.25f;
            enemy.phase += (speed / stride) * dt;

            float speedRatio = std::min(1.0f, speed / 1.5f);
            float swingAngle = std::sin(enemy.phase * JPH::JPH_PI * 2.0f) * 0.42f * speedRatio;
            float kneeAngleL = std::max(0.0f, std::cos(enemy.phase * JPH::JPH_PI * 2.0f)) * 0.85f * speedRatio;
            float kneeAngleR = std::max(0.0f, -std::cos(enemy.phase * JPH::JPH_PI * 2.0f)) * 0.85f * speedRatio;

            JPH::Quat bodyRot = JPH::Quat::sRotation(JPH::Vec3::sAxisY(), enemy.behavior.yaw);
            JPH::Vec3 rootPos = enemy.behavior.position;

            auto& pos = enemy.behavior.boneWorldPositions;

            pos[static_cast<size_t>(Rig::Joint::Hips)] = rootPos + bodyRot * Rig::GetBindPosition(Rig::Joint::Hips);

            pos[static_cast<size_t>(Rig::Joint::Spine)]   = pos[static_cast<size_t>(Rig::Joint::Hips)] + bodyRot * Rig::GetBindPosition(Rig::Joint::Spine);
            pos[static_cast<size_t>(Rig::Joint::Chest)]   = pos[static_cast<size_t>(Rig::Joint::Spine)] + bodyRot * Rig::GetBindPosition(Rig::Joint::Chest);
            pos[static_cast<size_t>(Rig::Joint::Neck)]    = pos[static_cast<size_t>(Rig::Joint::Chest)] + bodyRot * Rig::GetBindPosition(Rig::Joint::Neck);
            pos[static_cast<size_t>(Rig::Joint::Head)]    = pos[static_cast<size_t>(Rig::Joint::Neck)] + bodyRot * Rig::GetBindPosition(Rig::Joint::Head);
            pos[static_cast<size_t>(Rig::Joint::HeadEnd)] = pos[static_cast<size_t>(Rig::Joint::Head)] + bodyRot * Rig::GetBindPosition(Rig::Joint::HeadEnd);

            JPH::Quat thighRotL = JPH::Quat::sRotation(JPH::Vec3::sAxisX(), swingAngle);
            JPH::Quat kneeRotL  = thighRotL * JPH::Quat::sRotation(JPH::Vec3::sAxisX(), kneeAngleL);

            pos[static_cast<size_t>(Rig::Joint::ThighL)] = pos[static_cast<size_t>(Rig::Joint::Hips)] + bodyRot * Rig::GetBindPosition(Rig::Joint::ThighL);
            pos[static_cast<size_t>(Rig::Joint::ShinL)]  = pos[static_cast<size_t>(Rig::Joint::ThighL)] +
                                                           bodyRot * (thighRotL * Rig::GetBindPosition(Rig::Joint::ShinL));
            pos[static_cast<size_t>(Rig::Joint::FootL)]  = pos[static_cast<size_t>(Rig::Joint::ShinL)] +
                                                           bodyRot * (kneeRotL * Rig::GetBindPosition(Rig::Joint::FootL));
            pos[static_cast<size_t>(Rig::Joint::ToeL)]   = pos[static_cast<size_t>(Rig::Joint::FootL)] +
                                                           bodyRot * (kneeRotL * Rig::GetBindPosition(Rig::Joint::ToeL));

            JPH::Quat thighRotR = JPH::Quat::sRotation(JPH::Vec3::sAxisX(), -swingAngle);
            JPH::Quat kneeRotR  = thighRotR * JPH::Quat::sRotation(JPH::Vec3::sAxisX(), kneeAngleR);

            pos[static_cast<size_t>(Rig::Joint::ThighR)] = pos[static_cast<size_t>(Rig::Joint::Hips)] + bodyRot * Rig::GetBindPosition(Rig::Joint::ThighR);
            pos[static_cast<size_t>(Rig::Joint::ShinR)]  = pos[static_cast<size_t>(Rig::Joint::ThighR)] +
                                                           bodyRot * (thighRotR * Rig::GetBindPosition(Rig::Joint::ShinR));
            pos[static_cast<size_t>(Rig::Joint::FootR)]  = pos[static_cast<size_t>(Rig::Joint::ShinR)] +
                                                           bodyRot * (kneeRotR * Rig::GetBindPosition(Rig::Joint::FootR));
            pos[static_cast<size_t>(Rig::Joint::ToeR)]   = pos[static_cast<size_t>(Rig::Joint::FootR)] +
                                                           bodyRot * (kneeRotR * Rig::GetBindPosition(Rig::Joint::ToeR));

            pos[static_cast<size_t>(Rig::Joint::ClavicleL)] = pos[static_cast<size_t>(Rig::Joint::Chest)] +
                                                              bodyRot * Rig::GetBindPosition(Rig::Joint::ClavicleL);
            pos[static_cast<size_t>(Rig::Joint::ClavicleR)] = pos[static_cast<size_t>(Rig::Joint::Chest)] +
                                                              bodyRot * Rig::GetBindPosition(Rig::Joint::ClavicleR);

            pos[static_cast<size_t>(Rig::Joint::UpperArmL)] = pos[static_cast<size_t>(Rig::Joint::ClavicleL)] +
                                                              bodyRot * Rig::GetBindPosition(Rig::Joint::UpperArmL);
            pos[static_cast<size_t>(Rig::Joint::UpperArmR)] = pos[static_cast<size_t>(Rig::Joint::ClavicleR)] +
                                                              bodyRot * Rig::GetBindPosition(Rig::Joint::UpperArmR);

            JPH::Vec3 worldWeaponPos = enemy.behavior.position + enemy.anim.weaponPos;
            JPH::Quat weaponQuat     = enemy.anim.weaponQuat;

            JPH::Vec3 gripR = worldWeaponPos + weaponQuat * JPH::Vec3(0.0f, -0.075f, -0.115f);
            JPH::Vec3 gripL = worldWeaponPos + weaponQuat * JPH::Vec3(0.0f, -0.020f, 0.170f);

            JPH::Vec3 shR                                  = pos[static_cast<size_t>(Rig::Joint::UpperArmR)];
            pos[static_cast<size_t>(Rig::Joint::ForearmR)] = (shR + gripR) * 0.5f + bodyRot * JPH::Vec3(-0.12f, -0.12f, -0.05f);
            pos[static_cast<size_t>(Rig::Joint::HandR)]    = gripR;
            pos[static_cast<size_t>(Rig::Joint::HandEndR)] = gripR + weaponQuat * JPH::Vec3(0.0f, 0.0f, 0.08f);

            JPH::Vec3 shL                                  = pos[static_cast<size_t>(Rig::Joint::UpperArmL)];
            pos[static_cast<size_t>(Rig::Joint::ForearmL)] = (shL + gripL) * 0.5f + bodyRot * JPH::Vec3(0.12f, -0.12f, -0.05f);
            pos[static_cast<size_t>(Rig::Joint::HandL)]    = gripL;
            pos[static_cast<size_t>(Rig::Joint::HandEndL)] = gripL + weaponQuat * JPH::Vec3(0.0f, 0.0f, 0.08f);
        }

        if (auto* meshComp = reg.Get<Components::MeshComponent>(enemyEnt)) {
            JPH::Mat44 rootMatrix = JPH::Mat44::sRotationTranslation(JPH::Quat::sRotation(JPH::Vec3::sAxisY(), enemy.behavior.yaw), enemy.behavior.position);
            JPH::Mat44 invRoot    = rootMatrix.Inversed();

            std::array<JPH::Mat44, Rig::JointCount> jointWorldMatrices;

            for (uint32_t j = 0; j < Rig::JointCount; ++j) {
                int32_t   child = s_JointChild[j];
                JPH::Vec3 pJ    = enemy.behavior.boneWorldPositions[j];

                if (child >= 0) {
                    JPH::Vec3 pC      = enemy.behavior.boneWorldPositions[child];
                    JPH::Vec3 dir     = pC - pJ;
                    float     dLen    = dir.Length();
                    JPH::Vec3 bindDir = (s_BindWorldPos[child] - s_BindWorldPos[j]).Normalized();

                    if (dLen > 0.001f && bindDir.LengthSq() > 0.001f) {
                        JPH::Quat rot         = JPH::Quat::sFromTo(bindDir, dir / dLen);
                        jointWorldMatrices[j] = JPH::Mat44::sRotationTranslation(rot, pJ);
                    } else {
                        jointWorldMatrices[j] = JPH::Mat44::sTranslation(pJ);
                    }
                } else {
                    int32_t parent = Rig::GetParentIndex(static_cast<Rig::Joint>(j));
                    if (parent >= 0) {
                        jointWorldMatrices[j] = jointWorldMatrices[parent] * JPH::Mat44::sTranslation(Rig::GetBindPosition(static_cast<Rig::Joint>(j)));
                    } else {
                        jointWorldMatrices[j] = JPH::Mat44::sTranslation(pJ);
                    }
                }
            }

            std::array<JPH::Mat44, Rig::JointCount> gpuMatrices;
            for (uint32_t j = 0; j < Rig::JointCount; ++j) {
                gpuMatrices[j] = invRoot * jointWorldMatrices[j] * s_InvBindMatrices[j];
            }

            rc.UpdateJointMatrices(meshComp->jointOffset, gpuMatrices.data(), Rig::JointCount);
        }

        if (!enemy.behavior.weaponDropped && enemy.weaponEntity != NullEntity && reg.IsAlive(enemy.weaponEntity)) {
            if (auto* wTrans = reg.Get<Components::TransformComponent>(enemy.weaponEntity)) {
                wTrans->position = enemy.behavior.position + enemy.anim.weaponPos;
                wTrans->rotation = enemy.anim.weaponQuat;
            }
        }

        if (!enemy.behavior.alive && !enemy.behavior.weaponDropped) {
            Pickups::WeaponPickup pu {
                .entity   = enemy.weaponEntity,
                .weaponId = enemy.weaponId,
                .ammo     = Weapons::GetWeaponDef(enemy.weaponId).magSize,
                .position = enemy.behavior.position + JPH::Vec3(0, 1.0f, 0),
                .velocity = JPH::Vec3(0, 3.0f, 0),
                .spin     = JPH::Vec3(4.0f, 2.0f, 1.0f)
            };
            state.pickups.push_back(pu);
            enemy.behavior.weaponDropped = true;
            enemy.weaponEntity           = NullEntity;
        }

        ++it;
    }

    uint32_t targetHostiles = state.hordeMode ? state.hordeTarget : (3 + std::min(6u, state.wave));
    if (state.enemies.size() < targetHostiles) {
        state.waveTimer -= dt;
        if (state.waveTimer <= 0.0f) {
            state.waveTimer             = state.hordeMode ? 0.2f : 2.4f;
            float             randAngle = ((std::rand() % 100) / 100.0f) * 6.283f;
            Weapons::WeaponId wId       = (((std::rand() % 100) / 100.0f) < 0.35f) ? Weapons::WeaponId::Shotgun : Weapons::WeaponId::Rifle;
            SpawnEnemy(engine, JPH::Vec3(std::cos(randAngle) * 22.0f, 0.0f, std::sin(randAngle) * 22.0f), wId);
        }
    }
}

} // namespace ZHLN::EnemyAI
