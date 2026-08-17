// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Vec4.h>
#include <Zahlen/ProceduralAnimation.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace ZHLN::Animation {

namespace SecondaryDetail {

[[nodiscard]] inline JPH::Vec3 SafeNormalized(JPH::Vec3Arg value, JPH::Vec3Arg fallback) noexcept {
    return value.LengthSq() > 1.0e-10f ? value.Normalized() : JPH::Vec3(fallback);
}

[[nodiscard]] inline JPH::Quat ExtractRotation(const JPH::Mat44& matrix) noexcept {
    const float     xScale = matrix.GetColumn3(0).Length();
    const float     yScale = matrix.GetColumn3(1).Length();
    const float     zScale = matrix.GetColumn3(2).Length();
    const JPH::Vec3 x      = xScale > 1.0e-6f ? matrix.GetColumn3(0) / xScale : JPH::Vec3::sAxisX();
    const JPH::Vec3 y      = yScale > 1.0e-6f ? matrix.GetColumn3(1) / yScale : JPH::Vec3::sAxisY();
    const JPH::Vec3 z      = zScale > 1.0e-6f ? matrix.GetColumn3(2) / zScale : JPH::Vec3::sAxisZ();
    return JPH::Mat44(JPH::Vec4(x, 0.0f), JPH::Vec4(y, 0.0f), JPH::Vec4(z, 0.0f), JPH::Vec4(0.0f, 0.0f, 0.0f, 1.0f)).GetQuaternion().Normalized();
}

inline void ProjectSphere(JPH::Vec3& point, JPH::Vec3Arg center, float radius, JPH::Vec3Arg restPoint) noexcept {
    const float allowedBindRadius = (restPoint - center).Length();
    const float effectiveRadius   = std::min(radius, allowedBindRadius);
    JPH::Vec3   delta             = point - center;
    float       length            = delta.Length();
    if (length >= effectiveRadius || effectiveRadius <= 1.0e-6f) {
        return;
    }
    point = center + SafeNormalized(delta, restPoint - center) * effectiveRadius;
}

inline void ProjectEllipsoid(JPH::Vec3& point, JPH::Vec3Arg center, JPH::QuatArg rotation, JPH::Vec3Arg radii, JPH::Vec3Arg restPoint) noexcept {
    const JPH::Quat invRotation = rotation.Inversed();
    const JPH::Vec3 local       = invRotation * (point - center);
    JPH::Vec3       scaled(
        local.GetX() / std::max(radii.GetX(), 1.0e-4f), local.GetY() / std::max(radii.GetY(), 1.0e-4f), local.GetZ() / std::max(radii.GetZ(), 1.0e-4f)
    );
    const JPH::Vec3 restLocal = invRotation * (restPoint - center);
    const JPH::Vec3 restScaled(
        restLocal.GetX() / std::max(radii.GetX(), 1.0e-4f), restLocal.GetY() / std::max(radii.GetY(), 1.0e-4f),
        restLocal.GetZ() / std::max(radii.GetZ(), 1.0e-4f)
    );
    const float effectiveBoundary = std::min(1.0f, restScaled.Length());
    if (scaled.Length() >= effectiveBoundary || effectiveBoundary <= 1.0e-6f) {
        return;
    }

    scaled = SafeNormalized(scaled, restScaled) * effectiveBoundary;
    const JPH::Vec3 projected(scaled.GetX() * radii.GetX(), scaled.GetY() * radii.GetY(), scaled.GetZ() * radii.GetZ());
    point = center + rotation * projected;
}

inline void SolveDistanceConstraint(
    JPH::Vec3& positionA,
    JPH::Vec3& positionB,
    float      inverseMassA,
    float      inverseMassB,
    float      restLength,
    float      compliance,
    float      dtSquared,
    float&     lambda
) noexcept {
    JPH::Vec3 delta       = positionB - positionA;
    float     length      = std::max(delta.Length(), 1.0e-6f);
    float     alpha       = std::max(compliance, 0.0f) / dtSquared;
    float     denominator = inverseMassA + inverseMassB + alpha;
    if (denominator <= 1.0e-8f) {
        return;
    }

    const float deltaLambda = (-(length - restLength) - alpha * lambda) / denominator;
    lambda += deltaLambda;
    const JPH::Vec3 correction = delta / length * deltaLambda;
    positionA -= correction * inverseMassA;
    positionB += correction * inverseMassB;
}

inline void SolveShapeConstraint(JPH::Vec3& position, JPH::Vec3Arg target, float compliance, float dtSquared, float& lambda) noexcept {
    JPH::Vec3 delta  = position - target;
    float     length = delta.Length();
    if (length <= 1.0e-7f) {
        return;
    }

    const float alpha       = std::max(compliance, 0.0f) / dtSquared;
    const float deltaLambda = (-length - alpha * lambda) / (1.0f + alpha);
    lambda += deltaLambda;
    position += delta / length * deltaLambda;
}

inline void FinalizeRestConstraints(HairStrandsComponent& hair) noexcept {
    for (size_t strand = 0; strand < HairStrandsComponent::kStrandCount; ++strand) {
        const size_t base            = strand * HairStrandsComponent::kLinksPerStrand;
        hair.rootBindOffsets[strand] = hair.restLocalPositions[base];
        hair.segmentLengths[base]    = 0.0f;
        hair.bendLengths[base]       = 0.0f;

        for (size_t link = 1; link < HairStrandsComponent::kLinksPerStrand; ++link) {
            const size_t index         = base + link;
            const float  segmentLength = (hair.restLocalPositions[index] - hair.restLocalPositions[index - 1]).Length();
            hair.segmentLengths[index] = std::max(segmentLength, 1.0e-4f);
            hair.bendLengths[index]    = link >= 2 ? (hair.restLocalPositions[index] - hair.restLocalPositions[index - 2]).Length() : 0.0f;
        }

        for (size_t link = 0; link < HairStrandsComponent::kLinksPerStrand; ++link) {
            const size_t index = base + link;
            JPH::Vec3    direction;
            if (link + 1 < HairStrandsComponent::kLinksPerStrand) {
                direction = hair.restLocalPositions[index + 1] - hair.restLocalPositions[index];
            } else {
                direction = hair.restLocalPositions[index] - hair.restLocalPositions[index - 1];
            }
            hair.restLocalDirections[index] = SafeNormalized(direction, JPH::Vec3(0.0f, -1.0f, 0.0f));
        }
    }
    hair.bindPoseInitialized = true;
    hair.initialized         = false;
}

inline void GenerateFallbackRestPose(HairStrandsComponent& hair) noexcept {
    constexpr float kDefaultLength = 0.105f;
    constexpr float kTwoPi         = 2.0f * std::numbers::pi_v<float>;

    for (size_t strand = 0; strand < HairStrandsComponent::kStrandCount; ++strand) {
        const float     angle = kTwoPi * static_cast<float>(strand) / static_cast<float>(HairStrandsComponent::kStrandCount);
        const JPH::Vec3 rootOffset(std::cos(angle) * 0.150f, 0.075f + 0.020f * std::sin(angle * 2.0f), std::sin(angle) * 0.150f);
        const JPH::Vec3 outward(std::cos(angle), 0.0f, std::sin(angle));
        const size_t    base = strand * HairStrandsComponent::kLinksPerStrand;
        for (size_t link = 0; link < HairStrandsComponent::kLinksPerStrand; ++link) {
            const size_t index             = base + link;
            hair.restLocalPositions[index] = rootOffset + JPH::Vec3(0.0f, -kDefaultLength * static_cast<float>(link), 0.0f) +
                                             outward * (0.012f * static_cast<float>(link));
            hair.restLocalRotations[index] = JPH::Quat::sIdentity();
        }
    }
    FinalizeRestConstraints(hair);

    // Align fallback rotations to the generated strand directions. Imported
    // GLBs retain their authored bind rotations instead.
    for (size_t index = 0; index < HairStrandsComponent::kTotalParticles; ++index) {
        hair.restLocalRotations[index] = JPH::Quat::sFromTo(JPH::Vec3(0.0f, -1.0f, 0.0f), hair.restLocalDirections[index]);
    }
}

inline void InitializeParticles(HairStrandsComponent& hair, JPH::Vec3Arg headPosition, JPH::QuatArg headRotation) noexcept {
    if (!hair.bindPoseInitialized) {
        GenerateFallbackRestPose(hair);
    }

    for (size_t index = 0; index < HairStrandsComponent::kTotalParticles; ++index) {
        const JPH::Vec3 position  = JPH::Vec3(headPosition) + headRotation * hair.restLocalPositions[index];
        hair.positions[index]     = position;
        hair.prevPositions[index] = position;
    }
    hair.initialized = true;
}

} // namespace SecondaryDetail

