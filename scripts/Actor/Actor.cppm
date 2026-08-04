module;
#include <Jolt/Jolt.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <random>
#include <string_view>
#include <vector>

export module ZHLN.Actor;

import ZHLN.Rig;
import ZHLN.Ragdoll;
import ZHLN.MathUtils;

export namespace ZHLN::Actor {

enum class AIState : uint8_t { Idle, Patrol, Engage, Reposition, Suppressed, Dead };

struct BodyHit {
    float      t      = 0.0f;
    JPH::Vec3  point  = JPH::Vec3::sZero();
    JPH::Vec3  normal = JPH::Vec3::sZero();
    Rig::Joint joint  = Rig::Joint::Hips;
    float      mult   = 1.0f;
    uint32_t   zone   = 2; // 0=Head, 1=Torso, 2=Limb
};

struct WeaponStance {
    JPH::Vec3 position    = JPH::Vec3::sZero();
    JPH::Quat rotation    = JPH::Quat::sIdentity();
    JPH::Vec3 muzzleWorld = JPH::Vec3::sZero();
    JPH::Vec3 aimDir      = JPH::Vec3::sAxisZ();
};

struct ActorContext {
    JPH::Vec3 playerPos   = JPH::Vec3::sZero();
    bool      playerAlive = false;
    float     time        = 0.0f;
    float     floorY      = 0.0f;

    struct {
        std::function<bool(JPH::Vec3Arg, float)>                                 pointBlocked;
        std::function<bool(JPH::Vec3Arg, JPH::Vec3Arg)>                          lineOfSight;
        std::function<JPH::Vec3(uint32_t)>                                       getRandomSpawnPoint;
        std::function<std::vector<JPH::Vec3>(JPH::Vec3Arg, float)>               queryCoverPoints;
        std::function<std::optional<BodyHit>(JPH::Vec3Arg, JPH::Vec3Arg, float)> raycastWorld;
    } world;

    struct {
        std::function<void(float, float, float)>                      playBeep;
        std::function<void(JPH::Vec3Arg, JPH::Vec3Arg, uint32_t)>     spawnImpact;
        std::function<void(JPH::Vec3Arg, JPH::Vec3Arg, uint32_t)>     spawnTracer;
        std::function<void(JPH::Vec3Arg, JPH::Vec3Arg, float)>        spawnMuzzleFlash;
        std::function<void(JPH::Vec3Arg, JPH::Vec3Arg, JPH::Vec3Arg)> ejectCasing;
        std::function<void()>                                         fireWeapon;
        std::function<void()>                                         reloadWeapon;
    } fx;

    std::function<WeaponStance(float dt, float speed, float crouch, float aimYaw, float aimPitch, float aiming)> updateAnimation;
    std::function<void(float, JPH::Vec3Arg)>                                                                     damagePlayer;
    std::function<void(bool)>                                                                                    onKilled;
};

struct SegmentIntersection {
    float     t            = 0.0f;
    float     dist         = 0.0f;
    JPH::Vec3 point        = JPH::Vec3::sZero();
    JPH::Vec3 segmentPoint = JPH::Vec3::sZero();
};

inline std::optional<SegmentIntersection> RaySegmentDistance(JPH::Vec3Arg o, JPH::Vec3Arg d, JPH::Vec3Arg a, JPH::Vec3Arg b) noexcept {
    JPH::Vec3 ab = b - a, ao = o - a;
    float     abab = ab.Dot(ab), abd = ab.Dot(d), abao = ab.Dot(ao), dao = d.Dot(ao), den = abab - abd * abd;
    float     s = (std::abs(den) < 1e-6f) ? 0.0f : MathUtils::Clamp((abd * dao - abao) / den * -1.0f, 0.0f, 1.0f);
    float     t = (std::abs(den) < 1e-6f) ? -dao : abd * s - dao;

    if (t < 0.0f)
        return std::nullopt;

    JPH::Vec3 pRay = o + d * t, pSeg = a + ab * s;
    return SegmentIntersection {.t = t, .dist = (pRay - pSeg).Length(), .point = pRay, .segmentPoint = pSeg};
}

class StandardActor {
  public:
    JPH::Vec3 position = JPH::Vec3::sZero(), velocity = JPH::Vec3::sZero();
    float     yaw = 0.0f, aimYaw = 0.0f, aimPitch = 0.0f, speed = 0.0f, crouch = 0.0f, aiming = 0.0f;
    float     health = 100.0f, maxHealth = 100.0f, flash = 0.0f, stateT = 0.0f, sawTime = -99.0f;
    bool      alive = true, headshot = false, canSeePlayer = false, weaponDropped = false;

