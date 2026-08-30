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
#include <memory>
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
    HairCount  = 144,
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
inline constexpr size_t kMaxRigNodes           = 1024;
inline constexpr size_t kMaxChildOfConstraints = 64;
inline constexpr size_t kMaxFingerJoints       = 64;

enum class RigChildOfKind : uint8_t {
    Hand,
    Chest,
    Neck,
    Head,
    FootAttachment,
    Knee,
    Ankle,
};

struct RigChildOfConstraint {
    // Knee constraints pin only the child joint position. This preserves the
    // authored/procedural shin rotation while preventing a flattened export
    // hierarchy from opening a gap between the thigh and shin.
    RigNodeIndex   parent         = InvalidRigNode;
    RigNodeIndex   child          = InvalidRigNode;
    RigChildOfKind kind           = RigChildOfKind::Hand;
    JPH::Mat44     bindRelative   = JPH::Mat44::sIdentity();
    JPH::Mat44     localPoseDelta = JPH::Mat44::sIdentity();
};

enum class FingerDigit : uint8_t {
    Thumb,
    Index,
    Middle,
    Ring,
    Pinky,
};

struct RigFingerJointConstraint {
    RigNodeIndex parent         = InvalidRigNode;
    RigNodeIndex child          = InvalidRigNode;
    FingerDigit  digit          = FingerDigit::Index;
    uint8_t      side           = 0; // 0 = left, 1 = right
    uint8_t      chain          = 0; // 0 = standard hand, 1 = claw/alternate hand
    uint8_t      segment        = 0;
    bool         repairRelation = false;
    float        hingeFlexSign  = -1.0f;
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
    std::array<JPH::Mat44, kBoneCount> inverseBindMatrices = [] {
        std::array<JPH::Mat44, kBoneCount> values;
        values.fill(JPH::Mat44::sIdentity());
        return values;
    }();

    std::array<RigNodeIndex, kMaxRigNodes> parentIndices = [] {
        std::array<RigNodeIndex, kMaxRigNodes> values {};
        values.fill(InvalidRigNode);
        return values;
    }();
    std::array<JPH::Mat44, kMaxRigNodes> bindLocalTransforms = [] {
        std::array<JPH::Mat44, kMaxRigNodes> values;
        values.fill(JPH::Mat44::sIdentity());
        return values;
    }();
    std::array<JPH::Mat44, kMaxRigNodes> localTransforms = bindLocalTransforms;
    std::array<JPH::Mat44, kMaxRigNodes> modelTransforms = bindLocalTransforms;

    std::array<JPH::Quat, 2>                               handBoneToPalmRotations {JPH::Quat::sIdentity(), JPH::Quat::sIdentity()};
    std::array<bool, 2>                                    handPalmFramesValid {false, false};
    std::array<RigFingerJointConstraint, kMaxFingerJoints> fingerJointConstraints {};
    size_t                                                 fingerJointConstraintCount = 0;

    std::array<RigChildOfConstraint, kMaxChildOfConstraints> childOfConstraints {};
    size_t                                                   childOfConstraintCount = 0;

    // Spring-damper keyframe state for the 21 semantic controls. Authored clip
    // samples become physical targets; procedural passes then layer on top.
    std::array<JPH::Vec3, kCoreBoneCount> poseTranslations {};
    std::array<JPH::Vec3, kCoreBoneCount> poseTranslationVelocities {};
    std::array<JPH::Quat, kCoreBoneCount> poseRotations = [] {
        std::array<JPH::Quat, kCoreBoneCount> values;
        values.fill(JPH::Quat::sIdentity());
        return values;
    }();
    std::array<JPH::Vec3, kCoreBoneCount> poseAngularVelocities {};
    std::array<JPH::Vec3, kCoreBoneCount> poseScales = [] {
        std::array<JPH::Vec3, kCoreBoneCount> values;
        values.fill(JPH::Vec3::sReplicate(1.0f));
        return values;
    }();
    std::array<JPH::Vec3, kCoreBoneCount> poseScaleVelocities {};
    float                                 poseSpringStiffness     = 2500.0f;
    float                                 poseSpringDampingFactor = 0.90f;
    int32_t                               springPoseTrack         = -1;
    bool                                  springPoseInitialized   = false;

