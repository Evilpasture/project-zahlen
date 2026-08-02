module;
#include <Jolt/Jolt.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <algorithm>
#include <cmath>
#include <optional>
#include <random>
#include <string_view>
#include <vector>
export module ZHLN.Actor;

import ZHLN.Rig;
import ZHLN.Ragdoll;
import ZHLN.MathUtils; // Importing our shared math utils (Lerp, Clamp, Damp, AngleWrap)

export namespace ZHLN::Actor {

enum class AIState : uint8_t { Idle, Patrol, Engage, Reposition, Suppressed, Dead };

struct BodyHit {
    float      t;
    JPH::Vec3  point;
    JPH::Vec3  normal;
    Rig::Joint joint;
    float      mult;
    uint32_t   zone; // 0=Head, 1=Torso, 2=Limb
};

struct WeaponStance {
    JPH::Vec3 position    = JPH::Vec3::sZero();
    JPH::Quat rotation    = JPH::Quat::sIdentity();
    JPH::Vec3 muzzleWorld = JPH::Vec3::sZero();
    JPH::Vec3 aimDir      = JPH::Vec3::sAxisZ();
};

/**
 * @brief Represents external systems passed to the actor during updates.
 * Completely decouples the actor from global singleton dependencies.
 */
struct ActorContext {
    JPH::Vec3 playerPos;
    bool      playerAlive;
    float     time;
    float     floorY;

    // Abstract interface hook for pathfinding and cover queries
    struct {
        std::function<bool(JPH::Vec3Arg, float)>                                 pointBlocked;
        std::function<bool(JPH::Vec3Arg, JPH::Vec3Arg)>                          lineOfSight;
        std::function<JPH::Vec3(uint32_t)>                                       getRandomSpawnPoint;
        std::function<std::vector<JPH::Vec3>(JPH::Vec3Arg, float)>               queryCoverPoints;
        std::function<std::optional<BodyHit>(JPH::Vec3Arg, JPH::Vec3Arg, float)> raycastWorld;
    } world;

    // Interface hooks for visual and auditory feedback
    struct {
        std::function<void(float, float, float)>                      playBeep;    // Freq, Duration, Vol
        std::function<void(JPH::Vec3Arg, JPH::Vec3Arg, uint32_t)>     spawnImpact; // Pos, Normal, Type
        std::function<void(JPH::Vec3Arg, JPH::Vec3Arg, uint32_t)>     spawnTracer; // Start, End, Color
        std::function<void(JPH::Vec3Arg, JPH::Vec3Arg, float)>        spawnMuzzleFlash;
        std::function<void(JPH::Vec3Arg, JPH::Vec3Arg, JPH::Vec3Arg)> ejectCasing;  // Pos, Right, Up
        std::function<void()>                                         fireWeapon;   // Trigger recoil kick
        std::function<void()>                                         reloadWeapon; // Trigger reload sequence
    } fx;

    // Decoupled Animation driver callback
    std::function<WeaponStance(float dt, float speed, float crouch, float aimYaw, float aimPitch, float aiming)> updateAnimation;

    std::function<void(float, JPH::Vec3Arg)> damagePlayer;
    std::function<void(bool)>                onKilled; // true if headshot
};

// ============================================================================
// AUXILIARY MATHEMATICS
// ============================================================================

struct SegmentIntersection {
    float     t;
    float     dist;
    JPH::Vec3 point;
    JPH::Vec3 segmentPoint;
};

/**
 * @brief Calculates the closest approach (SDF) between a ray and a line segment.
 * Extremely critical for locational capsule damage registration.
 */
inline std::optional<SegmentIntersection> RaySegmentDistance(JPH::Vec3Arg o, JPH::Vec3Arg d, JPH::Vec3Arg a, JPH::Vec3Arg b) noexcept {
    JPH::Vec3 ab   = b - a;
    JPH::Vec3 ao   = o - a;
    float     abab = ab.Dot(ab);
    float     abd  = ab.Dot(d);
    float     abao = ab.Dot(ao);
    float     dao  = d.Dot(ao);
    float     den  = abab - abd * abd;

    float s = 0.0f;
    float t = 0.0f;

    if (std::abs(den) < 1e-6f) {
        s = 0.0f;
        t = -dao;
    } else {
        s = MathUtils::Clamp((abd * dao - abao) / den * -1.0f, 0.0f, 1.0f);
        t = abd * s - dao;
    }

    if (t < 0.0f)
        return std::nullopt;

    JPH::Vec3 pointOnRay     = o + d * t;
    JPH::Vec3 pointOnSegment = a + ab * s;

    return SegmentIntersection {.t = t, .dist = (pointOnRay - pointOnSegment).Length(), .point = pointOnRay, .segmentPoint = pointOnSegment};
}

// ============================================================================
// THE ACTOR CLASS
// ============================================================================

class StandardActor {
  public:
    // Transform & Locomotion
    JPH::Vec3 position = JPH::Vec3::sZero();
    JPH::Vec3 velocity = JPH::Vec3::sZero();
    float     yaw      = 0.0f;
    float     aimYaw   = 0.0f;
    float     aimPitch = 0.0f;
    float     speed    = 0.0f;
    float     crouch   = 0.0f;
    float     aiming   = 0.0f;

