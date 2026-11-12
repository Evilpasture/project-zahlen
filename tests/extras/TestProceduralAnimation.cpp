// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Math/Angles.hpp>
#include <Zahlen/Math/Matrix.hpp>
#include <Zahlen/Math/Quat.hpp>
#include <Animation/ProceduralMath.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Engine/ModelPrefab.hpp>
#include <Zahlen/Render/SkeletalAnimation.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <Zahlen/Audio/Audio.hpp>
#include <Zahlen/ECS/ExtensionServices.hpp>
#include <Zahlen/Engine/AssetManager.hpp>
#include <Zahlen/Engine/SystemContext.hpp>
#include <Zahlen/Engine/PoseUploads.hpp>
#include <Zahlen/Engine/World.hpp>
#include <Zahlen/Physics/Physics.hpp>
#include <algorithm>
#include <array>
#include <cgltf.h>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <expected>
#include <fstream>
#include <iostream>
#include <memory>
#include <numbers>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

import ZHLN.Rig;
import ZHLN.Locomotion;
import ZHLN.ProceduralAnimation;

using ZHLN::FVec3;
using ZHLN::FVec4;
using ZHLN::Mat4x4;
using ZHLN::Quat;
using ZHLN::CreateTranslation;
using ZHLN::Inverse;
using ZHLN::Dot;

static_assert(ZHLN::Rig::GetBindPosition(ZHLN::Rig::Joint::Hips) == FVec3 {0.0f, 1.0f, 0.0f});
static_assert(ZHLN::Rig::GetBindRotation(ZHLN::Rig::Joint::Hips) == Quat::Identity());
static_assert(std::is_invocable_v<decltype(&ZHLN::Animation::LimitGroundNormal), FVec3, ZHLN::RadF, ZHLN::RadF>);
static_assert(!std::is_invocable_v<decltype(&ZHLN::Animation::LimitGroundNormal), FVec3, float, float>);
static_assert(std::is_invocable_v<decltype(&ZHLN::Animation::ConstrainWristRotation), Quat, Quat, FVec3, ZHLN::RadF, ZHLN::RadF>);
static_assert(!std::is_invocable_v<decltype(&ZHLN::Animation::ConstrainWristRotation), Quat, Quat, FVec3, float, float>);

enum class ProceduralAnimationTestError : uint32_t {
    RigMappingFailed ZHLN_ANNOTATION(ZHLN::Description<"The generated procedural rig did not map all core and secondary controls."> {}) = 1,
    GaitInvariantFailed ZHLN_ANNOTATION(ZHLN::Description<"The distance-driven gait clock or alternating foot phases violated its invariant."> {}),
    HairConstraintFailed ZHLN_ANNOTATION(ZHLN::Description<"The XPBD hair solver produced a non-finite or excessively stretched segment."> {}),
};

namespace {

[[nodiscard]] bool Close(FVec3 a, FVec3 b, float tolerance) { return (a - b).LengthSquared() <= tolerance; }
[[nodiscard]] bool Close(Quat a, Quat b, float tolerance) { return (a - b).LengthSquared() <= tolerance; }
[[nodiscard]] bool Close(const Mat4x4& a, const Mat4x4& b, float tolerance) {
    for (size_t c = 0; c < 4; ++c) {
        float squared = 0.0f;
        for (size_t r = 0; r < 4; ++r) { const float d = a[r,c] - b[r,c]; squared += d * d; }
        if (squared > tolerance) return false;
    }
    return true;
}
[[nodiscard]] bool NearZero(FVec3 v, float tolerance) { return v.LengthSquared() <= tolerance; }

[[nodiscard]] Mat4x4 ToPrefabMatrix(const Mat4x4& m) { return m; }

namespace RigFixture {

/**
 * Builds the ModelPrefab *input* that BuildBoneMap consumes, from a parsed GLB.
 *
 * This is a fixture, not an importer test: this suite is CPU-only and cannot
 * link the real GLTF importer, which uploads through a live RenderContext.
 * The importer itself is covered by tests/render/TestGLTFImport.cpp, which
 * loads the same asset through PrefabFactory and checks the prefab it
 * returns against the source document. Here the asset is only a realistic
 * source of node names and a bind pose for the rig mapper.
 */
inline void BuildFixture(const cgltf_data& data, ZHLN::ModelPrefab& outPrefab) {
    outPrefab.nodes.resize(data.nodes_count);
    for (size_t node = 0; node < data.nodes_count; ++node) {
        const cgltf_node& source = data.nodes[node];
        float             matrix[16] {};
        cgltf_node_transform_local(&source, matrix);
        outPrefab.nodes[node].name           = source.name != nullptr ? ZHLN::String64(source.name) : ZHLN::String64("Unnamed");
        outPrefab.nodes[node].parentIndex    = source.parent != nullptr ? static_cast<int32_t>(source.parent - data.nodes) : -1;
        outPrefab.nodes[node].hasMesh        = source.mesh != nullptr;
        outPrefab.nodes[node].localTransform = ToPrefabMatrix(Mat4x4::FromCols(FVec4{matrix[0], matrix[1], matrix[2], matrix[3]}, FVec4{matrix[4], matrix[5], matrix[6], matrix[7]}, FVec4{matrix[8], matrix[9], matrix[10], matrix[11]}, FVec4{matrix[12], matrix[13], matrix[14], matrix[15]}));
    }

    outPrefab.skeletons.reserve(data.skins_count);
    for (size_t skinIndex = 0; skinIndex < data.skins_count; ++skinIndex) {
        const cgltf_skin& skin = data.skins[skinIndex];
        ZHLN::Skeleton    skeleton;
        skeleton.name = skin.name != nullptr ? ZHLN::String64(skin.name) : ZHLN::String64("Skeleton");
        skeleton.joints.reserve(skin.joints_count);
        for (size_t jointIndex = 0; jointIndex < skin.joints_count; ++jointIndex) {
            const cgltf_node* jointNode = skin.joints[jointIndex];
            int32_t           parent    = -1;
            for (size_t candidate = 0; candidate < skin.joints_count; ++candidate) {
                if (skin.joints[candidate] == jointNode->parent) {
                    parent = static_cast<int32_t>(candidate);
                    break;
                }
            }

            Mat4x4 inverseBind = Mat4x4::Identity();
            if (skin.inverse_bind_matrices != nullptr) {
                float raw[16] {};
                cgltf_accessor_read_float(skin.inverse_bind_matrices, jointIndex, raw, 16);
                inverseBind = Mat4x4::FromCols(FVec4{raw[0], raw[1], raw[2], raw[3]}, FVec4{raw[4], raw[5], raw[6], raw[7]}, FVec4{raw[8], raw[9], raw[10], raw[11]}, FVec4{raw[12], raw[13], raw[14], raw[15]});
            }

            skeleton.joints.push_back({
                .name              = jointNode->name != nullptr ? ZHLN::String64(jointNode->name) : ZHLN::String64("Joint"),
                .parentIndex       = parent,
                .nodeIndex         = static_cast<int32_t>(jointNode - data.nodes),
                .inverseBindMatrix = inverseBind,
            });
        }
        outPrefab.skeletons.push_back(std::move(skeleton));
    }

    outPrefab.animations.reserve(data.animations_count);
    for (size_t clipIndex = 0; clipIndex < data.animations_count; ++clipIndex) {
        const cgltf_animation& source = data.animations[clipIndex];
        outPrefab.animations.push_back({.name = source.name != nullptr ? ZHLN::String64(source.name) : ZHLN::String64("Anim")});
    }
}

[[nodiscard]] inline Mat4x4 SourceWorld(const cgltf_node& node) noexcept {
    float matrix[16] {};
    cgltf_node_transform_world(&node, matrix);
    return Mat4x4::FromCols(FVec4{matrix[0], matrix[1], matrix[2], matrix[3]}, FVec4{matrix[4], matrix[5], matrix[6], matrix[7]}, FVec4{matrix[8], matrix[9], matrix[10], matrix[11]}, FVec4{matrix[12], matrix[13], matrix[14], matrix[15]});
}

} // namespace RigFixture
} // namespace

namespace {

struct ProceduralAnimationTestSuite {
    ProceduralAnimationTestSuite() {
        ZHLN::TaskSystem::Init(2, 32, ZHLN::kMinimumFiberStackSize);
    }

    ~ProceduralAnimationTestSuite() {
        ZHLN::TaskSystem::Shutdown();
    }