/**
 * Captures every mapped hair transform from the GLB bind pose. Missing tail
 * links are extrapolated only for the internal solver and are never uploaded to
 * unmapped bones.
 */
void ConfigureHairBindPose(HairStrandsComponent& hair, const JPH::Mat44* bindModelTransforms, const RigBoneMap& map) noexcept {
    if (bindModelTransforms == nullptr || map.nodeCount == 0) {
        return;
    }

    const int32_t headNode = map.nodeIndices[static_cast<size_t>(CharacterBone::Head)];
    if (headNode < 0 || headNode >= static_cast<int32_t>(map.nodeCount)) {
        SecondaryDetail::GenerateFallbackRestPose(hair);
        return;
    }

    constexpr float kDefaultLength      = 0.105f;
    const JPH::Vec3 headPosition        = bindModelTransforms[headNode].GetTranslation();
    const JPH::Quat headRotation        = SecondaryDetail::ExtractRotation(bindModelTransforms[headNode]);
    const JPH::Quat inverseHeadRotation = headRotation.Inversed();

    for (size_t strand = 0; strand < HairStrandsComponent::kStrandCount; ++strand) {
        const float  angle = 2.0f * std::numbers::pi_v<float> * static_cast<float>(strand) / static_cast<float>(HairStrandsComponent::kStrandCount);
        const size_t base  = strand * HairStrandsComponent::kLinksPerStrand;
        for (size_t link = 0; link < HairStrandsComponent::kLinksPerStrand; ++link) {
            const size_t  particle = base + link;
            const size_t  semantic = static_cast<size_t>(CharacterBone::HairStart) + particle;
            const int32_t node     = map.nodeIndices[semantic];
            if (node >= 0 && node < static_cast<int32_t>(map.nodeCount)) {
                hair.restLocalPositions[particle] = inverseHeadRotation * (bindModelTransforms[node].GetTranslation() - headPosition);
                hair.restLocalRotations[particle] = (inverseHeadRotation * SecondaryDetail::ExtractRotation(bindModelTransforms[node])).Normalized();
                continue;
            }

            if (link == 0) {
                hair.restLocalPositions[particle] = JPH::Vec3(std::cos(angle) * 0.150f, 0.075f, std::sin(angle) * 0.150f);
                hair.restLocalRotations[particle] = JPH::Quat::sIdentity();
                continue;
            }

            JPH::Vec3 direction(0.0f, -1.0f, 0.0f);
            if (link >= 2) {
                direction = SecondaryDetail::SafeNormalized(hair.restLocalPositions[particle - 1] - hair.restLocalPositions[particle - 2], direction);
            }
            hair.restLocalPositions[particle] = hair.restLocalPositions[particle - 1] + direction * kDefaultLength;
            hair.restLocalRotations[particle] = hair.restLocalRotations[particle - 1];
        }
    }

    SecondaryDetail::FinalizeRestConstraints(hair);
}