    const ModelPrefab* sourcePrefab                 = nullptr;
    size_t             nodeCount                    = 0;
    size_t             sourceHairStrandCount        = 0;
    uint32_t           jointOffset                  = 0;
    uint32_t           jointCount                   = 0;
    int32_t            skeletonIndex                = -1;
    uint64_t           poseVersion                  = 0;
    size_t             synchronizedAttachmentCount  = 0;
    size_t             synchronizedSkinPaletteCount = 0;
    bool               initialized                  = false;
    bool               poseValid                    = false;
    // Baked control rigs animate CTR/FK/IK/MCH/ORG and DEF nodes together.
    // Filtering only the semantic DEF nodes through independent springs breaks
    // those authored relationships, so their complete hierarchy is sampled as
    // one coherent bicubic pose before procedural model-space passes.
    bool preserveAuthoredHierarchy = false;

    void Reset() noexcept {
        std::destroy_at(this);
        std::construct_at(this);
    }
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
    bool      ikReachClampedL  = false;
    bool      ikReachClampedR  = false;

    float forwardLean             = 0.0f;
    float lateralBank             = 0.0f;
    float tiltPitchVelocity       = 0.0f;
    float tiltRollVelocity        = 0.0f;
    float ikBodyTiltPitch         = 0.0f;
    float ikBodyTiltRoll          = 0.0f;
    float ikBodyTiltPitchVelocity = 0.0f;
    float ikBodyTiltRollVelocity  = 0.0f;
    float pelvisBob               = 0.0f;
    float pelvisSway              = 0.0f;
    float pelvisDrop              = 0.0f;
    float targetPelvisDrop        = 0.0f;
    float pelvisDropVelocity      = 0.0f;
    float gravityBounce           = 0.0f;
    float bounceGravity           = 9.81f;
    float maxBounceHeight         = 0.045f;
    float turnRate                = 0.0f;

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

/** Fixed-capacity 24 x 6 particle state; rigs may populate 18-24 strands. */
struct alignas(64) HairStrandsComponent {
    static constexpr size_t kMinimumStrandCount = 18;
    static constexpr size_t kStrandCount        = 24;
    static constexpr size_t kLinksPerStrand     = 6;
    static constexpr size_t kTotalParticles     = kStrandCount * kLinksPerStrand;

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

struct FirstPersonVisibilityComponent {
    JPH::Vec3 eyeOffsetModel      = JPH::Vec3(0.0f, 0.07f, 0.08f);
    JPH::Vec3 smoothedEyeModel    = JPH::Vec3::sZero();
    JPH::Vec3 eyeVelocity         = JPH::Vec3::sZero();
    JPH::Quat smoothedViewModel   = JPH::Quat::sIdentity();
    JPH::Vec3 viewAngularVelocity = JPH::Vec3::sZero();
    float     lookYawDegrees      = 0.0f;
    float     lookPitchDegrees    = 0.0f;
    float     positionStiffness   = 120.0f;
    float     rotationStiffness   = 160.0f;
    float     dampingFactor       = 1.0f;
    float     fov                 = 75.0f;
    float     nearPlane           = 0.03f;
    bool      enabled             = false;
    bool      hideHead            = true;
    bool      hideHair            = true;
    bool      cameraInitialized   = false;
};

namespace Animation {

enum class ItemDriverMode : uint8_t {
    HandAnchored,
    AimGuided,
    BodyMounted,
    WorldAnchored,
};

enum class GraspShape : uint8_t {
    Cylinder,
    TriggerGrip,
    FlatPalm,
    Pinch,
    RelaxedOpen,
};

enum class FingerCurlAxisMode : uint8_t {
    AutomaticPalm, // Select the sign that bends each finger toward the inferred palm.
    LocalNegativeX,
    MirroredLocalX,
};

enum class GripOrientationMode : uint8_t {
    AutomaticHanded,   // +Z faces forward; palm faces inward based on assigned hand.
    ExplicitPalmFrame, // Grip rotation already supplies palm +X, thumb +Y, fingers +Z.
};

struct GraspDesc {
    GraspShape         shape        = GraspShape::Cylinder;
    FingerCurlAxisMode curlAxisMode = FingerCurlAxisMode::AutomaticPalm;
    float              gripRadius   = 0.025f;
    float              tightness    = 1.0f;
    float              triggerCurl  = 1.0f;
};

struct GripPoint {
    CharacterBone       assignedLimb       = CharacterBone::HandR;
    GripOrientationMode orientationMode    = GripOrientationMode::AutomaticHanded;
    JPH::Mat44          localTransform     = JPH::Mat44::sIdentity();
    JPH::Vec3           poleHintOffset     = JPH::Vec3(0.0f, -1.0f, -0.5f);
    float               ikWeight           = 1.0f;
    float               ikWeightVelocity   = 0.0f;
    float               evaluatedIKWeight  = 1.0f;
    float               rotationWeight     = 1.0f;
    float               forearmTwistWeight = 0.65f;
    float               maxArmExtension    = 0.98f;
    float               maxForearmTwistDeg = 55.0f;
    float               maxWristTwistDeg   = 40.0f;
    float               maxWristSwingDeg   = 50.0f;
    GraspDesc           grasp;
};

struct ItemObstacleAvoidance {
    float probeDistance = 0.50f;
    float probeRadius   = 0.12f;
    float pushbackScale = 0.85f;
    float tiltScale     = 0.35f;
};

struct ItemSwayState {
    JPH::Vec3 positionOffset         = JPH::Vec3::sZero();
    JPH::Vec3 positionVelocity       = JPH::Vec3::sZero();
    JPH::Quat rotationOffset         = JPH::Quat::sIdentity();
    JPH::Vec3 angularVelocity        = JPH::Vec3::sZero();
    JPH::Vec3 previousDriverPosition = JPH::Vec3::sZero();
    JPH::Quat previousDriverRotation = JPH::Quat::sIdentity();
    float     massKg                 = 2.5f;
    float     stiffness              = 130.0f;
    float     damping                = 0.84f;
    bool      driverInitialized      = false;
};

struct ItemHandlingComponent {
    bool           enabled    = true;
    ItemDriverMode driverMode = ItemDriverMode::HandAnchored;
    Entity         itemEntity {};

