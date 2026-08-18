// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module;

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <Zahlen/Common.h>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/ModelPrefab.hpp>
#include <Zahlen/Render.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

export module ZHLN.ProceduralAnimation;

export namespace ZHLN {

/**
 * Stable semantic slots used by the procedural solver. The first 21 entries are
 * the humanoid controls used by TestRig.glb. Slots [32, 140) are the 18 six-link
 * secondary-motion chains. The gap is intentional and leaves room for fingers
 * and future facial controls without invalidating serialized maps.
 */
enum class CharacterBone : size_t {
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

using RigNodeIndex                           = size_t;
inline constexpr RigNodeIndex InvalidRigNode = std::numeric_limits<RigNodeIndex>::max();

[[nodiscard]] constexpr size_t BoneSlot(CharacterBone bone) noexcept {
    return static_cast<size_t>(bone);
}
[[nodiscard]] constexpr bool IsValidRigNode(RigNodeIndex node, size_t nodeCount) noexcept {
    return node != InvalidRigNode && node < nodeCount;
}

inline constexpr size_t kBoneCount     = BoneSlot(CharacterBone::TotalBones);
inline constexpr size_t kCoreBoneCount = BoneSlot(CharacterBone::ToeR) + 1;
// Real production GLBs commonly contain hundreds of mesh/attachment nodes in
// addition to the 140 deform controls. Keep fixed-capacity evaluation while
// leaving enough headroom for those imported hierarchies.
inline constexpr size_t kMaxRigNodes = 512;

struct RigChildOfConstraint {
    RigNodeIndex parent         = InvalidRigNode;
    RigNodeIndex child          = InvalidRigNode;
    JPH::Mat44   bindRelative   = JPH::Mat44::sIdentity();
    JPH::Mat44   localPoseDelta = JPH::Mat44::sIdentity();
};

/**
 * Allocation-free runtime map from semantic controls to glTF nodes.
 *
 * The first two members are the persistent skin map described by the public
 * animation contract. The remaining arrays are fixed-capacity scratch storage
 * so pose evaluation and forward kinematics do not allocate every frame.
 */
struct alignas(64) RigBoneMap {
    std::array<RigNodeIndex, kBoneCount> nodeIndices = [] {
        std::array<RigNodeIndex, kBoneCount> values {};
        values.fill(InvalidRigNode);
        return values;
    }();
    std::array<JPH::Mat44, kBoneCount> inverseBindMatrices {};

    std::array<RigNodeIndex, kMaxRigNodes> parentIndices = [] {
        std::array<RigNodeIndex, kMaxRigNodes> values {};
        values.fill(InvalidRigNode);
        return values;
    }();
    std::array<JPH::Mat44, kMaxRigNodes> bindLocalTransforms {};
    std::array<JPH::Mat44, kMaxRigNodes> localTransforms {};
    std::array<JPH::Mat44, kMaxRigNodes> modelTransforms {};

    std::array<RigChildOfConstraint, 4> childOfConstraints {};
    size_t                              childOfConstraintCount = 0;

    // Spring-damper keyframe state for the 21 semantic controls. Authored clip
    // samples become physical targets; procedural passes then layer on top.
    std::array<JPH::Vec3, kCoreBoneCount> poseTranslations {};
    std::array<JPH::Vec3, kCoreBoneCount> poseTranslationVelocities {};
    std::array<JPH::Quat, kCoreBoneCount> poseRotations {};
    std::array<JPH::Vec3, kCoreBoneCount> poseAngularVelocities {};
    std::array<JPH::Vec3, kCoreBoneCount> poseScales {};
    std::array<JPH::Vec3, kCoreBoneCount> poseScaleVelocities {};
    float                                 poseSpringStiffness     = 2500.0f;
    float                                 poseSpringDampingFactor = 0.90f;
    int32_t                               springPoseTrack         = -1;
    bool                                  springPoseInitialized   = false;

    const ModelPrefab* sourcePrefab                 = nullptr;
    size_t             nodeCount                    = 0;
    uint32_t           jointOffset                  = 0;
    uint32_t           jointCount                   = 0;
    int32_t            skeletonIndex                = -1;
    uint64_t           poseVersion                  = 0;
    size_t             synchronizedAttachmentCount  = 0;
    size_t             synchronizedSkinPaletteCount = 0;
    bool               initialized                  = false;
    bool               poseValid                    = false;
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

