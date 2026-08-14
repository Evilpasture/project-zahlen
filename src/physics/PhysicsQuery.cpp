// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PhysicsWorld.hpp"
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Zahlen/physics/Physics.hpp>

namespace ZHLN {

namespace {

class QueryFilter final: public JPH::BodyFilter {
    JPH::BodyID _ignoreID;

  public:
    explicit QueryFilter(JPH::BodyID ignore): _ignoreID(ignore) {
    }
    bool ShouldCollide(const JPH::BodyID& inBodyID) const override {
        return inBodyID != _ignoreID;
    }
};

static bool TryGetValidHandle(const Physics::PhysicsWorld& world, JPH::BodyID bodyID, ZHLN::Entity& outHandle) {
    if (bodyID.IsInvalid()) [[unlikely]]
        return false;

    const uint64_t     rawData = world.bodyInterface->GetUserData(bodyID);
    const ZHLN::Entity handle  = ZHLN::Entity::Unpack(rawData);

    if (handle.index >= world.slotCapacity) [[unlikely]]
        return false;

    const uint8_t                state = world.slotStates[handle.index].load(std::memory_order::acquire);
    const Physics::SlotPredicate pred  = Physics::GetSlotPredicate(state);

    if (pred.isActive) {
        outHandle = handle;
        return true;
    }
    return false;
}

} // namespace

Physics::RaycastResult PhysicsContext::Raycast(JPH::RVec3Arg origin, JPH::Vec3Arg direction, float maxDistance, ZHLN::Entity ignore) const {
    const auto& world = GetWorld();

    if (world.isStepping.load(std::memory_order::relaxed))
        return {};

    float lengthSq = direction.LengthSq();
    if (lengthSq < 1e-6f)
        return {};

    JPH::Vec3          scaledDir = direction.Normalized() * maxDistance;
    JPH::RRayCast      ray {origin, scaledDir};
    JPH::RayCastResult hit;

    JPH::BodyID ignoreID = Physics::GetBodyID(world, ignore);
    QueryFilter filter(ignoreID);

    const auto* query  = &world.system->GetNarrowPhaseQuery();
    bool        hasHit = query->CastRay(ray, hit, {}, {}, filter);

    Physics::RaycastResult result {};
    if (hasHit) {
        if (TryGetValidHandle(world, hit.mBodyID, result.handle)) {
            result.hasHit   = true;
            result.fraction = hit.mFraction;
            result.position = ray.GetPointOnRay(hit.mFraction);

            const auto*       lockInterface = &world.system->GetBodyLockInterfaceNoLock();
            JPH::BodyLockRead lock(*lockInterface, hit.mBodyID);
            if (lock.Succeeded()) {
                result.normal = lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, result.position);
            } else {
                result.normal = JPH::Vec3::sAxisY();
            }
        }
    }
    return result;
}

void PhysicsContext::RaycastAll(
    JPH::RVec3Arg                       origin,
    JPH::Vec3Arg                        direction,
    float                               maxDistance,
    JPH::Array<Physics::RaycastResult>& outResults,
    ZHLN::Entity                        ignore
) const {
    const auto& world = GetWorld();

    if (world.isStepping.load(std::memory_order::relaxed))
        return;

    float lengthSq = direction.LengthSq();
    if (lengthSq < 1e-6f)
        return;

    JPH::Vec3     scaledDir = direction.Normalized() * maxDistance;
    JPH::RRayCast ray {origin, scaledDir};

    JPH::RayCastSettings settings;
    settings.mBackFaceModeTriangles = JPH::EBackFaceMode::CollideWithBackFaces;
    settings.mBackFaceModeConvex    = JPH::EBackFaceMode::CollideWithBackFaces;
    settings.mTreatConvexAsSolid    = false;

    JPH::BodyID ignoreID = Physics::GetBodyID(world, ignore);
    QueryFilter filter(ignoreID);

    JPH::AllHitCollisionCollector<JPH::CastRayCollector> collector;
    const auto*                                          query = &world.system->GetNarrowPhaseQuery();
    query->CastRay(ray, settings, collector, {}, {}, filter);

    collector.Sort();

    if (collector.HadHit()) {
        const auto* lockInterface = &world.system->GetBodyLockInterfaceNoLock();

        for (const auto& hit: collector.mHits) {
            Physics::RaycastResult result {};
            if (TryGetValidHandle(world, hit.mBodyID, result.handle)) {
                result.hasHit   = true;
                result.fraction = hit.mFraction;
                result.position = ray.GetPointOnRay(hit.mFraction);

                JPH::BodyLockRead lock(*lockInterface, hit.mBodyID);
                if (lock.Succeeded()) {
                    result.normal = lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, result.position);
                } else {
                    result.normal = JPH::Vec3::sAxisY();
                }
                outResults.push_back(result);
            }
        }
    }
}

