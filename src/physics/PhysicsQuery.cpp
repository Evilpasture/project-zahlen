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
#include <Zahlen/Math/ValueTypes.hpp>
#include <Zahlen/Physics/Physics.hpp>
#include <cmath>
#include <optional>
#include <vector>

namespace ZHLN {

namespace {

[[nodiscard]] constexpr auto IsQueryableBodyState(Physics::SlotState state) noexcept -> bool {
    return state == Physics::SlotState::Alive || state == Physics::SlotState::PendingCreateRigid;
}

auto FindBodyID(const Physics::PhysicsWorld& world, JPH::BodyID bodyID) noexcept -> std::optional<Physics::BodyID> {
    if (bodyID.IsInvalid()) [[unlikely]] {
        return std::nullopt;
    }

    const auto&    bt        = world.bodyTable;
    const uint32_t joltIndex = bodyID.GetIndexAndSequenceNumber() & JPH::BodyID::cMaxBodyIndex;
    if (joltIndex >= bt.joltIndexToBodyID.size()) [[unlikely]] {
        return std::nullopt;
    }

    const uint64_t rawData = bt.joltIndexToBodyID[joltIndex].load(std::memory_order::acquire);
    if (rawData == 0u) {
        return std::nullopt;
    }

    const Physics::BodyID handle = Physics::BodyID::Unpack(rawData);
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
        return FindBodyID(_world, inBodyID).has_value();
    }
};

} // namespace

auto PhysicsContext::Raycast(ZHLN::Double3 origin, ZHLN::Float3 direction, float maxDistance, Physics::BodyID ignore) const -> Physics::RaycastResult {
    const JPH::Vec3 dir   = static_cast<JPH::Vec3>(direction);
    const auto&     world = GetWorld();

    if (world.isStepping.load(std::memory_order::acquire)) {
        return {};
    }

    const float lengthSq = dir.LengthSq();
    if (lengthSq < 1e-6f) {
        return {};
    }

    const JPH::Vec3     scaledDir = dir.Normalized() * maxDistance;
    const JPH::RRayCast ray {static_cast<JPH::RVec3>(origin), scaledDir};
    JPH::RayCastResult  hit;

    const JPH::BodyID ignoreID = Physics::GetBodyID(world, ignore);
    const QueryFilter filter(world, ignoreID);

    const auto* query  = &world.system->GetNarrowPhaseQuery();
    const bool  hasHit = query->CastRay(ray, hit, {}, {}, filter);

    Physics::RaycastResult result {};
    if (hasHit) {
        if (const auto handle = FindBodyID(world, hit.mBodyID)) {
            result.bodyID   = *handle;
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

auto PhysicsContext::RaycastAll(ZHLN::Double3 origin, ZHLN::Float3 direction, float maxDistance, Physics::BodyID ignore) const
    -> std::vector<Physics::RaycastResult> {
    std::vector<Physics::RaycastResult> results;
    const JPH::Vec3 dir   = static_cast<JPH::Vec3>(direction);
    const auto&     world = GetWorld();

    if (world.isStepping.load(std::memory_order::acquire)) {
        return results;
    }

    const float lengthSq = dir.LengthSq();
    if (lengthSq < 1e-6f) {
        return results;
    }

    const JPH::Vec3     scaledDir = dir.Normalized() * maxDistance;
    const JPH::RRayCast ray {static_cast<JPH::RVec3>(origin), scaledDir};

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
            if (const auto handle = FindBodyID(world, hit.mBodyID)) {
                result.bodyID   = *handle;
                result.hasHit   = true;
                result.fraction = hit.mFraction;
                result.position = ray.GetPointOnRay(hit.mFraction);

                JPH::BodyLockRead lock(*lockInterface, hit.mBodyID);
                if (lock.Succeeded()) {
                    result.normal = lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, result.position);
                } else {
                    result.normal = JPH::Vec3::sAxisY();
                }
                results.push_back(result);
            }
        }
    }
    return results;
}

auto PhysicsContext::RaycastAllPenetrations(
    ZHLN::Double3 origin, ZHLN::Float3 direction, float maxDistance, Physics::BodyID ignore
) const -> std::vector<Physics::RaycastPenetrationResult> {
    std::vector<Physics::RaycastPenetrationResult> results;
    const JPH::Vec3 dir   = static_cast<JPH::Vec3>(direction);
    const auto&     world = GetWorld();
    if (world.isStepping.load(std::memory_order::acquire)) {
        return results;
    }

    const float lengthSq = dir.LengthSq();
    if (lengthSq < 1e-6f) {
        return results;
    }

    const JPH::Vec3     dirNorm   = dir.Normalized();
    const JPH::Vec3     scaledDir = dirNorm * maxDistance;
    const JPH::RRayCast ray {static_cast<JPH::RVec3>(origin), scaledDir};

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
        return results;
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
        if (FindBodyID(world, hit.mBodyID).has_value()) {
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
        const auto  handle   = FindBodyID(world, entryHit.bodyID);
        if (!handle) {
            continue;
        }

        processed[i] = true;

        Physics::RaycastPenetrationResult res {};
        res.hasHit     = true;
        res.bodyID     = *handle;
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
            res.entryPosition = static_cast<JPH::RVec3>(origin);
            res.entryNormal   = -dirNorm;
            res.entryFraction = 0.0f;
            res.exitPosition  = entryHit.position;
            res.exitNormal    = entryHit.normal;
            res.exitFraction  = entryHit.fraction;
        }

        res.thickness = static_cast<float>((res.exitPosition - res.entryPosition).Length());
        results.push_back(res);
    }
    return results;
}