    AIState   state     = AIState::Idle;
    JPH::Vec3 targetPos = JPH::Vec3::sZero(), lastKnownPlayerPos = JPH::Vec3::sZero();
    int32_t   ammo = 30, burstCount = 0;
    float     fireCd = 0.0f, reloadT = 0.0f, strafeDir = 1.0f, strafeT = 0.0f, alertness = 0.0f, deathTime = -1.0f;

    Physics::VerletSolver                  ragdoll;
    std::array<JPH::Vec3, Rig::JointCount> boneWorldPositions;

    JPH::Vec3 weaponPos = JPH::Vec3::sZero(), weaponVel = JPH::Vec3::sZero(), weaponSpin = JPH::Vec3::sZero();
    JPH::Quat weaponRot = JPH::Quat::sIdentity();

    StandardActor() {
        std::fill(boneWorldPositions.begin(), boneWorldPositions.end(), JPH::Vec3::sZero());
        std::array<JPH::Vec3, Rig::JointCount> bindWorld;
        for (uint32_t i = 0; i < Rig::JointCount; ++i) {
            int32_t parent = Rig::GetParentIndex(static_cast<Rig::Joint>(i));
            bindWorld[i]   = (parent >= 0) ? bindWorld[parent] + Rig::GetBindPosition(static_cast<Rig::Joint>(i)) :
                                             Rig::GetBindPosition(static_cast<Rig::Joint>(i));
            ragdoll.AddParticle(bindWorld[i], 1.0f);
        }

        for (uint32_t i = 0; i < Rig::JointCount; i++) {
            int32_t parent = Rig::GetParentIndex(static_cast<Rig::Joint>(i));
            if (parent >= 0)
                ragdoll.AddConstraint(static_cast<uint32_t>(parent), i, 1.0f);
        }

        ragdoll.AddConstraint(static_cast<uint32_t>(Rig::Joint::ThighL), static_cast<uint32_t>(Rig::Joint::ThighR), 0.8f);
        ragdoll.AddConstraint(static_cast<uint32_t>(Rig::Joint::ClavicleL), static_cast<uint32_t>(Rig::Joint::ClavicleR), 0.8f);
        ragdoll.AddConstraint(static_cast<uint32_t>(Rig::Joint::Hips), static_cast<uint32_t>(Rig::Joint::Chest), 0.9f);
    }

    void SetPosition(JPH::Vec3Arg pos) {
        position = targetPos = pos;
    }

    [[nodiscard]] std::optional<BodyHit> Raycast(JPH::Vec3Arg origin, JPH::Vec3Arg direction, float maxT) const noexcept {
        JPH::Vec3 center   = boneWorldPositions[static_cast<size_t>(Rig::Joint::Spine)];
        JPH::Vec3 toCenter = center - origin;
        float     along    = toCenter.Dot(direction);
        if (along < -1.5f || along > maxT + 1.5f || (toCenter - direction * along).LengthSq() > 6.76f)
            return std::nullopt;

        std::optional<BodyHit> bestHit = std::nullopt;
        float                  bestT   = maxT;

        for (const auto& cap: Rig::HIT_CAPSULES) {
            auto isect = RaySegmentDistance(origin, direction, boneWorldPositions[static_cast<size_t>(cap.a)], boneWorldPositions[static_cast<size_t>(cap.b)]);
            if (isect && isect->dist <= cap.r && isect->t < bestT) {
                bestT   = isect->t;
                bestHit = BodyHit {
                    .t      = isect->t,
                    .point  = isect->point,
                    .normal = (isect->point - isect->segmentPoint).Normalized(),
                    .joint  = cap.a,
                    .mult   = cap.mult,
                    .zone   = cap.zone
                };
            }
        }
        return bestHit;
    }

