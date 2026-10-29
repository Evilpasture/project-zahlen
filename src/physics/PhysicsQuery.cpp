// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PhysicsWorld.hpp"
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Zahlen/physics/Physics.hpp>
#include <cmath>
#include <optional>

namespace ZHLN {

namespace {

[[nodiscard]] constexpr auto IsQueryableBodyState(Physics::SlotState state) noexcept -> bool {
    return state == Physics::SlotState::Alive || state == Physics::SlotState::PendingCreateRigid;
}

auto FindBodyHandle(const Physics::PhysicsWorld& world, JPH::BodyID bodyID) noexcept -> std::optional<Physics::BodyHandle> {
    if (bodyID.IsInvalid()) [[unlikely]] {
        return std::nullopt;
    }

    const auto&    bt        = world.bodyTable;
    const uint32_t joltIndex = bodyID.GetIndexAndSequenceNumber() & JPH::BodyID::cMaxBodyIndex;
    if (joltIndex >= bt.idToHandleMap.size()) [[unlikely]] {
        return std::nullopt;
    }

    const uint64_t rawData = bt.idToHandleMap[joltIndex].load(std::memory_order::acquire);
    if (rawData == 0u) {
        return std::nullopt;
    }

    const Physics::BodyHandle handle = Physics::BodyHandle::Unpack(rawData);
    if (handle.index >= bt.slotCapacity || bt.generations[handle.index].load(std::memory_order::acquire) != handle.generation) [[unlikely]] {
        return std::nullopt;
    }

    if (IsQueryableBodyState(bt.LoadSlotState(handle.index))) {
        return handle;
    }
    return std::nullopt;
}

class QueryFilter final: public JPH::BodyFilter {
    const Physics::PhysicsWorld& _world;
    JPH::BodyID                  _ignoreID;

