// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Animation/Context.hpp>
#include <Animation/Components.hpp>
#include <Animation/PrefabRig.hpp>
#include <Animation/ProceduralMath.hpp>
#include <CharacterController/CharacterComponents.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/ILogicWorld.hpp>
#include <Zahlen/COM/LogicWorldFactory.hpp>
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Math/Matrix.hpp>
#include <Zahlen/Math/Quat.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Render/SkeletalAnimation.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <glTF/GLTFDocument.hpp>

import ZHLN.Locomotion;
import ZHLN.ProceduralAnimation;

#include <cmath>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using ZHLN::FVec3;
using ZHLN::Mat4x4;
using ZHLN::Quat;

[[nodiscard]] auto RigFromDocument(const ZHLN::GLTF::Document& doc) -> ZHLN::Animation::PrefabRig {
    ZHLN::Animation::PrefabRig rig;
    rig.nodes      = doc.nodes;
    rig.skeletons  = doc.skeletons;
    rig.animations = doc.animations;
    rig.parts.reserve(doc.parts.size());
    for (const auto& part : doc.parts) {
        if (part.primitive >= doc.primitives.size()) continue;
        const auto& prim = doc.primitives[part.primitive];
        rig.parts.push_back(ZHLN::COM::PrefabPartBoundsInfo {
            .nodeIndex      = part.nodeIndex,
            .localTransform = Mat4x4::Identity(),
            .localMin       = prim.localMin,
            .localMax       = prim.localMax,
        });
    }
    return rig;
}

struct ProceduralAnimationTestSuite {
    ZHLN::TaskSystem::Scope taskScope {2, 32};

    struct Tests {
        std::expected<void, ZHLN::ErrorCode> rig_bone_map_defaults_and_reset_are_valid() {
            ZHLN::RigBoneMap rig {};
            ZHLN::Test::ExpectFalse(rig.initialized);
            ZHLN::Test::ExpectEq(rig.nodeIndices[ZHLN::BoneSlot(ZHLN::CharacterBone::Hips)], ZHLN::InvalidRigNode);
            rig.nodeCount   = 3;
            rig.initialized = true;
            rig.parentIndices[0] = ZHLN::InvalidRigNode;
            rig.parentIndices[1] = 0;
            rig.parentIndices[2] = 1;
            rig.localTransforms[0] = ZHLN::ComposeTransform({0.0f, 1.0f, 0.0f}, Quat::Identity(), FVec3::One());
            rig.localTransforms[1] = ZHLN::ComposeTransform({0.0f, 0.5f, 0.0f}, Quat::Identity(), FVec3::One());
            rig.localTransforms[2] = ZHLN::ComposeTransform({0.25f, 0.0f, 0.0f}, Quat::Identity(), FVec3::One());
            ZHLN::ProceduralAnimation::ResolveModelTransforms(rig);
            ZHLN::Test::ExpectTrue(std::abs(rig.modelTransforms[2].Column(3).x - 0.25f) < 1e-4f);
            ZHLN::Test::ExpectTrue(std::abs(rig.modelTransforms[2].Column(3).y - 1.50f) < 1e-4f);
            rig.Reset();
            ZHLN::Test::ExpectFalse(rig.initialized);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> imported_bounds_fit_virtual_character_hull() {
            ZHLN::Animation::PrefabRig rig;
            rig.nodes.push_back({.name = ZHLN::String64("Body"), .parentIndex = -1, .localTransform = Mat4x4::Identity(), .hasMesh = true});
            rig.parts.push_back({
                .nodeIndex      = 0,
                .localTransform = Mat4x4::Identity(),
                .localMin       = {-0.28f, 0.0f, -0.22f},
                .localMax       = { 0.28f, 1.72f,  0.22f},
            });
            const auto bounds = ZHLN::Locomotion::MeasurePrefabBounds(rig);
            ZHLN::Test::ExpectTrue(bounds.valid);
            ZHLN::Test::ExpectTrue(std::abs(bounds.GetSize().y - 1.72f) < 1e-4f);
            const auto hull = ZHLN::Locomotion::FitDualShapeToBounds(bounds);
            ZHLN::Test::ExpectTrue(hull.lifterRadius > 0.05f && hull.bumperRadiusY > 0.1f);
            const float top = hull.GetBumperOffsetY() + hull.bumperRadiusY;
            ZHLN::Test::ExpectTrue(top > 1.2f && top < 2.2f);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> base_rig_glb_decodes_maps_and_evaluates_via_com() {
            const std::filesystem::path glbPath =
                std::filesystem::path(ZHLN_TEST_SOURCE_DIR) / "resources/assets/ProceduralAnimationBaseRig.glb";
            if (!std::filesystem::exists(glbPath)) {
                return {};
            }
            const auto doc = ZHLN::GLTF::Read(glbPath.string());
            ZHLN::Test::ExpectTrue(doc.has_value());
            if (!doc) return {};
            const auto rig = RigFromDocument(*doc);
            ZHLN::Test::ExpectTrue(!rig.nodes.empty() && !rig.skeletons.empty());
            const auto bounds = ZHLN::Locomotion::MeasurePrefabBounds(rig);
            ZHLN::Test::ExpectTrue(bounds.valid && bounds.GetSize().y > 0.5f);

            auto worldRes = ZHLN::COM::CreateLogicWorldRef();
            ZHLN::Test::ExpectTrue(worldRes.has_value());
            auto& entities = *(*worldRes)->Entities();
            constexpr ZHLN::AssetID kRigAsset {909};
            ZHLN::Animation::Context animCtx;
            animCtx.CacheRig(kRigAsset, rig);

            const auto actor = entities.Spawn(
                ZHLN::Components::TransformComponent {.position = {0.0f, 0.0f, 0.0f}, .rotation = Quat::Identity(), .scale = FVec3::One()},
                ([&]{ ZHLN::Character::MovementComponent m{}; m.speed = 3.0f; m.inputZ = 1.0f; m.isGrounded = true; return m; }()),
                ZHLN::Animation::AnimatorComponent {.prefabAsset = kRigAsset, .currentTrackIdx = 0, .currentLoop = true},
                ZHLN::RigBoneMap {},
                ZHLN::ProceduralLocomotionComponent {},
                ZHLN::ProceduralAnimationConfigComponent {}
            );
            uint32_t paletteUploads = 0;
            entities.Spawn(
                ZHLN::Components::HierarchyComponent {.parent = actor},
                ZHLN::Components::TransformComponent {},
                ZHLN::Components::MeshComponent {.nodeIndex = 0},
                ZHLN::Components::SkeletalMeshComponent {.jointOffset = 0, .skeletonIndex = 0}
            );
            for (int i = 0; i < 4; ++i) {
                ZHLN::ProceduralAnimation::Step(
                    entities, animCtx, 1.0f / 60.0f, (*worldRes)->Simulation(), nullptr, nullptr,
                    ZHLN::Animation::PoseUploadCallback {
                        .user = &paletteUploads,
                        .uploadPalette = +[](void* u, uint32_t, std::span<const Mat4x4>) { ++*static_cast<uint32_t*>(u); },
                    }
                );
            }
            const auto* mapped = entities.Get<ZHLN::RigBoneMap>(actor);
            const auto* gait   = entities.Get<ZHLN::ProceduralLocomotionComponent>(actor);
            ZHLN::Test::ExpectTrue(mapped != nullptr && mapped->initialized);
            ZHLN::Test::ExpectTrue(gait != nullptr && std::isfinite(gait->phase));
            ZHLN::Test::ExpectTrue(paletteUploads >= 1u);
            return {};
        }
    };
};

} // namespace
