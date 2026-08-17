// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <Zahlen/Common.h>
#include <Zahlen/Entity.hpp>
#include <array>
#include <cstddef>
#include <cstdint>

namespace ZHLN {

class RenderContext;
struct ModelPrefab;
struct Skeleton;

/**
 * Stable semantic slots used by the procedural solver. The first 21 entries are
 * the humanoid controls used by TestRig.glb. Slots [32, 140) are the 18 six-link
 * secondary-motion chains. The gap is intentional and leaves room for fingers
 * and future facial controls without invalidating serialized maps.
 */
enum class CharacterBone : uint16_t {
    Root = 0,
    Hips,
    Spine,
    SupSpine,
    Chest,
    Neck,
    Head,
    UpperArmL,
    ForearmL,
    HandL,
    UpperArmR,
    ForearmR,
    HandR,
    ThighL,
    ShinL,
    FootL,
    ToeL,
    ThighR,
    ShinR,
    FootR,
    ToeR,
    HairStart  = 32,
    HairCount  = 108,
    TotalBones = HairStart + HairCount
};

inline constexpr size_t kBoneCount     = static_cast<size_t>(CharacterBone::TotalBones);
inline constexpr size_t kCoreBoneCount = static_cast<size_t>(CharacterBone::ToeR) + 1;
// Real production GLBs commonly contain hundreds of mesh/attachment nodes in
// addition to the 140 deform controls. Keep fixed-capacity evaluation while
// leaving enough headroom for those imported hierarchies.
inline constexpr size_t kMaxRigNodes = 512;

/**
 * Allocation-free runtime map from semantic controls to glTF nodes.
 *
 * The first two members are the persistent skin map described by the public
 * animation contract. The remaining arrays are fixed-capacity scratch storage
 * so pose evaluation and forward kinematics do not allocate every frame.
 */
struct alignas(64) RigBoneMap {
    std::array<int32_t, kBoneCount> nodeIndices = [] {
        std::array<int32_t, kBoneCount> values {};
        values.fill(-1);
        return values;
    }();
    std::array<JPH::Mat44, kBoneCount> inverseBindMatrices {};

    std::array<int32_t, kMaxRigNodes> parentIndices = [] {
        std::array<int32_t, kMaxRigNodes> values {};
        values.fill(-1);
        return values;
    }();
    std::array<JPH::Mat44, kMaxRigNodes> bindLocalTransforms {};
    std::array<JPH::Mat44, kMaxRigNodes> localTransforms {};
    std::array<JPH::Mat44, kMaxRigNodes> modelTransforms {};

    // Critically damped keyframe state for the 21 semantic controls. Authored
    // clip samples become spring targets; procedural passes then layer on top.
    std::array<JPH::Vec3, kCoreBoneCount> poseTranslations {};
    std::array<JPH::Vec3, kCoreBoneCount> poseTranslationVelocities {};
    std::array<JPH::Quat, kCoreBoneCount> poseRotations {};
    std::array<JPH::Vec3, kCoreBoneCount> poseAngularVelocities {};
    std::array<JPH::Vec3, kCoreBoneCount> poseScales {};
    std::array<JPH::Vec3, kCoreBoneCount> poseScaleVelocities {};
    float                                 poseSpringFrequency   = 8.0f;
    float                                 poseSpringDamping     = 0.90f;
    int32_t                               springPoseTrack       = -1;
    bool                                  springPoseInitialized = false;

    const ModelPrefab* sourcePrefab  = nullptr;
    uint32_t           nodeCount     = 0;
    uint32_t           jointOffset   = 0;
    uint32_t           jointCount    = 0;
    int32_t            skeletonIndex = -1;
    uint64_t           poseVersion   = 0;
    bool               initialized   = false;
    bool               poseValid     = false;
};

/** Parametric gait state, including persistent world-space foot locks. */
struct ProceduralLocomotionComponent {
    float phase        = 0.0f;
    float strideLength = 1.60f;
    float stepHeight   = 0.28f;
    float legReach     = 0.85f;

    JPH::Vec3 localFootTargetL = JPH::Vec3::sZero();
    JPH::Vec3 localFootTargetR = JPH::Vec3::sZero();
    JPH::Vec3 footNormalL      = JPH::Vec3::sAxisY();
    JPH::Vec3 footNormalR      = JPH::Vec3::sAxisY();
    float     plantWeightL     = 1.0f;
    float     plantWeightR     = 1.0f;