/**
 * Stage 5: allocation-free Verlet/XPBD evaluation. Distance and bend constraints
 * preserve strand curvature, while a compliant head-local shape constraint
 * drives every particle back toward its authored GLB bind pose.
 */
void StepHairSimulation(
    HairStrandsComponent& hair,
    JPH::Vec3Arg          headWorldPosition,
    JPH::QuatArg          headWorldRotation,
    JPH::Vec3Arg          characterVelocity,
    float                 dt
) noexcept {
    if (!hair.initialized) {
        SecondaryDetail::InitializeParticles(hair, headWorldPosition, headWorldRotation);
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
            const JPH::Vec3 root = JPH::Vec3(headWorldPosition) + headWorldRotation * hair.restLocalPositions[base];

            hair.positions[base]     = root;
            hair.prevPositions[base] = root;
            for (size_t link = 1; link < HairStrandsComponent::kLinksPerStrand; ++link) {
                const size_t    index     = base + link;
                const JPH::Vec3 velocity  = (hair.positions[index] - hair.prevPositions[index]) * std::clamp(hair.damping, 0.0f, 1.0f);
                hair.prevPositions[index] = hair.positions[index];
                hair.positions[index] += velocity + acceleration * dtSq;
            }

            std::array<float, HairStrandsComponent::kLinksPerStrand> distanceLambdas {};
            std::array<float, HairStrandsComponent::kLinksPerStrand> bendLambdas {};
            std::array<float, HairStrandsComponent::kLinksPerStrand> shapeLambdas {};
            for (uint32_t iteration = 0; iteration < 4; ++iteration) {
                hair.positions[base] = root;

                for (size_t link = 1; link < HairStrandsComponent::kLinksPerStrand; ++link) {
                    const size_t previous = base + link - 1;
                    const size_t current  = base + link;
                    SecondaryDetail::SolveDistanceConstraint(
                        hair.positions[previous], hair.positions[current], link == 1 ? 0.0f : 1.0f, 1.0f, hair.segmentLengths[current], hair.compliance, dtSq,
                        distanceLambdas[link]
                    );
                }

                for (size_t link = 2; link < HairStrandsComponent::kLinksPerStrand; ++link) {
                    const size_t previous = base + link - 2;
                    const size_t current  = base + link;
                    SecondaryDetail::SolveDistanceConstraint(
                        hair.positions[previous], hair.positions[current], link == 2 ? 0.0f : 1.0f, 1.0f, hair.bendLengths[current], hair.bendCompliance, dtSq,
                        bendLambdas[link]
                    );
                }

                for (size_t link = 1; link < HairStrandsComponent::kLinksPerStrand; ++link) {
                    const size_t    current    = base + link;
                    const JPH::Vec3 restTarget = JPH::Vec3(headWorldPosition) + headWorldRotation * hair.restLocalPositions[current];
                    SecondaryDetail::SolveShapeConstraint(hair.positions[current], restTarget, hair.shapeCompliance, dtSq, shapeLambdas[link]);
                }

                for (size_t link = 1; link < HairStrandsComponent::kLinksPerStrand; ++link) {
                    const size_t    current    = base + link;
                    const JPH::Vec3 restTarget = JPH::Vec3(headWorldPosition) + headWorldRotation * hair.restLocalPositions[current];
                    SecondaryDetail::ProjectSphere(hair.positions[current], headWorldPosition, hair.headColliderRadius, restTarget);
                    SecondaryDetail::ProjectEllipsoid(hair.positions[current], torsoCenter, headWorldRotation, torsoRadii, restTarget);
                    SecondaryDetail::ProjectEllipsoid(hair.positions[current], shoulderL, headWorldRotation, shoulderRadii, restTarget);
                    SecondaryDetail::ProjectEllipsoid(hair.positions[current], shoulderR, headWorldRotation, shoulderRadii, restTarget);
                }
            }
        }
    });
}