    // Vitals
    float health    = 100.0f;
    float maxHealth = 100.0f;
    bool  alive     = true;
    float flash     = 0.0f;
    bool  headshot  = false;

    // AI & Combat State Machine
    AIState   state              = AIState::Idle;
    float     stateT             = 0.0f;
    JPH::Vec3 targetPos          = JPH::Vec3::sZero();
    JPH::Vec3 lastKnownPlayerPos = JPH::Vec3::sZero();
    bool      canSeePlayer       = false;
    float     sawTime            = -99.0f;
    int32_t   ammo               = 30;
    int32_t   burstCount         = 0;
    float     fireCd             = 0.0f;
    float     reloadT            = 0.0f;
    float     strafeDir          = 1.0f;
    float     strafeT            = 0.0f;
    float     alertness          = 0.0f;
    float     deathTime          = -1.0f;

    // Custom Physical Verlet Solver
    Physics::VerletSolver                  ragdoll;
    std::array<JPH::Vec3, Rig::JointCount> boneWorldPositions;

    // Weapon physics state on drop
    bool      weaponDropped = false;
    JPH::Vec3 weaponPos     = JPH::Vec3::sZero();
    JPH::Quat weaponRot     = JPH::Quat::sIdentity();
    JPH::Vec3 weaponVel     = JPH::Vec3::sZero();
    JPH::Vec3 weaponSpin    = JPH::Vec3::sZero();

    StandardActor() {
        std::fill(boneWorldPositions.begin(), boneWorldPositions.end(), JPH::Vec3::sZero());
        // Custom Verlet Ragdoll initialization
        for (uint32_t i = 0; i < Rig::JointCount; i++) {
            ragdoll.AddParticle(Rig::GetBindPosition(static_cast<Rig::Joint>(i)), 1.0f);
        }
        for (const auto& cap: Rig::HIT_CAPSULES) {
            ragdoll.AddConstraint(static_cast<uint32_t>(cap.a), static_cast<uint32_t>(cap.b), 1.0f);
        }
    }

    void SetPosition(JPH::Vec3Arg pos) {
        position  = pos;
        targetPos = pos;
    }

    /**
     * @brief Performs analytical raycasting against the actor's hierarchical hit capsules.
     */
    [[nodiscard]] std::optional<BodyHit> Raycast(JPH::Vec3Arg origin, JPH::Vec3Arg direction, float maxT) const noexcept {
        // Broad phase bounding sphere check
        JPH::Vec3 center   = boneWorldPositions[static_cast<size_t>(Rig::Joint::Spine)];
        JPH::Vec3 toCenter = center - origin;
        float     along    = toCenter.Dot(direction);
        if (along < -1.5f || along > maxT + 1.5f)
            return std::nullopt;
        if ((toCenter - direction * along).LengthSq() > 2.6f * 2.6f)
            return std::nullopt;

        std::optional<BodyHit> bestHit = std::nullopt;
        float                  bestT   = maxT;

        for (const auto& cap: Rig::HIT_CAPSULES) {
            JPH::Vec3 pA = boneWorldPositions[static_cast<size_t>(cap.a)];
            JPH::Vec3 pB = boneWorldPositions[static_cast<size_t>(cap.b)];

            auto intersection = RaySegmentDistance(origin, direction, pA, pB);
            if (intersection && intersection->dist <= cap.r && intersection->t < bestT) {
                bestT            = intersection->t;
                JPH::Vec3 normal = (intersection->point - intersection->segmentPoint).Normalized();
                bestHit = BodyHit {.t = intersection->t, .point = intersection->point, .normal = normal, .joint = cap.a, .mult = cap.mult, .zone = cap.zone};
            }
        }
        return bestHit;
    }