    float forwardLean        = 0.0f;
    float lateralBank        = 0.0f;
    float tiltPitchVelocity  = 0.0f;
    float tiltRollVelocity   = 0.0f;
    float pelvisBob          = 0.0f;
    float pelvisSway         = 0.0f;
    float pelvisDrop         = 0.0f;
    float targetPelvisDrop   = 0.0f;
    float pelvisDropVelocity = 0.0f;
    float gravityBounce      = 0.0f;
    float bounceGravity      = 9.81f;
    float maxBounceHeight    = 0.045f;
    float turnRate           = 0.0f;

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

enum class PoseInterpolationMode : uint8_t {
    Bicubic,
    SpringDamper,
};

/** Runtime layer switches and pose interpolation settings. */
struct ProceduralAnimationConfigComponent {
    PoseInterpolationMode poseInterpolation       = PoseInterpolationMode::SpringDamper;
    float                 springStiffness         = 2500.0f;
    float                 springDampingFactor     = 0.90f;
    float                 bicubicTension          = 0.0f;
    float                 legIKWeight             = 0.65f;
    float                 pelvisDropWeight        = 1.0f;
    float                 maxFootHeightCorrection = 0.18f;

    bool enableGait                         = true;
    bool enableGravityBounce                = true;
    bool enableLegIK                        = true;
    bool preserveAuthoredFootXZ             = true;
    bool worldLockFeet                      = false;
    bool enableAccelerationTilt             = true;
    bool enableUpperBody                    = true;
    bool enableSecondaryMotion              = true;
    bool enforceHandChildOf                 = true;
    bool layerUpperBodyOverAuthoredChannels = false;

    // Overrides all switches above and evaluates only the authored keyframe
    // pose through the selected bicubic or spring-damper interpolator.
    bool authoredPoseOnly = false;
};

/** Authored locomotion clips synchronized to the procedural stride wheel. */
struct ProceduralLocomotionTracksComponent {
    int32_t idleTrack = -1;
    int32_t walkTrack = -1;
    int32_t runTrack  = -1;

