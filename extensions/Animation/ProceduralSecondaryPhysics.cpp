// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module;

// clang-format off
#include <Animation/ProceduralMath.hpp>
// clang-format on
#include <Zahlen/Threading/TaskSystem.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

module ZHLN.ProceduralAnimation;

namespace ZHLN::Animation {

// Anonymous, not `namespace SecondaryDetail`: this is an implementation unit of
// ZHLN.ProceduralAnimation, so nothing here is reachable from outside the module
// and a name would only advertise privacy that the file already guarantees.
// Internal linkage is what these helpers actually need -- SafeNormalized is also
// defined in the module's ProceduralGait and ProceduralItemHandling units, and
// only an unnamed namespace keeps three definitions of one name from colliding.
namespace {

[[nodiscard]] FVec3 SafeNormalized(FVec3 value, FVec3 fallback) noexcept {
    return value.LengthSquared() > 1.0e-10f ? value.Normalized() : FVec3(fallback);
}

[[nodiscard]] Quat ExtractRotation(const Mat4x4& matrix) noexcept {
    const float     xScale = ProceduralMath::Column3(matrix, 0).Length();
    const float     yScale = ProceduralMath::Column3(matrix, 1).Length();
    const float     zScale = ProceduralMath::Column3(matrix, 2).Length();
    const FVec3 x      = xScale > 1.0e-6f ? ProceduralMath::Column3(matrix, 0) / xScale : FVec3::UnitX();
    const FVec3 y      = yScale > 1.0e-6f ? ProceduralMath::Column3(matrix, 1) / yScale : FVec3::UnitY();
    const FVec3 z      = zScale > 1.0e-6f ? ProceduralMath::Column3(matrix, 2) / zScale : FVec3::UnitZ();
    return ProceduralMath::Rotation(Mat4x4::FromCols(FVec4(x, 0.0f), FVec4(y, 0.0f), FVec4(z, 0.0f), FVec4(0.0f, 0.0f, 0.0f, 1.0f)));
}

void ProjectSphere(FVec3& point, FVec3 center, float radius, FVec3 restPoint) noexcept {
    const float allowedBindRadius = (restPoint - center).Length();
    const float effectiveRadius   = std::min(radius, allowedBindRadius);
    FVec3   delta             = point - center;
    float       length            = delta.Length();
    if (length >= effectiveRadius || effectiveRadius <= 1.0e-6f) {
        return;
    }
    point = center + SafeNormalized(delta, restPoint - center) * effectiveRadius;
}

void ProjectEllipsoid(FVec3& point, FVec3 center, Quat rotation, FVec3 radii, FVec3 restPoint) noexcept {
    const Quat invRotation = rotation.Inversed();
    const FVec3 local       = invRotation * (point - center);
    FVec3       scaled(
        local.x / std::max(radii.x, 1.0e-4f), local.y / std::max(radii.y, 1.0e-4f), local.z / std::max(radii.z, 1.0e-4f)
    );
    const FVec3 restLocal = invRotation * (restPoint - center);
    const FVec3 restScaled(
        restLocal.x / std::max(radii.x, 1.0e-4f), restLocal.y / std::max(radii.y, 1.0e-4f),
        restLocal.z / std::max(radii.z, 1.0e-4f)
    );
    const float effectiveBoundary = std::min(1.0f, restScaled.Length());
    if (scaled.Length() >= effectiveBoundary || effectiveBoundary <= 1.0e-6f) {
        return;
    }

    scaled = SafeNormalized(scaled, restScaled) * effectiveBoundary;
    const FVec3 projected(scaled.x * radii.x, scaled.y * radii.y, scaled.z * radii.z);
    point = center + rotation * projected;
}

void SolveDistanceConstraint(
    FVec3& positionA,
    FVec3& positionB,
    float      inverseMassA,
    float      inverseMassB,
    float      restLength,
    float      compliance,
    float      dtSquared,
    float&     lambda
) noexcept {
    FVec3 delta       = positionB - positionA;
    float     length      = std::max(delta.Length(), 1.0e-6f);
    float     alpha       = std::max(compliance, 0.0f) / dtSquared;
    float     denominator = inverseMassA + inverseMassB + alpha;
    if (denominator <= 1.0e-8f) {
        return;
    }

    const float deltaLambda = (-(length - restLength) - alpha * lambda) / denominator;
    lambda += deltaLambda;
    const FVec3 correction = delta / length * deltaLambda;
    positionA -= correction * inverseMassA;
    positionB += correction * inverseMassB;
}

void SolveShapeConstraint(FVec3& position, FVec3 target, float compliance, float dtSquared, float& lambda) noexcept {
    FVec3 delta  = position - target;
    float     length = delta.Length();
    if (length <= 1.0e-7f) {
        return;
    }

    const float alpha       = std::max(compliance, 0.0f) / dtSquared;
    const float deltaLambda = (-length - alpha * lambda) / (1.0f + alpha);
    lambda += deltaLambda;
    position += delta / length * deltaLambda;
}

void FinalizeRestConstraints(HairStrandsComponent& hair) noexcept {
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
            FVec3    direction;
            if (link + 1 < HairStrandsComponent::kLinksPerStrand) {
                direction = hair.restLocalPositions[index + 1] - hair.restLocalPositions[index];
            } else {
                direction = hair.restLocalPositions[index] - hair.restLocalPositions[index - 1];
            }
            hair.restLocalDirections[index] = SafeNormalized(direction, FVec3(0.0f, -1.0f, 0.0f));
        }
    }
    hair.bindPoseInitialized = true;
    hair.initialized         = false;
}

