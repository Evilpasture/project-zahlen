// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/ModelPrefab.hpp>
#include <Zahlen/SkeletalAnimation.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <array>
#include <cgltf.h>
#include <cmath>
#include <cstdio>
#include <expected>
#include <fstream>
#include <iostream>
#include <memory>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

import ZHLN.ProceduralAnimation;

enum class ProceduralAnimationTestError : uint32_t {
    Success = 0,
    RigMappingFailed[[= ZHLN::Reflect::Description("The generated procedural rig did not map all core and secondary controls.")]],
    GaitInvariantFailed[[= ZHLN::Reflect::Description("The distance-driven gait clock or alternating foot phases violated its invariant.")]],
    HairConstraintFailed[[= ZHLN::Reflect::Description("The XPBD hair solver produced a non-finite or excessively stretched segment.")]],
};

struct ProceduralAnimationTestSuite {
    ProceduralAnimationTestSuite() {
        ZHLN::TaskSystem::Init(2, 32, 131072);
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

        std::expected<void, ZHLN::Error> standard_rig_maps_every_control() {
            ZHLN::RigBoneMap map;
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

            ZHLN::RigBoneMap importedMap;
            if (!ZHLN::BuildBoneMap(prefab, skeleton, importedMap) || importedMap.parentIndices[malformedNode] != ZHLN::InvalidRigNode ||
                importedMap.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::Spine)] != 3 ||
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

            const cgltf_skin& sourceSkin = data->skins[0];
            ZHLN::Skeleton    skeleton;
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

            ZHLN::RigBoneMap map;
            ZHLN::BuildBoneMap(prefab, skeleton, map);
            const std::array<ZHLN::RigNodeIndex, 2> footNodes = {
                map.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::FootL)],
                map.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::FootR)],
            };
            if (!ZHLN::IsValidRigNode(footNodes[0], map.nodeCount) || !ZHLN::IsValidRigNode(footNodes[1], map.nodeCount)) {
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