void PhysicsContext::RaycastAllPenetrations(
    JPH::RVec3Arg                                  origin,
    JPH::Vec3Arg                                   direction,
    float                                          maxDistance,
    JPH::Array<Physics::RaycastPenetrationResult>& outResults,
    ZHLN::Entity                                   ignore
) const {
    const auto& world = GetWorld();
    if (world.isStepping.load(std::memory_order::relaxed))
        return;

    float lengthSq = direction.LengthSq();
    if (lengthSq < 1e-6f)
        return;

    JPH::Vec3     dirNorm   = direction.Normalized();
    JPH::Vec3     scaledDir = dirNorm * maxDistance;
    JPH::RRayCast ray {origin, scaledDir};

    JPH::RayCastSettings settings;
    settings.mBackFaceModeTriangles = JPH::EBackFaceMode::CollideWithBackFaces;
    settings.mBackFaceModeConvex    = JPH::EBackFaceMode::CollideWithBackFaces;
    settings.mTreatConvexAsSolid    = false;

    JPH::BodyID ignoreID = Physics::GetBodyID(world, ignore);
    QueryFilter filter(ignoreID);

    JPH::AllHitCollisionCollector<JPH::CastRayCollector> collector;
    const auto*                                          query = &world.system->GetNarrowPhaseQuery();
    query->CastRay(ray, settings, collector, {}, {}, filter);

    collector.Sort();

    if (!collector.HadHit())
        return;

    struct IntermediateHit {
        JPH::BodyID bodyID;
        float       fraction;
        JPH::RVec3  position;
        JPH::Vec3   normal;
        bool        isBackFace;
    };

    const auto* lockInterface = &world.system->GetBodyLockInterfaceNoLock();

    JPH::Array<IntermediateHit> hits;
    hits.reserve(collector.mHits.size());

    for (const auto& hit: collector.mHits) {
        ZHLN::Entity handle;
        if (TryGetValidHandle(world, hit.mBodyID, handle)) {
            JPH::BodyLockRead lock(*lockInterface, hit.mBodyID);
            if (lock.Succeeded()) {
                JPH::RVec3 hitPos     = ray.GetPointOnRay(hit.mFraction);
                JPH::Vec3  norm       = lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, hitPos);
                bool       isBackFace = (dirNorm.Dot(norm) > 0.0f);
                hits.push_back({.bodyID = hit.mBodyID, .fraction = hit.mFraction, .position = hitPos, .normal = norm, .isBackFace = isBackFace});
            }
        }
    }

    JPH::Array<bool> processed(hits.size(), false);

    for (size_t i = 0; i < hits.size(); ++i) {
        if (processed[i])
            continue;

        const auto&  entryHit = hits[i];
        ZHLN::Entity handle;
        if (!TryGetValidHandle(world, entryHit.bodyID, handle))
            continue;

        processed[i] = true;

        Physics::RaycastPenetrationResult res {};
        res.hasHit = true;
        res.handle = handle;

        uint32_t dense = world.slotToDense[handle.index];
        res.materialID = world.materialIDs[dense];

        if (!entryHit.isBackFace) {
            res.entryPosition = entryHit.position;
            res.entryNormal   = entryHit.normal;
            res.entryFraction = entryHit.fraction;

            bool foundExit = false;
            for (size_t j = i + 1; j < hits.size(); ++j) {
                if (!processed[j] && hits[j].bodyID == entryHit.bodyID && hits[j].isBackFace) {
                    processed[j]     = true;
                    res.exitPosition = hits[j].position;
                    res.exitNormal   = hits[j].normal;
                    res.exitFraction = hits[j].fraction;
                    foundExit        = true;
                    break;
                }
            }

            if (!foundExit) {
                res.exitFraction = 1.0f;
                res.exitPosition = ray.GetPointOnRay(1.0f);
                res.exitNormal   = -dirNorm;
            }
        } else {
            res.entryPosition = origin;
            res.entryNormal   = -dirNorm;
            res.entryFraction = 0.0f;
            res.exitPosition  = entryHit.position;
            res.exitNormal    = entryHit.normal;
            res.exitFraction  = entryHit.fraction;
        }

        res.thickness = static_cast<float>((res.exitPosition - res.entryPosition).Length());
        outResults.push_back(res);
    }
}