void GenerateFallbackRestPose(HairStrandsComponent& hair) noexcept {
    constexpr float kDefaultLength = 0.105f;
    constexpr float kTwoPi         = 2.0f * std::numbers::pi_v<float>;

    for (size_t strand = 0; strand < HairStrandsComponent::kStrandCount; ++strand) {
        const float     angle = kTwoPi * static_cast<float>(strand) / static_cast<float>(HairStrandsComponent::kStrandCount);
        const FVec3 rootOffset(std::cos(angle) * 0.105f, 0.17f + 0.010f * std::sin(angle * 2.0f), std::sin(angle) * 0.105f);
        const FVec3 outward(std::cos(angle), 0.0f, std::sin(angle));
        const size_t    base = strand * HairStrandsComponent::kLinksPerStrand;
        for (size_t link = 0; link < HairStrandsComponent::kLinksPerStrand; ++link) {
            const size_t index             = base + link;
            hair.restLocalPositions[index] = rootOffset + FVec3(0.0f, -kDefaultLength * static_cast<float>(link), 0.0f) +
                                             outward * (0.012f * static_cast<float>(link));
            hair.restLocalRotations[index] = Quat::Identity();
        }
    }
    FinalizeRestConstraints(hair);

    // Align fallback rotations to the generated strand directions. Imported
    // GLBs retain their authored bind rotations instead.
    for (size_t index = 0; index < HairStrandsComponent::kTotalParticles; ++index) {
        hair.restLocalRotations[index] = FromTo(FVec3(0.0f, -1.0f, 0.0f), hair.restLocalDirections[index]);
    }
}

void InitializeParticles(HairStrandsComponent& hair, FVec3 headPosition, Quat headRotation) noexcept {
    if (!hair.bindPoseInitialized) {
        GenerateFallbackRestPose(hair);
    }

    for (size_t index = 0; index < HairStrandsComponent::kTotalParticles; ++index) {
        const FVec3 position  = FVec3(headPosition) + headRotation * hair.restLocalPositions[index];
        hair.positions[index]     = position;
        hair.prevPositions[index] = position;
    }
    hair.initialized = true;
}

} // namespace

/**
 * Captures every mapped hair transform from the GLB bind pose. Missing tail
 * links are extrapolated only for the internal solver and are never uploaded to
 * unmapped bones.
 */