    void Damage(float amount, const BodyHit& hit, JPH::Vec3Arg dir, ActorContext& ctx, bool fromPlayer) {
        if (!alive) {
            // Corpse pushing
            ragdoll.ApplyImpulse(static_cast<uint32_t>(hit.joint), dir * 4.5f);
            ctx.fx.spawnImpact(hit.point, -dir, 1); // 1 = Flesh/Blood
            return;
        }

        float damageDealt = amount * hit.mult;
        health -= damageDealt;
        flash     = 1.0f;
        alertness = 1.0f;

        if (fromPlayer) {
            lastKnownPlayerPos = ctx.playerPos;
            sawTime            = ctx.time;
        }
        ctx.fx.spawnImpact(hit.point, -dir, 1); // Blood Mist

        if (health <= 0.0f) {
            alive     = false;
            state     = AIState::Dead;
            headshot  = (hit.zone == 0);
            deathTime = ctx.time;

            float power = headshot ? 8.5f : 5.5f;

            // Trigger Custom Verlet Ragdoll Dissolve
            // Capture current animated pose into Verlet positions first
            for (uint32_t i = 0; i < Rig::JointCount; i++) {
                ragdoll.positions[i]         = boneWorldPositions[i];
                ragdoll.previousPositions[i] = boneWorldPositions[i];
            }

            // Execute the custom physical impulse directly on the Verlet Solver
            std::random_device                    rd;
            std::mt19937                          gen(rd());
            std::uniform_real_distribution<float> dis(0.75f, 1.25f);

            // Custom physical kill
            ragdoll.ApplyImpulse(static_cast<uint32_t>(hit.joint), dir * (power * dis(gen)));
            DropWeapon(dir);

            if (ctx.onKilled) {
                ctx.onKilled(headshot);
            }
            return;
        }

        // Flinching reaction
        std::vector<uint32_t> flinchRegion;
        if (hit.zone == 0) {
            flinchRegion = {static_cast<uint32_t>(Rig::Joint::Head), static_cast<uint32_t>(Rig::Joint::Neck)};
        } else {
            flinchRegion = {static_cast<uint32_t>(Rig::Joint::Spine), static_cast<uint32_t>(Rig::Joint::Chest)};
        }

        for (uint32_t b: flinchRegion) {
            ragdoll.ApplyImpulse(b, dir * (hit.zone == 0 ? 5.0f : 3.5f));
        }

        velocity += dir * 1.6f;
        if (((std::rand() % 100) / 100.0f) < 0.45f || health < 40.0f) {
            state  = AIState::Suppressed;
            stateT = 0.5f + ((std::rand() % 100) / 100.0f) * 0.8f;
        }
    }

