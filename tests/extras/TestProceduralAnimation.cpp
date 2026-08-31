// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/ModelPrefab.hpp>
#include <Zahlen/SkeletalAnimation.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/physics/Physics.hpp>
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
#include <utility>
#include <vector>

import ZHLN.Locomotion;
import ZHLN.ProceduralAnimation;

enum class ProceduralAnimationTestError : uint32_t {
    RigMappingFailed[[= ZHLN::Description<"The generated procedural rig did not map all core and secondary controls.">{}]] = 1,
    GaitInvariantFailed[[= ZHLN::Description<"The distance-driven gait clock or alternating foot phases violated its invariant.">{}]],
    HairConstraintFailed[[= ZHLN::Description<"The XPBD hair solver produced a non-finite or excessively stretched segment.">{}]],
};

struct ProceduralAnimationTestSuite {
    ProceduralAnimationTestSuite() {
        ZHLN::TaskSystem::Init(2, 32, ZHLN::kMinimumFiberStackSize);
    }

    ~ProceduralAnimationTestSuite() {
        ZHLN::TaskSystem::Shutdown();
    }

    struct Tests {
        std::expected<void, ZHLN::Error> authored_track_lookup_prefers_exact_idle() {
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

        std::expected<void, ZHLN::Error> imported_bounds_fit_virtual_character_hull() {
            ZHLN::ModelPrefab prefab;
            prefab.nodes.push_back({.name = ZHLN::String64("Root"), .localTransform = JPH::Mat44::sTranslation(JPH::Vec3(0.0f, 1.0f, 0.0f))});
            prefab.nodes.push_back(
                {.name = ZHLN::String64("Nested"), .parentIndex = 0, .localTransform = JPH::Mat44::sTranslation(JPH::Vec3(1.0f, 2.0f, -1.0f))}
            );

            ZHLN::ModelPart nested;
            nested.nodeIndex      = 1;
            nested.localTransform = JPH::Mat44::sTranslation(JPH::Vec3(0.5f, 0.0f, 0.25f));
            nested.localMin[0]    = -1.0f;
            nested.localMin[1]    = -2.0f;
            nested.localMin[2]    = -0.5f;
            nested.localMax[0]    = 1.0f;
            nested.localMax[1]    = 2.0f;
            nested.localMax[2]    = 0.5f;
            prefab.parts.push_back(std::move(nested));

            ZHLN::ModelPart rootPart;
            rootPart.nodeIndex   = 0;
            rootPart.localMin[0] = -2.0f;
            rootPart.localMin[1] = 0.0f;
            rootPart.localMin[2] = -1.0f;
            rootPart.localMax[0] = -1.0f;
            rootPart.localMax[1] = 1.0f;
            rootPart.localMax[2] = 1.0f;
            prefab.parts.push_back(std::move(rootPart));

            const ZHLN::Locomotion::CharacterBoundsEstimate bounds = ZHLN::Locomotion::EstimateCharacterBounds(prefab);
            const ZHLN::Locomotion::DualShapeFitOptions     options;
            const ZHLN::Physics::DualShapeConfig            hull = ZHLN::Locomotion::FitDualShapeToBounds(bounds, ZHLN::Physics::DualShapeConfig {}, options);
            const float                                     horizontalExtent = std::max(2.5f * options.lateralCoverage, 1.25f) * options.horizontalPadding;
            const float                                     hullTop          = hull.GetBumperOffsetY() + hull.bumperRadiusY;
            if (!bounds.valid || !bounds.min.IsClose(JPH::Vec3(-2.0f, 1.0f, -1.25f), 0.0001f) || !bounds.max.IsClose(JPH::Vec3(2.5f, 5.0f, 1.0f), 0.0001f) ||
                std::abs(hull.bumperRadiusXZ - horizontalExtent) > 0.0001f || hull.lifterRadius >= hull.bumperRadiusXZ ||
                hull.bumperRadiusY <= hull.bumperRadiusXZ || hullTop < 5.0f * options.verticalPadding - 0.0001f) {
                return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::Error> def_only_rig_filters_control_bones_and_maps_24_hair_strands() {
            ZHLN::ModelPrefab prefab;
            auto              addNode = [&](std::string_view name, int32_t parent, JPH::Vec3Arg translation) {
                const size_t index = prefab.nodes.size();
                prefab.nodes.push_back({.name = ZHLN::String64(name), .parentIndex = parent, .localTransform = JPH::Mat44::sTranslation(translation)});
                return index;
            };
            const size_t root = addNode("Root", -1, JPH::Vec3::sZero());
            static_cast<void>(addNode("CTR-Hand.L", static_cast<int32_t>(root), JPH::Vec3(0.2f, 1.0f, 0.0f)));
            const size_t hand = addNode("DEF-Hand.L", static_cast<int32_t>(root), JPH::Vec3(0.3f, 1.0f, 0.0f));
            static_cast<void>(addNode("CTR-Index_1.L", static_cast<int32_t>(hand), JPH::Vec3(0.04f, 0.0f, 0.0f)));
            const size_t finger1 = addNode("DEF-Index_1.L", static_cast<int32_t>(hand), JPH::Vec3(0.05f, 0.0f, 0.0f));
            static_cast<void>(addNode("CTR-Index_2.L", static_cast<int32_t>(finger1), JPH::Vec3(0.03f, 0.0f, 0.0f)));
            const size_t finger2 = addNode("DEF-Index_2.L", static_cast<int32_t>(finger1), JPH::Vec3(0.03f, 0.0f, 0.0f));
            const size_t finger3 = addNode("DEF-Index_3.L", static_cast<int32_t>(finger2), JPH::Vec3(0.02f, 0.0f, 0.0f));
            const size_t hair24  = addNode("DEF-Hair_S24_04", static_cast<int32_t>(root), JPH::Vec3(0.0f, 1.5f, 0.0f));

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

        std::expected<void, ZHLN::Error> standard_rig_maps_every_control() {
            ZHLN::RigBoneMap map;
            map.nodeCount          = 7;
            map.modelTransforms[0] = JPH::Mat44::sTranslation(JPH::Vec3(1.0f, 2.0f, 3.0f));
            map.Reset();
            if (map.nodeCount != 0 || map.nodeIndices[0] != ZHLN::InvalidRigNode || !map.modelTransforms[0].IsClose(JPH::Mat44::sIdentity(), 0.0001f) ||
                !map.poseRotations[0].IsClose(JPH::Quat::sIdentity(), 0.0001f) || !map.poseScales[0].IsClose(JPH::Vec3::sReplicate(1.0f), 0.0001f)) {
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

            const JPH::Vec3 thighPosition = map.modelTransforms[map.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::ThighL)]].GetTranslation();
            const JPH::Vec3 shinPosition  = map.modelTransforms[map.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::ShinL)]].GetTranslation();
            const JPH::Vec3 footPosition  = map.modelTransforms[map.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::FootL)]].GetTranslation();
            const JPH::Vec3 headPosition  = map.modelTransforms[map.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::Head)]].GetTranslation();
            const JPH::Vec3 hairRoot = map.modelTransforms[map.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::HairStart)]].GetTranslation() - headPosition;
            if (!(shinPosition.GetZ() > thighPosition.GetZ() && shinPosition.GetZ() > footPosition.GetZ()) || hairRoot.GetY() < 0.15f ||
                JPH::Vec3(hairRoot.GetX(), 0.0f, hairRoot.GetZ()).Length() < 0.08f) {
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
                prefab.nodes[node].localTransform = map.bindLocalTransforms[node];
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
            prefab.nodes[handLNode].localTransform = map.modelTransforms[hipsNode].Inversed() * map.modelTransforms[handLNode];
            prefab.nodes[handRNode].localTransform = map.modelTransforms[hipsNode].Inversed() * map.modelTransforms[handRNode];
            prefab.nodes[shinLNode].localTransform = map.modelTransforms[hipsNode].Inversed() * map.modelTransforms[shinLNode];
            prefab.nodes[shinRNode].localTransform = map.modelTransforms[hipsNode].Inversed() * map.modelTransforms[shinRNode];
            prefab.nodes[chestNode].localTransform = map.modelTransforms[hipsNode].Inversed() * map.modelTransforms[chestNode];
            prefab.nodes[neckNode].localTransform  = map.modelTransforms[hipsNode].Inversed() * map.modelTransforms[neckNode];
            prefab.nodes[headNode].localTransform  = map.modelTransforms[hipsNode].Inversed() * map.modelTransforms[headNode];

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
                const JPH::Mat44         targetModel  = map.modelTransforms[footNode] * JPH::Mat44::sTranslation(JPH::Vec3(0.0f, -0.035f, 0.065f));
                const size_t             shoeRootNode = prefab.nodes.size();
                prefab.nodes.push_back({
                    .name           = ZHLN::String64(opaqueName),
                    .parentIndex    = static_cast<int32_t>(characterRootNode),
                    .localTransform = map.modelTransforms[characterRootNode].Inversed() * targetModel,
                    .hasMesh        = !detachedContainer,
                });

                size_t meshNode = shoeRootNode;
                if (detachedContainer) {
                    meshNode = prefab.nodes.size();
                    prefab.nodes.push_back({
                        .name           = ZHLN::String64("OpaquePart"),
                        .parentIndex    = static_cast<int32_t>(shoeRootNode),
                        .localTransform = JPH::Mat44::sIdentity(),
                        .hasMesh        = true,
                    });
                }

                ZHLN::ModelPart part;
                part.name        = ZHLN::String64("OpaquePart");
                part.isSkinned   = skinned;
                part.nodeIndex   = static_cast<int32_t>(meshNode);
                part.localMin[0] = -0.12f * extentScale;
                part.localMin[1] = -0.06f * extentScale;
                part.localMin[2] = -0.18f * extentScale;
                part.localMax[0] = 0.12f * extentScale;
                part.localMax[1] = 0.10f * extentScale;
                part.localMax[2] = 0.22f * extentScale;
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
                    .localTransform = map.modelTransforms[hipsNode].Inversed() * map.modelTransforms[footNode],
                });
                footwearSkin.joints.push_back({
                    .name              = ZHLN::String64(jointName),
                    .parentIndex       = -1,
                    .nodeIndex         = static_cast<int32_t>(node),
                    .inverseBindMatrix = map.modelTransforms[footNode].Inversed(),
                });
                return node;
            };
            const size_t secondaryFootLNode = addSecondaryFootJoint(ZHLN::CharacterBone::FootL, "DEF-Foot.L");
            const size_t secondaryFootRNode = addSecondaryFootJoint(ZHLN::CharacterBone::FootR, "DEF-Foot.R");
            prefab.skeletons.push_back(std::move(footwearSkin));

            ZHLN::RigBoneMap importedMap;
            if (!ZHLN::BuildBoneMap(prefab, skeleton, importedMap) || importedMap.parentIndices[malformedNode] != ZHLN::InvalidRigNode ||
                importedMap.childOfConstraintCount != 14 || importedMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::Spine)] != 3 ||
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
            const JPH::Vec3          handBefore       = importedMap.modelTransforms[handL].GetTranslation();
            const JPH::Vec3          chestBefore      = importedMap.modelTransforms[chest].GetTranslation();
            const JPH::Vec3          neckBefore       = importedMap.modelTransforms[neck].GetTranslation();
            const JPH::Vec3          headBefore       = importedMap.modelTransforms[head].GetTranslation();
            const JPH::Mat44         rigidFootLBefore = importedMap.modelTransforms[rigidFootL.root];
            const JPH::Mat44         rigidFootRBefore = importedMap.modelTransforms[rigidFootR.root];
            const JPH::Mat44 rigidFootLPartRelative   = importedMap.modelTransforms[rigidFootL.root].Inversed() * importedMap.modelTransforms[rigidFootL.part];
            const JPH::Mat44 rigidFootRPartRelative   = importedMap.modelTransforms[rigidFootR.root].Inversed() * importedMap.modelTransforms[rigidFootR.part];
            const JPH::Mat44 secondaryFootLBefore     = importedMap.modelTransforms[secondaryFootLNode];
            const JPH::Mat44 secondaryFootRBefore     = importedMap.modelTransforms[secondaryFootRNode];
            importedMap.localTransforms[forearmL]     = importedMap.localTransforms[forearmL] *
                                                        JPH::Mat44::sRotation(JPH::Quat::sRotation(JPH::Vec3::sAxisY(), 0.45f));
            importedMap.localTransforms[supSpine]     = importedMap.localTransforms[supSpine] *
                                                        JPH::Mat44::sRotation(JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), 0.25f));
            importedMap.localTransforms[footL] = importedMap.localTransforms[footL] * JPH::Mat44::sRotation(JPH::Quat::sRotation(JPH::Vec3::sAxisX(), 0.30f));
            importedMap.localTransforms[footR] = importedMap.localTransforms[footR] * JPH::Mat44::sRotation(JPH::Quat::sRotation(JPH::Vec3::sAxisX(), -0.25f));
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
            auto offsetAuthoredShin = [&](ZHLN::RigNodeIndex shin, JPH::Vec3Arg offset) {
                JPH::Mat44 authored = importedMap.modelTransforms[shin];
                authored.SetTranslation(authored.GetTranslation() + offset);
                ZHLN::Animation::SetModelTransformAndCarrySubtree(importedMap.modelTransforms.data(), importedMap, shin, authored);
            };
            offsetAuthoredShin(shinL, JPH::Vec3(0.025f, 0.010f, 0.0f));
            offsetAuthoredShin(shinR, JPH::Vec3(-0.025f, 0.010f, 0.0f));
            ZHLN::ProceduralAnimation::CaptureAuthoredConstraintPoseDeltas(importedMap);

            // Simulate a model-space procedural thigh correction after the
            // authored GLB hierarchy has been evaluated. Flattened shins are
            // not imported descendants, so this creates the gap Stage 7 must
            // close without discarding the captured authored knee relation.
            auto rotateDetachedThigh = [&](ZHLN::RigNodeIndex thigh, float angle) {
                const JPH::Mat44 current = importedMap.modelTransforms[thigh];
                const JPH::Mat44 target  = JPH::Mat44::sRotationTranslation(
                    (JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), angle) * current.GetQuaternion()).Normalized(), current.GetTranslation()
                );
                ZHLN::Animation::SetModelTransformAndCarrySubtree(importedMap.modelTransforms.data(), importedMap, thigh, target);
            };
            rotateDetachedThigh(thighL, 0.35f);
            rotateDetachedThigh(thighR, -0.30f);

            const JPH::Mat44 detachedShinL = importedMap.modelTransforms[shinL];
            const JPH::Mat44 detachedShinR = importedMap.modelTransforms[shinR];
            const JPH::Mat44 detachedFootL = importedMap.modelTransforms[footL];
            const JPH::Mat44 detachedFootR = importedMap.modelTransforms[footR];
            const JPH::Vec3  expectedKneeL =
                (importedMap.modelTransforms[thighL] * kneeLConstraint->bindRelative * kneeLConstraint->localPoseDelta).GetTranslation();
            const JPH::Vec3 expectedKneeR =
                (importedMap.modelTransforms[thighR] * kneeRConstraint->bindRelative * kneeRConstraint->localPoseDelta).GetTranslation();
            // Ankle constraints use the shin positions after knee constraints are applied.
            // Knee constraints pin shin translation but preserve rotation.
            const JPH::Mat44 pinnedShinL = JPH::Mat44::sRotationTranslation(detachedShinL.GetQuaternion(), expectedKneeL);
            const JPH::Mat44 pinnedShinR = JPH::Mat44::sRotationTranslation(detachedShinR.GetQuaternion(), expectedKneeR);
            const JPH::Vec3  expectedAnkleL =
                (pinnedShinL * ankleLConstraint->bindRelative * ankleLConstraint->localPoseDelta).GetTranslation();
            const JPH::Vec3 expectedAnkleR =
                (pinnedShinR * ankleRConstraint->bindRelative * ankleRConstraint->localPoseDelta).GetTranslation();

            // Knee and ankle joints are structural and remain active even when
            // every optional child-of relationship is disabled. They pin position
            // without replacing the authored or IK-produced basis.
            if (ZHLN::ProceduralAnimation::ApplyChildOfConstraints(importedMap, false, false, false, false, false) != 4 ||
                !importedMap.modelTransforms[shinL].GetTranslation().IsClose(expectedKneeL, 0.0001f) ||
                !importedMap.modelTransforms[shinR].GetTranslation().IsClose(expectedKneeR, 0.0001f) ||
                !importedMap.modelTransforms[footL].GetTranslation().IsClose(expectedAnkleL, 0.0001f) ||
                !importedMap.modelTransforms[footR].GetTranslation().IsClose(expectedAnkleR, 0.0001f) ||
                !importedMap.modelTransforms[shinL].Multiply3x3(JPH::Vec3::sAxisZ()).IsClose(detachedShinL.Multiply3x3(JPH::Vec3::sAxisZ()), 0.0001f) ||
                !importedMap.modelTransforms[shinR].Multiply3x3(JPH::Vec3::sAxisZ()).IsClose(detachedShinR.Multiply3x3(JPH::Vec3::sAxisZ()), 0.0001f) ||
                !importedMap.modelTransforms[footL].Multiply3x3(JPH::Vec3::sAxisZ()).IsClose(detachedFootL.Multiply3x3(JPH::Vec3::sAxisZ()), 0.0001f) ||
                !importedMap.modelTransforms[footR].Multiply3x3(JPH::Vec3::sAxisZ()).IsClose(detachedFootR.Multiply3x3(JPH::Vec3::sAxisZ()), 0.0001f) ||
                detachedShinL.GetTranslation().IsClose(expectedKneeL, 0.0001f) || detachedShinR.GetTranslation().IsClose(expectedKneeR, 0.0001f) ||
                detachedFootL.GetTranslation().IsClose(expectedAnkleL, 0.0001f) || detachedFootR.GetTranslation().IsClose(expectedAnkleR, 0.0001f)) {
                return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
            }

            if (ZHLN::ProceduralAnimation::ApplyChildOfConstraints(importedMap) != 14 || handLConstraint == nullptr || handRConstraint == nullptr ||
                ankleLConstraint == nullptr || ankleRConstraint == nullptr || rigidFootLConstraint == nullptr || rigidFootRConstraint == nullptr ||
                secondaryFootLConstraint == nullptr || secondaryFootRConstraint == nullptr || toeRConstraint == nullptr ||
                !importedMap.modelTransforms[handL].IsClose(
                    importedMap.modelTransforms[forearmL] * handLConstraint->bindRelative * handLConstraint->localPoseDelta, 0.0001f
                ) ||
                !importedMap.modelTransforms[handR].IsClose(
                    importedMap.modelTransforms[forearmR] * handRConstraint->bindRelative * handRConstraint->localPoseDelta, 0.0001f
                ) ||
                !importedMap.modelTransforms[rigidFootL.root].IsClose(
                    importedMap.modelTransforms[footL] * rigidFootLConstraint->bindRelative * rigidFootLConstraint->localPoseDelta, 0.0001f
                ) ||
                !importedMap.modelTransforms[rigidFootR.root].IsClose(
                    importedMap.modelTransforms[footR] * rigidFootRConstraint->bindRelative * rigidFootRConstraint->localPoseDelta, 0.0001f
                ) ||
                !importedMap.modelTransforms[rigidFootL.part].IsClose(importedMap.modelTransforms[rigidFootL.root] * rigidFootLPartRelative, 0.0001f) ||
                !importedMap.modelTransforms[rigidFootR.part].IsClose(importedMap.modelTransforms[rigidFootR.root] * rigidFootRPartRelative, 0.0001f) ||
                !importedMap.modelTransforms[secondaryFootLNode].IsClose(
                    importedMap.modelTransforms[footL] * secondaryFootLConstraint->bindRelative * secondaryFootLConstraint->localPoseDelta, 0.0001f
                ) ||
                !importedMap.modelTransforms[secondaryFootRNode].IsClose(
                    importedMap.modelTransforms[footR] * secondaryFootRConstraint->bindRelative * secondaryFootRConstraint->localPoseDelta, 0.0001f
                ) ||
                !importedMap.modelTransforms[toeR].IsClose(
                    importedMap.modelTransforms[footR] * toeRConstraint->bindRelative * toeRConstraint->localPoseDelta, 0.0001f
                ) ||
                importedMap.modelTransforms[rigidFootL.root].IsClose(rigidFootLBefore, 0.0001f) ||
                importedMap.modelTransforms[rigidFootR.root].IsClose(rigidFootRBefore, 0.0001f) ||
                importedMap.modelTransforms[secondaryFootLNode].IsClose(secondaryFootLBefore, 0.0001f) ||
                importedMap.modelTransforms[secondaryFootRNode].IsClose(secondaryFootRBefore, 0.0001f) ||
                (importedMap.modelTransforms[handL].GetTranslation() - handBefore).LengthSq() < 0.0001f ||
                (importedMap.modelTransforms[chest].GetTranslation() - chestBefore).LengthSq() < 0.0001f ||
                (importedMap.modelTransforms[neck].GetTranslation() - neckBefore).LengthSq() < 0.0001f ||
                (importedMap.modelTransforms[head].GetTranslation() - headBefore).LengthSq() < 0.0001f) {
                return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::Error> first_person_palette_masks_head_and_hair() {
            ZHLN::RigBoneMap map;
            ZHLN::BuildStandardProceduralRig(map);
            const ZHLN::RigNodeIndex head = map.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::Head)];
            const ZHLN::RigNodeIndex hair = map.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::HairStart)];
            const ZHLN::RigNodeIndex hand = map.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::HandL)];
            ZHLN::Skeleton           skeleton;
            skeleton.joints.push_back({.name = ZHLN::String64("DEF-Head"), .nodeIndex = static_cast<int32_t>(head)});
            skeleton.joints.push_back({.name = ZHLN::String64("DEF-Hair_S01_01"), .nodeIndex = static_cast<int32_t>(hair)});
            skeleton.joints.push_back({.name = ZHLN::String64("DEF-Hand.L"), .nodeIndex = static_cast<int32_t>(hand)});
            std::array<JPH::Mat44, 3> palette {JPH::Mat44::sIdentity(), JPH::Mat44::sIdentity(), JPH::Mat44::sIdentity()};
            if (ZHLN::ProceduralAnimation::MaskFirstPersonPalette(skeleton, map, palette, true, true) != 2 || palette[0].GetColumn3(0).LengthSq() > 1.0e-8f ||
                palette[1].GetColumn3(1).LengthSq() > 1.0e-8f || !palette[2].IsClose(JPH::Mat44::sIdentity(), 0.0001f)) {
                return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::Error> gait_clock_tracks_distance() {
            ZHLN::ProceduralLocomotionComponent gait;
            gait.strideLength = 1.60f;

            ZHLN::Animation::EvaluateGait(gait, JPH::Vec3(0.0f, 0.0f, 4.0f), 0.25f);
            const bool phaseIsDistanceDriven  = std::abs(gait.phase - 0.625f) < 0.0001f;
            const bool feetAreOpposed         = gait.plantWeightL != gait.plantWeightR;
            const bool authoredSwingPreserved = gait.plantWeightR < 0.001f && gait.plantWeightL > 0.99f;
            const bool accelerationIsFinite   = std::isfinite(gait.directionalAcceleration.GetZ());
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

        std::expected<void, ZHLN::Error> gravity_bounce_flattens_with_speed() {
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

        std::expected<void, ZHLN::Error> model_space_ik_and_ankle_alignment() {
            const JPH::Vec3  down(0.0f, -1.0f, 0.0f);
            const JPH::Vec3  solvedDirection = JPH::Vec3(0.18f, -0.97f, 0.12f).Normalized();
            const JPH::Mat44 authoredBone    = JPH::Mat44::sIdentity();
            const JPH::Vec3  solvedPosition(0.1f, 0.7f, -0.2f);
            const JPH::Mat44 corrected = ZHLN::Animation::CorrectBoneDirection(authoredBone, down, solvedDirection, solvedPosition);
            if (!corrected.Multiply3x3(down).Normalized().IsClose(solvedDirection, 0.0001f) || !corrected.GetTranslation().IsClose(solvedPosition, 0.0001f)) {
                return std::unexpected(ProceduralAnimationTestError::GaitInvariantFailed);
            }

            const JPH::Quat  authoredYaw  = JPH::Quat::sRotation(JPH::Vec3::sAxisY(), 0.4f);
            const JPH::Mat44 authoredFoot = JPH::Mat44::sRotationTranslation(authoredYaw, JPH::Vec3::sZero());
            const JPH::Vec3  footTarget(0.2f, 0.05f, 0.3f);
            const JPH::Mat44 flatFoot = ZHLN::Animation::AlignFootToGround(authoredFoot, footTarget, JPH::Vec3::sAxisY());
            if (!flatFoot.GetTranslation().IsClose(footTarget, 0.0001f) ||
                !flatFoot.Multiply3x3(JPH::Vec3::sAxisZ()).Normalized().IsClose(authoredFoot.Multiply3x3(JPH::Vec3::sAxisZ()).Normalized(), 0.0001f)) {
                return std::unexpected(ProceduralAnimationTestError::GaitInvariantFailed);
            }

            const JPH::Vec3  slopeNormal = JPH::Vec3(0.25f, 0.95f, 0.18f).Normalized();
            const JPH::Mat44 slopeFoot   = ZHLN::Animation::AlignFootToGround(authoredFoot, footTarget, slopeNormal);
            if (!slopeFoot.Multiply3x3(JPH::Vec3::sAxisY()).Normalized().IsClose(slopeNormal, 0.0001f)) {
                return std::unexpected(ProceduralAnimationTestError::GaitInvariantFailed);
            }

            constexpr float maxSideways     = 0.20f;
            constexpr float maxForward      = 0.50f;
            const JPH::Vec3 extremeNormal   = JPH::Vec3(0.82f, 0.45f, 0.35f).Normalized();
            const JPH::Vec3 limitedNormal   = ZHLN::Animation::LimitGroundNormal(extremeNormal, maxSideways, maxForward);
            const float     limitedSideways = std::abs(std::atan2(-limitedNormal.GetX(), limitedNormal.GetY()));
            const float     limitedForward  = std::abs(std::asin(std::clamp(limitedNormal.GetZ(), -1.0f, 1.0f)));
            if (limitedSideways > maxSideways + 0.0001f || limitedForward > maxForward + 0.0001f) {
                return std::unexpected(ProceduralAnimationTestError::GaitInvariantFailed);
            }

            ZHLN::RigBoneMap subtreeMap;
            ZHLN::BuildStandardProceduralRig(subtreeMap);
            const ZHLN::RigNodeIndex footNode        = subtreeMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::FootL)];
            const ZHLN::RigNodeIndex toeNode         = subtreeMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::ToeL)];
            const ZHLN::RigNodeIndex meshPartNode    = subtreeMap.nodeCount++;
            subtreeMap.parentIndices[meshPartNode]   = footNode;
            subtreeMap.modelTransforms[meshPartNode] = subtreeMap.modelTransforms[footNode] * JPH::Mat44::sTranslation(JPH::Vec3(0.04f, -0.02f, 0.08f));
            const JPH::Mat44 oldFoot                 = subtreeMap.modelTransforms[footNode];
            const JPH::Mat44 oldToe                  = subtreeMap.modelTransforms[toeNode];
            const JPH::Mat44 oldMeshPart             = subtreeMap.modelTransforms[meshPartNode];
            const JPH::Mat44 carriedFoot             = JPH::Mat44::sRotationTranslation(
                JPH::Quat::sRotation(JPH::Vec3::sAxisX(), 0.35f) * oldFoot.GetQuaternion(), oldFoot.GetTranslation() + JPH::Vec3(0.0f, 0.04f, 0.0f)
            );
            const JPH::Mat44 correction = carriedFoot * oldFoot.Inversed();
            if (ZHLN::Animation::SetModelTransformAndCarrySubtree(subtreeMap.modelTransforms.data(), subtreeMap, footNode, carriedFoot) < 3 ||
                !subtreeMap.modelTransforms[footNode].IsClose(carriedFoot, 0.0001f) ||
                !subtreeMap.modelTransforms[toeNode].IsClose(correction * oldToe, 0.0001f) ||
                !subtreeMap.modelTransforms[meshPartNode].IsClose(correction * oldMeshPart, 0.0001f)) {
                return std::unexpected(ProceduralAnimationTestError::GaitInvariantFailed);
            }

            ZHLN::RigBoneMap tiltMap;
            ZHLN::BuildStandardProceduralRig(tiltMap);
            const auto                          baseTransforms    = tiltMap.modelTransforms;
            const ZHLN::RigNodeIndex            tiltThigh         = tiltMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::ThighL)];
            const ZHLN::RigNodeIndex            tiltHips          = tiltMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::Hips)];
            const JPH::Vec3                     unreachableTarget = tiltMap.modelTransforms[tiltThigh].GetTranslation() + JPH::Vec3(1.5f, -0.35f, 0.0f);
            constexpr float                     maxBodyTilt       = 0.14f;
            ZHLN::ProceduralLocomotionComponent tiltGait;
            for (uint32_t frame = 0; frame < 120; ++frame) {
                tiltMap.modelTransforms = baseTransforms;
                ZHLN::Animation::ApplyIKReachTilt(
                    tiltGait, tiltMap.modelTransforms.data(), tiltMap, unreachableTarget, JPH::Vec3::sZero(), 1.0f, 0.0f, 0.96f, maxBodyTilt, 1.0f / 60.0f
                );
            }
            const float appliedTilt = std::sqrt(tiltGait.ikBodyTiltPitch * tiltGait.ikBodyTiltPitch + tiltGait.ikBodyTiltRoll * tiltGait.ikBodyTiltRoll);
            if (appliedTilt < 0.01f || appliedTilt > maxBodyTilt + 0.0001f || tiltMap.modelTransforms[tiltHips].IsClose(baseTransforms[tiltHips], 0.0001f)) {
                return std::unexpected(ProceduralAnimationTestError::GaitInvariantFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::Error> item_handling_drives_constrained_grips() {
            ZHLN::Animation::ItemHandlingComponent handling;
            handling.driverMode           = ZHLN::Animation::ItemDriverMode::AimGuided;
            handling.aimProgress          = 1.0f;
            const JPH::Vec3  aimDirection = JPH::Vec3(1.0f, 0.1f, 0.2f).Normalized();
            const JPH::Mat44 aimPose      = ZHLN::Animation::SolveItemBasePose(
                handling, JPH::Mat44::sIdentity(), JPH::Mat44::sIdentity(), JPH::Mat44::sIdentity(), JPH::Vec3(0.0f, 1.6f, 0.0f), aimDirection,
                JPH::Mat44::sIdentity()
            );
            if (!aimPose.Multiply3x3(JPH::Vec3::sAxisZ()).Normalized().IsClose(aimDirection, 0.0001f)) {
                return std::unexpected(ProceduralAnimationTestError::GaitInvariantFailed);
            }

            handling.driverMode        = ZHLN::Animation::ItemDriverMode::WorldAnchored;
            const JPH::Mat44 worldPose = ZHLN::Animation::SolveItemBasePose(
                handling, JPH::Mat44::sIdentity(), JPH::Mat44::sIdentity(), JPH::Mat44::sTranslation(JPH::Vec3(-1.0f, 0.0f, 0.0f)), JPH::Vec3::sZero(),
                JPH::Vec3::sAxisZ(), JPH::Mat44::sTranslation(JPH::Vec3(3.0f, 0.0f, 0.0f))
            );
            if (!worldPose.GetTranslation().IsClose(JPH::Vec3(2.0f, 0.0f, 0.0f), 0.0001f)) {
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

            const JPH::Quat limitedWrist = ZHLN::Animation::ConstrainWristRotation(
                JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), 1.2f), JPH::Quat::sIdentity(), JPH::Vec3::sAxisZ(), 0.30f, 0.45f
            );
            JPH::Vec3 wristAxis;
            float     wristAngle = 0.0f;
            limitedWrist.GetAxisAngle(wristAxis, wristAngle);
            if (std::abs(wristAngle) > 0.3001f) {
                return std::unexpected(ProceduralAnimationTestError::GaitInvariantFailed);
            }

            ZHLN::RigBoneMap map;
            ZHLN::BuildStandardProceduralRig(map);
            const ZHLN::RigNodeIndex upper       = map.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::UpperArmR)];
            const ZHLN::RigNodeIndex fore        = map.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::ForearmR)];
            const ZHLN::RigNodeIndex hand        = map.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::HandR)];
            const float              upperLength = (map.modelTransforms[fore].GetTranslation() - map.modelTransforms[upper].GetTranslation()).Length();
            const float              lowerLength = (map.modelTransforms[hand].GetTranslation() - map.modelTransforms[fore].GetTranslation()).Length();
            const JPH::Quat rightBindPalmFrame   = (map.modelTransforms[hand].GetQuaternion().Normalized() * map.handBoneToPalmRotations[1]).Normalized();
            const JPH::Vec3 gripPosition         = map.modelTransforms[upper].GetTranslation() + JPH::Vec3(-0.38f, -0.05f, 0.12f);
            ZHLN::Animation::GripPoint grip;
            grip.assignedLimb      = ZHLN::CharacterBone::HandR;
            grip.evaluatedIKWeight = 1.0f;
            grip.maxArmExtension   = 0.98f;
            grip.maxWristTwistDeg  = 179.0f;
            grip.maxWristSwingDeg  = 179.0f;
            ZHLN::Animation::SolveLimbIK(
                map.modelTransforms.data(), map, ZHLN::CharacterBone::UpperArmR, ZHLN::CharacterBone::ForearmR, ZHLN::CharacterBone::HandR,
                JPH::Mat44::sRotationTranslation(JPH::Quat::sIdentity(), gripPosition), grip
            );
            const float solvedUpperLength = (map.modelTransforms[fore].GetTranslation() - map.modelTransforms[upper].GetTranslation()).Length();
            const float solvedLowerLength = (map.modelTransforms[hand].GetTranslation() - map.modelTransforms[fore].GetTranslation()).Length();
            if (std::abs(solvedUpperLength - upperLength) > 0.0001f || std::abs(solvedLowerLength - lowerLength) > 0.0001f ||
                !map.modelTransforms[hand].GetTranslation().IsClose(gripPosition, 0.0001f)) {
                return std::unexpected(ProceduralAnimationTestError::GaitInvariantFailed);
            }
            const JPH::Quat solvedPalmFrame = (map.modelTransforms[hand].GetQuaternion().Normalized() * map.handBoneToPalmRotations[1]).Normalized();
            if ((solvedPalmFrame * JPH::Vec3::sAxisZ()).Dot(JPH::Vec3::sAxisZ()) < 0.99f ||
                (solvedPalmFrame * JPH::Vec3::sAxisX()).Dot(JPH::Vec3::sAxisX()) < 0.99f) {
                return std::unexpected(ProceduralAnimationTestError::GaitInvariantFailed);
            }

            ZHLN::RigBoneMap leftGripMap;
            ZHLN::BuildStandardProceduralRig(leftGripMap);
            const ZHLN::RigNodeIndex   leftUpper    = leftGripMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::UpperArmL)];
            const ZHLN::RigNodeIndex   leftGripHand = leftGripMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::HandL)];
            ZHLN::Animation::GripPoint leftGrip;
            leftGrip.assignedLimb            = ZHLN::CharacterBone::HandL;
            leftGrip.evaluatedIKWeight       = 1.0f;
            const JPH::Vec3 leftGripPosition = leftGripMap.modelTransforms[leftUpper].GetTranslation() + JPH::Vec3(0.38f, -0.05f, 0.12f);
            ZHLN::Animation::SolveLimbIK(
                leftGripMap.modelTransforms.data(), leftGripMap, ZHLN::CharacterBone::UpperArmL, ZHLN::CharacterBone::ForearmL, ZHLN::CharacterBone::HandL,
                JPH::Mat44::sTranslation(leftGripPosition), leftGrip
            );
            const JPH::Quat solvedLeftPalm =
                (leftGripMap.modelTransforms[leftGripHand].GetQuaternion().Normalized() * leftGripMap.handBoneToPalmRotations[0]).Normalized();
            if ((solvedLeftPalm * JPH::Vec3::sAxisZ()).Dot(JPH::Vec3::sAxisZ()) < 0.80f ||
                (solvedLeftPalm * JPH::Vec3::sAxisX()).Dot(-JPH::Vec3::sAxisX()) < 0.80f) {
                return std::unexpected(ProceduralAnimationTestError::GaitInvariantFailed);
            }

            if (!map.handPalmFramesValid[0] || !map.handPalmFramesValid[1]) {
                return std::unexpected(ProceduralAnimationTestError::GaitInvariantFailed);
            }
            const ZHLN::RigNodeIndex leftHand      = map.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::HandL)];
            const JPH::Quat          leftPalmFrame = (map.modelTransforms[leftHand].GetQuaternion().Normalized() * map.handBoneToPalmRotations[0]).Normalized();
            if ((leftPalmFrame * JPH::Vec3::sAxisX()).GetY() > -0.5f || (rightBindPalmFrame * JPH::Vec3::sAxisX()).GetY() > -0.5f) {
                return std::unexpected(ProceduralAnimationTestError::GaitInvariantFailed);
            }

            ZHLN::RigBoneMap reachMap;
            ZHLN::BuildStandardProceduralRig(reachMap);
            ZHLN::Animation::ItemHandlingComponent reachHandling;
            reachHandling.driverMode                 = ZHLN::Animation::ItemDriverMode::AimGuided;
            reachHandling.itemModelTransform         = JPH::Mat44::sTranslation(JPH::Vec3(0.0f, 1.4f, 4.0f));
            reachHandling.grips[0].assignedLimb      = ZHLN::CharacterBone::HandR;
            reachHandling.grips[0].localTransform    = JPH::Mat44::sTranslation(JPH::Vec3(-0.05f, 0.0f, 0.0f));
            reachHandling.grips[0].evaluatedIKWeight = 1.0f;
            reachHandling.grips[0].maxArmExtension   = 0.96f;
            reachHandling.grips[1].assignedLimb      = ZHLN::CharacterBone::HandL;
            reachHandling.grips[1].localTransform    = JPH::Mat44::sTranslation(JPH::Vec3(0.05f, 0.0f, 0.0f));
            reachHandling.grips[1].evaluatedIKWeight = 1.0f;
            reachHandling.grips[1].maxArmExtension   = 0.96f;
            reachHandling.gripCount                  = 2;
            ZHLN::Animation::ConstrainItemToGripReach(reachHandling, reachMap.modelTransforms.data(), reachMap);
            const ZHLN::RigNodeIndex reachUpper = reachMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::UpperArmR)];
            const ZHLN::RigNodeIndex reachFore  = reachMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::ForearmR)];
            const ZHLN::RigNodeIndex reachHand  = reachMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::HandR)];
            const float              maximumReach =
                ((reachMap.modelTransforms[reachFore].GetTranslation() - reachMap.modelTransforms[reachUpper].GetTranslation()).Length() +
                 (reachMap.modelTransforms[reachHand].GetTranslation() - reachMap.modelTransforms[reachFore].GetTranslation()).Length()) *
                reachHandling.grips[0].maxArmExtension;
            const float              constrainedDistance = ((reachHandling.itemModelTransform * reachHandling.grips[0].localTransform).GetTranslation() -
                                                            reachMap.modelTransforms[reachUpper].GetTranslation())
                                                               .Length();
            const ZHLN::RigNodeIndex reachUpperL         = reachMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::UpperArmL)];
            const ZHLN::RigNodeIndex reachForeL          = reachMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::ForearmL)];
            const ZHLN::RigNodeIndex reachHandL          = reachMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::HandL)];
            const float              maximumReachL =
                ((reachMap.modelTransforms[reachForeL].GetTranslation() - reachMap.modelTransforms[reachUpperL].GetTranslation()).Length() +
                 (reachMap.modelTransforms[reachHandL].GetTranslation() - reachMap.modelTransforms[reachForeL].GetTranslation()).Length()) *
                reachHandling.grips[1].maxArmExtension;
            const float constrainedDistanceL = ((reachHandling.itemModelTransform * reachHandling.grips[1].localTransform).GetTranslation() -
                                                reachMap.modelTransforms[reachUpperL].GetTranslation())
                                                   .Length();
            if (constrainedDistance > maximumReach + 0.0001f || constrainedDistanceL > maximumReachL + 0.0001f) {
                return std::unexpected(ProceduralAnimationTestError::GaitInvariantFailed);
            }

            ZHLN::RigBoneMap fingerMap;
            fingerMap.nodeCount                                               = 4;
            fingerMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::HandL)] = 0;
            fingerMap.modelTransforms[0]                                      = JPH::Mat44::sIdentity();
            fingerMap.modelTransforms[1]                                      = JPH::Mat44::sTranslation(JPH::Vec3(0.10f, 0.0f, 0.0f));
            fingerMap.modelTransforms[2]                                      = JPH::Mat44::sTranslation(JPH::Vec3(0.18f, 0.0f, 0.0f));
            fingerMap.modelTransforms[3]                                      = JPH::Mat44::sTranslation(JPH::Vec3(0.24f, 0.0f, 0.0f));
            fingerMap.fingerJointConstraintCount                              = 3;
            for (size_t segment = 0; segment < 3; ++segment) {
                auto& constraint          = fingerMap.fingerJointConstraints[segment];
                constraint.parent         = segment;
                constraint.child          = segment + 1;
                constraint.digit          = ZHLN::FingerDigit::Index;
                constraint.side           = 0;
                constraint.segment        = static_cast<uint8_t>(segment);
                constraint.repairRelation = true;
                constraint.bindRelative   = fingerMap.modelTransforms[segment].Inversed() * fingerMap.modelTransforms[segment + 1];
            }
            fingerMap.modelTransforms[0] = JPH::Mat44::sRotationTranslation(JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), 0.4f), JPH::Vec3(0.3f, 1.0f, -0.2f));
            if (ZHLN::ProceduralAnimation::ApplyFingerRelationConstraints(fingerMap) != 3 ||
                !fingerMap.modelTransforms[3].IsClose(fingerMap.modelTransforms[2] * fingerMap.fingerJointConstraints[2].bindRelative, 0.0001f)) {
                return std::unexpected(ProceduralAnimationTestError::GaitInvariantFailed);
            }

            const JPH::Quat hingeTarget = (JPH::Quat::sRotation(JPH::Vec3::sAxisY(), 0.5f) * JPH::Quat::sRotation(JPH::Vec3::sAxisX(), 1.2f)).Normalized();
            const JPH::Quat hinge = ZHLN::Animation::ConstrainFingerHingeRotation(JPH::Quat::sIdentity(), hingeTarget, JPH::Vec3::sAxisX(), 1.0f, 0.70f, 0.10f);
            JPH::Vec3       hingeAxis;
            float           hingeAngle = 0.0f;
            hinge.GetAxisAngle(hingeAxis, hingeAngle);
            if (std::abs(hingeAngle - 0.70f) > 0.001f || std::abs(std::abs(hingeAxis.Dot(JPH::Vec3::sAxisX())) - 1.0f) > 0.001f) {
                return std::unexpected(ProceduralAnimationTestError::GaitInvariantFailed);
            }
            const JPH::Quat backwards = ZHLN::Animation::ConstrainFingerHingeRotation(
                JPH::Quat::sIdentity(), JPH::Quat::sRotation(JPH::Vec3::sAxisX(), -0.5f), JPH::Vec3::sAxisX(), 1.0f, 0.70f, 0.0f
            );
            JPH::Vec3 backwardsAxis;
            float     backwardsAngle = 0.0f;
            backwards.GetAxisAngle(backwardsAxis, backwardsAngle);
            if (std::abs(backwardsAngle) > 0.0001f) {
                return std::unexpected(ProceduralAnimationTestError::GaitInvariantFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::Error> non_skinned_attachments_follow_evaluated_nodes() {
            ZHLN::ECS::Registry registry;
            registry.RegisterAllComponentsIn<ZHLN::Components>();
            const ZHLN::Entity root       = registry.Create();
            const ZHLN::Entity attachment = registry.Create(
                ZHLN::Components::TransformComponent {}, ZHLN::Components::MeshComponent {.nodeIndex = 2}, ZHLN::Components::HierarchyComponent {.parent = root}
            );
            const ZHLN::Entity skinned = registry.Create(
                ZHLN::Components::TransformComponent {.position = JPH::Vec3(9.0f, 9.0f, 9.0f)}, ZHLN::Components::MeshComponent {.nodeIndex = 2},
                ZHLN::Components::SkeletalMeshComponent {.skeletonIndex = 0}, ZHLN::Components::HierarchyComponent {.parent = root}
            );

            ZHLN::RigBoneMap map;
            map.nodeCount = 3;
            const JPH::Vec3 expectedPosition(0.3f, 1.4f, -0.2f);
            const JPH::Vec3 expectedScale(1.2f, 0.8f, 1.1f);
            const JPH::Quat expectedRotation = JPH::Quat::sRotation(JPH::Vec3::sAxisY(), 0.35f);
            map.modelTransforms[2]           = JPH::Mat44::sRotationTranslation(expectedRotation, expectedPosition).PreScaled(expectedScale);

            const size_t synchronizedCount   = ZHLN::ProceduralAnimation::SyncNonSkinnedAttachments(registry, root, map);
            const auto*  attachmentTransform = registry.Get<ZHLN::Components::TransformComponent>(attachment);
            const auto*  skinnedTransform    = registry.Get<ZHLN::Components::TransformComponent>(skinned);
            if (synchronizedCount != 1 || attachmentTransform == nullptr || !attachmentTransform->position.IsClose(expectedPosition, 0.0001f) ||
                !attachmentTransform->scale.IsClose(expectedScale, 0.0001f) ||
                !(attachmentTransform->rotation * JPH::Vec3::sAxisZ()).IsClose(expectedRotation * JPH::Vec3::sAxisZ(), 0.0001f) ||
                skinnedTransform == nullptr || !skinnedTransform->position.IsClose(JPH::Vec3(9.0f, 9.0f, 9.0f), 0.0001f)) {
                return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::Error> real_glb_foot_descendants_follow_pose() {
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
                prefab.nodes[node].localTransform = JPH::Mat44(
                    JPH::Vec4(matrix[0], matrix[1], matrix[2], matrix[3]), JPH::Vec4(matrix[4], matrix[5], matrix[6], matrix[7]),
                    JPH::Vec4(matrix[8], matrix[9], matrix[10], matrix[11]), JPH::Vec4(matrix[12], matrix[13], matrix[14], matrix[15])
                );
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
                        .inverseBindMatrix = JPH::Mat44::sIdentity(),
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
                std::vector<JPH::Mat44> before;
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
                JPH::Mat44         before;
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
                map.localTransforms[footNode] = map.localTransforms[footNode] * JPH::Mat44::sRotation(JPH::Quat::sRotation(JPH::Vec3::sAxisX(), 0.3f));
            }
            ZHLN::ProceduralAnimation::ResolveModelTransforms(map);
            for (const SkinPaletteCheck& check: paletteChecks) {
                const ZHLN::Skeleton&   skeleton = importedSkeletons[check.skinIndex];
                std::vector<JPH::Mat44> after(skeleton.joints.size());
                ZHLN::ProceduralAnimation::BuildSkinningPalette(skeleton, map, after);
                for (size_t footJoint: check.footJoints) {
                    const bool changed = !(after[footJoint].GetColumn3(1) - check.before[footJoint].GetColumn3(1)).IsNearZero(0.0001f) ||
                                         !(after[footJoint].GetColumn3(2) - check.before[footJoint].GetColumn3(2)).IsNearZero(0.0001f);
                    if (!changed) {
                        return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
                    }
                }
            }
            for (const Descendant& descendant: descendants) {
                const JPH::Mat44& after = map.modelTransforms[descendant.node];
                const bool        moved = !(after.GetTranslation() - descendant.before.GetTranslation()).IsNearZero(0.0001f) ||
                                          !(after.GetColumn3(1) - descendant.before.GetColumn3(1)).IsNearZero(0.0001f);
                if (!moved) {
                    return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
                }
            }

            if (ZHLN::ProceduralAnimation::SyncNonSkinnedAttachments(registry, root, map) != descendants.size()) {
                return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
            }
            for (const Descendant& descendant: descendants) {
                const auto* transform = registry.Get<ZHLN::Components::TransformComponent>(descendant.entity);
                if (transform == nullptr || !transform->position.IsClose(map.modelTransforms[descendant.node].GetTranslation(), 0.0001f)) {
                    return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
                }
            }
            return {};
        }

        std::expected<void, ZHLN::Error> authored_upper_body_can_suppress_procedural_layers() {
            ZHLN::RigBoneMap map;
            ZHLN::BuildStandardProceduralRig(map);
            const ZHLN::RigNodeIndex armNode    = map.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::UpperArmL)];
            const ZHLN::RigNodeIndex headNode   = map.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::Head)];
            const JPH::Mat44         armBefore  = map.modelTransforms[armNode];
            const JPH::Mat44         headBefore = map.modelTransforms[headNode];

            ZHLN::ProceduralLocomotionComponent gait;
            gait.phase            = 0.25f;
            gait.previousVelocity = JPH::Vec3(0.0f, 0.0f, 2.0f);
            ZHLN::ProceduralLookAtComponent lookAt {
                .targetWorldPos = headBefore.GetTranslation() + JPH::Vec3(1.0f, 0.2f, 2.0f),
                .weight         = 1.0f,
            };

            ZHLN::Animation::SolveUpperBody(gait, &lookAt, JPH::Vec3::sZero(), JPH::Quat::sIdentity(), map.modelTransforms.data(), map, false, false);
            if (!map.modelTransforms[armNode].IsClose(armBefore, 0.0001f) || !map.modelTransforms[headNode].IsClose(headBefore, 0.0001f)) {
                return std::unexpected(ProceduralAnimationTestError::GaitInvariantFailed);
            }

            ZHLN::Animation::SolveUpperBody(gait, &lookAt, JPH::Vec3::sZero(), JPH::Quat::sIdentity(), map.modelTransforms.data(), map, true, true);
            if (map.modelTransforms[armNode].IsClose(armBefore, 0.0001f) || map.modelTransforms[headNode].IsClose(headBefore, 0.0001f)) {
                return std::unexpected(ProceduralAnimationTestError::GaitInvariantFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::Error> acceleration_tilt_preserves_com_radius() {
            ZHLN::RigBoneMap map;
            ZHLN::BuildStandardProceduralRig(map);
            const ZHLN::RigNodeIndex headNode   = map.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::Head)];
            const JPH::Vec3          headBefore = map.modelTransforms[headNode].GetTranslation();

            ZHLN::ProceduralLocomotionComponent gait;
            gait.forwardLean = 0.18f;
            gait.lateralBank = -0.11f;
            ZHLN::Animation::ApplyAccelerationTilt(gait, map.modelTransforms.data(), map);

            const JPH::Vec3 headAfter    = map.modelTransforms[headNode].GetTranslation();
            const float     beforeRadius = (headBefore - gait.centerOfMassModel).Length();
            const float     afterRadius  = (headAfter - gait.centerOfMassModel).Length();
            if ((headAfter - headBefore).LengthSq() < 1.0e-6f || std::abs(beforeRadius - afterRadius) > 0.0001f) {
                return std::unexpected(ProceduralAnimationTestError::GaitInvariantFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::Error> xpbd_hair_preserves_segment_lengths() {
            ZHLN::HairStrandsComponent hair;
            ZHLN::Animation::StepHairSimulation(hair, JPH::Vec3(0.0f, 2.0f, 0.0f), JPH::Quat::sIdentity(), JPH::Vec3(3.0f, 0.0f, 0.0f), 1.0f / 60.0f);

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

        std::expected<void, ZHLN::Error> xpbd_hair_returns_to_authored_bind_shape() {
            ZHLN::RigBoneMap map;
            ZHLN::BuildStandardProceduralRig(map);
            const ZHLN::RigNodeIndex headNode     = map.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::Head)];
            const JPH::Vec3          headPosition = map.modelTransforms[headNode].GetTranslation();

            // Author a deliberately horizontal first strand. A gravity-only
            // rope would collapse; shape and bend constraints must retain it.
            for (size_t link = 0; link < ZHLN::HairStrandsComponent::kLinksPerStrand; ++link) {
                const size_t             semantic = ZHLN::BoneSlot(ZHLN::CharacterBone::HairStart) + link;
                const ZHLN::RigNodeIndex node     = map.nodeIndices[semantic];
                map.modelTransforms[node]         = JPH::Mat44::sTranslation(headPosition + JPH::Vec3(0.15f + static_cast<float>(link) * 0.08f, 0.08f, 0.0f));
            }

            ZHLN::HairStrandsComponent hair;
            ZHLN::Animation::ConfigureHairBindPose(hair, map.modelTransforms.data(), map);
            for (uint32_t frame = 0; frame < 120; ++frame) {
                ZHLN::Animation::StepHairSimulation(hair, headPosition, JPH::Quat::sIdentity(), JPH::Vec3::sZero(), 1.0f / 60.0f);
            }

            constexpr size_t tip         = ZHLN::HairStrandsComponent::kLinksPerStrand - 1;
            const JPH::Vec3  authoredTip = headPosition + hair.restLocalPositions[tip];
            if ((hair.positions[tip] - authoredTip).Length() > 0.04f) {
                return std::unexpected(ProceduralAnimationTestError::HairConstraintFailed);
            }
            return {};
        }
    };
};

int main() {
    return ZHLN::Test::Runner::Run<ProceduralAnimationTestSuite>();
}