void ConfigureHairBindPose(HairStrandsComponent& hair, const Mat4x4* bindModelTransforms, const RigBoneMap& map) noexcept {
    if (bindModelTransforms == nullptr || map.nodeCount == 0) {
        return;
    }

    const RigNodeIndex headNode = map.nodeIndices[BoneSlot(CharacterBone::Head)];
    if (!IsValidRigNode(headNode, map.nodeCount)) {
        GenerateFallbackRestPose(hair);
        return;
    }

    constexpr float kDefaultLength      = 0.105f;
    const FVec3 headPosition        = ProceduralMath::Translation(bindModelTransforms[headNode]);
    const Quat headRotation        = ExtractRotation(bindModelTransforms[headNode]);
    const Quat inverseHeadRotation = headRotation.Inversed();

    for (size_t strand = 0; strand < HairStrandsComponent::kStrandCount; ++strand) {
        const float  angle = 2.0f * std::numbers::pi_v<float> * static_cast<float>(strand) / static_cast<float>(HairStrandsComponent::kStrandCount);
        const size_t base  = strand * HairStrandsComponent::kLinksPerStrand;
        for (size_t link = 0; link < HairStrandsComponent::kLinksPerStrand; ++link) {
            const size_t       particle = base + link;
            const size_t       semantic = BoneSlot(CharacterBone::HairStart) + particle;
            const RigNodeIndex node     = map.nodeIndices[semantic];
            if (IsValidRigNode(node, map.nodeCount)) {
                hair.restLocalPositions[particle] = inverseHeadRotation * (ProceduralMath::Translation(bindModelTransforms[node]) - headPosition);
                hair.restLocalRotations[particle] = (inverseHeadRotation * ExtractRotation(bindModelTransforms[node])).Normalized();
                continue;
            }

            if (link == 0) {
                hair.restLocalPositions[particle] = FVec3(std::cos(angle) * 0.105f, 0.17f, std::sin(angle) * 0.105f);
                hair.restLocalRotations[particle] = Quat::Identity();
                continue;
            }

            FVec3 direction(0.0f, -1.0f, 0.0f);
            if (link >= 2) {
                direction = SafeNormalized(hair.restLocalPositions[particle - 1] - hair.restLocalPositions[particle - 2], direction);
            }
            hair.restLocalPositions[particle] = hair.restLocalPositions[particle - 1] + direction * kDefaultLength;
            hair.restLocalRotations[particle] = hair.restLocalRotations[particle - 1];
        }
    }

    FinalizeRestConstraints(hair);
}

/**
 * Stage 5: allocation-free Verlet/XPBD evaluation. Distance and bend constraints
 * preserve strand curvature, while a compliant head-local shape constraint
 * drives every particle back toward its authored GLB bind pose.
 */
void StepHairSimulation(
    HairStrandsComponent& hair,
    FVec3          headWorldPosition,
    Quat          headWorldRotation,
    FVec3          characterVelocity,
    float                 dt
) noexcept {
    if (!hair.initialized) {
        InitializeParticles(hair, headWorldPosition, headWorldRotation);
    }

    const float     safeDt       = std::clamp(dt, 0.0001f, 1.0f / 30.0f);
    const float     dtSq         = safeDt * safeDt;
    const FVec3 inertialWind = characterVelocity * -0.035f;
    const FVec3 acceleration = inertialWind + FVec3(0.0f, hair.gravity, 0.0f);

    const FVec3 torsoCenter = FVec3(headWorldPosition) + headWorldRotation * FVec3(0.0f, -0.48f, -0.015f);
    const FVec3 shoulderL   = FVec3(headWorldPosition) + headWorldRotation * FVec3(0.24f, -0.31f, 0.0f);
    const FVec3 shoulderR   = FVec3(headWorldPosition) + headWorldRotation * FVec3(-0.24f, -0.31f, 0.0f);
    const FVec3 torsoRadii(hair.torsoColliderRadiusXZ, hair.torsoColliderRadiusY, hair.torsoColliderRadiusXZ * 0.75f);
    const FVec3 shoulderRadii(hair.shoulderColliderRadius, hair.shoulderColliderRadius * 0.62f, hair.shoulderColliderRadius * 0.78f);

    TaskSystem::ParallelFor(static_cast<uint32_t>(HairStrandsComponent::kStrandCount), 1, [&](uint32_t first, uint32_t last, uint32_t) {
        for (uint32_t strand = first; strand < last; ++strand) {
            const size_t    base = static_cast<size_t>(strand) * HairStrandsComponent::kLinksPerStrand;
            const FVec3 root = FVec3(headWorldPosition) + headWorldRotation * hair.restLocalPositions[base];

            hair.positions[base]     = root;
            hair.prevPositions[base] = root;
            for (size_t link = 1; link < HairStrandsComponent::kLinksPerStrand; ++link) {
                const size_t    index     = base + link;
                const FVec3 velocity  = (hair.positions[index] - hair.prevPositions[index]) * std::clamp(hair.damping, 0.0f, 1.0f);
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
                    SolveDistanceConstraint(
                        hair.positions[previous], hair.positions[current], link == 1 ? 0.0f : 1.0f, 1.0f, hair.segmentLengths[current], hair.compliance, dtSq,
                        distanceLambdas[link]
                    );
                }

                for (size_t link = 2; link < HairStrandsComponent::kLinksPerStrand; ++link) {
                    const size_t previous = base + link - 2;
                    const size_t current  = base + link;
                    SolveDistanceConstraint(
                        hair.positions[previous], hair.positions[current], link == 2 ? 0.0f : 1.0f, 1.0f, hair.bendLengths[current], hair.bendCompliance, dtSq,
                        bendLambdas[link]
                    );
                }

                for (size_t link = 1; link < HairStrandsComponent::kLinksPerStrand; ++link) {
                    const size_t    current    = base + link;
                    const FVec3 restTarget = FVec3(headWorldPosition) + headWorldRotation * hair.restLocalPositions[current];
                    SolveShapeConstraint(hair.positions[current], restTarget, hair.shapeCompliance, dtSq, shapeLambdas[link]);
                }

                for (size_t link = 1; link < HairStrandsComponent::kLinksPerStrand; ++link) {
                    const size_t    current    = base + link;
                    const FVec3 restTarget = FVec3(headWorldPosition) + headWorldRotation * hair.restLocalPositions[current];
                    ProjectSphere(hair.positions[current], headWorldPosition, hair.headColliderRadius, restTarget);
                    ProjectEllipsoid(hair.positions[current], torsoCenter, headWorldRotation, torsoRadii, restTarget);
                    ProjectEllipsoid(hair.positions[current], shoulderL, headWorldRotation, shoulderRadii, restTarget);
                    ProjectEllipsoid(hair.positions[current], shoulderR, headWorldRotation, shoulderRadii, restTarget);
                }
            }
        }
    });
}