    JPH::Mat44 hipLocalOffset = JPH::Mat44::sTranslation(JPH::Vec3(0.20f, -0.16f, 0.40f));
    JPH::Mat44 aimLocalOffset = JPH::Mat44::sTranslation(JPH::Vec3(0.00f, -0.05f, 0.22f));
    JPH::Mat44 worldAnchor    = JPH::Mat44::sIdentity();
    float      aimProgress    = 0.0f;
    float      aimVelocity    = 0.0f;

    std::array<GripPoint, 4> grips {};
    size_t                   gripCount = 0;
    ItemObstacleAvoidance    avoidance {};
    ItemSwayState            sway {};
    JPH::Mat44               itemModelTransform = JPH::Mat44::sIdentity();
    float                    shoulderLeadWeight = 0.20f;
    float                    torsoReachWeight   = 0.25f;
};

struct FingerCurlDesc {
    float thumb  = 0.0f;
    float index  = 0.0f;
    float middle = 0.0f;
    float ring   = 0.0f;
    float pinky  = 0.0f;
};

} // namespace Animation

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
    float                 maxLegExtension         = 0.98f;
    float                 maxIKBodyTiltDegrees    = 10.0f;
    float                 maxAnkleSidewaysDegrees = 15.0f;
    float                 maxAnkleForwardDegrees  = 35.0f;