    float movementThreshold        = 0.08f;
    float runSpeedThreshold        = 3.20f;
    float synchronizedPhase        = 0.0f;
    float synchronizedTime         = 0.0f;
    float passWeight               = 0.0f;
    float reachWeight              = 1.0f;
    bool  synchronizeToStrideWheel = true;
};

/** Core procedural algorithms. Extras may consume these APIs; core never consumes extras. */
namespace Animation {

float      EvaluateGravityBounce(const ProceduralLocomotionComponent& gait, float speed) noexcept;
float      EvaluateTwoKeyPosePhase(float stridePhase) noexcept;
void       EvaluateGait(ProceduralLocomotionComponent& gait, JPH::Vec3Arg velocity, float angularVelocity, float dt) noexcept;
void       EvaluateGait(ProceduralLocomotionComponent& gait, JPH::Vec3Arg velocity, float dt) noexcept;
void       ApplyAccelerationTilt(ProceduralLocomotionComponent& gait, JPH::Mat44* nodeTransforms, const RigBoneMap& map) noexcept;
JPH::Mat44 CorrectBoneDirection(
    const JPH::Mat44& authoredTransform,
    JPH::Vec3Arg      currentDirection,
    JPH::Vec3Arg      solvedDirection,
    JPH::Vec3Arg      solvedPosition
) noexcept;
JPH::Mat44 AlignFootToGround(const JPH::Mat44& authoredFoot, JPH::Vec3Arg target, JPH::Vec3Arg modelNormal) noexcept;
void ApplyPelvisGaitOffset(const ProceduralLocomotionComponent& gait, JPH::Mat44* nodeTransforms, const RigBoneMap& map, bool includeDrop = true) noexcept;
void SolveLegGrounding(
    Engine&                        engine,
    JPH::Vec3Arg                   rootPosition,
    JPH::QuatArg                   rootRotation,
    ProceduralLocomotionComponent& gait,
    JPH::Mat44*                    nodeTransforms,
    const RigBoneMap&              map,
    Entity                         ignoredPhysicsHandle    = {},
    float                          ikWeight                = 1.0f,
    bool                           preserveAuthoredFootXZ  = true,
    bool                           worldLockFeet           = false,
    float                          maxFootHeightCorrection = 0.18f,
    float                          dt                      = 1.0f / 60.0f,
    float                          pelvisDropWeight        = 1.0f
) noexcept;
void SolveUpperBody(
    const ProceduralLocomotionComponent& gait,
    const ProceduralLookAtComponent*     lookAt,
    JPH::Vec3Arg                         rootPosition,
    JPH::QuatArg                         rootRotation,
    JPH::Mat44*                          nodeTransforms,
    const RigBoneMap&                    map,
    bool                                 applyArmSwing = true,
    bool                                 applyLookAt   = true
) noexcept;
void SolveUpperBody(
    const ProceduralLocomotionComponent& gait,
    const ProceduralLookAtComponent*     lookAt,
    JPH::Mat44*                          nodeTransforms,
    const RigBoneMap&                    map
) noexcept;

void ConfigureHairBindPose(HairStrandsComponent& hair, const JPH::Mat44* bindModelTransforms, const RigBoneMap& map) noexcept;
void StepHairSimulation(
    HairStrandsComponent& hair,
    JPH::Vec3Arg          headWorldPosition,
    JPH::QuatArg          headWorldRotation,
    JPH::Vec3Arg          characterVelocity,
    float                 dt
) noexcept;
void ExtractHairBoneTransforms(const HairStrandsComponent& hair, JPH::QuatArg headWorldRotation, JPH::Mat44* nodeTransforms, const RigBoneMap& map) noexcept;
void ExtractHairBoneTransforms(const HairStrandsComponent& hair, JPH::Mat44* nodeTransforms, const RigBoneMap& map) noexcept;

} // namespace Animation

/** Finds an authored animation by case-insensitive exact name, then substring. */
int32_t FindAnimationTrack(const ModelPrefab& prefab, std::string_view name) noexcept;

/**
 * Builds a map for an imported glTF rig. Matching is case/separator insensitive
 * and accepts common Blender, Mixamo, and TestRig aliases.
 */
bool BuildBoneMap(const ModelPrefab& prefab, const Skeleton& skeleton, RigBoneMap& outMap) noexcept;

/** Builds the same hierarchy procedurally for the self-contained sample. */
void BuildStandardProceduralRig(RigBoneMap& outMap) noexcept;

/** Draws the most recently evaluated pose, including contact normals and hair. */
void DrawProceduralDebugRig(
    RenderContext&                       renderContext,
    JPH::Vec3Arg                         rootPosition,
    JPH::QuatArg                         rootRotation,
    const RigBoneMap&                    boneMap,
    const ProceduralLocomotionComponent* gait = nullptr
) noexcept;

namespace ProceduralAnimation {

/** Registers ECS types and inserts the optional evaluator before articulation. */
void Register(Engine& engine);
/** Direct evaluation entry point for custom schedules. */
void   Update(Engine& engine, float dt) noexcept;
void   ResolveModelTransforms(RigBoneMap& boneMap) noexcept;
void   CaptureChildOfPoseDeltas(RigBoneMap& boneMap) noexcept;
size_t ApplyChildOfConstraints(RigBoneMap& boneMap) noexcept;
size_t BuildSkinningPalette(const Skeleton& skeleton, const RigBoneMap& boneMap, std::span<JPH::Mat44> output) noexcept;
size_t SyncNonSkinnedAttachments(ECS::Registry& registry, Entity rootEntity, const RigBoneMap& boneMap) noexcept;
void   DrawDebugRig(
    RenderContext&                       renderContext,
    JPH::Vec3Arg                         rootPosition,
    JPH::QuatArg                         rootRotation,
    const RigBoneMap&                    boneMap,
    const ProceduralLocomotionComponent* gait = nullptr
) noexcept;

} // namespace ProceduralAnimation

} // namespace ZHLN