/** Generates one world transform for every mapped secondary bone. */
void ExtractHairBoneTransforms(const HairStrandsComponent& hair, Quat headWorldRotation, Mat4x4* nodeTransforms, const RigBoneMap& map) noexcept {
    if (nodeTransforms == nullptr || !hair.initialized) {
        return;
    }

    constexpr size_t hairBoneOffset = BoneSlot(CharacterBone::HairStart);
    for (size_t strand = 0; strand < HairStrandsComponent::kStrandCount; ++strand) {
        const size_t base = strand * HairStrandsComponent::kLinksPerStrand;
        for (size_t link = 0; link < HairStrandsComponent::kLinksPerStrand; ++link) {
            const size_t       particleIndex = base + link;
            const RigNodeIndex nodeIndex     = map.nodeIndices[hairBoneOffset + particleIndex];
            if (!IsValidRigNode(nodeIndex, map.nodeCount)) {
                continue;
            }

            FVec3 direction;
            if (link + 1 < HairStrandsComponent::kLinksPerStrand) {
                direction = hair.positions[particleIndex + 1] - hair.positions[particleIndex];
            } else {
                direction = hair.positions[particleIndex] - hair.positions[particleIndex - 1];
            }
            direction = SafeNormalized(direction, headWorldRotation * hair.restLocalDirections[particleIndex]);

            const FVec3 restDirectionWorld = headWorldRotation * hair.restLocalDirections[particleIndex];
            const Quat deformation        = FromTo(restDirectionWorld, direction);
            const Quat bindWorldRotation  = (headWorldRotation * hair.restLocalRotations[particleIndex]).Normalized();
            const Quat rotation           = (deformation * bindWorldRotation).Normalized();
            const FVec3 scale(
                ProceduralMath::Column3(nodeTransforms[nodeIndex], 0).Length(), ProceduralMath::Column3(nodeTransforms[nodeIndex], 1).Length(),
                ProceduralMath::Column3(nodeTransforms[nodeIndex], 2).Length()
            );
            nodeTransforms[nodeIndex] = ProceduralMath::Transform(rotation, hair.positions[particleIndex], scale);
        }
    }
}

void ExtractHairBoneTransforms(const HairStrandsComponent& hair, Mat4x4* nodeTransforms, const RigBoneMap& map) noexcept {
    ExtractHairBoneTransforms(hair, Quat::Identity(), nodeTransforms, map);
}

} // namespace ZHLN::Animation