/** Generates one world transform for every mapped secondary bone. */
void ExtractHairBoneTransforms(const HairStrandsComponent& hair, JPH::QuatArg headWorldRotation, JPH::Mat44* nodeTransforms, const RigBoneMap& map) noexcept {
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
            direction = SecondaryDetail::SafeNormalized(direction, headWorldRotation * hair.restLocalDirections[particleIndex]);

            const JPH::Vec3 restDirectionWorld = headWorldRotation * hair.restLocalDirections[particleIndex];
            const JPH::Quat deformation        = JPH::Quat::sFromTo(restDirectionWorld, direction);
            const JPH::Quat bindWorldRotation  = (headWorldRotation * hair.restLocalRotations[particleIndex]).Normalized();
            const JPH::Quat rotation           = (deformation * bindWorldRotation).Normalized();
            const JPH::Vec3 scale(
                nodeTransforms[nodeIndex].GetColumn3(0).Length(), nodeTransforms[nodeIndex].GetColumn3(1).Length(),
                nodeTransforms[nodeIndex].GetColumn3(2).Length()
            );
            nodeTransforms[nodeIndex] = JPH::Mat44::sRotationTranslation(rotation, hair.positions[particleIndex]).PreScaled(scale);
        }
    }
}

void ExtractHairBoneTransforms(const HairStrandsComponent& hair, JPH::Mat44* nodeTransforms, const RigBoneMap& map) noexcept {
    ExtractHairBoneTransforms(hair, JPH::Quat::sIdentity(), nodeTransforms, map);
}

} // namespace ZHLN::Animation