    void Update(float dt, ActorContext& ctx) {
        if (flash > 0.0f) {
            flash = std::max(0.0f, flash - dt * 5.0f);
        }

        if (alive) {
            UpdateAI(dt, ctx);

            // Integrate movement
            position += velocity * dt;

            // Snap to terrain height
            float groundY = ctx.world.pointBlocked(position, 0.42f) ? position.GetY() : ctx.floorY;
            position.SetY(MathUtils::Damp(position.GetY(), groundY, 12.0f, dt));

            // Obstacle Avoidance / Boundary Constraints
            float boundLimit = 42.5f; // Simplified boundary check
            position.SetX(MathUtils::Clamp(position.GetX(), -boundLimit, boundLimit));
            position.SetZ(MathUtils::Clamp(position.GetZ(), -boundLimit, boundLimit));

            speed = JPH::Vec3(velocity.GetX(), 0.0f, velocity.GetZ()).Length();

            // Footstep timing loop
            m_stepPhase += (speed / 1.4f) * dt;
            if (m_stepPhase > 1.0f) {
                m_stepPhase -= 1.0f;
                if ((position - ctx.playerPos).LengthSq() < 18.0f * 18.0f && ctx.fx.playBeep) {
                    ctx.fx.playBeep(220.0f, 0.05f, 0.15f); // Sizzling footstep click
                }
            }

            // Sync procedural animation through the abstract callback
            if (ctx.updateAnimation) {
                m_stance = ctx.updateAnimation(dt, speed, crouch, aimYaw, aimPitch, aiming);
            }

            // Populate current animated world space bone array
            UpdateBoneWorldMatrices();
        } else {
            // Solve Verlet Ragdoll
            ragdoll.Step(dt, -18.0f, 0.992f, 4, [&](JPH::Vec3& p, float) {
                if (p.GetY() < ctx.floorY + 0.1f) {
                    p.SetY(ctx.floorY + 0.1f);
                }
            });

            // Sync boneWorldPositions array straight to the solved Verlet particles
            for (uint32_t i = 0; i < Rig::JointCount; i++) {
                boneWorldPositions[i] = ragdoll.positions[i];
            }
        }

        if (weaponDropped) {
            weaponVel.SetY(weaponVel.GetY() - 20.0f * dt);
            weaponPos += weaponVel * dt;

            // Simple spinning Euler rotation
            JPH::Quat rotX = JPH::Quat::sRotation(JPH::Vec3::sAxisX(), weaponSpin.GetX() * dt);
            JPH::Quat rotY = JPH::Quat::sRotation(JPH::Vec3::sAxisY(), weaponSpin.GetY() * dt);
            JPH::Quat rotZ = JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), weaponSpin.GetZ() * dt);
            weaponRot      = (rotX * rotY * rotZ * weaponRot).Normalized();

            float groundY = ctx.floorY + 0.05f;
            if (weaponPos.GetY() < groundY) {
                weaponPos.SetY(groundY);
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

        // Solve FK recursively to project local bind pose bones to world space
        for (uint32_t i = 0; i < Rig::JointCount; i++) {
            JPH::Mat44 local =
                JPH::Mat44::sRotationTranslation(Rig::GetBindRotation(static_cast<Rig::Joint>(i)), Rig::GetBindPosition(static_cast<Rig::Joint>(i)));
            int32_t parent = Rig::GetParentIndex(static_cast<Rig::Joint>(i));

            JPH::Mat44 world      = (parent >= 0) ? JPH::Mat44::sRotationTranslation(JPH::Quat::sIdentity(), boneWorldPositions[parent]) * local :
                                                    rootMatrix * local;
            boneWorldPositions[i] = world.GetTranslation();
        }
    }

    void DropWeapon(JPH::Vec3Arg dir) {
        weaponDropped = true;
        weaponPos     = m_stance.position;
        weaponRot     = m_stance.rotation;

        std::random_device                    rd;
        std::mt19937                          gen(rd());
        std::uniform_real_distribution<float> dis(-6.0f, 6.0f);

        weaponVel = dir * (1.5f + ((std::rand() % 100) / 100.0f) * 2.0f);
        weaponVel.SetY(weaponVel.GetY() + 2.0f + ((std::rand() % 100) / 100.0f) * 2.0f);
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

        // Obstacle Avoidance: probe 1.5m ahead
        JPH::Vec3 probe = position + dir * 1.5f;
        if (ctx.world.pointBlocked(probe, 0.7f)) {
            float     s  = std::sin(0.9f) * MathUtils::Clamp(strafeDir, -1.0f, 1.0f);
            float     c  = std::cos(0.9f);
            float     nx = dir.GetX() * c - dir.GetZ() * s;
            float     nz = dir.GetX() * s + dir.GetZ() * c;
            JPH::Vec3 alternative(nx, 0.0f, nz);

            if (ctx.world.pointBlocked(position + alternative * 1.5f, 0.7f)) {
                dir = JPH::Vec3(dir.GetX() * c + dir.GetZ() * s, 0.0f, -dir.GetX() * s + dir.GetZ() * c);
            } else {
                dir = alternative;
            }
        }

        float accel = 14.0f;
        velocity.SetX(MathUtils::Damp(velocity.GetX(), dir.GetX() * maxSpeed, accel * 0.5f, dt));
        velocity.SetZ(MathUtils::Damp(velocity.GetZ(), dir.GetZ() * maxSpeed, accel * 0.5f, dt));
    }