    void Damage(float amount, const BodyHit& hit, JPH::Vec3Arg dir, ActorContext& ctx, bool fromPlayer) {
        if (!alive) {
            ragdoll.ApplyImpulse(static_cast<uint32_t>(hit.joint), dir * 4.5f);
            ctx.fx.spawnImpact(hit.point, -dir, 1);
            return;
        }

        health -= amount * hit.mult;
        flash = alertness = 1.0f;

        if (fromPlayer) {
            lastKnownPlayerPos = ctx.playerPos;
            sawTime            = ctx.time;
        }
        ctx.fx.spawnImpact(hit.point, -dir, 1);

        if (health <= 0.0f) {
            alive     = false;
            state     = AIState::Dead;
            headshot  = (hit.zone == 0);
            deathTime = ctx.time;

            for (uint32_t i = 0; i < Rig::JointCount; i++) {
                ragdoll.positions[i] = ragdoll.previousPositions[i] = boneWorldPositions[i];
            }

            std::mt19937 gen(std::random_device {}());
            ragdoll.ApplyImpulse(static_cast<uint32_t>(hit.joint), dir * ((headshot ? 8.5f : 5.5f) * std::uniform_real_distribution<float>(0.75f, 1.25f)(gen)));
            DropWeapon(dir);

            if (ctx.onKilled)
                ctx.onKilled(headshot);
            return;
        }

        for (uint32_t b: (hit.zone == 0) ?
                             std::initializer_list<uint32_t> {static_cast<uint32_t>(Rig::Joint::Head), static_cast<uint32_t>(Rig::Joint::Neck)} :
                             std::initializer_list<uint32_t> {static_cast<uint32_t>(Rig::Joint::Spine), static_cast<uint32_t>(Rig::Joint::Chest)}) {
            ragdoll.ApplyImpulse(b, dir * (hit.zone == 0 ? 5.0f : 3.5f));
        }

        velocity += dir * 1.6f;
        if (((std::rand() % 100) / 100.0f) < 0.45f || health < 40.0f) {
            state  = AIState::Suppressed;
            stateT = 0.5f + ((std::rand() % 100) / 100.0f) * 0.8f;
        }
    }

    void Update(float dt, ActorContext& ctx) {
        if (flash > 0.0f)
            flash = std::max(0.0f, flash - dt * 5.0f);

        if (alive) {
            UpdateAI(dt, ctx);
            position += velocity * dt;

            float groundY = ctx.world.pointBlocked(position, 0.42f) ? position.GetY() : ctx.floorY;
            position.SetY(MathUtils::Damp(position.GetY(), groundY, 12.0f, dt));
            position.SetX(MathUtils::Clamp(position.GetX(), -42.5f, 42.5f));
            position.SetZ(MathUtils::Clamp(position.GetZ(), -42.5f, 42.5f));

            speed = JPH::Vec3(velocity.GetX(), 0.0f, velocity.GetZ()).Length();
            m_stepPhase += (speed / 1.4f) * dt;
            if (m_stepPhase > 1.0f) {
                m_stepPhase -= 1.0f;
                if ((position - ctx.playerPos).LengthSq() < 324.0f && ctx.fx.playBeep)
                    ctx.fx.playBeep(220.0f, 0.05f, 0.15f);
            }

            if (ctx.updateAnimation)
                m_stance = ctx.updateAnimation(dt, speed, crouch, aimYaw, aimPitch, aiming);

            UpdateBoneWorldMatrices();
        } else {
            ragdoll.Step(dt, -18.0f, 0.992f, 4, [&](JPH::Vec3& p, float) {
                if (p.GetY() < ctx.floorY + 0.1f)
                    p.SetY(ctx.floorY + 0.1f);
            });
            for (uint32_t i = 0; i < Rig::JointCount; i++)
                boneWorldPositions[i] = ragdoll.positions[i];
        }

        if (weaponDropped) {
            weaponVel.SetY(weaponVel.GetY() - 20.0f * dt);
            weaponPos += weaponVel * dt;
            weaponRot = (JPH::Quat::sRotation(JPH::Vec3::sAxisX(), weaponSpin.GetX() * dt) * JPH::Quat::sRotation(JPH::Vec3::sAxisY(), weaponSpin.GetY() * dt) *
                         JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), weaponSpin.GetZ() * dt) * weaponRot)
                            .Normalized();

