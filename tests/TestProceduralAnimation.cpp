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
#include <string_view>

import ZHLN.Gait;
import ZHLN.SecondaryPhysics;

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
            constexpr std::array<std::string_view, ZHLN::kCoreBoneCount> coreNames = {
                "Root",      "Hips",   "Spine",   "Sup_Spine", "Chest",  "Neck",  "Head",    "Upper_Arm_L", "Forearm_L", "Hand_L", "Upper_Arm_R",
                "Forearm_R", "Hand_R", "Thigh_L", "Shin_L",    "Foot_L", "Toe_L", "Thigh_R", "Shin_R",      "Foot_R",    "Toe_R",
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
            if (!ZHLN::BuildBoneMap(prefab, skeleton, importedMap) || importedMap.parentIndices[malformedNode] != -1) {
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
            if (!phaseIsDistanceDriven || !feetAreOpposed || !accelerationIsFinite) {
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
    };
};

int main() {
    return ZHLN::Test::Runner::Run<ProceduralAnimationTestSuite>();
}