    bool enableGait                         = true;
    bool enableGravityBounce                = true;
    bool enableLegIK                        = true;
    bool preserveAuthoredFootXZ             = true;
    bool worldLockFeet                      = false;
    bool enableAccelerationTilt             = true;
    bool enableUpperBody                    = true;
    bool enableSecondaryMotion              = true;
    bool enforceHandChildOf                 = true;
    bool enforceChestChildOf                = true;
    bool enforceNeckChildOf                 = true;
    bool enforceHeadChildOf                 = true;
    bool enforceFootAttachments             = true;
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
JPH::Vec3  LimitGroundNormal(JPH::Vec3Arg modelNormal, float maxSidewaysRadians, float maxForwardRadians) noexcept;
JPH::Mat44 AlignFootToGround(
    const JPH::Mat44& authoredFoot,
    JPH::Vec3Arg      target,
    JPH::Vec3Arg      modelNormal,
    float             maxSidewaysRadians = JPH::DegreesToRadians(15.0f),
    float             maxForwardRadians  = JPH::DegreesToRadians(35.0f)
) noexcept;
size_t SetModelTransformAndCarrySubtree(JPH::Mat44* nodeTransforms, const RigBoneMap& map, RigNodeIndex rootNode, const JPH::Mat44& target) noexcept;
void   ApplyIKReachTilt(
    ProceduralLocomotionComponent& gait,
    JPH::Mat44*                    nodeTransforms,
    const RigBoneMap&              map,
    JPH::Vec3Arg                   targetL,
    JPH::Vec3Arg                   targetR,
    float                          weightL,
    float                          weightR,
    float                          maxLegExtension,
    float                          maxBodyTiltRadians,
    float                          dt
) noexcept;
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
    float                          pelvisDropWeight        = 1.0f,
    float                          maxLegExtension         = 0.98f,
    float                          maxBodyTiltRadians      = JPH::DegreesToRadians(10.0f),
    float                          maxAnkleSidewaysRadians = JPH::DegreesToRadians(15.0f),
    float                          maxAnkleForwardRadians  = JPH::DegreesToRadians(35.0f)
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

[[nodiscard]] JPH::Mat44 SolveItemBasePose(
    const ItemHandlingComponent& handling,
    const JPH::Mat44&            primaryHandModel,
    const JPH::Mat44&            chestModel,
    const JPH::Mat44&            worldToModel,
    JPH::Vec3Arg                 headPosModel,
    JPH::Vec3Arg                 aimDirModel,
    const JPH::Mat44&            worldAnchor
) noexcept;
float UpdateGripWeight(GripPoint& grip, float dt) noexcept;
void  UpdateItemDynamics(
    Engine&                engine,
    Entity                 characterEntity,
    ItemHandlingComponent& handling,
    JPH::Vec3Arg           rootPosition,
    JPH::QuatArg           rootRotation,
    float                  dt
) noexcept;
void ConstrainItemToGripReach(ItemHandlingComponent& handling, const JPH::Mat44* nodeTransforms, const RigBoneMap& map) noexcept;
void ApplyClavicleLead(JPH::Mat44* nodeTransforms, const RigBoneMap& map, CharacterBone upperArmBone, JPH::Vec3Arg targetGripPos, float weight) noexcept;
void ApplyTorsoReachCompensation(
    JPH::Mat44*       nodeTransforms,
    const RigBoneMap& map,
    CharacterBone     upperArmBone,
    CharacterBone     forearmBone,
    CharacterBone     handBone,
    JPH::Vec3Arg      targetGripPos,
    float             weight
) noexcept;
[[nodiscard]] JPH::Quat ConstrainWristRotation(
    JPH::QuatArg targetRotation,
    JPH::QuatArg authoredRotation,
    JPH::Vec3Arg twistAxis,
    float        maxTwistRadians,
    float        maxSwingRadians
) noexcept;
void SolveLimbIK(
    JPH::Mat44*       nodeTransforms,
    const RigBoneMap& map,
    CharacterBone     upperBone,
    CharacterBone     foreBone,
    CharacterBone     handBone,
    const JPH::Mat44& targetGripTransform,
    const GripPoint&  grip
) noexcept;
[[nodiscard]] FingerCurlDesc EvaluateFingerCurl(const GraspDesc& grasp) noexcept;
[[nodiscard]] JPH::Quat      ConstrainFingerHingeRotation(
    JPH::QuatArg authoredRotation,
    JPH::QuatArg desiredRotation,
    JPH::Vec3Arg hingeAxis,
    float        flexSign,
    float        maxFlexRadians,
    float        maxExtensionRadians
) noexcept;
void ApplyKinematicFingers(
    JPH::Mat44*           nodeTransforms,
    const RigBoneMap&     map,
    CharacterBone         handBone,
    const FingerCurlDesc& curl,
    float                 weight,
    FingerCurlAxisMode    axisMode = FingerCurlAxisMode::AutomaticPalm
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
void   CaptureAuthoredConstraintPoseDeltas(RigBoneMap& boneMap) noexcept;
void   CaptureFingerPoseDeltas(RigBoneMap& boneMap) noexcept;
size_t ApplyFingerRelationConstraints(RigBoneMap& boneMap) noexcept;
size_t ApplyChildOfConstraints(
    RigBoneMap& boneMap,
    bool        applyHands           = true,
    bool        applyChest           = true,
    bool        applyNeck            = true,
    bool        applyHead            = true,
    bool        applyFootAttachments = true
) noexcept;
size_t BuildSkinningPalette(const Skeleton& skeleton, const RigBoneMap& boneMap, std::span<JPH::Mat44> output) noexcept;
size_t MaskFirstPersonPalette(
    const Skeleton&       skeleton,
    const RigBoneMap&     boneMap,
    std::span<JPH::Mat44> palette,
    bool                  hideHead = true,
    bool                  hideHair = true
) noexcept;
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