auto PhysicsContext::RaycastPenetration(ZHLN::Double3 origin, ZHLN::Float3 direction, float maxDistance, Physics::BodyID ignore) const
    -> Physics::RaycastPenetrationResult {
    const auto results = RaycastAllPenetrations(origin, direction, maxDistance, ignore);
    if (!results.empty()) {
        return results[0];
    }
    return {};
}

auto PhysicsContext::Shapecast(
    const JPH::ShapeRefC& shape,
    ZHLN::Double3         pos,
    ZHLN::Float4          rot,
    ZHLN::Float3          direction,
    float                 maxDistance,
    Physics::BodyID   ignore
) const -> Physics::ShapeCastResult {
    const JPH::Vec3 dir   = static_cast<JPH::Vec3>(direction);
    const auto&     world = GetWorld();
    if (world.isStepping.load(std::memory_order::acquire)) {
        return {};
    }

    const float lengthSq = dir.LengthSq();
    if (lengthSq < 1e-6f) {
        return {};
    }

    const JPH::Vec3       scaledDir = dir.Normalized() * maxDistance;
    const JPH::RShapeCast cast(
        shape, JPH::Vec3::sReplicate(1.0f), JPH::RMat44::sRotationTranslation(static_cast<JPH::Quat>(rot), static_cast<JPH::RVec3>(pos)), scaledDir
    );

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
        if (const auto handle = FindBodyID(world, collector.mHit.mBodyID2)) {
            result.bodyID       = *handle;
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

auto PhysicsContext::OverlapSphere(ZHLN::Double3 center, float radius) const -> std::vector<Physics::BodyID> {
    std::vector<Physics::BodyID> results;
    const auto& world = GetWorld();
    if (world.isStepping.load(std::memory_order::acquire)) {
        return results;
    }

    const JPH::SphereShapeSettings settings(radius);
    const JPH::ShapeRefC           shape = settings.Create().Get();

    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    const auto*                                               query = &world.system->GetNarrowPhaseQuery();

    query->CollideShape(shape, JPH::Vec3::sReplicate(1.0f), JPH::RMat44::sTranslation(static_cast<JPH::RVec3>(center)), {}, JPH::RVec3::sZero(), collector);

    for (const auto& hit: collector.mHits) {
        if (const auto handle = FindBodyID(world, hit.mBodyID2)) {
            results.push_back(*handle);
        }
    }
    return results;
}

auto PhysicsContext::OverlapAABB(ZHLN::Double3 minBox, ZHLN::Double3 maxBox) const -> std::vector<Physics::BodyID> {
    std::vector<Physics::BodyID> results;
    const auto& world = GetWorld();
    if (world.isStepping.load(std::memory_order::acquire)) {
        return results;
    }

    const JPH::AABox                                              box {static_cast<JPH::RVec3>(minBox), static_cast<JPH::RVec3>(maxBox)};
    JPH::AllHitCollisionCollector<JPH::CollideShapeBodyCollector> collector;
    const auto*                                                   query = &world.system->GetBroadPhaseQuery();

    query->CollideAABox(box, collector);

    for (const auto& hitID: collector.mHits) {
        if (const auto handle = FindBodyID(world, hitID)) {
            results.push_back(*handle);
        }
    }
    return results;
}

auto PhysicsContext::QueryAABB(ZHLN::Float3 min, ZHLN::Float3 max) const -> std::vector<Physics::BodyID> {
    std::vector<Physics::BodyID> results;
    const auto& world = GetWorld();
    if (world.isStepping.load(std::memory_order::acquire)) {
        return results;
    }

    const JPH::AABox box(static_cast<JPH::Vec3>(min), static_cast<JPH::Vec3>(max));

    struct SimpleCollector final: public JPH::CollideShapeBodyCollector {
        const Physics::PhysicsWorld&     worldRef;
        std::vector<Physics::BodyID>& out;

        SimpleCollector(const Physics::PhysicsWorld& w, std::vector<Physics::BodyID>& o): worldRef(w), out(o) {
        }

        void AddHit(const JPH::BodyID& inBodyID) override {
            if (const auto handle = FindBodyID(worldRef, inBodyID)) {
                out.push_back(*handle);
            }
        }
    };

    SimpleCollector collector(world, results);
    world.system->GetBroadPhaseQuery().CollideAABox(box, collector);
    return results;
}

auto PhysicsContext::FrustumCull(const JPH::Mat44& viewProj, const Frustum& frustum) const -> std::vector<Physics::BodyID> {
    std::vector<Physics::BodyID> results;
    const auto& world = GetWorld();
    if (world.isStepping.load(std::memory_order::acquire)) {
        return results;
    }

    const JPH::AABox frustumAABB = Math::CalculateFrustumAABB(viewProj);

    struct CullCollector final: public JPH::CollideShapeBodyCollector {
        const Physics::PhysicsWorld&     worldRef;
        const Frustum&                   frustumRef;
        std::vector<Physics::BodyID>& out;

        CullCollector(const Physics::PhysicsWorld& w, const Frustum& f, std::vector<Physics::BodyID>& o): worldRef(w), frustumRef(f), out(o) {
        }

        void AddHit(const JPH::BodyID& inBodyID) override {
            if (const auto handle = FindBodyID(worldRef, inBodyID)) {
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

    CullCollector collector(world, frustum, results);
    world.system->GetBroadPhaseQuery().CollideAABox(frustumAABB, collector);
    return results;
}

} // namespace ZHLN