            if (weaponPos.GetY() < ctx.floorY + 0.05f) {
                weaponPos.SetY(ctx.floorY + 0.05f);
                weaponVel *= 0.35f;
                weaponVel.SetY(weaponVel.GetY() * -0.3f);
                weaponSpin *= 0.3f;
            }
        }
    }

  private:
    float        m_stepPhase = 0.0f;
    WeaponStance m_stance {};

    void UpdateBoneWorldMatrices() {
        JPH::Mat44 rootMatrix = JPH::Mat44::sRotationTranslation(JPH::Quat::sRotation(JPH::Vec3::sAxisY(), yaw), position);
        for (uint32_t i = 0; i < Rig::JointCount; i++) {
            JPH::Mat44 local =
                JPH::Mat44::sRotationTranslation(Rig::GetBindRotation(static_cast<Rig::Joint>(i)), Rig::GetBindPosition(static_cast<Rig::Joint>(i)));
            int32_t parent = Rig::GetParentIndex(static_cast<Rig::Joint>(i));
            boneWorldPositions[i] =
                ((parent >= 0) ? JPH::Mat44::sRotationTranslation(JPH::Quat::sIdentity(), boneWorldPositions[parent]) * local : rootMatrix * local)
                    .GetTranslation();
        }
    }

    void DropWeapon(JPH::Vec3Arg dir) {
        weaponDropped = true;
        weaponPos     = m_stance.position;
        weaponRot     = m_stance.rotation;
        std::mt19937                          gen(std::random_device {}());
        std::uniform_real_distribution<float> dis(-6.0f, 6.0f);
        weaponVel  = dir * (1.5f + ((std::rand() % 100) / 100.0f) * 2.0f) + JPH::Vec3(0, 2.0f + ((std::rand() % 100) / 100.0f) * 2.0f, 0);
        weaponSpin = JPH::Vec3(dis(gen), dis(gen), dis(gen));
    }

    void Steer(float dt, ActorContext& ctx, JPH::Vec3Arg desired, float maxSpeed) {
        JPH::Vec3 dir = desired - position;
        dir.SetY(0.0f);
        float d = dir.Length();
        if (d < 0.4f) {
            velocity *= std::max(0.0f, 1.0f - 6.0f * dt);
            return;
        }
        dir /= d;

        if (ctx.world.pointBlocked(position + dir * 1.5f, 0.7f)) {
            float     s = std::sin(0.9f) * MathUtils::Clamp(strafeDir, -1.0f, 1.0f), c = std::cos(0.9f);
            JPH::Vec3 alt(dir.GetX() * c - dir.GetZ() * s, 0.0f, dir.GetX() * s + dir.GetZ() * c);
            dir = ctx.world.pointBlocked(position + alt * 1.5f, 0.7f) ? JPH::Vec3(dir.GetX() * c + dir.GetZ() * s, 0.0f, -dir.GetX() * s + dir.GetZ() * c) :
                                                                        alt;
        }

        velocity.SetX(MathUtils::Damp(velocity.GetX(), dir.GetX() * maxSpeed, 7.0f, dt));
        velocity.SetZ(MathUtils::Damp(velocity.GetZ(), dir.GetZ() * maxSpeed, 7.0f, dt));
    }

    void UpdateAI(float dt, ActorContext& ctx) {
        JPH::Vec3 toPlayer = ctx.playerPos - position;
        float     dist     = toPlayer.Length();
        JPH::Vec3 eyePos = position + JPH::Vec3(0, 1.55f, 0), targetPoint = ctx.playerPos + JPH::Vec3(0, 1.3f, 0);

        canSeePlayer = ctx.playerAlive && dist < 62.0f &&
                       (std::abs(MathUtils::AngleWrap(std::atan2(toPlayer.GetX(), toPlayer.GetZ()) - yaw)) < 1.5f || alertness > 0.4f) &&
                       ctx.world.lineOfSight(eyePos, targetPoint);
        if (canSeePlayer) {
            lastKnownPlayerPos = ctx.playerPos;
            sawTime            = ctx.time;
            alertness          = std::min(1.0f, alertness + dt * 2.5f);
        } else {
            alertness = std::max(0.0f, alertness - dt * 0.12f);
        }

        stateT -= dt;
        bool recent = (ctx.time - sawTime < 6.0f);

        switch (state) {
            case AIState::Idle:
            case AIState::Patrol:
                aiming = MathUtils::Damp(aiming, 0.15f, 4.0f, dt);
                crouch = MathUtils::Damp(crouch, 0.0f, 4.0f, dt);
                if (canSeePlayer) {
                    state  = AIState::Engage;
                    stateT = 1.2f + ((std::rand() % 100) / 100.0f);
                    fireCd = std::max(fireCd, 0.35f + ((std::rand() % 100) / 100.0f) * 0.55f);
                    break;
                }
                if (stateT <= 0.0f) {
                    targetPos = ctx.world.getRandomSpawnPoint ? ctx.world.getRandomSpawnPoint(0) : position;
                    stateT    = 4.0f + ((std::rand() % 100) / 100.0f) * 4.0f;
                }
                if (recent)
                    targetPos = lastKnownPlayerPos;
                Steer(dt, ctx, targetPos, recent ? 4.4f : 1.9f);
                break;
            case AIState::Engage:
                aiming = MathUtils::Damp(aiming, 1.0f, 6.0f, dt);
                crouch = MathUtils::Damp(crouch, dist > 26.0f ? 0.35f : 0.0f, 3.0f, dt);
                if (!canSeePlayer) {
                    state  = recent ? AIState::Reposition : AIState::Patrol;
                    stateT = recent ? 2.0f + ((std::rand() % 100) / 100.0f) * 2.0f : 0.0f;
                    if (recent)
                        targetPos = lastKnownPlayerPos;
                    break;
                }

                strafeT -= dt;
                if (strafeT <= 0.0f) {
                    strafeDir = ((std::rand() % 100) / 100.0f < 0.5f) ? -1.0f : 1.0f;
                    strafeT   = 1.0f + ((std::rand() % 100) / 100.0f) * 2.0f;
                }

                Steer(
                    dt, ctx,
                    position + toPlayer.Normalized() * MathUtils::Clamp(dist - 13.0f, -7.0f, 9.0f) +
                        JPH::Vec3(-toPlayer.Normalized().GetZ(), 0.0f, toPlayer.Normalized().GetX()) * (4.5f * strafeDir),
                    dist > 19.0f ? 4.2f : 2.4f
                );

                if (stateT <= 0.0f && ((std::rand() % 100) / 100.0f) < 0.5f) {
                    state  = AIState::Reposition;
                    stateT = 1.6f + ((std::rand() % 100) / 100.0f) * 1.8f;
                    if (ctx.world.queryCoverPoints) {
                        auto covers = ctx.world.queryCoverPoints(ctx.playerPos, 30.0f);
                        if (!covers.empty())
                            targetPos = covers[0];
                    }
                }
                break;
            case AIState::Reposition:
                aiming = MathUtils::Damp(aiming, 0.6f, 4.0f, dt);
                crouch = MathUtils::Damp(crouch, 0.0f, 4.0f, dt);
                Steer(dt, ctx, targetPos, 4.6f);
                if (stateT <= 0.0f || (position - targetPos).Length() < 1.2f) {
                    state  = canSeePlayer ? AIState::Engage : AIState::Patrol;
                    stateT = 1.5f + ((std::rand() % 100) / 100.0f);
                }
                break;
            case AIState::Suppressed:
                aiming = MathUtils::Damp(aiming, 0.4f, 5.0f, dt);
                crouch = MathUtils::Damp(crouch, 0.85f, 7.0f, dt);
                velocity *= std::max(0.0f, 1.0f - 5.0f * dt);
                if (stateT <= 0.0f) {
                    state  = AIState::Engage;
                    stateT = 1.5f;
                }
                break;
            default:
                break;
        }

        JPH::Vec3 aimAt     = canSeePlayer ? targetPoint : lastKnownPlayerPos + JPH::Vec3(0, 1.3f, 0);
        JPH::Vec3 deltaAim  = aimAt - eyePos;
        float     wantYaw   = std::atan2(deltaAim.GetX(), deltaAim.GetZ());
        float     wantPitch = -std::atan2(deltaAim.GetY(), JPH::Vec3(deltaAim.GetX(), 0, deltaAim.GetZ()).Length());

        float turnSpeed = canSeePlayer ? 9.0f : 3.0f;
        aimYaw += MathUtils::AngleWrap(wantYaw - aimYaw) * std::min(1.0f, turnSpeed * dt);
        aimPitch = MathUtils::Damp(aimPitch, wantPitch, turnSpeed, dt);

        float moveSpeed = velocity.Length();
        float bodyWant  = (moveSpeed > 2.6f && !canSeePlayer) ? std::atan2(velocity.GetX(), velocity.GetZ()) :
                          (moveSpeed > 0.5f)                  ? aimYaw + MathUtils::AngleWrap(std::atan2(velocity.GetX(), velocity.GetZ()) - aimYaw) * 0.35f :
                                                                aimYaw;
        yaw += MathUtils::AngleWrap(bodyWant - yaw) * std::min(1.0f, 7.0f * dt);

        fireCd -= dt;
        if (reloadT > 0.0f) {
            reloadT -= dt;
            if (reloadT <= 0.0f)
                ammo = 30;
            return;
        }

        if (canSeePlayer && state != AIState::Suppressed && std::abs(MathUtils::AngleWrap(aimYaw - wantYaw)) < 0.22f && dist < 55.0f && ctx.playerAlive) {
            if (ammo <= 0) {
                reloadT = 2.4f;
                if (ctx.fx.reloadWeapon)
                    ctx.fx.reloadWeapon();
                return;
            }
            if (burstCount <= 0 && fireCd <= 0.0f) {
                burstCount = 2 + (std::rand() % 3);
                fireCd     = 0.0f;
            }
            if (burstCount > 0 && fireCd <= 0.0f) {
                ExecuteWeaponShoot(ctx, dist);
                burstCount--;
                fireCd = (burstCount > 0) ? 0.11f : 0.55f + ((std::rand() % 100) / 100.0f) * 0.9f;
            }
        }
    }

    void ExecuteWeaponShoot(ActorContext& ctx, float dist) {
        ammo--;
        if (ctx.fx.fireWeapon)
            ctx.fx.fireWeapon();

        JPH::Vec3 origin = m_stance.muzzleWorld, dir = m_stance.aimDir;
        JPH::Vec3 toTarget = (ctx.playerPos + JPH::Vec3(0, 1.2f, 0) - origin);
        if (toTarget.Length() > 1e-4f)
            dir = (dir + (toTarget.Normalized() - dir) * 0.85f).Normalized();

        std::mt19937                          gen(std::random_device {}());
        std::uniform_real_distribution<float> spreadDis(-1.0f, 1.0f);
        float                                 spread = 0.012f + dist * 0.0022f + (state == AIState::Reposition ? 0.03f : 0.0f);
        dir                                          = (dir + JPH::Vec3(spreadDis(gen), spreadDis(gen), spreadDis(gen)) * spread).Normalized();

        float     maxD     = 90.0f;
        JPH::Vec3 endPoint = origin + dir * maxD;

        if (ctx.world.raycastWorld) {
            if (auto hit = ctx.world.raycastWorld(origin, dir, maxD)) {
                endPoint = hit->point;
                if (ctx.fx.spawnImpact)
                    ctx.fx.spawnImpact(hit->point, hit->normal, 0);
            }
        }

        auto playerHit = RaySegmentDistance(origin, dir, ctx.playerPos + JPH::Vec3(0, 0.3f, 0), ctx.playerPos + JPH::Vec3(0, 1.55f, 0));
        if (ctx.playerAlive && playerHit && playerHit->dist < 0.38f && playerHit->t < maxD) {
            endPoint = playerHit->point;
            if (ctx.damagePlayer)
                ctx.damagePlayer(5.0f + ((std::rand() % 100) / 100.0f) * 5.0f, origin);
        }

        if (ctx.fx.spawnTracer)
            ctx.fx.spawnTracer(origin, endPoint, 0xffb347);
        if (ctx.fx.spawnMuzzleFlash)
            ctx.fx.spawnMuzzleFlash(origin, dir, 0.55f);
        if (ctx.fx.playBeep)
            ctx.fx.playBeep(900.0f, 0.12f, std::max(0.05f, 1.0f - (origin - ctx.playerPos).Length() / 55.0f));
    }
};

} // namespace ZHLN::Actor