    float forwardLean       = 0.0f;
    float lateralBank       = 0.0f;
    float tiltPitchVelocity = 0.0f;
    float tiltRollVelocity  = 0.0f;
    float pelvisBob         = 0.0f;
    float pelvisSway        = 0.0f;
    float pelvisDrop        = 0.0f;
    float turnRate          = 0.0f;

    // Gait-wheel instrumentation. Pass peaks as a foot crosses below the COM;
    // reach peaks near the forward/back stride extrema.
    float     strideWheelAngle  = 0.0f;
    float     passWeightL       = 0.0f;
    float     passWeightR       = 0.0f;
    float     reachWeightL      = 0.0f;
    float     reachWeightR      = 0.0f;
    JPH::Vec3 centerOfMassModel = JPH::Vec3(0.0f, 1.12f, 0.0f);

    JPH::Vec3 previousVelocity        = JPH::Vec3::sZero();
    JPH::Vec3 directionalAcceleration = JPH::Vec3::sZero();
    JPH::Quat previousRootRotation    = JPH::Quat::sIdentity();
    bool      orientationInitialized  = false;

    JPH::Vec3 plantedFootWorldL = JPH::Vec3::sZero();
    JPH::Vec3 plantedFootWorldR = JPH::Vec3::sZero();
    bool      footLockValidL    = false;
    bool      footLockValidR    = false;
    bool      wasPlantedL       = true;
    bool      wasPlantedR       = true;
};

/** Fixed 18 x 6 particle state for hair, cloth strips, and accessories. */
struct alignas(64) HairStrandsComponent {
    static constexpr size_t kStrandCount    = 18;
    static constexpr size_t kLinksPerStrand = 6;
    static constexpr size_t kTotalParticles = kStrandCount * kLinksPerStrand;

    std::array<JPH::Vec3, kTotalParticles> positions {};
    std::array<JPH::Vec3, kTotalParticles> prevPositions {};

    // Rest state is stored in head-local space and populated directly from the
    // imported GLB bind pose. This lets every mapped bone return to its authored
    // silhouette instead of treating all strands as gravity-only ropes.
    std::array<JPH::Vec3, kTotalParticles> restLocalPositions {};
    std::array<JPH::Quat, kTotalParticles> restLocalRotations {};
    std::array<JPH::Vec3, kTotalParticles> restLocalDirections {};
    std::array<float, kTotalParticles>     segmentLengths {};
    std::array<float, kTotalParticles>     bendLengths {};
    std::array<JPH::Vec3, kStrandCount>    rootBindOffsets {};

    float damping                = 0.94f;
    float gravity                = -9.81f;
    float compliance             = 0.000002f;
    float bendCompliance         = 0.000020f;
    float shapeCompliance        = 0.000010f;
    float headColliderRadius     = 0.16f;
    float torsoColliderRadiusXZ  = 0.24f;
    float torsoColliderRadiusY   = 0.38f;
    float shoulderColliderRadius = 0.20f;
    bool  bindPoseInitialized    = false;
    bool  initialized            = false;
};

struct ProceduralLookAtComponent {
    JPH::Vec3 targetWorldPos = JPH::Vec3::sZero();
    float     weight         = 1.0f;
    float     maxAngleDeg    = 75.0f;
};

/** Runtime layer switches for animation authoring and isolation workflows. */
struct ProceduralAnimationConfigComponent {
    bool enableGait             = true;
    bool enableLegIK            = true;
    bool enableAccelerationTilt = true;
    bool enableUpperBody        = true;
    bool enableSecondaryMotion  = true;

    // Overrides all switches above and evaluates only the authored keyframe
    // pose, minimum-jerk key sampling, and semantic pose springs.
    bool keyframeOnly = false;
};

/**
 * Builds a map for an imported glTF rig. Matching is case/separator insensitive
 * and accepts common Blender, Mixamo, and TestRig aliases.
 */
ZHLN_API bool BuildBoneMap(const ModelPrefab& prefab, const Skeleton& skeleton, RigBoneMap& outMap) noexcept;

/** Builds the same hierarchy procedurally for the self-contained sample. */
ZHLN_API void BuildStandardProceduralRig(RigBoneMap& outMap) noexcept;

/** Draws the most recently evaluated pose, including contact normals and hair. */
ZHLN_API void DrawProceduralDebugRig(
    RenderContext&                       renderContext,
    JPH::Vec3Arg                         rootPosition,
    JPH::QuatArg                         rootRotation,
    const RigBoneMap&                    boneMap,
    const ProceduralLocomotionComponent* gait = nullptr
) noexcept;

} // namespace ZHLN
