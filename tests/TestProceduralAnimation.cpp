// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/ModelPrefab.hpp>
#include <Zahlen/ProceduralAnimation.hpp>
#include <Zahlen/SkeletalAnimation.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <array>
#include <cmath>
#include <cstdio>
#include <expected>
#include <numbers>
#include <string_view>

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
                if (map.nodeIndices[i] < 0) {
                    return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
                }
            }
            for (size_t i = static_cast<size_t>(ZHLN::CharacterBone::HairStart); i < ZHLN::kBoneCount; ++i) {
                if (map.nodeIndices[i] < 0) {
                    return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
                }
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
                } else if (node >= static_cast<size_t>(ZHLN::CharacterBone::HairStart)) {
                    const size_t particle = node - static_cast<size_t>(ZHLN::CharacterBone::HairStart);
                    char         hairName[32] {};
                    std::snprintf(
                        hairName, sizeof(hairName), "DEF-Hair_S%02zu_%02zu", particle / ZHLN::HairStrandsComponent::kLinksPerStrand + 1,
                        particle % ZHLN::HairStrandsComponent::kLinksPerStrand + 1
                    );
                    prefab.nodes[node].name = ZHLN::String64(hairName);
                }
                prefab.nodes[node].parentIndex    = map.parentIndices[node];
                prefab.nodes[node].localTransform = map.bindLocalTransforms[node];
                if (node < ZHLN::kCoreBoneCount || node >= static_cast<size_t>(ZHLN::CharacterBone::HairStart)) {
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
            const size_t malformedNode              = static_cast<size_t>(ZHLN::CharacterBone::ToeR);
            prefab.nodes[malformedNode].parentIndex = static_cast<int32_t>(ZHLN::kMaxRigNodes + 7);

            ZHLN::RigBoneMap importedMap;
            if (!ZHLN::BuildBoneMap(prefab, skeleton, importedMap) || importedMap.parentIndices[malformedNode] != -1 ||
                importedMap.nodeIndices[static_cast<size_t>(ZHLN::CharacterBone::Spine)] != 3 ||
                importedMap.nodeIndices[static_cast<size_t>(ZHLN::CharacterBone::SupSpine)] != 2 ||
                importedMap.nodeIndices[static_cast<size_t>(ZHLN::CharacterBone::UpperArmL)] != 8 ||
                importedMap.nodeIndices[static_cast<size_t>(ZHLN::CharacterBone::ForearmL)] != 7 ||
                importedMap.nodeIndices[static_cast<size_t>(ZHLN::CharacterBone::UpperArmR)] != 11 ||
                importedMap.nodeIndices[static_cast<size_t>(ZHLN::CharacterBone::ForearmR)] != 10) {
                return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
            }
            for (size_t slot = 0; slot < ZHLN::HairStrandsComponent::kTotalParticles; ++slot) {
                const size_t semantic = static_cast<size_t>(ZHLN::CharacterBone::HairStart) + slot;
                if (importedMap.nodeIndices[semantic] != static_cast<int32_t>(semantic)) {
                    return std::unexpected(ProceduralAnimationTestError::RigMappingFailed);
                }
            }
            return {};
        }

        std::expected<void, ZHLN::Error> gait_clock_tracks_distance() {
            ZHLN::ProceduralLocomotionComponent gait;
            gait.strideLength = 1.60f;

            ZHLN::Animation::EvaluateGait(gait, JPH::Vec3(0.0f, 0.0f, 4.0f), 0.25f);
            const bool phaseIsDistanceDriven = std::abs(gait.phase - 0.625f) < 0.0001f;
            const bool feetAreOpposed        = gait.plantWeightL != gait.plantWeightR;
            const bool accelerationIsFinite  = std::isfinite(gait.directionalAcceleration.GetZ());
            const bool wheelTracksPhase      = std::abs(gait.strideWheelAngle - gait.phase * 2.0f * std::numbers::pi_v<float>) < 0.0001f;
            const bool passReachPartition    = std::abs(gait.passWeightL + gait.reachWeightL - 1.0f) < 0.0001f;
            const bool twoKeySynchronization = std::abs(ZHLN::Animation::EvaluateTwoKeyPosePhase(0.0f)) < 0.0001f &&
                                               std::abs(ZHLN::Animation::EvaluateTwoKeyPosePhase(0.25f) - 0.5f) < 0.0001f &&
                                               std::abs(ZHLN::Animation::EvaluateTwoKeyPosePhase(0.5f) - 1.0f) < 0.0001f &&
                                               std::abs(ZHLN::Animation::EvaluateTwoKeyPosePhase(0.75f) - 0.5f) < 0.0001f;
            if (!phaseIsDistanceDriven || !feetAreOpposed || !accelerationIsFinite || !wheelTracksPhase || !passReachPartition || !twoKeySynchronization) {
                return std::unexpected(ProceduralAnimationTestError::GaitInvariantFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::Error> gravity_bounce_flattens_with_speed() {
            ZHLN::ProceduralLocomotionComponent gait;
            gait.strideLength        = 1.40f;
            gait.bounceGravity       = 9.81f;
            gait.maxBounceFlightTime = 0.36f;

            const float slowSpeed    = 1.0f;
            const float slowInterval = gait.strideLength / (2.0f * slowSpeed);
            gait.phase               = (0.18f / slowInterval) * 0.5f;
            const float slowApex     = ZHLN::Animation::EvaluateGravityBounce(gait, slowSpeed);

            const float fastSpeed    = 4.0f;
            const float fastInterval = gait.strideLength / (2.0f * fastSpeed);
            gait.phase               = 0.25f; // Midpoint of the shortened support interval.
            const float fastApex     = ZHLN::Animation::EvaluateGravityBounce(gait, fastSpeed);

            constexpr float      sampleStep = 0.06f;
            std::array<float, 3> samples {};
            for (size_t i = 0; i < samples.size(); ++i) {
                const float elapsed = 0.12f + sampleStep * static_cast<float>(i);
                gait.phase          = (elapsed / slowInterval) * 0.5f;
                samples[i]          = ZHLN::Animation::EvaluateGravityBounce(gait, slowSpeed);
            }
            const float measuredAcceleration = (samples[2] - 2.0f * samples[1] + samples[0]) / (sampleStep * sampleStep);
            if (!(slowApex > fastApex) || std::abs(measuredAcceleration + gait.bounceGravity) > 0.01f) {
                return std::unexpected(ProceduralAnimationTestError::GaitInvariantFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::Error> acceleration_tilt_preserves_com_radius() {
            ZHLN::RigBoneMap map;
            ZHLN::BuildStandardProceduralRig(map);
            const int32_t   headNode   = map.nodeIndices[static_cast<size_t>(ZHLN::CharacterBone::Head)];
            const JPH::Vec3 headBefore = map.modelTransforms[static_cast<size_t>(headNode)].GetTranslation();

            ZHLN::ProceduralLocomotionComponent gait;
            gait.forwardLean = 0.18f;
            gait.lateralBank = -0.11f;
            ZHLN::Animation::ApplyAccelerationTilt(gait, map.modelTransforms.data(), map);

            const JPH::Vec3 headAfter    = map.modelTransforms[static_cast<size_t>(headNode)].GetTranslation();
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
            const int32_t   headNode     = map.nodeIndices[static_cast<size_t>(ZHLN::CharacterBone::Head)];
            const JPH::Vec3 headPosition = map.modelTransforms[static_cast<size_t>(headNode)].GetTranslation();

            // Author a deliberately horizontal first strand. A gravity-only
            // rope would collapse; shape and bend constraints must retain it.
            for (size_t link = 0; link < ZHLN::HairStrandsComponent::kLinksPerStrand; ++link) {
                const size_t  semantic = static_cast<size_t>(ZHLN::CharacterBone::HairStart) + link;
                const int32_t node     = map.nodeIndices[semantic];
                map.modelTransforms[static_cast<size_t>(node)] =
                    JPH::Mat44::sTranslation(headPosition + JPH::Vec3(0.15f + static_cast<float>(link) * 0.08f, 0.08f, 0.0f));
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
