// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module;

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <Zahlen/ProceduralAnimation.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

export module ZHLN.SecondaryPhysics;

export namespace ZHLN::Animation {

namespace SecondaryDetail {

[[nodiscard]] inline JPH::Vec3 SafeNormalized(JPH::Vec3Arg value, JPH::Vec3Arg fallback) noexcept {
    return value.LengthSq() > 1.0e-10f ? value.Normalized() : JPH::Vec3(fallback);
}

inline void ProjectSphere(JPH::Vec3& point, JPH::Vec3Arg center, float radius) noexcept {
    JPH::Vec3 delta  = point - center;
    float     length = delta.Length();
    if (length >= radius) {
        return;
    }
    point = center + SafeNormalized(delta, JPH::Vec3::sAxisY()) * radius;
}

inline void ProjectEllipsoid(JPH::Vec3& point, JPH::Vec3Arg center, JPH::QuatArg rotation, JPH::Vec3Arg radii) noexcept {
    const JPH::Quat invRotation = rotation.Inversed();
    const JPH::Vec3 local       = invRotation * (point - center);
    JPH::Vec3       scaled(
        local.GetX() / std::max(radii.GetX(), 1.0e-4f), local.GetY() / std::max(radii.GetY(), 1.0e-4f), local.GetZ() / std::max(radii.GetZ(), 1.0e-4f)
    );
    if (scaled.LengthSq() >= 1.0f) {
        return;
    }

    scaled = SafeNormalized(scaled, JPH::Vec3::sAxisY());
    const JPH::Vec3 projected(scaled.GetX() * radii.GetX(), scaled.GetY() * radii.GetY(), scaled.GetZ() * radii.GetZ());
    point = center + rotation * projected;
}

inline void InitializeHair(HairStrandsComponent& hair, JPH::Vec3Arg headPosition, JPH::QuatArg headRotation) noexcept {
    constexpr float kDefaultLength = 0.105f;
    constexpr float kTwoPi         = 2.0f * std::numbers::pi_v<float>;

    bool offsetsAreEmpty = true;
    for (JPH::Vec3Arg offset: hair.rootBindOffsets) {
        offsetsAreEmpty = offsetsAreEmpty && offset.LengthSq() < 1.0e-8f;
    }

    for (size_t strand = 0; strand < HairStrandsComponent::kStrandCount; ++strand) {
        const float angle = kTwoPi * static_cast<float>(strand) / static_cast<float>(HairStrandsComponent::kStrandCount);
        if (offsetsAreEmpty) {
            hair.rootBindOffsets[strand] = JPH::Vec3(std::cos(angle) * 0.150f, 0.075f + 0.020f * std::sin(angle * 2.0f), std::sin(angle) * 0.150f);
        }

        const size_t    base    = strand * HairStrandsComponent::kLinksPerStrand;
        const JPH::Vec3 root    = JPH::Vec3(headPosition) + headRotation * hair.rootBindOffsets[strand];
        const JPH::Vec3 outward = headRotation * JPH::Vec3(std::cos(angle), 0.0f, std::sin(angle));
        for (size_t link = 0; link < HairStrandsComponent::kLinksPerStrand; ++link) {
            const size_t index         = base + link;
            const float  length        = (hair.segmentLengths[index] > 1.0e-4f) ? hair.segmentLengths[index] : kDefaultLength;
            hair.segmentLengths[index] = (link == 0) ? 0.0f : length;
            const JPH::Vec3 position = root + JPH::Vec3(0.0f, -kDefaultLength * static_cast<float>(link), 0.0f) + outward * (0.012f * static_cast<float>(link));
            hair.positions[index]    = position;
            hair.prevPositions[index] = position;
        }
    }
    hair.initialized = true;
}

} // namespace SecondaryDetail

/**
 * Stage 5: allocation-free Verlet/XPBD evaluation. Each strand is independent,
 * so integration, four constraint iterations, and body projection all execute
 * on a worker fiber without cross-strand synchronization.
 */
inline void StepHairSimulation(
    HairStrandsComponent& hair,
    JPH::Vec3Arg          headWorldPosition,
    JPH::QuatArg          headWorldRotation,
    JPH::Vec3Arg          characterVelocity,
    float                 dt
) noexcept {
    if (!hair.initialized) {
        SecondaryDetail::InitializeHair(hair, headWorldPosition, headWorldRotation);
    }

    const float     safeDt       = std::clamp(dt, 0.0001f, 1.0f / 30.0f);
    const float     dtSq         = safeDt * safeDt;
    const JPH::Vec3 inertialWind = characterVelocity * -0.035f;
    const JPH::Vec3 acceleration = inertialWind + JPH::Vec3(0.0f, hair.gravity, 0.0f);

    const JPH::Vec3 torsoCenter = JPH::Vec3(headWorldPosition) + headWorldRotation * JPH::Vec3(0.0f, -0.48f, -0.015f);
    const JPH::Vec3 shoulderL   = JPH::Vec3(headWorldPosition) + headWorldRotation * JPH::Vec3(0.24f, -0.31f, 0.0f);
    const JPH::Vec3 shoulderR   = JPH::Vec3(headWorldPosition) + headWorldRotation * JPH::Vec3(-0.24f, -0.31f, 0.0f);
    const JPH::Vec3 torsoRadii(hair.torsoColliderRadiusXZ, hair.torsoColliderRadiusY, hair.torsoColliderRadiusXZ * 0.75f);
    const JPH::Vec3 shoulderRadii(hair.shoulderColliderRadius, hair.shoulderColliderRadius * 0.62f, hair.shoulderColliderRadius * 0.78f);

    TaskSystem::ParallelFor(static_cast<uint32_t>(HairStrandsComponent::kStrandCount), 1, [&](uint32_t first, uint32_t last, uint32_t) {
        for (uint32_t strand = first; strand < last; ++strand) {
            const size_t    base = static_cast<size_t>(strand) * HairStrandsComponent::kLinksPerStrand;
            const JPH::Vec3 root = JPH::Vec3(headWorldPosition) + headWorldRotation * hair.rootBindOffsets[strand];

            hair.positions[base]     = root;
            hair.prevPositions[base] = root;
            for (size_t link = 1; link < HairStrandsComponent::kLinksPerStrand; ++link) {
                const size_t    index     = base + link;
                const JPH::Vec3 velocity  = (hair.positions[index] - hair.prevPositions[index]) * std::clamp(hair.damping, 0.0f, 1.0f);
                hair.prevPositions[index] = hair.positions[index];
                hair.positions[index] += velocity + acceleration * dtSq;
            }

            std::array<float, HairStrandsComponent::kLinksPerStrand> constraintLambdas {};
            for (uint32_t iteration = 0; iteration < 4; ++iteration) {
                hair.positions[base] = root;
                for (size_t link = 1; link < HairStrandsComponent::kLinksPerStrand; ++link) {
                    const size_t    previous            = base + link - 1;
                    const size_t    current             = base + link;
                    JPH::Vec3       delta               = hair.positions[current] - hair.positions[previous];
                    const float     length              = std::max(delta.Length(), 1.0e-6f);
                    const JPH::Vec3 direction           = delta / length;
                    const float     constraint          = length - std::max(hair.segmentLengths[current], 1.0e-4f);
                    const float     inverseMassPrevious = (link == 1) ? 0.0f : 1.0f;
                    constexpr float inverseMassCurrent  = 1.0f;
                    const float     alpha               = std::max(hair.compliance, 0.0f) / dtSq;
                    const float     deltaLambda         = (-constraint - alpha * constraintLambdas[link]) / (inverseMassPrevious + inverseMassCurrent + alpha);
                    constraintLambdas[link] += deltaLambda;
                    const JPH::Vec3 correction = direction * deltaLambda;

                    hair.positions[previous] -= correction * inverseMassPrevious;
                    hair.positions[current] += correction * inverseMassCurrent;

                    SecondaryDetail::ProjectSphere(hair.positions[current], headWorldPosition, hair.headColliderRadius);
                    SecondaryDetail::ProjectEllipsoid(hair.positions[current], torsoCenter, headWorldRotation, torsoRadii);
                    SecondaryDetail::ProjectEllipsoid(hair.positions[current], shoulderL, headWorldRotation, shoulderRadii);
                    SecondaryDetail::ProjectEllipsoid(hair.positions[current], shoulderR, headWorldRotation, shoulderRadii);
                }
            }
        }
    });
}

/** Generates one model/world transform for all 108 mapped secondary bones. */
inline void ExtractHairBoneTransforms(const HairStrandsComponent& hair, JPH::Mat44* nodeTransforms, const RigBoneMap& map) noexcept {
    if (nodeTransforms == nullptr || !hair.initialized) {
        return;
    }

    constexpr size_t hairBoneOffset = static_cast<size_t>(CharacterBone::HairStart);
    for (size_t strand = 0; strand < HairStrandsComponent::kStrandCount; ++strand) {
        const size_t base = strand * HairStrandsComponent::kLinksPerStrand;
        for (size_t link = 0; link < HairStrandsComponent::kLinksPerStrand; ++link) {
            const size_t  particleIndex = base + link;
            const int32_t nodeIndex     = map.nodeIndices[hairBoneOffset + particleIndex];
            if (nodeIndex < 0 || nodeIndex >= static_cast<int32_t>(map.nodeCount)) {
                continue;
            }

            JPH::Vec3 direction;
            if (link + 1 < HairStrandsComponent::kLinksPerStrand) {
                direction = hair.positions[particleIndex + 1] - hair.positions[particleIndex];
            } else {
                direction = hair.positions[particleIndex] - hair.positions[particleIndex - 1];
            }
            direction = SecondaryDetail::SafeNormalized(direction, JPH::Vec3(0.0f, -1.0f, 0.0f));

            const JPH::Vec3 scale(
                nodeTransforms[nodeIndex].GetColumn3(0).Length(), nodeTransforms[nodeIndex].GetColumn3(1).Length(),
                nodeTransforms[nodeIndex].GetColumn3(2).Length()
            );
            const JPH::Quat rotation  = JPH::Quat::sFromTo(JPH::Vec3(0.0f, -1.0f, 0.0f), direction);
            nodeTransforms[nodeIndex] = JPH::Mat44::sRotationTranslation(rotation, hair.positions[particleIndex]).PreScaled(scale);
        }
    }
}

} // namespace ZHLN::Animation