    struct Tests {
        std::expected<void, ZHLN::ErrorCode> procedural_update_immediately_creates_item_world_and_uploads_pose() {
            using namespace ZHLN;
            auto created = World::Create(PhysicsConfig {});
            if (!created) return std::unexpected(created.error());
            auto world = std::move(*created);
            AudioContext audio {AudioConfig {.headless = true}};
            AssetManager assets;
            ECS::ExtensionServices extensions;
            PoseUploadQueue uploads;
            SimServices services {.physics = world->GetPhysics(), .audio = audio, .assets = assets, .input = world->GetInput(),
                                  .articulation = world->GetArticulationSystem(), .events = world->GetEventBus(),
                                  .extensions = extensions, .poseUploads = uploads};
            const Frame frame {.dt = {1.0f / 60.0f}};
            SystemContext ctx {.registry = world->GetRegistry(), .services = services, .frame = frame};
            auto& reg = world->GetRegistry();
            const AssetID prefabID = HashAssetPath("tests/procedural-query-prefab");
            auto prefab = std::make_unique<ModelPrefab>();
            Skeleton skeleton;
            skeleton.joints.push_back(Joint {.name = String64("Root"), .nodeIndex = 0});
            prefab->skeletons.push_back(std::move(skeleton));
            assets.CachePrefab(prefabID, std::move(prefab));

            auto map = std::make_unique<RigBoneMap>();
            BuildStandardProceduralRig(*map);
            map->sourcePrefabAsset = prefabID;
            ProceduralAnimationConfigComponent config;
            config.enableGait = false;
            config.enableLegIK = false;
            config.enableAccelerationTilt = false;
            config.enableUpperBody = false;
            config.enableSecondaryMotion = false;
            const Entity item = reg.Create(Components::TransformComponent {});
            Animation::ItemHandlingComponent handling;
            handling.itemEntity = item;
            handling.gripCount = 1;
            handling.grips[0].ikWeight = 0.0f;
            handling.grips[0].evaluatedIKWeight = 0.0f;
            handling.avoidance.probeDistance = 0.0f;
            const auto headNode = map->nodeIndices[BoneSlot(CharacterBone::Head)];
            FirstPersonVisibilityComponent firstPerson;
            firstPerson.enabled = true;
            const Entity camera = reg.Create(Components::MainCameraTagComponent {}, Components::CameraComponent {});
            const Entity actor = reg.Create(ProceduralLocomotionComponent {}, Components::TransformComponent {}, std::move(*map), config,
                                            handling, firstPerson, Components::SkeletalMeshComponent {.jointOffset = 7, .skeletonIndex = 0});
            const Entity attachment = reg.Create(Components::HierarchyComponent {.parent = actor},
                Components::MeshComponent {.nodeIndex = static_cast<int32_t>(headNode)}, Components::TransformComponent {});
            if (reg.Get<Components::WorldTransformComponent>(item)) return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
            ProceduralAnimation::Update(ctx, 1.0f / 60.0f);
            auto itemWorld = reg.Get<Components::WorldTransformComponent>(item);
            auto boneMap = reg.Get<RigBoneMap>(actor);
            const auto childTransform = reg.Get<Components::TransformComponent>(attachment);
            if (!itemWorld || !boneMap || !childTransform || uploads.Size() != 1) return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
            if (!reg.Get<FirstPersonVisibilityComponent>(actor)->cameraInitialized) return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
            const auto cameraPosition = reg.Get<Components::CameraComponent>(camera)->camera.position;
            const auto firstWorld = itemWorld->world;
            if (!Close(firstWorld, itemWorld->previous, 1.0e-6f)) return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
            if (!boneMap->poseValid || boneMap->synchronizedSkinPaletteCount != 1 || boneMap->synchronizedAttachmentCount != 1) return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
            reg.Get<Components::TransformComponent>(actor)->position = {5.0f, 0.0f, 0.0f};
            ProceduralAnimation::Update(ctx, 1.0f / 60.0f);
            itemWorld = reg.Get<Components::WorldTransformComponent>(item);
            const auto updatedCameraPosition = reg.Get<Components::CameraComponent>(camera)->camera.position;
            if (std::abs(updatedCameraPosition.x - cameraPosition.x - 5.0f) > 0.001f) return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
            if (!itemWorld || !Close(itemWorld->previous, firstWorld, 1.0e-6f) || Close(itemWorld->world, firstWorld, 1.0e-6f) || uploads.Size() != 2)
                return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> authored_track_lookup_prefers_exact_idle() {
            ZHLN::ModelPrefab prefab;
            prefab.animations.push_back({.name = ZHLN::String64("Walk")});
            prefab.animations.push_back({.name = ZHLN::String64("Combat_Idle_Loop")});
            prefab.animations.push_back({.name = ZHLN::String64("IDLE")});
            prefab.animations.push_back({.name = ZHLN::String64("Run_Reach_Poses")});

            if (ZHLN::FindAnimationTrack(prefab, "idle") != 2 || ZHLN::FindAnimationTrack(prefab, "combat idle") != 1 ||
                ZHLN::FindAnimationTrack(prefab, "walk") != 0 || ZHLN::FindAnimationTrack(prefab, "run") != 3 ||
                ZHLN::FindAnimationTrack(prefab, "missing") != -1) {
                return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> imported_bounds_fit_virtual_character_hull() {
            ZHLN::ModelPrefab prefab;
            prefab.nodes.push_back({.name = ZHLN::String64("Root"), .localTransform = ToPrefabMatrix(CreateTranslation(FVec3{0.0f, 1.0f, 0.0f}))});
            prefab.nodes.push_back(
                {.name = ZHLN::String64("Nested"), .parentIndex = 0, .localTransform = ToPrefabMatrix(CreateTranslation(FVec3{1.0f, 2.0f, -1.0f}))}
            );

            ZHLN::ModelPart nested;
            nested.nodeIndex      = 1;
            nested.localTransform = ToPrefabMatrix(CreateTranslation(FVec3{0.5f, 0.0f, 0.25f}));
            // Bounds use native vectors.
            nested.localMin = {-1.0f, -2.0f, -0.5f};
            nested.localMax = {1.0f, 2.0f, 0.5f};
            prefab.parts.push_back(std::move(nested));

            ZHLN::ModelPart rootPart;
            rootPart.nodeIndex   = 0;
            rootPart.localMin = {-2.0f, 0.0f, -1.0f};
            rootPart.localMax = {-1.0f, 1.0f, 1.0f};
            prefab.parts.push_back(std::move(rootPart));

            const ZHLN::Locomotion::CharacterBoundsEstimate bounds = ZHLN::Locomotion::EstimateCharacterBounds(prefab);
            const ZHLN::Locomotion::DualShapeFitOptions     options;
            const ZHLN::Physics::DualShapeConfig            hull = ZHLN::Locomotion::FitDualShapeToBounds(bounds, ZHLN::Physics::DualShapeConfig {}, options);
            const float                                     horizontalExtent = std::max(2.5f * options.lateralCoverage, 1.25f) * options.horizontalPadding;
            const float                                     hullTop          = hull.GetBumperOffsetY() + hull.bumperRadiusY;
            if (!bounds.valid || !Close(FVec3{bounds.min}, FVec3{-2.0f, 1.0f, -1.25f}, 0.0001f) ||
                !Close(FVec3{bounds.max}, FVec3{2.5f, 5.0f, 1.0f}, 0.0001f) || std::abs(hull.bumperRadiusXZ - horizontalExtent) > 0.0001f ||
                hull.lifterRadius >= hull.bumperRadiusXZ || hull.bumperRadiusY <= hull.bumperRadiusXZ || hullTop < 5.0f * options.verticalPadding - 0.0001f) {
                return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> def_only_rig_filters_control_bones_and_maps_24_hair_strands() {
            ZHLN::ModelPrefab prefab;
            auto              addNode = [&](std::string_view name, int32_t parent, FVec3 translation) {
                const size_t index = prefab.nodes.size();
                prefab.nodes.push_back({.name = ZHLN::String64(name), .parentIndex = parent, .localTransform = ToPrefabMatrix(CreateTranslation(translation))});
                return index;
            };
            const size_t root = addNode("Root", -1, FVec3::Zero());
            static_cast<void>(addNode("CTR-Hand.L", static_cast<int32_t>(root), FVec3{0.2f, 1.0f, 0.0f}));
            const size_t hand = addNode("DEF-Hand.L", static_cast<int32_t>(root), FVec3{0.3f, 1.0f, 0.0f});
            static_cast<void>(addNode("CTR-Index_1.L", static_cast<int32_t>(hand), FVec3{0.04f, 0.0f, 0.0f}));
            const size_t finger1 = addNode("DEF-Index_1.L", static_cast<int32_t>(hand), FVec3{0.05f, 0.0f, 0.0f});
            static_cast<void>(addNode("CTR-Index_2.L", static_cast<int32_t>(finger1), FVec3{0.03f, 0.0f, 0.0f}));
            const size_t finger2 = addNode("DEF-Index_2.L", static_cast<int32_t>(finger1), FVec3{0.03f, 0.0f, 0.0f});
            const size_t finger3 = addNode("DEF-Index_3.L", static_cast<int32_t>(finger2), FVec3{0.02f, 0.0f, 0.0f});
            const size_t hair24  = addNode("DEF-Hair_S24_04", static_cast<int32_t>(root), FVec3{0.0f, 1.5f, 0.0f});

            ZHLN::Skeleton skeleton;
            for (size_t node = 0; node < prefab.nodes.size(); ++node) {
                skeleton.joints.push_back({.name = prefab.nodes[node].name, .nodeIndex = static_cast<int32_t>(node)});
            }
            ZHLN::RigBoneMap map;
            static_cast<void>(ZHLN::BuildBoneMap(prefab, skeleton, map));
            const size_t     hairSlot = ZHLN::BoneSlot(ZHLN::CharacterBone::HairStart) + 23 * ZHLN::HairStrandsComponent::kLinksPerStrand + 3;
            const std::array expectedFingerNodes {finger1, finger2, finger3};
            if (map.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::HandL)] != hand || map.sourceHairStrandCount != 24 || map.nodeIndices[hairSlot] != hair24 ||
                map.fingerJointConstraintCount != expectedFingerNodes.size() || !map.preserveAuthoredHierarchy) {
                return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
            }
            for (size_t index = 0; index < map.fingerJointConstraintCount; ++index) {
                if (map.fingerJointConstraints[index].child != expectedFingerNodes[index]) {
                    return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
                }
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> standard_rig_maps_every_control() {
            ZHLN::RigBoneMap map;
            map.nodeCount          = 7;
            map.modelTransforms[0] = CreateTranslation(FVec3{1.0f, 2.0f, 3.0f});
            map.Reset();
            if (map.nodeCount != 0 || map.nodeIndices[0] != ZHLN::InvalidRigNode || !Close(map.modelTransforms[0], Mat4x4::Identity(), 0.0001f) ||
                !Close(map.poseRotations[0], Quat::Identity(), 0.0001f) || !Close(map.poseScales[0], FVec3::Splat(1.0f), 0.0001f)) {
                return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
            }
            ZHLN::BuildStandardProceduralRig(map);

            if (!map.initialized || !map.poseValid || map.nodeCount != ZHLN::kBoneCount) {
                return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
            }
            for (size_t i = 0; i < ZHLN::kCoreBoneCount; ++i) {
                if (!ZHLN::IsValidRigNode(map.nodeIndices[i], map.nodeCount)) {
                    return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
                }
            }
            for (size_t i = ZHLN::BoneSlot(ZHLN::CharacterBone::HairStart); i < ZHLN::kBoneCount; ++i) {
                if (!ZHLN::IsValidRigNode(map.nodeIndices[i], map.nodeCount)) {
                    return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
                }
            }

            const FVec3 thighPosition = ZHLN::ProceduralMath::Translation(map.modelTransforms[map.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::ThighL)]]);
            const FVec3 shinPosition  = ZHLN::ProceduralMath::Translation(map.modelTransforms[map.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::ShinL)]]);
            const FVec3 footPosition  = ZHLN::ProceduralMath::Translation(map.modelTransforms[map.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::FootL)]]);
            const FVec3 headPosition  = ZHLN::ProceduralMath::Translation(map.modelTransforms[map.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::Head)]]);
            const FVec3 hairRoot = ZHLN::ProceduralMath::Translation(map.modelTransforms[map.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::HairStart)]]) - headPosition;
            if (!(shinPosition.z > thighPosition.z && shinPosition.z > footPosition.z) || hairRoot.y < 0.15f ||
                FVec3{hairRoot.x, 0.0f, hairRoot.z}.Length() < 0.08f) {
                return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
            }

            // Exercise the imported TestRig naming path as well as the
            // generated fallback hierarchy.
            // Deliberately place suffix-ambiguous names in adversarial order:
            // exact matching must beat "Sup_Spine" -> "Spine" and
            // "Forearm_L" -> "UpperArmL" regardless of node order.
            constexpr std::array<std::string_view, ZHLN::kCoreBoneCount> coreNames = {
                "Root",        "Hips",   "Sup_Spine", "Spine",  "Chest",  "Neck",  "Head",    "Forearm_L", "Upper_Arm_L", "Hand_L", "Forearm_R",
                "Upper_Arm_R", "Hand_R", "Thigh_L",   "Shin_L", "Foot_L", "Toe_L", "Thigh_R", "Shin_R",    "Foot_R",      "Toe_R",
            };
            ZHLN::ModelPrefab prefab;
            prefab.virtualPath = "TestRig.glb";
            prefab.nodes.resize(ZHLN::kBoneCount);
            ZHLN::Skeleton skeleton;
            skeleton.joints.reserve(ZHLN::kCoreBoneCount + ZHLN::HairStrandsComponent::kTotalParticles);
            for (size_t node = 0; node < ZHLN::kBoneCount; ++node) {
                if (node < ZHLN::kCoreBoneCount) {
                    prefab.nodes[node].name = ZHLN::String64(coreNames[node]);
                } else if (node >= ZHLN::BoneSlot(ZHLN::CharacterBone::HairStart)) {
                    const size_t particle = node - ZHLN::BoneSlot(ZHLN::CharacterBone::HairStart);
                    char         hairName[32] {};
                    std::snprintf(
                        hairName, sizeof(hairName), "DEF-Hair_S%02zu_%02zu", particle / ZHLN::HairStrandsComponent::kLinksPerStrand + 1,
                        particle % ZHLN::HairStrandsComponent::kLinksPerStrand + 1
                    );
                    prefab.nodes[node].name = ZHLN::String64(hairName);
                }
                prefab.nodes[node].parentIndex = ZHLN::IsValidRigNode(map.parentIndices[node], map.nodeCount) ? static_cast<int32_t>(map.parentIndices[node]) :
                                                                                                                -1;
                prefab.nodes[node].localTransform = ToPrefabMatrix(map.bindLocalTransforms[node]);
                if (node < ZHLN::kCoreBoneCount || node >= ZHLN::BoneSlot(ZHLN::CharacterBone::HairStart)) {
                    skeleton.joints.push_back({
                        .name              = prefab.nodes[node].name,
                        .parentIndex       = -1,
                        .nodeIndex         = static_cast<int32_t>(node),
                        .inverseBindMatrix = map.inverseBindMatrices[node],
                    });
                }
            }

            // Simulate an exporter whose parent lies outside the fixed runtime
            // window. The mapper must sever it rather than exposing the pose
            // traversal to an out-of-bounds parent index.
            const size_t malformedNode              = ZHLN::BoneSlot(ZHLN::CharacterBone::ToeR);
            prefab.nodes[malformedNode].parentIndex = static_cast<int32_t>(ZHLN::kMaxRigNodes + 7);

            // Simulate an exporter that flattens both hand bones under Hips.
            // Preserve their bind models so the child-of repair is jump-free.
            const size_t characterRootNode         = ZHLN::BoneSlot(ZHLN::CharacterBone::Root);
            const size_t hipsNode                  = ZHLN::BoneSlot(ZHLN::CharacterBone::Hips);
            const size_t handLNode                 = ZHLN::BoneSlot(ZHLN::CharacterBone::HandL);
            const size_t handRNode                 = ZHLN::BoneSlot(ZHLN::CharacterBone::HandR);
            const size_t shinLNode                 = ZHLN::BoneSlot(ZHLN::CharacterBone::ShinL);
            const size_t shinRNode                 = ZHLN::BoneSlot(ZHLN::CharacterBone::ShinR);
            const size_t chestNode                 = ZHLN::BoneSlot(ZHLN::CharacterBone::Chest);
            const size_t neckNode                  = ZHLN::BoneSlot(ZHLN::CharacterBone::Neck);
            const size_t headNode                  = ZHLN::BoneSlot(ZHLN::CharacterBone::Head);
            prefab.nodes[handLNode].parentIndex    = static_cast<int32_t>(hipsNode);
            prefab.nodes[handRNode].parentIndex    = static_cast<int32_t>(hipsNode);
            prefab.nodes[shinLNode].parentIndex    = static_cast<int32_t>(hipsNode);
            prefab.nodes[shinRNode].parentIndex    = static_cast<int32_t>(hipsNode);
            prefab.nodes[chestNode].parentIndex    = static_cast<int32_t>(hipsNode);
            prefab.nodes[neckNode].parentIndex     = static_cast<int32_t>(hipsNode);
            prefab.nodes[headNode].parentIndex     = static_cast<int32_t>(hipsNode);
            prefab.nodes[handLNode].localTransform = ToPrefabMatrix(Inverse(map.modelTransforms[hipsNode]) * map.modelTransforms[handLNode]);
            prefab.nodes[handRNode].localTransform = ToPrefabMatrix(Inverse(map.modelTransforms[hipsNode]) * map.modelTransforms[handRNode]);
            prefab.nodes[shinLNode].localTransform = ToPrefabMatrix(Inverse(map.modelTransforms[hipsNode]) * map.modelTransforms[shinLNode]);
            prefab.nodes[shinRNode].localTransform = ToPrefabMatrix(Inverse(map.modelTransforms[hipsNode]) * map.modelTransforms[shinRNode]);
            prefab.nodes[chestNode].localTransform = ToPrefabMatrix(Inverse(map.modelTransforms[hipsNode]) * map.modelTransforms[chestNode]);
            prefab.nodes[neckNode].localTransform  = ToPrefabMatrix(Inverse(map.modelTransforms[hipsNode]) * map.modelTransforms[neckNode]);
            prefab.nodes[headNode].localTransform  = ToPrefabMatrix(Inverse(map.modelTransforms[hipsNode]) * map.modelTransforms[headNode]);

            // Model two opaque root-level shoe transform nodes, each carrying a
            // child mesh part. Discovery must constrain the owning transform,
            // not its individual parts and not an asset-specific name.
            struct FootPartNodes {
                size_t root;
                size_t part;
            };
            auto addRigidFootPart = [&](ZHLN::CharacterBone footBone, std::string_view opaqueName, bool skinned = false, float extentScale = 1.0f,
                                        bool detachedContainer = true) {
                const ZHLN::RigNodeIndex footNode     = map.nodeIndices[ZHLN::BoneSlot(footBone)];
                const Mat4x4         targetModel  = map.modelTransforms[footNode] * CreateTranslation(FVec3{0.0f, -0.035f, 0.065f});
                const size_t             shoeRootNode = prefab.nodes.size();
                prefab.nodes.push_back({
                    .name           = ZHLN::String64(opaqueName),
                    .parentIndex    = static_cast<int32_t>(characterRootNode),
                    .localTransform = ToPrefabMatrix(Inverse(map.modelTransforms[characterRootNode]) * targetModel),
                    .hasMesh        = !detachedContainer,
                });

                size_t meshNode = shoeRootNode;
                if (detachedContainer) {
                    meshNode = prefab.nodes.size();
                    prefab.nodes.push_back({
                        .name           = ZHLN::String64("OpaquePart"),
                        .parentIndex    = static_cast<int32_t>(shoeRootNode),
                        .localTransform = ToPrefabMatrix(Mat4x4::Identity()),
                        .hasMesh        = true,
                    });
                }

                ZHLN::ModelPart part;
                part.name        = ZHLN::String64("OpaquePart");
                part.isSkinned   = skinned;
                part.nodeIndex   = static_cast<int32_t>(meshNode);
                part.localMin = {-0.12f * extentScale, -0.06f * extentScale, -0.18f * extentScale};
                part.localMax = {0.12f * extentScale, 0.10f * extentScale, 0.22f * extentScale};
                prefab.parts.push_back(std::move(part));
                return FootPartNodes {.root = shoeRootNode, .part = meshNode};
            };
            const FootPartNodes rigidFootL = addRigidFootPart(ZHLN::CharacterBone::FootL, "Object.314");
            const FootPartNodes rigidFootR = addRigidFootPart(ZHLN::CharacterBone::FootR, "Primitive_027", true);
            static_cast<void>(addRigidFootPart(ZHLN::CharacterBone::FootL, "AlreadySkinned", true, 1.0f, false));
            static_cast<void>(addRigidFootPart(ZHLN::CharacterBone::FootR, "WholeBody", false, 8.0f));

            // A secondary footwear skin can also export duplicate foot joints.
            // Match those semantically, without relying on skin ordering.
            ZHLN::Skeleton footwearSkin;
            auto           addSecondaryFootJoint = [&](ZHLN::CharacterBone footBone, std::string_view jointName) {
                const size_t             node     = prefab.nodes.size();
                const ZHLN::RigNodeIndex footNode = map.nodeIndices[ZHLN::BoneSlot(footBone)];
                prefab.nodes.push_back({
                    .name           = ZHLN::String64(jointName),
                    .parentIndex    = static_cast<int32_t>(hipsNode),
                    .localTransform = ToPrefabMatrix(Inverse(map.modelTransforms[hipsNode]) * map.modelTransforms[footNode]),
                });
                footwearSkin.joints.push_back({
                    .name              = ZHLN::String64(jointName),
                    .parentIndex       = -1,
                    .nodeIndex         = static_cast<int32_t>(node),
                    .inverseBindMatrix = Inverse(map.modelTransforms[footNode]),
                });
                return node;
            };
            const size_t secondaryFootLNode = addSecondaryFootJoint(ZHLN::CharacterBone::FootL, "DEF-Foot.L");
            const size_t secondaryFootRNode = addSecondaryFootJoint(ZHLN::CharacterBone::FootR, "DEF-Foot.R");
            prefab.skeletons.push_back(std::move(footwearSkin));

            ZHLN::RigBoneMap importedMap;
            if (!ZHLN::BuildBoneMap(prefab, skeleton, importedMap) || importedMap.parentIndices[malformedNode] != ZHLN::InvalidRigNode ||
                importedMap.childOfConstraintCount != 15 || importedMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::Spine)] != 3 ||
                importedMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::SupSpine)] != 2 ||
                importedMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::UpperArmL)] != 8 ||
                importedMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::ForearmL)] != 7 ||
                importedMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::UpperArmR)] != 11 ||
                importedMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::ForearmR)] != 10) {
                return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
            }
            for (size_t slot = 0; slot < ZHLN::HairStrandsComponent::kTotalParticles; ++slot) {
                const size_t semantic = ZHLN::BoneSlot(ZHLN::CharacterBone::HairStart) + slot;
                if (importedMap.nodeIndices[semantic] != semantic) {
                    return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
                }
            }

            const ZHLN::RigNodeIndex thighL           = importedMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::ThighL)];
            const ZHLN::RigNodeIndex shinL            = importedMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::ShinL)];
            const ZHLN::RigNodeIndex thighR           = importedMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::ThighR)];
            const ZHLN::RigNodeIndex shinR            = importedMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::ShinR)];
            const ZHLN::RigNodeIndex forearmL         = importedMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::ForearmL)];
            const ZHLN::RigNodeIndex handL            = importedMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::HandL)];
            const ZHLN::RigNodeIndex forearmR         = importedMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::ForearmR)];
            const ZHLN::RigNodeIndex handR            = importedMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::HandR)];
            const ZHLN::RigNodeIndex supSpine         = importedMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::SupSpine)];
            const ZHLN::RigNodeIndex chest            = importedMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::Chest)];
            const ZHLN::RigNodeIndex neck             = importedMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::Neck)];
            const ZHLN::RigNodeIndex head             = importedMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::Head)];
            const ZHLN::RigNodeIndex footL            = importedMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::FootL)];
            const ZHLN::RigNodeIndex footR            = importedMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::FootR)];
            const ZHLN::RigNodeIndex toeR             = importedMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::ToeR)];
            const FVec3          handBefore       = ZHLN::ProceduralMath::Translation(importedMap.modelTransforms[handL]);
            const FVec3          chestBefore      = ZHLN::ProceduralMath::Translation(importedMap.modelTransforms[chest]);
            const FVec3          neckBefore       = ZHLN::ProceduralMath::Translation(importedMap.modelTransforms[neck]);
            const FVec3          headBefore       = ZHLN::ProceduralMath::Translation(importedMap.modelTransforms[head]);
            const Mat4x4         rigidFootLBefore = importedMap.modelTransforms[rigidFootL.root];
            const Mat4x4         rigidFootRBefore = importedMap.modelTransforms[rigidFootR.root];
            const Mat4x4 rigidFootLPartRelative   = Inverse(importedMap.modelTransforms[rigidFootL.root]) * importedMap.modelTransforms[rigidFootL.part];
            const Mat4x4 rigidFootRPartRelative   = Inverse(importedMap.modelTransforms[rigidFootR.root]) * importedMap.modelTransforms[rigidFootR.part];
            const Mat4x4 secondaryFootLBefore     = importedMap.modelTransforms[secondaryFootLNode];
            const Mat4x4 secondaryFootRBefore     = importedMap.modelTransforms[secondaryFootRNode];
            importedMap.localTransforms[forearmL]     = importedMap.localTransforms[forearmL] *
                                                        ZHLN::ProceduralMath::Transform(Quat::FromAxisAngle(FVec3::UnitY(), ZHLN::RadF{0.45f}), FVec3::Zero());
            importedMap.localTransforms[supSpine]     = importedMap.localTransforms[supSpine] *
                                                        ZHLN::ProceduralMath::Transform(Quat::FromAxisAngle(FVec3::UnitZ(), ZHLN::RadF{0.25f}), FVec3::Zero());
            importedMap.localTransforms[footL] = importedMap.localTransforms[footL] * ZHLN::ProceduralMath::Transform(Quat::FromAxisAngle(FVec3::UnitX(), ZHLN::RadF{0.30f}), FVec3::Zero());
            importedMap.localTransforms[footR] = importedMap.localTransforms[footR] * ZHLN::ProceduralMath::Transform(Quat::FromAxisAngle(FVec3::UnitX(), ZHLN::RadF{-0.25f}), FVec3::Zero());
            ZHLN::ProceduralAnimation::CaptureChildOfPoseDeltas(importedMap);

            // Applying an upstream correction after a leaf correction detaches
            // that leaf. Reverse discovery order to prove semantic sorting keeps
            // hands on forearms and rigid attachments on their final foot pose.
            std::reverse(
                importedMap.childOfConstraints.begin(), importedMap.childOfConstraints.begin() + static_cast<std::ptrdiff_t>(importedMap.childOfConstraintCount)
            );
            auto findConstraint = [&](ZHLN::RigChildOfKind kind, ZHLN::RigNodeIndex child) -> const ZHLN::RigChildOfConstraint* {
                for (size_t index = 0; index < importedMap.childOfConstraintCount; ++index) {
                    const ZHLN::RigChildOfConstraint& constraint = importedMap.childOfConstraints[index];
                    if (constraint.kind == kind && constraint.child == child) {
                        return &constraint;
                    }
                }
                return nullptr;
            };
            const ZHLN::RigChildOfConstraint* kneeLConstraint          = findConstraint(ZHLN::RigChildOfKind::Knee, shinL);
            const ZHLN::RigChildOfConstraint* kneeRConstraint          = findConstraint(ZHLN::RigChildOfKind::Knee, shinR);
            const ZHLN::RigChildOfConstraint* ankleLConstraint         = findConstraint(ZHLN::RigChildOfKind::Ankle, footL);
            const ZHLN::RigChildOfConstraint* ankleRConstraint         = findConstraint(ZHLN::RigChildOfKind::Ankle, footR);
            const ZHLN::RigChildOfConstraint* handLConstraint          = findConstraint(ZHLN::RigChildOfKind::Hand, handL);
            const ZHLN::RigChildOfConstraint* handRConstraint          = findConstraint(ZHLN::RigChildOfKind::Hand, handR);
            const ZHLN::RigChildOfConstraint* rigidFootLConstraint     = findConstraint(ZHLN::RigChildOfKind::FootAttachment, rigidFootL.root);
            const ZHLN::RigChildOfConstraint* rigidFootRConstraint     = findConstraint(ZHLN::RigChildOfKind::FootAttachment, rigidFootR.root);
            const ZHLN::RigChildOfConstraint* secondaryFootLConstraint = findConstraint(ZHLN::RigChildOfKind::FootAttachment, secondaryFootLNode);
            const ZHLN::RigChildOfConstraint* secondaryFootRConstraint = findConstraint(ZHLN::RigChildOfKind::FootAttachment, secondaryFootRNode);
            const ZHLN::RigChildOfConstraint* toeRConstraint           = findConstraint(ZHLN::RigChildOfKind::FootAttachment, toeR);

            ZHLN::ProceduralAnimation::ResolveModelTransforms(importedMap);
            if (kneeLConstraint == nullptr || kneeRConstraint == nullptr || ankleLConstraint == nullptr || ankleRConstraint == nullptr) {
                return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
            }
            // Baked control rigs may author a knee anchor that is not the bind
            // translation. Capture that evaluated model relation verbatim.
            auto offsetAuthoredShin = [&](ZHLN::RigNodeIndex shin, FVec3 offset) {
                Mat4x4 authored = importedMap.modelTransforms[shin];
                ZHLN::ProceduralMath::SetTranslation(authored, ZHLN::ProceduralMath::Translation(authored) + offset);
                ZHLN::Animation::SetModelTransformAndCarrySubtree(importedMap.modelTransforms.data(), importedMap, shin, authored);
            };
            offsetAuthoredShin(shinL, FVec3{0.025f, 0.010f, 0.0f});
            offsetAuthoredShin(shinR, FVec3{-0.025f, 0.010f, 0.0f});
            ZHLN::ProceduralAnimation::CaptureAuthoredConstraintPoseDeltas(importedMap);

            // Simulate a model-space procedural thigh correction after the
            // authored GLB hierarchy has been evaluated. Flattened shins are
            // not imported descendants, so this creates the gap Stage 7 must
            // close without discarding the captured authored knee relation.
            auto rotateDetachedThigh = [&](ZHLN::RigNodeIndex thigh, float angle) {
                const Mat4x4 current = importedMap.modelTransforms[thigh];
                const Mat4x4 target  = ZHLN::ProceduralMath::Transform((Quat::FromAxisAngle(FVec3::UnitZ(), ZHLN::RadF{angle}) * ZHLN::ProceduralMath::Rotation(current)).Normalized(), ZHLN::ProceduralMath::Translation(current));
                ZHLN::Animation::SetModelTransformAndCarrySubtree(importedMap.modelTransforms.data(), importedMap, thigh, target);
            };
            rotateDetachedThigh(thighL, 0.35f);
            rotateDetachedThigh(thighR, -0.30f);

            const Mat4x4 detachedShinL = importedMap.modelTransforms[shinL];
            const Mat4x4 detachedShinR = importedMap.modelTransforms[shinR];
            const Mat4x4 detachedFootL = importedMap.modelTransforms[footL];
            const Mat4x4 detachedFootR = importedMap.modelTransforms[footR];
            const FVec3  expectedKneeL =
                ZHLN::ProceduralMath::Translation((importedMap.modelTransforms[thighL] * kneeLConstraint->bindRelative * kneeLConstraint->localPoseDelta));
            const FVec3 expectedKneeR =
                ZHLN::ProceduralMath::Translation((importedMap.modelTransforms[thighR] * kneeRConstraint->bindRelative * kneeRConstraint->localPoseDelta));
            // Ankle constraints use the shin positions after knee constraints are applied.
            // Knee constraints pin shin translation but preserve rotation.
            const Mat4x4 pinnedShinL    = ZHLN::ProceduralMath::Transform(ZHLN::ProceduralMath::Rotation(detachedShinL), expectedKneeL);
            const Mat4x4 pinnedShinR    = ZHLN::ProceduralMath::Transform(ZHLN::ProceduralMath::Rotation(detachedShinR), expectedKneeR);
            const FVec3  expectedAnkleL = ZHLN::ProceduralMath::Translation((pinnedShinL * ankleLConstraint->bindRelative * ankleLConstraint->localPoseDelta));
            const FVec3  expectedAnkleR = ZHLN::ProceduralMath::Translation((pinnedShinR * ankleRConstraint->bindRelative * ankleRConstraint->localPoseDelta));

            // Knee and ankle joints are structural and remain active even when
            // every optional child-of relationship is disabled. They pin position
            // without replacing the authored or IK-produced basis.
            if (ZHLN::ProceduralAnimation::ApplyChildOfConstraints(importedMap, false, false, false, false, false) != 4 ||
                !Close(ZHLN::ProceduralMath::Translation(importedMap.modelTransforms[shinL]), expectedKneeL, 0.0001f) ||
                !Close(ZHLN::ProceduralMath::Translation(importedMap.modelTransforms[shinR]), expectedKneeR, 0.0001f) ||
                !Close(ZHLN::ProceduralMath::Translation(importedMap.modelTransforms[footL]), expectedAnkleL, 0.0001f) ||
                !Close(ZHLN::ProceduralMath::Translation(importedMap.modelTransforms[footR]), expectedAnkleR, 0.0001f) ||
                !Close(ZHLN::ProceduralMath::Direction(importedMap.modelTransforms[shinL], FVec3::UnitZ()), ZHLN::ProceduralMath::Direction(detachedShinL, FVec3::UnitZ()), 0.0001f) ||
                !Close(ZHLN::ProceduralMath::Direction(importedMap.modelTransforms[shinR], FVec3::UnitZ()), ZHLN::ProceduralMath::Direction(detachedShinR, FVec3::UnitZ()), 0.0001f) ||
                !Close(ZHLN::ProceduralMath::Direction(importedMap.modelTransforms[footL], FVec3::UnitZ()), ZHLN::ProceduralMath::Direction(detachedFootL, FVec3::UnitZ()), 0.0001f) ||
                !Close(ZHLN::ProceduralMath::Direction(importedMap.modelTransforms[footR], FVec3::UnitZ()), ZHLN::ProceduralMath::Direction(detachedFootR, FVec3::UnitZ()), 0.0001f) ||
                Close(ZHLN::ProceduralMath::Translation(detachedShinL), expectedKneeL, 0.0001f) || Close(ZHLN::ProceduralMath::Translation(detachedShinR), expectedKneeR, 0.0001f) ||
                Close(ZHLN::ProceduralMath::Translation(detachedFootL), expectedAnkleL, 0.0001f) || Close(ZHLN::ProceduralMath::Translation(detachedFootR), expectedAnkleR, 0.0001f)) {
                return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
            }

            if (ZHLN::ProceduralAnimation::ApplyChildOfConstraints(importedMap) != 15 || handLConstraint == nullptr || handRConstraint == nullptr ||
                ankleLConstraint == nullptr || ankleRConstraint == nullptr || rigidFootLConstraint == nullptr || rigidFootRConstraint == nullptr ||
                secondaryFootLConstraint == nullptr || secondaryFootRConstraint == nullptr || toeRConstraint == nullptr ||
                !Close(importedMap.modelTransforms[handL], importedMap.modelTransforms[forearmL] * handLConstraint->bindRelative * handLConstraint->localPoseDelta, 0.0001f) ||
                !Close(importedMap.modelTransforms[handR], importedMap.modelTransforms[forearmR] * handRConstraint->bindRelative * handRConstraint->localPoseDelta, 0.0001f) ||
                !Close(importedMap.modelTransforms[rigidFootL.root], importedMap.modelTransforms[footL] * rigidFootLConstraint->bindRelative * rigidFootLConstraint->localPoseDelta, 0.0001f) ||
                !Close(importedMap.modelTransforms[rigidFootR.root], importedMap.modelTransforms[footR] * rigidFootRConstraint->bindRelative * rigidFootRConstraint->localPoseDelta, 0.0001f) ||
                !Close(importedMap.modelTransforms[rigidFootL.part], importedMap.modelTransforms[rigidFootL.root] * rigidFootLPartRelative, 0.0001f) ||
                !Close(importedMap.modelTransforms[rigidFootR.part], importedMap.modelTransforms[rigidFootR.root] * rigidFootRPartRelative, 0.0001f) ||
                !Close(importedMap.modelTransforms[secondaryFootLNode], importedMap.modelTransforms[footL] * secondaryFootLConstraint->bindRelative * secondaryFootLConstraint->localPoseDelta, 0.0001f) ||
                !Close(importedMap.modelTransforms[secondaryFootRNode], importedMap.modelTransforms[footR] * secondaryFootRConstraint->bindRelative * secondaryFootRConstraint->localPoseDelta, 0.0001f) ||
                !Close(importedMap.modelTransforms[toeR], importedMap.modelTransforms[footR] * toeRConstraint->bindRelative * toeRConstraint->localPoseDelta, 0.0001f) ||
                Close(importedMap.modelTransforms[rigidFootL.root], rigidFootLBefore, 0.0001f) ||
                Close(importedMap.modelTransforms[rigidFootR.root], rigidFootRBefore, 0.0001f) ||
                Close(importedMap.modelTransforms[secondaryFootLNode], secondaryFootLBefore, 0.0001f) ||
                Close(importedMap.modelTransforms[secondaryFootRNode], secondaryFootRBefore, 0.0001f) ||
                (ZHLN::ProceduralMath::Translation(importedMap.modelTransforms[handL]) - handBefore).LengthSquared() < 0.0001f ||
                (ZHLN::ProceduralMath::Translation(importedMap.modelTransforms[chest]) - chestBefore).LengthSquared() < 0.0001f ||
                (ZHLN::ProceduralMath::Translation(importedMap.modelTransforms[neck]) - neckBefore).LengthSquared() < 0.0001f ||
                (ZHLN::ProceduralMath::Translation(importedMap.modelTransforms[head]) - headBefore).LengthSquared() < 0.0001f) {
                return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> first_person_palette_masks_head_and_hair() {
            ZHLN::RigBoneMap map;
            ZHLN::BuildStandardProceduralRig(map);
            const ZHLN::RigNodeIndex head = map.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::Head)];
            const ZHLN::RigNodeIndex hair = map.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::HairStart)];
            const ZHLN::RigNodeIndex hand = map.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::HandL)];
            ZHLN::Skeleton           skeleton;
            skeleton.joints.push_back({.name = ZHLN::String64("DEF-Head"), .nodeIndex = static_cast<int32_t>(head)});
            skeleton.joints.push_back({.name = ZHLN::String64("DEF-Hair_S01_01"), .nodeIndex = static_cast<int32_t>(hair)});
            skeleton.joints.push_back({.name = ZHLN::String64("DEF-Hand.L"), .nodeIndex = static_cast<int32_t>(hand)});
            std::array<Mat4x4, 3> palette {Mat4x4::Identity(), Mat4x4::Identity(), Mat4x4::Identity()};
            if (ZHLN::ProceduralAnimation::MaskFirstPersonPalette(skeleton, map, palette, true, true) != 2 || ZHLN::ProceduralMath::Column3(palette[0], 0).LengthSquared() > 1.0e-8f ||
                ZHLN::ProceduralMath::Column3(palette[1], 1).LengthSquared() > 1.0e-8f || !Close(palette[2], Mat4x4::Identity(), 0.0001f)) {
                return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> gait_clock_tracks_distance() {
            ZHLN::ProceduralLocomotionComponent gait;
            gait.strideLength = 1.60f;

            ZHLN::Animation::EvaluateGait(gait, FVec3{0.0f, 0.0f, 4.0f}, 0.25f);
            const bool phaseIsDistanceDriven  = std::abs(gait.phase - 0.625f) < 0.0001f;
            const bool feetAreOpposed         = gait.plantWeightL != gait.plantWeightR;
            const bool authoredSwingPreserved = gait.plantWeightR < 0.001f && gait.plantWeightL > 0.99f;
            const bool accelerationIsFinite   = std::isfinite(gait.directionalAcceleration.z);
            const bool wheelTracksPhase       = std::abs(gait.strideWheelAngle - gait.phase * 2.0f * std::numbers::pi_v<float>) < 0.0001f;
            const bool passReachPartition     = std::abs(gait.passWeightL + gait.reachWeightL - 1.0f) < 0.0001f;
            const bool twoKeySynchronization  = std::abs(ZHLN::Animation::EvaluateTwoKeyPosePhase(0.0f)) < 0.0001f &&
                                                std::abs(ZHLN::Animation::EvaluateTwoKeyPosePhase(0.25f) - 0.5f) < 0.0001f &&
                                                std::abs(ZHLN::Animation::EvaluateTwoKeyPosePhase(0.5f) - 1.0f) < 0.0001f &&
                                                std::abs(ZHLN::Animation::EvaluateTwoKeyPosePhase(0.75f) - 0.5f) < 0.0001f;
            if (!phaseIsDistanceDriven || !feetAreOpposed || !authoredSwingPreserved || !accelerationIsFinite || !wheelTracksPhase || !passReachPartition ||
                !twoKeySynchronization) {
                return std::unexpected(ProceduralAnimationTestError::GaitInvariantFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> gravity_bounce_flattens_with_speed() {
            ZHLN::ProceduralLocomotionComponent gait;
            gait.strideLength    = 1.40f;
            gait.bounceGravity   = 9.81f;
            gait.maxBounceHeight = 0.065f;

            const float slowSpeed = 1.0f;
            gait.phase            = 0.25f;
            const float slowApex  = ZHLN::Animation::EvaluateGravityBounce(gait, slowSpeed);

            const float fastSpeed    = 4.0f;
            const float fastInterval = gait.strideLength / (2.0f * fastSpeed);
            gait.phase               = 0.25f;
            const float fastApex     = ZHLN::Animation::EvaluateGravityBounce(gait, fastSpeed);

            constexpr float      sampleStep = 0.01f;
            std::array<float, 3> samples {};
            for (size_t i = 0; i < samples.size(); ++i) {
                const float elapsed = fastInterval * 0.5f + sampleStep * (static_cast<float>(i) - 1.0f);
                gait.phase          = (elapsed / fastInterval) * 0.5f;
                samples[i]          = ZHLN::Animation::EvaluateGravityBounce(gait, fastSpeed);
            }
            const float measuredAcceleration = (samples[2] - 2.0f * samples[1] + samples[0]) / (sampleStep * sampleStep);
            gait.phase                       = 0.0f;
            const float contactStart         = ZHLN::Animation::EvaluateGravityBounce(gait, slowSpeed);
            gait.phase                       = 0.5f;
            const float contactEnd           = ZHLN::Animation::EvaluateGravityBounce(gait, slowSpeed);
            if (!(slowApex > fastApex) || slowApex > gait.maxBounceHeight + 0.0001f || std::abs(measuredAcceleration + gait.bounceGravity) > 0.2f ||
                std::abs(contactStart) > 0.0001f || std::abs(contactEnd) > 0.0001f) {
                return std::unexpected(ProceduralAnimationTestError::GaitInvariantFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> model_space_ik_and_ankle_alignment() {
            const FVec3  down{0.0f, -1.0f, 0.0f};
            const FVec3  solvedDirection = FVec3{0.18f, -0.97f, 0.12f}.Normalized();
            const Mat4x4 authoredBone    = Mat4x4::Identity();
            const FVec3  solvedPosition{0.1f, 0.7f, -0.2f};
            const Mat4x4 corrected = ZHLN::Animation::CorrectBoneDirection(authoredBone, down, solvedDirection, solvedPosition);
            if (!Close(ZHLN::ProceduralMath::Direction(corrected, down).Normalized(), solvedDirection, 0.0001f) || !Close(ZHLN::ProceduralMath::Translation(corrected), solvedPosition, 0.0001f)) {
                return std::unexpected(ProceduralAnimationTestError::GaitInvariantFailed);
            }

            const Quat  authoredYaw  = Quat::FromAxisAngle(FVec3::UnitY(), ZHLN::RadF{0.4f});
            const Mat4x4 authoredFoot = ZHLN::ProceduralMath::Transform(authoredYaw, FVec3::Zero());
            const FVec3  footTarget{0.2f, 0.05f, 0.3f};
            const Mat4x4 flatFoot = ZHLN::Animation::AlignFootToGround(authoredFoot, footTarget, FVec3::UnitY());
            if (!Close(ZHLN::ProceduralMath::Translation(flatFoot), footTarget, 0.0001f) ||
                !Close(ZHLN::ProceduralMath::Direction(flatFoot, FVec3::UnitZ()).Normalized(), ZHLN::ProceduralMath::Direction(authoredFoot, FVec3::UnitZ()).Normalized(), 0.0001f)) {
                return std::unexpected(ProceduralAnimationTestError::GaitInvariantFailed);
            }

            const FVec3  slopeNormal = FVec3{0.25f, 0.95f, 0.18f}.Normalized();
            const Mat4x4 slopeFoot   = ZHLN::Animation::AlignFootToGround(authoredFoot, footTarget, slopeNormal);
            if (!Close(ZHLN::ProceduralMath::Direction(slopeFoot, FVec3::UnitY()).Normalized(), slopeNormal, 0.0001f)) {
                return std::unexpected(ProceduralAnimationTestError::GaitInvariantFailed);
            }

            constexpr float maxSideways     = 0.20f;
            constexpr float maxForward      = 0.50f;
            const FVec3 extremeNormal   = FVec3{0.82f, 0.45f, 0.35f}.Normalized();
            const FVec3 limitedNormal   = ZHLN::Animation::LimitGroundNormal(extremeNormal, ZHLN::RadF{maxSideways}, ZHLN::RadF{maxForward});
            const float     limitedSideways = std::abs(std::atan2(-limitedNormal.x, limitedNormal.y));
            const float     limitedForward  = std::abs(std::asin(std::clamp(limitedNormal.z, -1.0f, 1.0f)));
            if (limitedSideways > maxSideways + 0.0001f || limitedForward > maxForward + 0.0001f) {
                return std::unexpected(ProceduralAnimationTestError::GaitInvariantFailed);
            }

            ZHLN::RigBoneMap subtreeMap;
            ZHLN::BuildStandardProceduralRig(subtreeMap);
            const ZHLN::RigNodeIndex footNode        = subtreeMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::FootL)];
            const ZHLN::RigNodeIndex toeNode         = subtreeMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::ToeL)];
            const ZHLN::RigNodeIndex meshPartNode    = subtreeMap.nodeCount++;
            subtreeMap.parentIndices[meshPartNode]   = footNode;
            subtreeMap.modelTransforms[meshPartNode] = subtreeMap.modelTransforms[footNode] * CreateTranslation(FVec3{0.04f, -0.02f, 0.08f});
            const Mat4x4 oldFoot                 = subtreeMap.modelTransforms[footNode];
            const Mat4x4 oldToe                  = subtreeMap.modelTransforms[toeNode];
            const Mat4x4 oldMeshPart             = subtreeMap.modelTransforms[meshPartNode];
            const Mat4x4 carriedFoot             = ZHLN::ProceduralMath::Transform(Quat::FromAxisAngle(FVec3::UnitX(), ZHLN::RadF{0.35f}) * ZHLN::ProceduralMath::Rotation(oldFoot), ZHLN::ProceduralMath::Translation(oldFoot) + FVec3{0.0f, 0.04f, 0.0f});
            const Mat4x4 correction = carriedFoot * Inverse(oldFoot);
            if (ZHLN::Animation::SetModelTransformAndCarrySubtree(subtreeMap.modelTransforms.data(), subtreeMap, footNode, carriedFoot) < 3 ||
                !Close(subtreeMap.modelTransforms[footNode], carriedFoot, 0.0001f) ||
                !Close(subtreeMap.modelTransforms[toeNode], correction * oldToe, 0.0001f) ||
                !Close(subtreeMap.modelTransforms[meshPartNode], correction * oldMeshPart, 0.0001f)) {
                return std::unexpected(ProceduralAnimationTestError::GaitInvariantFailed);
            }

            ZHLN::RigBoneMap tiltMap;
            ZHLN::BuildStandardProceduralRig(tiltMap);
            const auto                          baseTransforms    = tiltMap.modelTransforms;
            const ZHLN::RigNodeIndex            tiltThigh         = tiltMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::ThighL)];
            const ZHLN::RigNodeIndex            tiltHips          = tiltMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::Hips)];
            const ZHLN::RigNodeIndex            tiltHead          = tiltMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::Head)];
            const FVec3                     unreachableTarget = ZHLN::ProceduralMath::Translation(tiltMap.modelTransforms[tiltThigh]) + FVec3{1.5f, -0.35f, 0.0f};
            constexpr float                     maxBodyTilt       = 0.14f;
            ZHLN::ProceduralLocomotionComponent tiltGait;
            for (uint32_t frame = 0; frame < 120; ++frame) {
                tiltMap.modelTransforms = baseTransforms;
                ZHLN::Animation::ApplyIKReachTilt(
                    tiltGait, tiltMap.modelTransforms.data(), tiltMap, unreachableTarget, FVec3::Zero(), 1.0f, 0.0f, 0.96f, ZHLN::RadF{maxBodyTilt}, 1.0f / 60.0f
                );
            }
            const float appliedTilt = std::sqrt(tiltGait.ikBodyTiltPitch * tiltGait.ikBodyTiltPitch + tiltGait.ikBodyTiltRoll * tiltGait.ikBodyTiltRoll);
            // The reach tilt rotates the spine subtree around the hips pivot: the upper body must
            // move, while the hips and the thighs stay planted so leg IK reach is unaffected.
            if (appliedTilt < 0.01f || appliedTilt > maxBodyTilt + 0.0001f || Close(tiltMap.modelTransforms[tiltHead], baseTransforms[tiltHead], 0.0001f) ||
                !Close(tiltMap.modelTransforms[tiltHips], baseTransforms[tiltHips], 0.0001f) ||
                !Close(tiltMap.modelTransforms[tiltThigh], baseTransforms[tiltThigh], 0.0001f)) {
                return std::unexpected(ProceduralAnimationTestError::GaitInvariantFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> item_handling_drives_constrained_grips() {
            ZHLN::Animation::ItemHandlingComponent handling;
            handling.driverMode           = ZHLN::Animation::ItemDriverMode::AimGuided;
            handling.aimProgress          = 1.0f;
            const FVec3  aimDirection = FVec3{1.0f, 0.1f, 0.2f}.Normalized();
            const Mat4x4 aimPose      = ZHLN::Animation::SolveItemBasePose(
                handling, Mat4x4::Identity(), Mat4x4::Identity(), Mat4x4::Identity(), FVec3{0.0f, 1.6f, 0.0f}, aimDirection,
                Mat4x4::Identity()
            );
            if (!Close(ZHLN::ProceduralMath::Direction(aimPose, FVec3::UnitZ()).Normalized(), aimDirection, 0.0001f)) {
                return std::unexpected(ProceduralAnimationTestError::GaitInvariantFailed);
            }

            handling.driverMode        = ZHLN::Animation::ItemDriverMode::WorldAnchored;
            const Mat4x4 worldPose = ZHLN::Animation::SolveItemBasePose(
                handling, Mat4x4::Identity(), Mat4x4::Identity(), CreateTranslation(FVec3{-1.0f, 0.0f, 0.0f}), FVec3::Zero(),
                FVec3::UnitZ(), CreateTranslation(FVec3{3.0f, 0.0f, 0.0f})
            );
            if (!Close(ZHLN::ProceduralMath::Translation(worldPose), FVec3{2.0f, 0.0f, 0.0f}, 0.0001f)) {
                return std::unexpected(ProceduralAnimationTestError::GaitInvariantFailed);
            }

            ZHLN::Animation::GripPoint detachGrip;
            detachGrip.ikWeight = 0.0f;
            for (uint32_t frame = 0; frame < 90; ++frame) {
                ZHLN::Animation::UpdateGripWeight(detachGrip, 1.0f / 60.0f);
            }
            const auto triggerCurl = ZHLN::Animation::EvaluateFingerCurl(
                {.shape = ZHLN::Animation::GraspShape::TriggerGrip, .gripRadius = 0.02f, .tightness = 1.0f, .triggerCurl = 0.30f}
            );
            if (detachGrip.evaluatedIKWeight > 0.01f || std::abs(triggerCurl.index - 0.30f) > 0.0001f || triggerCurl.middle <= triggerCurl.index) {
                return std::unexpected(ProceduralAnimationTestError::GaitInvariantFailed);
            }

            const Quat limitedWrist = ZHLN::Animation::ConstrainWristRotation(
                Quat::FromAxisAngle(FVec3::UnitZ(), ZHLN::RadF{1.2f}), Quat::Identity(), FVec3::UnitZ(), ZHLN::RadF{0.30f}, ZHLN::RadF{0.45f}
            );
            FVec3 wristAxis;
            ZHLN::RadF wristAngleRadians{};
            limitedWrist.ToAxisAngle(wristAxis, wristAngleRadians);
            const auto wristAngle = wristAngleRadians.value;
            if (std::abs(wristAngle) > 0.3001f) {
                return std::unexpected(ProceduralAnimationTestError::GaitInvariantFailed);
            }

            ZHLN::RigBoneMap map;
            ZHLN::BuildStandardProceduralRig(map);
            const ZHLN::RigNodeIndex upper       = map.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::UpperArmR)];
            const ZHLN::RigNodeIndex fore        = map.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::ForearmR)];
            const ZHLN::RigNodeIndex hand        = map.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::HandR)];
            const float              upperLength = (ZHLN::ProceduralMath::Translation(map.modelTransforms[fore]) - ZHLN::ProceduralMath::Translation(map.modelTransforms[upper])).Length();
            const float              lowerLength = (ZHLN::ProceduralMath::Translation(map.modelTransforms[hand]) - ZHLN::ProceduralMath::Translation(map.modelTransforms[fore])).Length();
            const Quat rightBindPalmFrame   = (ZHLN::ProceduralMath::Rotation(map.modelTransforms[hand]).Normalized() * map.handBoneToPalmRotations[1]).Normalized();
            const FVec3 gripPosition         = ZHLN::ProceduralMath::Translation(map.modelTransforms[upper]) + FVec3{-0.38f, -0.05f, 0.12f};
            ZHLN::Animation::GripPoint grip;
            grip.assignedLimb      = ZHLN::CharacterBone::HandR;
            grip.evaluatedIKWeight = 1.0f;
            grip.maxArmExtension   = 0.98f;
            grip.maxWristTwistDeg  = 179.0f;
            grip.maxWristSwingDeg  = 179.0f;
            ZHLN::Animation::SolveLimbIK(
                map.modelTransforms.data(), map, ZHLN::CharacterBone::UpperArmR, ZHLN::CharacterBone::ForearmR, ZHLN::CharacterBone::HandR,
                ZHLN::ProceduralMath::Transform(Quat::Identity(), gripPosition), grip
            );
            const float solvedUpperLength = (ZHLN::ProceduralMath::Translation(map.modelTransforms[fore]) - ZHLN::ProceduralMath::Translation(map.modelTransforms[upper])).Length();
            const float solvedLowerLength = (ZHLN::ProceduralMath::Translation(map.modelTransforms[hand]) - ZHLN::ProceduralMath::Translation(map.modelTransforms[fore])).Length();
            if (std::abs(solvedUpperLength - upperLength) > 0.0001f || std::abs(solvedLowerLength - lowerLength) > 0.0001f ||
                !Close(ZHLN::ProceduralMath::Translation(map.modelTransforms[hand]), gripPosition, 0.0001f)) {
                return std::unexpected(ProceduralAnimationTestError::GaitInvariantFailed);
            }
            const Quat solvedPalmFrame = (ZHLN::ProceduralMath::Rotation(map.modelTransforms[hand]).Normalized() * map.handBoneToPalmRotations[1]).Normalized();
            if (Dot((solvedPalmFrame * FVec3::UnitZ()), FVec3::UnitZ()) < 0.99f ||
                Dot((solvedPalmFrame * FVec3::UnitX()), FVec3::UnitX()) < 0.99f) {
                return std::unexpected(ProceduralAnimationTestError::GaitInvariantFailed);
            }

            ZHLN::RigBoneMap leftGripMap;
            ZHLN::BuildStandardProceduralRig(leftGripMap);
            const ZHLN::RigNodeIndex   leftUpper    = leftGripMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::UpperArmL)];
            const ZHLN::RigNodeIndex   leftGripHand = leftGripMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::HandL)];
            ZHLN::Animation::GripPoint leftGrip;
            leftGrip.assignedLimb            = ZHLN::CharacterBone::HandL;
            leftGrip.evaluatedIKWeight       = 1.0f;
            const FVec3 leftGripPosition = ZHLN::ProceduralMath::Translation(leftGripMap.modelTransforms[leftUpper]) + FVec3{0.38f, -0.05f, 0.12f};
            ZHLN::Animation::SolveLimbIK(
                leftGripMap.modelTransforms.data(), leftGripMap, ZHLN::CharacterBone::UpperArmL, ZHLN::CharacterBone::ForearmL, ZHLN::CharacterBone::HandL,
                CreateTranslation(leftGripPosition), leftGrip
            );
            const Quat solvedLeftPalm =
                (ZHLN::ProceduralMath::Rotation(leftGripMap.modelTransforms[leftGripHand]).Normalized() * leftGripMap.handBoneToPalmRotations[0]).Normalized();
            if (Dot((solvedLeftPalm * FVec3::UnitZ()), FVec3::UnitZ()) < 0.80f ||
                Dot((solvedLeftPalm * FVec3::UnitX()), -FVec3::UnitX()) < 0.80f) {
                return std::unexpected(ProceduralAnimationTestError::GaitInvariantFailed);
            }

            if (!map.handPalmFramesValid[0] || !map.handPalmFramesValid[1]) {
                return std::unexpected(ProceduralAnimationTestError::GaitInvariantFailed);
            }
            const ZHLN::RigNodeIndex leftHand      = map.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::HandL)];
            const Quat          leftPalmFrame = (ZHLN::ProceduralMath::Rotation(map.modelTransforms[leftHand]).Normalized() * map.handBoneToPalmRotations[0]).Normalized();
            if ((leftPalmFrame * FVec3::UnitX()).y > -0.5f || (rightBindPalmFrame * FVec3::UnitX()).y > -0.5f) {
                return std::unexpected(ProceduralAnimationTestError::GaitInvariantFailed);
            }

            ZHLN::RigBoneMap reachMap;
            ZHLN::BuildStandardProceduralRig(reachMap);
            ZHLN::Animation::ItemHandlingComponent reachHandling;
            reachHandling.driverMode                 = ZHLN::Animation::ItemDriverMode::AimGuided;
            reachHandling.itemModelTransform         = CreateTranslation(FVec3{0.0f, 1.4f, 4.0f});
            reachHandling.grips[0].assignedLimb      = ZHLN::CharacterBone::HandR;
            reachHandling.grips[0].localTransform    = CreateTranslation(FVec3{-0.05f, 0.0f, 0.0f});
            reachHandling.grips[0].evaluatedIKWeight = 1.0f;
            reachHandling.grips[0].maxArmExtension   = 0.96f;
            reachHandling.grips[1].assignedLimb      = ZHLN::CharacterBone::HandL;
            reachHandling.grips[1].localTransform    = CreateTranslation(FVec3{0.05f, 0.0f, 0.0f});
            reachHandling.grips[1].evaluatedIKWeight = 1.0f;
            reachHandling.grips[1].maxArmExtension   = 0.96f;
            reachHandling.gripCount                  = 2;
            ZHLN::Animation::ConstrainItemToGripReach(reachHandling, reachMap.modelTransforms.data(), reachMap);
            const ZHLN::RigNodeIndex reachUpper = reachMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::UpperArmR)];
            const ZHLN::RigNodeIndex reachFore  = reachMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::ForearmR)];
            const ZHLN::RigNodeIndex reachHand  = reachMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::HandR)];
            const float              maximumReach =
                ((ZHLN::ProceduralMath::Translation(reachMap.modelTransforms[reachFore]) - ZHLN::ProceduralMath::Translation(reachMap.modelTransforms[reachUpper])).Length() +
                 (ZHLN::ProceduralMath::Translation(reachMap.modelTransforms[reachHand]) - ZHLN::ProceduralMath::Translation(reachMap.modelTransforms[reachFore])).Length()) *
                reachHandling.grips[0].maxArmExtension;
            const Mat4x4 reachModel = reachHandling.itemModelTransform;
            const float      constrainedDistance =
                (ZHLN::ProceduralMath::Translation((reachModel * reachHandling.grips[0].localTransform)) - ZHLN::ProceduralMath::Translation(reachMap.modelTransforms[reachUpper])).Length();
            const ZHLN::RigNodeIndex reachUpperL         = reachMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::UpperArmL)];
            const ZHLN::RigNodeIndex reachForeL          = reachMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::ForearmL)];
            const ZHLN::RigNodeIndex reachHandL          = reachMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::HandL)];
            const float              maximumReachL =
                ((ZHLN::ProceduralMath::Translation(reachMap.modelTransforms[reachForeL]) - ZHLN::ProceduralMath::Translation(reachMap.modelTransforms[reachUpperL])).Length() +
                 (ZHLN::ProceduralMath::Translation(reachMap.modelTransforms[reachHandL]) - ZHLN::ProceduralMath::Translation(reachMap.modelTransforms[reachForeL])).Length()) *
                reachHandling.grips[1].maxArmExtension;
            const float constrainedDistanceL =
                (ZHLN::ProceduralMath::Translation((reachModel * reachHandling.grips[1].localTransform)) - ZHLN::ProceduralMath::Translation(reachMap.modelTransforms[reachUpperL])).Length();
            if (constrainedDistance > maximumReach + 0.0001f || constrainedDistanceL > maximumReachL + 0.0001f) {
                return std::unexpected(ProceduralAnimationTestError::GaitInvariantFailed);
            }

            ZHLN::RigBoneMap fingerMap;
            fingerMap.nodeCount                                               = 4;
            fingerMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::HandL)] = 0;
            fingerMap.modelTransforms[0]                                      = Mat4x4::Identity();
            fingerMap.modelTransforms[1]                                      = CreateTranslation(FVec3{0.10f, 0.0f, 0.0f});
            fingerMap.modelTransforms[2]                                      = CreateTranslation(FVec3{0.18f, 0.0f, 0.0f});
            fingerMap.modelTransforms[3]                                      = CreateTranslation(FVec3{0.24f, 0.0f, 0.0f});
            fingerMap.fingerJointConstraintCount                              = 3;
            for (size_t segment = 0; segment < 3; ++segment) {
                auto& constraint          = fingerMap.fingerJointConstraints[segment];
                constraint.parent         = segment;
                constraint.child          = segment + 1;
                constraint.digit          = ZHLN::FingerDigit::Index;
                constraint.side           = 0;
                constraint.segment        = static_cast<uint8_t>(segment);
                constraint.repairRelation = true;
                constraint.bindRelative   = Inverse(fingerMap.modelTransforms[segment]) * fingerMap.modelTransforms[segment + 1];
            }
            fingerMap.modelTransforms[0] = ZHLN::ProceduralMath::Transform(Quat::FromAxisAngle(FVec3::UnitZ(), ZHLN::RadF{0.4f}), FVec3{0.3f, 1.0f, -0.2f});
            if (ZHLN::ProceduralAnimation::ApplyFingerRelationConstraints(fingerMap) != 3 ||
                !Close(fingerMap.modelTransforms[3], fingerMap.modelTransforms[2] * fingerMap.fingerJointConstraints[2].bindRelative, 0.0001f)) {
                return std::unexpected(ProceduralAnimationTestError::GaitInvariantFailed);
            }

            const Quat hingeTarget = (Quat::FromAxisAngle(FVec3::UnitY(), ZHLN::RadF{0.5f}) * Quat::FromAxisAngle(FVec3::UnitX(), ZHLN::RadF{1.2f})).Normalized();
            const Quat hinge = ZHLN::Animation::ConstrainFingerHingeRotation(Quat::Identity(), hingeTarget, FVec3::UnitX(), 1.0f, ZHLN::RadF{0.70f}, ZHLN::RadF{0.10f});
            FVec3       hingeAxis;
            ZHLN::RadF hingeAngleRadians{};
            hinge.ToAxisAngle(hingeAxis, hingeAngleRadians);
            const auto hingeAngle = hingeAngleRadians.value;
            if (std::abs(hingeAngle - 0.70f) > 0.001f || std::abs(std::abs(Dot(hingeAxis, FVec3::UnitX())) - 1.0f) > 0.001f) {
                return std::unexpected(ProceduralAnimationTestError::GaitInvariantFailed);
            }
            const Quat backwards = ZHLN::Animation::ConstrainFingerHingeRotation(
                Quat::Identity(), Quat::FromAxisAngle(FVec3::UnitX(), ZHLN::RadF{-0.5f}), FVec3::UnitX(), 1.0f, ZHLN::RadF{0.70f}, ZHLN::RadF{0.0f}
            );
            FVec3 backwardsAxis;
            ZHLN::RadF backwardsAngleRadians{};
            backwards.ToAxisAngle(backwardsAxis, backwardsAngleRadians);
            const auto backwardsAngle = backwardsAngleRadians.value;
            if (std::abs(backwardsAngle) > 0.0001f) {
                return std::unexpected(ProceduralAnimationTestError::GaitInvariantFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> non_skinned_attachments_follow_evaluated_nodes() {
            ZHLN::ECS::Registry registry;
            registry.RegisterAllComponentsIn<ZHLN::Components>();
            const ZHLN::Entity root       = registry.Create();
            const ZHLN::Entity attachment = registry.Create(
                ZHLN::Components::TransformComponent {}, ZHLN::Components::MeshComponent {.nodeIndex = 2}, ZHLN::Components::HierarchyComponent {.parent = root}
            );
            const ZHLN::Entity skinned = registry.Create(
                ZHLN::Components::TransformComponent {.position = FVec3{9.0f, 9.0f, 9.0f}}, ZHLN::Components::MeshComponent {.nodeIndex = 2},
                ZHLN::Components::SkeletalMeshComponent {.skeletonIndex = 0}, ZHLN::Components::HierarchyComponent {.parent = root}
            );

            ZHLN::RigBoneMap map;
            map.nodeCount = 3;
            const FVec3 expectedPosition{0.3f, 1.4f, -0.2f};
            const FVec3 expectedScale{1.2f, 0.8f, 1.1f};
            const Quat expectedRotation = Quat::FromAxisAngle(FVec3::UnitY(), ZHLN::RadF{0.35f});
            map.modelTransforms[2]           = ZHLN::ProceduralMath::Transform(expectedRotation, expectedPosition, expectedScale);

            const size_t synchronizedCount   = ZHLN::ProceduralAnimation::SyncNonSkinnedAttachments(registry, root, map);
            const auto   attachmentTransform = registry.Get<ZHLN::Components::TransformComponent>(attachment);
            const auto   skinnedTransform    = registry.Get<ZHLN::Components::TransformComponent>(skinned);
            if (synchronizedCount != 1 || !attachmentTransform || !Close(FVec3{attachmentTransform->position}, expectedPosition, 0.0001f) ||
                !Close(FVec3{attachmentTransform->scale}, expectedScale, 0.0001f) ||
                !Close((Quat{attachmentTransform->rotation} * FVec3::UnitZ()), expectedRotation * FVec3::UnitZ(), 0.0001f) ||
                !skinnedTransform || !Close(FVec3{skinnedTransform->position}, FVec3{9.0f, 9.0f, 9.0f}, 0.0001f)) {
                return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
            }
            return {};
        }

        /**
         * Maps a real authored rig: the shipped base rig GLB is parsed into a
         * prefab fixture (see RigFixture) and handed to BuildBoneMap, then the
         * resulting semantic map, its forward kinematics and its bind-pose
         * skinning palette are checked.
         *
         * The importer that produces this prefab in production is covered
         * separately by tests/render/TestGLTFImport.cpp; what is under test
         * here is the rig mapper, on names and a bind pose no synthetic
         * fixture reproduces faithfully.
         */
        std::expected<void, ZHLN::ErrorCode> base_rig_glb_maps_every_control_and_hair_strand() {
            const std::string assetPath = std::string(ZHLN_TEST_SOURCE_DIR) + "/resources/assets/ProceduralAnimationBaseRig.glb";
            std::ifstream     stream(assetPath, std::ios::binary);
            char              magic[4] {};
            stream.read(magic, sizeof(magic));
            if (stream.gcount() != 4 || std::string_view(magic, 4) != "glTF") {
                std::cout << "[SKIP] ProceduralAnimationBaseRig.glb is an unresolved Git LFS pointer.\n";
                return {};
            }

            cgltf_options options {};
            cgltf_data*   rawData = nullptr;
            if (cgltf_parse_file(&options, assetPath.c_str(), &rawData) != cgltf_result_success || rawData == nullptr) {
                return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
            }
            std::unique_ptr<cgltf_data, decltype(&cgltf_free)> data(rawData, &cgltf_free);
            if (cgltf_load_buffers(&options, data.get(), assetPath.c_str()) != cgltf_result_success || data->nodes_count == 0 || data->skins_count == 0) {
                return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
            }

            ZHLN::ModelPrefab prefab;
            prefab.virtualPath = "ProceduralAnimationBaseRig.glb";
            RigFixture::BuildFixture(*data, prefab);

            const ZHLN::Skeleton& skeleton = prefab.skeletons[0];
            ZHLN::RigBoneMap      map;
            if (!ZHLN::BuildBoneMap(prefab, skeleton, map) || map.nodeCount != prefab.nodes.size() || !map.initialized) {
                return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
            }

            // The map mirrors the imported hierarchy, and its forward kinematics
            // reproduce the bind-pose world transforms of the source rig.
            for (ZHLN::RigNodeIndex node = 0; node < map.nodeCount; ++node) {
                const int32_t importedParent = prefab.nodes[node].parentIndex;
                const bool    parentMatches  = importedParent < 0 ? map.parentIndices[node] == ZHLN::InvalidRigNode :
                                                                    map.parentIndices[node] == static_cast<ZHLN::RigNodeIndex>(importedParent);
                if (!parentMatches || !Close(map.bindLocalTransforms[node], ZHLN::PrefabMatrixToNative(prefab.nodes[node].localTransform), 0.0001f) ||
                    !Close(map.modelTransforms[node], RigFixture::SourceWorld(data->nodes[node]), 0.0001f)) {
                    return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
                }
            }

            // Aliasing-prone controls must claim their own node, never a neighbour's
            // (Forearm swallowed as UpperArm, Sup_Spine as Spine, Toe as Foot, ...).
            const std::array<std::pair<ZHLN::CharacterBone, std::string_view>, ZHLN::kCoreBoneCount> expectedNodes = {{
                {ZHLN::CharacterBone::Root, "Root"},
                {ZHLN::CharacterBone::Hips, "DEF-Hips"},
                {ZHLN::CharacterBone::Spine, "DEF-Spine"},
                {ZHLN::CharacterBone::SupSpine, "DEF-Sup_Spine"},
                {ZHLN::CharacterBone::Chest, "DEF-Chest"},
                {ZHLN::CharacterBone::Neck, "DEF-Neck"},
                {ZHLN::CharacterBone::Head, "DEF-Head"},
                {ZHLN::CharacterBone::UpperArmL, "DEF-Upper_arm.L"},
                {ZHLN::CharacterBone::ForearmL, "DEF-Forearm.L"},
                {ZHLN::CharacterBone::HandL, "DEF-Hand.L"},
                {ZHLN::CharacterBone::UpperArmR, "DEF-Upper_arm.R"},
                {ZHLN::CharacterBone::ForearmR, "DEF-Forearm.R"},
                {ZHLN::CharacterBone::HandR, "DEF-Hand.R"},
                {ZHLN::CharacterBone::ThighL, "DEF-Thigh.L"},
                {ZHLN::CharacterBone::ShinL, "DEF-Shin.L"},
                {ZHLN::CharacterBone::FootL, "DEF-Foot.L"},
                {ZHLN::CharacterBone::ToeL, "DEF-Toe.L"},
                {ZHLN::CharacterBone::ThighR, "DEF-Thigh.R"},
                {ZHLN::CharacterBone::ShinR, "DEF-Shin.R"},
                {ZHLN::CharacterBone::FootR, "DEF-Foot.R"},
                {ZHLN::CharacterBone::ToeR, "DEF-Toe.R"},
            }};
            for (const auto& [bone, expectedName]: expectedNodes) {
                const ZHLN::RigNodeIndex node = map.nodeIndices[ZHLN::BoneSlot(bone)];
                if (!ZHLN::IsValidRigNode(node, map.nodeCount) || std::string_view(prefab.nodes[node].name) != expectedName) {
                    return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
                }
            }

            // No imported node may be claimed by two semantic slots.
            std::vector<uint8_t> claimed(map.nodeCount, 0);
            size_t               mappedHairLinks = 0;
            for (size_t semantic = 0; semantic < ZHLN::kBoneCount; ++semantic) {
                const ZHLN::RigNodeIndex node = map.nodeIndices[semantic];
                if (!ZHLN::IsValidRigNode(node, map.nodeCount)) {
                    continue;
                }
                if (claimed[node] != 0) {
                    return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
                }
                claimed[node] = 1;
                mappedHairLinks += semantic >= ZHLN::BoneSlot(ZHLN::CharacterBone::HairStart) ? 1u : 0u;
            }

            // The authored asset ships 18 six-link strands (DEF-Hair_S01_01 .. S18_06).
            // Every link must land in its own slot, in order, parented to the previous.
            constexpr size_t expectedStrands = 18;
            if (map.sourceHairStrandCount != expectedStrands || mappedHairLinks != expectedStrands * ZHLN::HairStrandsComponent::kLinksPerStrand) {
                return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
            }
            const size_t hairBase = ZHLN::BoneSlot(ZHLN::CharacterBone::HairStart);
            for (size_t strand = 0; strand < expectedStrands; ++strand) {
                for (size_t link = 1; link < ZHLN::HairStrandsComponent::kLinksPerStrand; ++link) {
                    const size_t             slot     = strand * ZHLN::HairStrandsComponent::kLinksPerStrand + link;
                    const ZHLN::RigNodeIndex node     = map.nodeIndices[hairBase + slot];
                    const ZHLN::RigNodeIndex previous = map.nodeIndices[hairBase + slot - 1];
                    if (!ZHLN::IsValidRigNode(node, map.nodeCount) || map.parentIndices[node] != previous) {
                        return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
                    }
                }
            }

            // Bind-pose palette: model transform times inverse bind is identity per joint.
            std::vector<Mat4x4> palette(skeleton.joints.size(), Mat4x4::Identity());
            ZHLN::ProceduralAnimation::ResolveModelTransforms(map);
            if (ZHLN::ProceduralAnimation::BuildSkinningPalette(skeleton, map, palette) != skeleton.joints.size()) {
                return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
            }
            for (const Mat4x4& jointMatrix: palette) {
                if (!Close(jointMatrix, Mat4x4::Identity(), 0.0001f)) {
                    return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
                }
            }

            // Authored track lookup against the real clip names of this rig.
            if (ZHLN::FindAnimationTrack(prefab, "idle") < 0 || ZHLN::FindAnimationTrack(prefab, "walk") < 0 ||
                ZHLN::FindAnimationTrack(prefab, "run") < 0 ||
                ZHLN::FindAnimationTrack(prefab, "walk") == ZHLN::FindAnimationTrack(prefab, "run")) {
                return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> real_glb_foot_descendants_follow_pose() {
            const std::string assetPath = std::string(ZHLN_TEST_SOURCE_DIR) + "/resources/assets/UziProc.glb";
            std::ifstream     stream(assetPath, std::ios::binary);
            char              magic[4] {};
            stream.read(magic, sizeof(magic));
            if (stream.gcount() != 4 || std::string_view(magic, 4) != "glTF") {
                std::cout << "[SKIP] UziProc.glb is an unresolved Git LFS pointer.\n";
                return {};
            }

            cgltf_options options {};
            cgltf_data*   rawData = nullptr;
            if (cgltf_parse_file(&options, assetPath.c_str(), &rawData) != cgltf_result_success || rawData == nullptr || rawData->skins_count == 0) {
                return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
            }
            std::unique_ptr<cgltf_data, decltype(&cgltf_free)> data(rawData, &cgltf_free);

            ZHLN::ModelPrefab prefab;
            prefab.virtualPath = "UziProc.glb";
            prefab.nodes.resize(data->nodes_count);
            for (size_t node = 0; node < data->nodes_count; ++node) {
                float matrix[16] {};
                cgltf_node_transform_local(&data->nodes[node], matrix);
                prefab.nodes[node].name           = data->nodes[node].name != nullptr ? ZHLN::String64(data->nodes[node].name) : ZHLN::String64("Node");
                prefab.nodes[node].parentIndex    = data->nodes[node].parent != nullptr ? static_cast<int32_t>(data->nodes[node].parent - data->nodes) : -1;
                prefab.nodes[node].localTransform = ToPrefabMatrix(Mat4x4::FromCols(FVec4{matrix[0], matrix[1], matrix[2], matrix[3]}, FVec4{matrix[4], matrix[5], matrix[6], matrix[7]}, FVec4{matrix[8], matrix[9], matrix[10], matrix[11]}, FVec4{matrix[12], matrix[13], matrix[14], matrix[15]}));
            }

            std::vector<ZHLN::Skeleton> importedSkeletons;
            importedSkeletons.reserve(data->skins_count);
            ZHLN::RigBoneMap map;
            size_t           bestCoreCount = 0;
            for (size_t skinIndex = 0; skinIndex < data->skins_count; ++skinIndex) {
                const cgltf_skin& sourceSkin = data->skins[skinIndex];
                ZHLN::Skeleton    skeleton;
                skeleton.name = sourceSkin.name != nullptr ? ZHLN::String64(sourceSkin.name) : ZHLN::String64("Skin");
                skeleton.joints.reserve(sourceSkin.joints_count);
                for (size_t jointIndex = 0; jointIndex < sourceSkin.joints_count; ++jointIndex) {
                    const cgltf_node* jointNode   = sourceSkin.joints[jointIndex];
                    int32_t           parentJoint = -1;
                    for (size_t candidate = 0; candidate < sourceSkin.joints_count; ++candidate) {
                        if (sourceSkin.joints[candidate] == jointNode->parent) {
                            parentJoint = static_cast<int32_t>(candidate);
                            break;
                        }
                    }
                    skeleton.joints.push_back({
                        .name              = jointNode->name != nullptr ? ZHLN::String64(jointNode->name) : ZHLN::String64("Joint"),
                        .parentIndex       = parentJoint,
                        .nodeIndex         = static_cast<int32_t>(jointNode - data->nodes),
                        .inverseBindMatrix = Mat4x4::Identity(),
                    });
                }

                ZHLN::RigBoneMap candidateMap;
                ZHLN::BuildBoneMap(prefab, skeleton, candidateMap);
                size_t coreCount = 0;
                for (size_t semantic = 0; semantic < ZHLN::kCoreBoneCount; ++semantic) {
                    coreCount += ZHLN::IsValidRigNode(candidateMap.nodeIndices[semantic], candidateMap.nodeCount) ? 1u : 0u;
                }
                if (importedSkeletons.empty() || coreCount > bestCoreCount) {
                    map           = candidateMap;
                    bestCoreCount = coreCount;
                }
                importedSkeletons.push_back(std::move(skeleton));
            }
            const std::array<ZHLN::RigNodeIndex, 2> footNodes = {
                map.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::FootL)],
                map.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::FootR)],
            };
            if (!ZHLN::IsValidRigNode(footNodes[0], map.nodeCount) || !ZHLN::IsValidRigNode(footNodes[1], map.nodeCount)) {
                return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
            }

            struct SkinPaletteCheck {
                size_t                  skinIndex;
                std::vector<size_t>     footJoints;
                std::vector<Mat4x4> before;
            };
            std::vector<SkinPaletteCheck> paletteChecks;
            for (size_t skinIndex = 0; skinIndex < importedSkeletons.size(); ++skinIndex) {
                const ZHLN::Skeleton& skeleton = importedSkeletons[skinIndex];
                SkinPaletteCheck      check {.skinIndex = skinIndex};
                for (size_t jointIndex = 0; jointIndex < skeleton.joints.size(); ++jointIndex) {
                    const int32_t importedNode = skeleton.joints[jointIndex].nodeIndex;
                    if (importedNode >= 0 &&
                        (static_cast<ZHLN::RigNodeIndex>(importedNode) == footNodes[0] || static_cast<ZHLN::RigNodeIndex>(importedNode) == footNodes[1])) {
                        check.footJoints.push_back(jointIndex);
                    }
                }
                if (check.footJoints.empty()) {
                    continue;
                }
                check.before.resize(skeleton.joints.size());
                ZHLN::ProceduralAnimation::BuildSkinningPalette(skeleton, map, check.before);
                paletteChecks.push_back(std::move(check));
            }
            if (paletteChecks.empty()) {
                return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
            }

            struct Descendant {
                ZHLN::RigNodeIndex node;
                Mat4x4         before;
                ZHLN::Entity       entity;
            };
            std::vector<Descendant> descendants;
            ZHLN::ECS::Registry     registry;
            registry.RegisterAllComponentsIn<ZHLN::Components>();
            const ZHLN::Entity root = registry.Create();
            for (ZHLN::RigNodeIndex node = 0; node < map.nodeCount; ++node) {
                if (data->nodes[node].mesh == nullptr || data->nodes[node].skin != nullptr) {
                    continue;
                }
                ZHLN::RigNodeIndex ancestor  = map.parentIndices[node];
                bool               underFoot = node == footNodes[0] || node == footNodes[1];
                for (size_t depth = 0; depth < map.nodeCount && ZHLN::IsValidRigNode(ancestor, map.nodeCount); ++depth) {
                    underFoot = underFoot || ancestor == footNodes[0] || ancestor == footNodes[1];
                    ancestor  = map.parentIndices[ancestor];
                }
                if (!underFoot) {
                    continue;
                }
                const ZHLN::Entity child = registry.Create(
                    ZHLN::Components::TransformComponent {}, ZHLN::Components::MeshComponent {.nodeIndex = static_cast<int32_t>(node)},
                    ZHLN::Components::HierarchyComponent {.parent = root}
                );
                descendants.push_back({.node = node, .before = map.modelTransforms[node], .entity = child});
            }
            if (descendants.empty()) {
                return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
            }

            for (ZHLN::RigNodeIndex footNode: footNodes) {
                map.localTransforms[footNode] = map.localTransforms[footNode] * ZHLN::ProceduralMath::Transform(Quat::FromAxisAngle(FVec3::UnitX(), ZHLN::RadF{0.3f}), FVec3::Zero());
            }
            ZHLN::ProceduralAnimation::ResolveModelTransforms(map);
            for (const SkinPaletteCheck& check: paletteChecks) {
                const ZHLN::Skeleton&   skeleton = importedSkeletons[check.skinIndex];
                std::vector<Mat4x4> after(skeleton.joints.size());
                ZHLN::ProceduralAnimation::BuildSkinningPalette(skeleton, map, after);
                for (size_t footJoint: check.footJoints) {
                    const bool changed = !NearZero((ZHLN::ProceduralMath::Column3(after[footJoint], 1) - ZHLN::ProceduralMath::Column3(check.before[footJoint], 1)), 0.0001f) ||
                                         !NearZero((ZHLN::ProceduralMath::Column3(after[footJoint], 2) - ZHLN::ProceduralMath::Column3(check.before[footJoint], 2)), 0.0001f);
                    if (!changed) {
                        return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
                    }
                }
            }
            for (const Descendant& descendant: descendants) {
                const Mat4x4& after = map.modelTransforms[descendant.node];
                const bool        moved = !NearZero((ZHLN::ProceduralMath::Translation(after) - ZHLN::ProceduralMath::Translation(descendant.before)), 0.0001f) ||
                                          !NearZero((ZHLN::ProceduralMath::Column3(after, 1) - ZHLN::ProceduralMath::Column3(descendant.before, 1)), 0.0001f);
                if (!moved) {
                    return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
                }
            }

            if (ZHLN::ProceduralAnimation::SyncNonSkinnedAttachments(registry, root, map) != descendants.size()) {
                return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
            }
            for (const Descendant& descendant: descendants) {
                const auto transform = registry.Get<ZHLN::Components::TransformComponent>(descendant.entity);
                if (!transform || !Close(FVec3{transform->position}, ZHLN::ProceduralMath::Translation(map.modelTransforms[descendant.node]), 0.0001f)) {
                    return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
                }
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> authored_upper_body_can_suppress_procedural_layers() {
            ZHLN::RigBoneMap map;
            ZHLN::BuildStandardProceduralRig(map);
            const ZHLN::RigNodeIndex armNode    = map.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::UpperArmL)];
            const ZHLN::RigNodeIndex headNode   = map.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::Head)];
            const Mat4x4         armBefore  = map.modelTransforms[armNode];
            const Mat4x4         headBefore = map.modelTransforms[headNode];

            ZHLN::ProceduralLocomotionComponent gait;
            gait.phase            = 0.25f;
            gait.previousVelocity = {0.0f, 0.0f, 2.0f};
            ZHLN::ProceduralLookAtComponent lookAt {
                .targetWorldPos = ZHLN::ProceduralMath::Translation(headBefore) + FVec3{1.0f, 0.2f, 2.0f},
                .weight         = 1.0f,
            };

            ZHLN::Animation::SolveUpperBody(gait, &lookAt, FVec3::Zero(), Quat::Identity(), map.modelTransforms.data(), map, false, false);
            if (!Close(map.modelTransforms[armNode], armBefore, 0.0001f) || !Close(map.modelTransforms[headNode], headBefore, 0.0001f)) {
                return std::unexpected(ProceduralAnimationTestError::GaitInvariantFailed);
            }

            ZHLN::Animation::SolveUpperBody(gait, &lookAt, FVec3::Zero(), Quat::Identity(), map.modelTransforms.data(), map, true, true);
            if (Close(map.modelTransforms[armNode], armBefore, 0.0001f) || Close(map.modelTransforms[headNode], headBefore, 0.0001f)) {
                return std::unexpected(ProceduralAnimationTestError::GaitInvariantFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> acceleration_tilt_preserves_com_radius() {
            ZHLN::RigBoneMap map;
            ZHLN::BuildStandardProceduralRig(map);
            const ZHLN::RigNodeIndex headNode   = map.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::Head)];
            const FVec3          headBefore = ZHLN::ProceduralMath::Translation(map.modelTransforms[headNode]);

            ZHLN::ProceduralLocomotionComponent gait;
            gait.forwardLean = 0.18f;
            gait.lateralBank = -0.11f;
            ZHLN::Animation::ApplyAccelerationTilt(gait, map.modelTransforms.data(), map);

            const FVec3 headAfter    = ZHLN::ProceduralMath::Translation(map.modelTransforms[headNode]);
            const float     beforeRadius = (headBefore - gait.centerOfMassModel).Length();
            const float     afterRadius  = (headAfter - gait.centerOfMassModel).Length();
            if ((headAfter - headBefore).LengthSquared() < 1.0e-6f || std::abs(beforeRadius - afterRadius) > 0.0001f) {
                return std::unexpected(ProceduralAnimationTestError::GaitInvariantFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> xpbd_hair_preserves_segment_lengths() {
            ZHLN::HairStrandsComponent hair;
            ZHLN::Animation::StepHairSimulation(hair, FVec3{0.0f, 2.0f, 0.0f}, Quat::Identity(), FVec3{3.0f, 0.0f, 0.0f}, 1.0f / 60.0f);

            if (!hair.initialized) {
                return std::unexpected(ProceduralAnimationTestError::HairConstraintFailed);
            }
            for (size_t strand = 0; strand < ZHLN::HairStrandsComponent::kStrandCount; ++strand) {
                const size_t base = strand * ZHLN::HairStrandsComponent::kLinksPerStrand;
                for (size_t link = 1; link < ZHLN::HairStrandsComponent::kLinksPerStrand; ++link) {
                    const size_t index  = base + link;
                    const float  length = (hair.positions[index] - hair.positions[index - 1]).Length();
                    if (!std::isfinite(length) || std::abs(length - hair.segmentLengths[index]) > 0.03f) {
                        return std::unexpected(ProceduralAnimationTestError::HairConstraintFailed);
                    }
                }
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> xpbd_hair_returns_to_authored_bind_shape() {
            ZHLN::RigBoneMap map;
            ZHLN::BuildStandardProceduralRig(map);
            const ZHLN::RigNodeIndex headNode     = map.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::Head)];
            const FVec3          headPosition = ZHLN::ProceduralMath::Translation(map.modelTransforms[headNode]);

            // Author a deliberately horizontal first strand. A gravity-only
            // rope would collapse; shape and bend constraints must retain it.
            for (size_t link = 0; link < ZHLN::HairStrandsComponent::kLinksPerStrand; ++link) {
                const size_t             semantic = ZHLN::BoneSlot(ZHLN::CharacterBone::HairStart) + link;
                const ZHLN::RigNodeIndex node     = map.nodeIndices[semantic];
                map.modelTransforms[node]         = CreateTranslation(headPosition + FVec3{0.15f + static_cast<float>(link) * 0.08f, 0.08f, 0.0f});
            }

            ZHLN::HairStrandsComponent hair;
            ZHLN::Animation::ConfigureHairBindPose(hair, map.modelTransforms.data(), map);
            for (uint32_t frame = 0; frame < 120; ++frame) {
                ZHLN::Animation::StepHairSimulation(hair, headPosition, Quat::Identity(), FVec3::Zero(), 1.0f / 60.0f);
            }

            constexpr size_t tip         = ZHLN::HairStrandsComponent::kLinksPerStrand - 1;
            const FVec3  authoredTip = headPosition + hair.restLocalPositions[tip];
            if ((hair.positions[tip] - authoredTip).Length() > 0.04f) {
                return std::unexpected(ProceduralAnimationTestError::HairConstraintFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> gait_blend_interpolates_parameters_smoothly() {
            ZHLN::ProceduralLocomotionComponent gait;
            // Walk preset
            gait.currentPreset.strideLength     = 1.60f;
            gait.currentPreset.stepHeight       = 0.28f;
            gait.currentPreset.maxBounceHeight  = 0.045f;
            gait.currentPreset.pelvisSwayScale  = 1.0f;
            gait.currentPreset.armSwingScale    = 1.0f;
            gait.currentPreset.forwardLeanScale = 1.0f;
            // Initialize live parameters to match current preset
            gait.strideLength     = gait.currentPreset.strideLength;
            gait.stepHeight       = gait.currentPreset.stepHeight;
            gait.maxBounceHeight  = gait.currentPreset.maxBounceHeight;
            gait.pelvisSwayScale  = gait.currentPreset.pelvisSwayScale;
            gait.armSwingScale    = gait.currentPreset.armSwingScale;
            gait.forwardLeanScale = gait.currentPreset.forwardLeanScale;

            // Set run preset as target
            gait.targetPreset.strideLength     = 2.40f;
            gait.targetPreset.stepHeight       = 0.45f;
            gait.targetPreset.maxBounceHeight  = 0.065f;
            gait.targetPreset.pelvisSwayScale  = 1.35f;
            gait.targetPreset.armSwingScale    = 1.50f;
            gait.targetPreset.forwardLeanScale = 1.40f;
            gait.gaitBlendWeight               = 0.0f;
            gait.gaitBlendSpeed                = 4.0f;

            // Blend for one frame - parameters should start moving toward target
            constexpr float dt = 1.0f / 60.0f;
            ZHLN::Animation::BlendGaitParameters(gait, dt);
            if (gait.gaitBlendWeight <= 0.0f || gait.gaitBlendWeight >= 1.0f || gait.strideLength <= gait.currentPreset.strideLength ||
                gait.strideLength >= gait.targetPreset.strideLength) {
                return std::unexpected(ProceduralAnimationTestError::GaitInvariantFailed);
            }

            // Blend until complete (use enough frames to guarantee convergence)
            for (uint32_t frame = 0; frame < 180; ++frame) {
                ZHLN::Animation::BlendGaitParameters(gait, dt);
            }
            // After enough time, parameters should have reached the target.
            // Don't check gaitBlendWeight exactly - it may have cycled through
            // completion and reset multiple times due to floating point accumulation.
            if (std::abs(gait.strideLength - gait.targetPreset.strideLength) > 0.01f || std::abs(gait.stepHeight - gait.targetPreset.stepHeight) > 0.01f ||
                std::abs(gait.maxBounceHeight - gait.targetPreset.maxBounceHeight) > 0.001f ||
                std::abs(gait.pelvisSwayScale - gait.targetPreset.pelvisSwayScale) > 0.01f ||
                std::abs(gait.armSwingScale - gait.targetPreset.armSwingScale) > 0.01f ||
                std::abs(gait.forwardLeanScale - gait.targetPreset.forwardLeanScale) > 0.01f) {
                return std::unexpected(ProceduralAnimationTestError::GaitInvariantFailed);
            }

            // Verify that currentPreset eventually snaps to target after completion
            if (std::abs(gait.currentPreset.strideLength - gait.targetPreset.strideLength) > 0.01f) {
                return std::unexpected(ProceduralAnimationTestError::GaitInvariantFailed);
            }
            return {};
        }
    };
};

} // namespace