    void UpdateAI(float dt, ActorContext& ctx) {
        JPH::Vec3 toPlayer    = ctx.playerPos - position;
        float     dist        = toPlayer.Length();
        JPH::Vec3 eyePos      = position + JPH::Vec3(0, 1.55f, 0);
        JPH::Vec3 targetPoint = ctx.playerPos + JPH::Vec3(0, 1.3f, 0);

        float angleToPlayer = std::atan2(toPlayer.GetX(), toPlayer.GetZ());
        float facing        = std::abs(MathUtils::AngleWrap(angleToPlayer - yaw));

        canSeePlayer = ctx.playerAlive && dist < 62.0f && (facing < 1.5f || alertness > 0.4f) && ctx.world.lineOfSight(eyePos, targetPoint);
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
            case AIState::Patrol: {
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
            }
            case AIState::Engage: {
                aiming = MathUtils::Damp(aiming, 1.0f, 6.0f, dt);
                crouch = MathUtils::Damp(crouch, dist > 26.0f ? 0.35f : 0.0f, 3.0f, dt);
                if (!canSeePlayer) {
                    if (!recent) {
                        state  = AIState::Patrol;
                        stateT = 0.0f;
                    } else {
                        state     = AIState::Reposition;
                        stateT    = 2.0f + ((std::rand() % 100) / 100.0f) * 2.0f;
                        targetPos = lastKnownPlayerPos;
                    }
                    break;
                }

                strafeT -= dt;
                if (strafeT <= 0.0f) {
                    strafeDir = ((std::rand() % 100) / 100.0f < 0.5f) ? -1.0f : 1.0f;
                    strafeT   = 1.0f + ((std::rand() % 100) / 100.0f) * 2.0f;
                }

                float     ideal = 13.0f;
                JPH::Vec3 dir   = toPlayer.Normalized();
                JPH::Vec3 side(-dir.GetZ(), 0.0f, dir.GetX());
                JPH::Vec3 want = position + dir * MathUtils::Clamp(dist - ideal, -7.0f, 9.0f) + side * (4.5f * strafeDir);

                Steer(dt, ctx, want, dist > ideal + 6.0f ? 4.2f : 2.4f);

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
            }
            case AIState::Reposition: {
                aiming = MathUtils::Damp(aiming, 0.6f, 4.0f, dt);
                crouch = MathUtils::Damp(crouch, 0.0f, 4.0f, dt);
                Steer(dt, ctx, targetPos, 4.6f);
                if (stateT <= 0.0f || (position - targetPos).Length() < 1.2f) {
                    state  = canSeePlayer ? AIState::Engage : AIState::Patrol;
                    stateT = 1.5f + ((std::rand() % 100) / 100.0f);
                }
                break;
            }
            case AIState::Suppressed: {
                aiming = MathUtils::Damp(aiming, 0.4f, 5.0f, dt);
                crouch = MathUtils::Damp(crouch, 0.85f, 7.0f, dt);
                velocity *= std::max(0.0f, 1.0f - 5.0f * dt);
                if (stateT <= 0.0f) {
                    state  = AIState::Engage;
                    stateT = 1.5f;
                }
                break;
            }
            default:
                break;
        }

        // Aiming interpolations
        JPH::Vec3 aimAt     = canSeePlayer ? targetPoint : lastKnownPlayerPos + JPH::Vec3(0, 1.3f, 0);
        JPH::Vec3 deltaAim  = aimAt - eyePos;
        float     wantYaw   = std::atan2(deltaAim.GetX(), deltaAim.GetZ());
        float     wantPitch = -std::atan2(deltaAim.GetY(), JPH::Vec3(deltaAim.GetX(), 0, deltaAim.GetZ()).Length());