Physics::RaycastPenetrationResult
    PhysicsContext::RaycastPenetration(JPH::RVec3Arg origin, JPH::Vec3Arg direction, float maxDistance, ZHLN::Entity ignore) const {
    JPH::Array<Physics::RaycastPenetrationResult> results;
    RaycastAllPenetrations(origin, direction, maxDistance, results, ignore);
    if (!results.empty()) {
        return results[0];
    }
    return {};
}

Physics::ShapeCastResult
    PhysicsContext::Shapecast(JPH::ShapeRefC shape, JPH::RVec3Arg pos, JPH::QuatArg rot, JPH::Vec3Arg direction, float maxDistance, ZHLN::Entity ignore) const {
    const auto& world = GetWorld();
    if (world.isStepping.load(std::memory_order::relaxed))
        return {};

    float lengthSq = direction.LengthSq();
    if (lengthSq < 1e-6f)
        return {};

    JPH::Vec3 scaledDir = direction.Normalized() * maxDistance;

    JPH::RShapeCast cast(shape, JPH::Vec3::sReplicate(1.0f), JPH::RMat44::sRotationTranslation(rot, pos), scaledDir);

    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
    JPH::BodyID                                                ignoreID = Physics::GetBodyID(world, ignore);
    QueryFilter                                                filter(ignoreID);

    const auto*            query = &world.system->GetNarrowPhaseQuery();
    JPH::ShapeCastSettings settings;
    settings.mBackFaceModeTriangles = JPH::EBackFaceMode::IgnoreBackFaces;
    settings.mBackFaceModeConvex    = JPH::EBackFaceMode::IgnoreBackFaces;

    query->CastShape(cast, settings, JPH::RVec3::sZero(), collector, {}, {}, filter);

    Physics::ShapeCastResult result {};
    if (collector.HadHit()) {
        if (TryGetValidHandle(world, collector.mHit.mBodyID2, result.handle)) {
            result.hasHit       = true;
            result.fraction     = collector.mHit.mFraction;
            result.contactPoint = JPH::RVec3(collector.mHit.mContactPointOn2);

            JPH::Vec3 axis       = -collector.mHit.mPenetrationAxis;
            float     lenSq      = axis.LengthSq();
            result.contactNormal = (lenSq > 1e-6f) ? (axis / sqrt(lenSq)) : JPH::Vec3::sAxisY();
        }
    }
    return result;
}