  public:
    QueryFilter(const Physics::PhysicsWorld& world, JPH::BodyID ignore): _world(world), _ignoreID(ignore) {
    }
    [[nodiscard]] auto ShouldCollide(const JPH::BodyID& inBodyID) const -> bool override {
        if (inBodyID == _ignoreID) {
            return false;
        }
        return FindBodyHandle(_world, inBodyID).has_value();
    }
};

} // namespace

auto PhysicsContext::Raycast(JPH::RVec3Arg origin, JPH::Vec3Arg direction, float maxDistance, Physics::BodyHandle ignore) const -> Physics::RaycastResult {
    const auto& world = GetWorld();

    if (world.isStepping.load(std::memory_order::acquire)) {
        return {};
    }

    const float lengthSq = direction.LengthSq();
    if (lengthSq < 1e-6f) {
        return {};
    }

    const JPH::Vec3    scaledDir = direction.Normalized() * maxDistance;
    const JPH::RRayCast ray {origin, scaledDir};
    JPH::RayCastResult hit;

    const JPH::BodyID ignoreID = Physics::GetBodyID(world, ignore);
    const QueryFilter filter(world, ignoreID);

    const auto* query  = &world.system->GetNarrowPhaseQuery();
    const bool  hasHit = query->CastRay(ray, hit, {}, {}, filter);

    Physics::RaycastResult result {};
    if (hasHit) {
        if (const auto handle = FindBodyHandle(world, hit.mBodyID)) {
            result.handle   = *handle;
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
    Physics::BodyHandle                 ignore
) const {
    const auto& world = GetWorld();

    if (world.isStepping.load(std::memory_order::acquire)) {
        return;
    }

    const float lengthSq = direction.LengthSq();
    if (lengthSq < 1e-6f) {
        return;
    }

    const JPH::Vec3     scaledDir = direction.Normalized() * maxDistance;
    const JPH::RRayCast ray {origin, scaledDir};

    JPH::RayCastSettings settings;
    settings.mBackFaceModeTriangles = JPH::EBackFaceMode::CollideWithBackFaces;
    settings.mBackFaceModeConvex    = JPH::EBackFaceMode::CollideWithBackFaces;
    settings.mTreatConvexAsSolid    = false;

    const JPH::BodyID ignoreID = Physics::GetBodyID(world, ignore);
    const QueryFilter filter(world, ignoreID);

    JPH::AllHitCollisionCollector<JPH::CastRayCollector> collector;
    const auto*                                          query = &world.system->GetNarrowPhaseQuery();
    query->CastRay(ray, settings, collector, {}, {}, filter);

    collector.Sort();

    if (collector.HadHit()) {
        const auto* lockInterface = &world.system->GetBodyLockInterfaceNoLock();

        for (const auto& hit: collector.mHits) {
            Physics::RaycastResult result {};
            if (const auto handle = FindBodyHandle(world, hit.mBodyID)) {
                result.handle   = *handle;
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
    Physics::BodyHandle                            ignore
) const {
    const auto& world = GetWorld();
    if (world.isStepping.load(std::memory_order::acquire)) {
        return;
    }

    const float lengthSq = direction.LengthSq();
    if (lengthSq < 1e-6f) {
        return;
    }

    const JPH::Vec3     dirNorm   = direction.Normalized();
    const JPH::Vec3     scaledDir = dirNorm * maxDistance;
    const JPH::RRayCast ray {origin, scaledDir};

    JPH::RayCastSettings settings;
    settings.mBackFaceModeTriangles = JPH::EBackFaceMode::CollideWithBackFaces;
    settings.mBackFaceModeConvex    = JPH::EBackFaceMode::CollideWithBackFaces;
    settings.mTreatConvexAsSolid    = false;

    const JPH::BodyID ignoreID = Physics::GetBodyID(world, ignore);
    const QueryFilter filter(world, ignoreID);

    JPH::AllHitCollisionCollector<JPH::CastRayCollector> collector;
    const auto*                                          query = &world.system->GetNarrowPhaseQuery();
    query->CastRay(ray, settings, collector, {}, {}, filter);

    collector.Sort();

    if (!collector.HadHit()) {
        return;
    }

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
        if (FindBodyHandle(world, hit.mBodyID).has_value()) {
            JPH::BodyLockRead lock(*lockInterface, hit.mBodyID);
            if (lock.Succeeded()) {
                const JPH::RVec3 hitPos     = ray.GetPointOnRay(hit.mFraction);
                const JPH::Vec3  norm       = lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, hitPos);
                const bool       isBackFace = (dirNorm.Dot(norm) > 0.0f);
                hits.push_back(IntermediateHit {
                    .bodyID     = hit.mBodyID,
                    .fraction   = hit.mFraction,
                    .position   = hitPos,
                    .normal     = norm,
                    .isBackFace = isBackFace,
                });
            }
        }
    }

    JPH::Array<bool> processed(hits.size(), false);

    for (size_t i = 0; i < hits.size(); ++i) {
        if (processed[i]) {
            continue;
        }

        const auto& entryHit = hits[i];
        const auto  handle   = FindBodyHandle(world, entryHit.bodyID);
        if (!handle) {
            continue;
        }

        processed[i] = true;

        Physics::RaycastPenetrationResult res {};
        res.hasHit     = true;
        res.handle     = *handle;
        res.materialID = world.bodyTable.slotMaterialIDs[handle->index].load(std::memory_order::relaxed);

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

auto PhysicsContext::RaycastPenetration(JPH::RVec3Arg origin, JPH::Vec3Arg direction, float maxDistance, Physics::BodyHandle ignore) const
    -> Physics::RaycastPenetrationResult {
    JPH::Array<Physics::RaycastPenetrationResult> results;
    RaycastAllPenetrations(origin, direction, maxDistance, results, ignore);
    if (!results.empty()) {
        return results[0];
    }
    return {};
}

auto PhysicsContext::Shapecast(
    const JPH::ShapeRefC& shape,
    JPH::RVec3Arg         pos,
    JPH::QuatArg          rot,
    JPH::Vec3Arg          direction,
    float                 maxDistance,
    Physics::BodyHandle   ignore
) const -> Physics::ShapeCastResult {
    const auto& world = GetWorld();
    if (world.isStepping.load(std::memory_order::acquire)) {
        return {};
    }

    const float lengthSq = direction.LengthSq();
    if (lengthSq < 1e-6f) {
        return {};
    }

    const JPH::Vec3       scaledDir = direction.Normalized() * maxDistance;
    const JPH::RShapeCast cast(shape, JPH::Vec3::sReplicate(1.0f), JPH::RMat44::sRotationTranslation(rot, pos), scaledDir);

    JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
    const JPH::BodyID                                          ignoreID = Physics::GetBodyID(world, ignore);
    const QueryFilter                                          filter(world, ignoreID);

    const auto*                  query = &world.system->GetNarrowPhaseQuery();
    const JPH::ShapeCastSettings settings {
        .mBackFaceModeTriangles = JPH::EBackFaceMode::IgnoreBackFaces,
        .mBackFaceModeConvex    = JPH::EBackFaceMode::IgnoreBackFaces,
    };

    query->CastShape(cast, settings, JPH::RVec3::sZero(), collector, {}, {}, filter);

    Physics::ShapeCastResult result {};
    if (collector.HadHit()) {
        if (const auto handle = FindBodyHandle(world, collector.mHit.mBodyID2)) {
            result.handle       = *handle;
            result.hasHit       = true;
            result.fraction     = collector.mHit.mFraction;
            result.contactPoint = JPH::RVec3(collector.mHit.mContactPointOn2);

            const JPH::Vec3 axis  = -collector.mHit.mPenetrationAxis;
            const float     lenSq = axis.LengthSq();
            result.contactNormal  = (lenSq > 1e-6f) ? (axis / std::sqrt(lenSq)) : JPH::Vec3::sAxisY();
        }
    }
    return result;
}

void PhysicsContext::OverlapSphere(JPH::RVec3Arg center, float radius, JPH::Array<Physics::BodyHandle>& outResults) const {
    const auto& world = GetWorld();
    if (world.isStepping.load(std::memory_order::acquire)) {
        return;
    }

    const JPH::SphereShapeSettings settings(radius);
    const JPH::ShapeRefC           shape = settings.Create().Get();

    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    const auto*                                               query = &world.system->GetNarrowPhaseQuery();

    query->CollideShape(shape, JPH::Vec3::sReplicate(1.0f), JPH::RMat44::sTranslation(center), {}, JPH::RVec3::sZero(), collector);

    for (const auto& hit: collector.mHits) {
        if (const auto handle = FindBodyHandle(world, hit.mBodyID2)) {
            outResults.push_back(*handle);
        }
    }
}

void PhysicsContext::OverlapAABB(JPH::RVec3Arg minBox, JPH::RVec3Arg maxBox, JPH::Array<Physics::BodyHandle>& outResults) const {
    const auto& world = GetWorld();
    if (world.isStepping.load(std::memory_order::acquire)) {
        return;
    }

    const JPH::AABox                                              box {JPH::Vec3(minBox), JPH::Vec3(maxBox)};
    JPH::AllHitCollisionCollector<JPH::CollideShapeBodyCollector> collector;
    const auto*                                                   query = &world.system->GetBroadPhaseQuery();

    query->CollideAABox(box, collector);

    for (const auto& hitID: collector.mHits) {
        if (const auto handle = FindBodyHandle(world, hitID)) {
            outResults.push_back(*handle);
        }
    }
}

void PhysicsContext::QueryAABB(JPH::Vec3Arg min, JPH::Vec3Arg max, JPH::Array<Physics::BodyHandle>& outEntities) const {
    const auto& world = GetWorld();
    if (world.isStepping.load(std::memory_order::acquire)) {
        return;
    }

    const JPH::AABox box(min, max);

    struct SimpleCollector final: public JPH::CollideShapeBodyCollector {
        const Physics::PhysicsWorld&     worldRef;
        JPH::Array<Physics::BodyHandle>& out;

        SimpleCollector(const Physics::PhysicsWorld& w, JPH::Array<Physics::BodyHandle>& o): worldRef(w), out(o) {
        }

        void AddHit(const JPH::BodyID& inBodyID) override {
            if (const auto handle = FindBodyHandle(worldRef, inBodyID)) {
                out.push_back(*handle);
            }
        }
    };

    SimpleCollector collector(world, outEntities);
    world.system->GetBroadPhaseQuery().CollideAABox(box, collector);
}

void PhysicsContext::FrustumCull(const JPH::Mat44& viewProj, const Frustum& frustum, JPH::Array<Physics::BodyHandle>& outEntities) const {
    const auto& world = GetWorld();
    if (world.isStepping.load(std::memory_order::acquire)) {
        return;
    }

    const JPH::AABox frustumAABB = Math::CalculateFrustumAABB(viewProj);

    struct CullCollector final: public JPH::CollideShapeBodyCollector {
        const Physics::PhysicsWorld&     worldRef;
        const Frustum&                   frustumRef;
        JPH::Array<Physics::BodyHandle>& out;

        CullCollector(const Physics::PhysicsWorld& w, const Frustum& f, JPH::Array<Physics::BodyHandle>& o): worldRef(w), frustumRef(f), out(o) {
        }

        void AddHit(const JPH::BodyID& inBodyID) override {
            if (const auto handle = FindBodyHandle(worldRef, inBodyID)) {
                JPH::BodyLockRead lock(worldRef.system->GetBodyLockInterfaceNoLock(), inBodyID);
                if (lock.Succeeded()) {
                    const JPH::AABox bounds = lock.GetBody().GetWorldSpaceBounds();
                    const JPH::Vec3  center = bounds.GetCenter();
                    const float      radius = bounds.GetExtent().Length();

                    if (frustumRef.IsSphereVisible(center, radius)) {
                        out.push_back(*handle);
                    }
                }
            }
        }
    };

    CullCollector collector(world, frustum, outEntities);
    world.system->GetBroadPhaseQuery().CollideAABox(frustumAABB, collector);
}

} // namespace ZHLN