        float turnSpeed = canSeePlayer ? 9.0f : 3.0f;
        aimYaw          = aimYaw + MathUtils::AngleWrap(wantYaw - aimYaw) * std::min(1.0f, turnSpeed * dt);
        aimPitch        = MathUtils::Damp(aimPitch, wantPitch, turnSpeed, dt);

        float moveSpeed = velocity.Length();
        float bodyWant  = aimYaw;
        if (moveSpeed > 2.6f && !canSeePlayer) {
            bodyWant = std::atan2(velocity.GetX(), velocity.GetZ());
        } else if (moveSpeed > 0.5f) {
            bodyWant = aimYaw + MathUtils::AngleWrap(std::atan2(velocity.GetX(), velocity.GetZ()) - aimYaw) * 0.35f;
        }
        yaw = yaw + MathUtils::AngleWrap(bodyWant - yaw) * std::min(1.0f, 7.0f * dt);

        // Firing Logic
        fireCd -= dt;
        if (reloadT > 0.0f) {
            reloadT -= dt;
            if (reloadT <= 0.0f)
                ammo = 30;
            return;
        }

        float aimError = std::abs(MathUtils::AngleWrap(aimYaw - wantYaw));
        if (canSeePlayer && state != AIState::Suppressed && aimError < 0.22f && dist < 55.0f && ctx.playerAlive) {
            if (ammo <= 0) {
                reloadT = 2.4f;
                if (ctx.fx.reloadWeapon) { // trigger reload sequence
                    ctx.fx.reloadWeapon();
                }
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
        if (ctx.fx.fireWeapon) {
            ctx.fx.fireWeapon();
        }

        JPH::Vec3 origin = m_stance.muzzleWorld;
        JPH::Vec3 dir    = m_stance.aimDir;

        JPH::Vec3 targetCenter = ctx.playerPos + JPH::Vec3(0, 1.2f, 0);
        JPH::Vec3 toTarget     = (targetCenter - origin);
        float     dLen         = toTarget.Length();
        if (dLen > 1e-4f) {
            dir = (dir + (toTarget / dLen - dir) * 0.85f).Normalized();
        }

        std::random_device                    rd;
        std::mt19937                          gen(rd());
        std::uniform_real_distribution<float> spreadDis(-1.0f, 1.0f);
        float                                 spread = 0.012f + dist * 0.0022f + (state == AIState::Reposition ? 0.03f : 0.0f);

        dir += JPH::Vec3(spreadDis(gen), spreadDis(gen), spreadDis(gen)) * spread;
        dir = dir.Normalized();

        float     maxD     = 90.0f;
        JPH::Vec3 endPoint = origin + dir * maxD;

        if (ctx.world.raycastWorld) {
            auto hit = ctx.world.raycastWorld(origin, dir, maxD);
            if (hit) {
                endPoint = hit->point;
                if (ctx.fx.spawnImpact)
                    ctx.fx.spawnImpact(hit->point, hit->normal, 0); // 0 = Concrete
            }
        }

        // Ray vs Player Capsule Hit Test
        auto playerHit = RaySegmentDistance(origin, dir, ctx.playerPos + JPH::Vec3(0, 0.3f, 0), ctx.playerPos + JPH::Vec3(0, 1.55f, 0));
        if (ctx.playerAlive && playerHit && playerHit->dist < 0.38f && playerHit->t < maxD) {
            endPoint = playerHit->point;
            if (ctx.damagePlayer) {
                ctx.damagePlayer(5.0f + ((std::rand() % 100) / 100.0f) * 5.0f, origin);
            }
        }

        if (ctx.fx.spawnTracer)
            ctx.fx.spawnTracer(origin, endPoint, 0xffb347);
        if (ctx.fx.spawnMuzzleFlash)
            ctx.fx.spawnMuzzleFlash(origin, dir, 0.55f);

        // Sound cue via the callback
        if (ctx.fx.playBeep) {
            float dCam = (origin - ctx.playerPos).Length();
            float vol  = std::max(0.05f, 1.0f - dCam / 55.0f);
            ctx.fx.playBeep(900.0f, 0.12f, vol); // Low synthetic burst
        }
    }
};

} // namespace ZHLN::Actor