void PhysicsContext::OverlapSphere(JPH::RVec3Arg center, float radius, JPH::Array<ZHLN::Entity>& outResults) const {
    const auto& world = GetWorld();
    if (world.isStepping.load(std::memory_order::relaxed))
        return;

    JPH::SphereShapeSettings settings(radius);
    JPH::ShapeRefC           shape = settings.Create().Get();

    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    const auto*                                               query = &world.system->GetNarrowPhaseQuery();

    query->CollideShape(shape, JPH::Vec3::sReplicate(1.0f), JPH::RMat44::sTranslation(center), {}, JPH::RVec3::sZero(), collector);

    ZHLN::Entity handle {};
    for (const auto& hit: collector.mHits) {
        if (TryGetValidHandle(world, hit.mBodyID2, handle)) {
            outResults.push_back(handle);
        }
    }
}

void PhysicsContext::OverlapAABB(JPH::RVec3Arg minBox, JPH::RVec3Arg maxBox, JPH::Array<ZHLN::Entity>& outResults) const {
    const auto& world = GetWorld();
    if (world.isStepping.load(std::memory_order::relaxed))
        return;

    JPH::AABox                                                    box((JPH::Vec3) minBox, (JPH::Vec3) maxBox);
    JPH::AllHitCollisionCollector<JPH::CollideShapeBodyCollector> collector;
    const auto*                                                   query = &world.system->GetBroadPhaseQuery();

    query->CollideAABox(box, collector);

    ZHLN::Entity handle {};
    for (const auto& hitID: collector.mHits) {
        if (TryGetValidHandle(world, hitID, handle)) {
            outResults.push_back(handle);
        }
    }
}

void PhysicsContext::QueryAABB(JPH::Vec3Arg min, JPH::Vec3Arg max, JPH::Array<ZHLN::Entity>& outEntities) const {
    const auto& world = GetWorld();
    if (world.isStepping.load(std::memory_order::relaxed))
        return;

    JPH::AABox box(min, max);

    struct SimpleCollector: public JPH::CollideShapeBodyCollector {
        const Physics::PhysicsWorld& world;
        JPH::Array<ZHLN::Entity>&    out;

        SimpleCollector(const Physics::PhysicsWorld& w, JPH::Array<ZHLN::Entity>& o): world(w), out(o) {
        }

        void AddHit(const JPH::BodyID& inBodyID) override {
            ZHLN::Entity handle;
            if (TryGetValidHandle(world, inBodyID, handle)) {
                out.push_back(handle);
            }
        }
    };

    SimpleCollector collector(world, outEntities);
    world.system->GetBroadPhaseQuery().CollideAABox(box, collector);
}

void PhysicsContext::FrustumCull(const JPH::Mat44& viewProj, const Frustum& frustum, JPH::Array<ZHLN::Entity>& outEntities) const {
    const auto& world = GetWorld();
    if (world.isStepping.load(std::memory_order::relaxed))
        return;

    JPH::AABox frustumAABB = Math::CalculateFrustumAABB(viewProj);

    struct CullCollector: public JPH::CollideShapeBodyCollector {
        const Physics::PhysicsWorld& world;
        const Frustum&               frustum;
        JPH::Array<ZHLN::Entity>&    out;

        CullCollector(const Physics::PhysicsWorld& w, const Frustum& f, JPH::Array<ZHLN::Entity>& o): world(w), frustum(f), out(o) {
        }

        void AddHit(const JPH::BodyID& inBodyID) override {
            ZHLN::Entity handle {};
            if (TryGetValidHandle(world, inBodyID, handle)) {
                JPH::BodyLockRead lock(world.system->GetBodyLockInterfaceNoLock(), inBodyID);
                if (lock.Succeeded()) {
                    JPH::AABox bounds = lock.GetBody().GetWorldSpaceBounds();
                    JPH::Vec3  center = bounds.GetCenter();
                    float      radius = bounds.GetExtent().Length();

                    if (frustum.IsSphereVisible(center, radius)) {
                        out.push_back(handle);
                    }
                }
            }
        }
    };

    CullCollector collector(world, frustum, outEntities);
    world.system->GetBroadPhaseQuery().CollideAABox(frustumAABB, collector);
}

} // namespace ZHLN
