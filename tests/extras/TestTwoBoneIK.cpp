// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Animation/Context.hpp>
#include <Animation/AnimationSystem.hpp>
#include <Animation/Components.hpp>
#include <Animation/IK.hpp>
#include <Animation/PrefabRig.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/ILogicWorld.hpp>
#include <Zahlen/COM/LogicWorldFactory.hpp>
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Math/Matrix.hpp>
#include <Zahlen/Math/Quat.hpp>
#include <Zahlen/Math/Transform.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <array>
#include <cmath>
#include <cstdint>
#include <expected>
#include <vector>

namespace {

using ZHLN::FVec3;
using ZHLN::Mat4x4;
using ZHLN::Quat;

[[nodiscard]] auto MakeThreeBoneArmRig() -> ZHLN::Animation::PrefabRig {
    ZHLN::Animation::PrefabRig rig;
    rig.nodes = {
        {.name = ZHLN::String64("UpperArm"), .parentIndex = -1, .localTransform = ZHLN::ComposeTransform({0.0f, 0.0f, 0.0f}, Quat::Identity(), FVec3::One())},
        {.name = ZHLN::String64("ForeArm"),  .parentIndex = 0,  .localTransform = ZHLN::ComposeTransform({0.0f, -0.5f, 0.0f}, Quat::Identity(), FVec3::One())},
        {.name = ZHLN::String64("Hand"),     .parentIndex = 1,  .localTransform = ZHLN::ComposeTransform({0.0f, -0.5f, 0.0f}, Quat::Identity(), FVec3::One())},
        {.name = ZHLN::String64("Finger"),   .parentIndex = 2,  .localTransform = ZHLN::ComposeTransform({0.0f, -0.1f, 0.0f}, Quat::Identity(), FVec3::One())},
    };
    ZHLN::Skeleton skel;
    for (int32_t i = 0; i < 4; ++i) {
        Mat4x4 model = rig.nodes[static_cast<size_t>(i)].localTransform;
        for (int32_t p = rig.nodes[static_cast<size_t>(i)].parentIndex; p >= 0; p = rig.nodes[static_cast<size_t>(p)].parentIndex) {
            model = rig.nodes[static_cast<size_t>(p)].localTransform * model;
        }
        skel.joints.push_back(ZHLN::Joint {
            .name              = rig.nodes[static_cast<size_t>(i)].name,
            .parentIndex       = rig.nodes[static_cast<size_t>(i)].parentIndex,
            .nodeIndex         = i,
            .inverseBindMatrix = model.Inversed(),
        });
    }
    rig.skeletons.push_back(std::move(skel));

    ZHLN::AnimationClip clip;
    clip.name     = ZHLN::String64("Idle");
    clip.duration = 1.0f;
    ZHLN::AnimationChannel ch;
    ch.targetNodeIndex = 0;
    ch.path            = ZHLN::AnimationPathType::Translation;
    ch.keyTimes      = {0.0f, 1.0f};
    ch.keyValues          = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    clip.channels.push_back(std::move(ch));
    rig.animations.push_back(std::move(clip));
    return rig;
}

struct TwoBoneIKTestSuite {
    struct Tests {
        std::expected<void, ZHLN::ErrorCode> two_bone_ik_solver() {
            const auto solved = ZHLN::IK::SolveTwoBone({
                .upperPosition  = {0.0f, 1.0f, 0.0f},
                .targetPosition = {0.0f, 0.2f, 0.0f},
                .poleVector     = {0.0f, 0.0f, 1.0f},
                .upperLength    = 0.5f,
                .lowerLength    = 0.5f,
            });
            ZHLN::Test::ExpectTrue(solved.valid);
            ZHLN::Test::ExpectFalse(solved.reachClamped);
            ZHLN::Test::ExpectTrue(std::abs((solved.midPosition - FVec3 {0.0f, 1.0f, 0.0f}).Length() - 0.5f) < 1e-4f);
            ZHLN::Test::ExpectTrue(std::abs((solved.endPosition - solved.midPosition).Length() - 0.5f) < 1e-4f);
            ZHLN::Test::ExpectTrue(solved.midPosition.z > 0.25f);

            const auto clamped = ZHLN::IK::SolveTwoBone({
                .upperPosition  = {0.0f, 0.0f, 0.0f},
                .targetPosition = {0.0f, 5.0f, 0.0f},
                .poleVector     = {0.0f, 0.0f, 1.0f},
                .upperLength    = 0.5f,
                .lowerLength    = 0.5f,
            });
            ZHLN::Test::ExpectTrue(clamped.valid && clamped.reachClamped);
            ZHLN::Test::ExpectTrue(clamped.solvedDistance < 1.0f);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> animation_context_generational_ids_and_playback() {
            constexpr ZHLN::AssetID kAsset {101};
            ZHLN::Animation::Context ctx;
            ctx.CacheRig(kAsset, MakeThreeBoneArmRig());

            const auto id1 = ctx.Create(kAsset);
            ZHLN::Test::ExpectTrue(ctx.Valid(id1));
            ZHLN::Test::ExpectEq(ctx.ActiveCount(), 1u);

            ctx.Sync(id1, {.prefabAsset = kAsset, .trackIndex = 0, .trackTime = 0.0f, .playbackSpeed = 1.0f, .loop = false});
            ctx.Step(0.4f);
            const auto state = ctx.Playback(id1);
            ZHLN::Test::ExpectTrue(static_cast<bool>(state) && std::abs(state->trackTime - 0.4f) < 1e-4f && !state->finished);

            ctx.Step(0.8f);
            const auto done = ctx.Playback(id1);
            ZHLN::Test::ExpectTrue(static_cast<bool>(done) && done->finished);

            const std::array dead {id1};
            ctx.Destroy(dead);
            ZHLN::Test::ExpectFalse(ctx.Valid(id1));
            const auto id2 = ctx.Create(kAsset);
            ZHLN::Test::ExpectTrue(ctx.Valid(id2) && id2 != id1);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> animation_context_ik_descendant_fk_and_pose_upload_sink() {
            constexpr ZHLN::AssetID kAsset {202};
            ZHLN::Animation::Context ctx;
            ctx.CacheRig(kAsset, MakeThreeBoneArmRig());

            const auto id = ctx.Create(kAsset);
            ctx.Sync(id, {.prefabAsset = kAsset, .trackIndex = -1});
            const std::array bindings {ZHLN::Animation::SkeletonBinding {.skeletonIndex = 0, .jointOffset = 8, .hasRagdoll = true}};
            ctx.BindSkeletons(id, bindings);

            const FVec3 target {0.0f, -0.6f, 0.25f};
            const std::array chains {ZHLN::Animation::IKChain {
                .upperNodeIndex = 0,
                .lowerNodeIndex = 1,
                .endNodeIndex   = 2,
                .targetPosition = target,
                .poleVector     = {0.0f, 0.0f, 1.0f},
                .weight         = 1.0f,
            }};
            ctx.SetIKChains(id, chains);

            struct CapturedUploads {
                uint32_t paletteCalls   = 0;
                uint32_t kinematicCalls = 0;
                uint32_t lastOffset     = 0;
                size_t   jointCount     = 0;
            } uploads;

            ctx.Step(1.0f / 60.0f, nullptr, nullptr, nullptr, ZHLN::Animation::PoseUploadCallback {
                .user = &uploads,
                .uploadPalette = +[](void* u, uint32_t off, std::span<const Mat4x4> m) {
                    auto* s = static_cast<CapturedUploads*>(u);
                    ++s->paletteCalls; s->lastOffset = off; s->jointCount = m.size();
                },
                .uploadKinematic = +[](void* u, uint32_t, std::span<const Mat4x4>) {
                    ++static_cast<CapturedUploads*>(u)->kinematicCalls;
                },
            });

            ZHLN::Test::ExpectEq(uploads.paletteCalls, 1u);
            ZHLN::Test::ExpectEq(uploads.kinematicCalls, 1u);
            ZHLN::Test::ExpectEq(uploads.lastOffset, 8u);
            ZHLN::Test::ExpectEq(uploads.jointCount, 4u);

            const auto pose = ctx.ModelPose(id);
            ZHLN::Test::ExpectEq(pose.size(), 4u);
            const FVec3 handPos   = ZHLN::DecomposeTransform(pose[2]).translation;
            const FVec3 fingerPos = ZHLN::DecomposeTransform(pose[3]).translation;
            ZHLN::Test::ExpectTrue((handPos - target).Length() < 1e-3f);
            ZHLN::Test::ExpectTrue(std::abs((fingerPos - handPos).Length() - 0.1f) < 1e-3f);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> two_bone_ik_ecs_step_tracks_world_target_entity() {
            constexpr ZHLN::AssetID kAsset {303};
            auto worldRes = ZHLN::COM::CreateLogicWorld();
            ZHLN::Test::ExpectTrue(worldRes.has_value());
            auto& entities = *(*worldRes)->Entities();

            ZHLN::Animation::Context ctx;
            ctx.CacheRig(kAsset, MakeThreeBoneArmRig());

            const auto targetEntity = entities.Spawn(
                ZHLN::Components::TransformComponent {.position = {0.0f, -0.55f, 0.30f}}
            );
            ZHLN::IK::TwoBoneComponent ikComp {};
            ikComp.chainCount = 1;
            ikComp.chains[0].upperNodeIndex = 0;
            ikComp.chains[0].lowerNodeIndex = 1;
            ikComp.chains[0].endNodeIndex   = 2;
            ikComp.chains[0].targetEntity   = targetEntity;
            ikComp.chains[0].poleVector     = {0.0f, 0.0f, 1.0f};
            ikComp.chains[0].weight         = 1.0f;
            const auto actor = entities.Spawn(
                ZHLN::Components::TransformComponent {},
                ZHLN::Animation::AnimatorComponent {.prefabAsset = kAsset, .currentTrackIdx = -1},
                ikComp
            );

            ZHLN::Animation::Step(entities, ctx, 1.0f / 60.0f);
            const auto anim = entities.Get<ZHLN::Animation::AnimatorComponent>(actor);
            ZHLN::Test::ExpectTrue(static_cast<bool>(anim) && anim->animationID.IsValid());
            const auto pose = ctx.ModelPose(anim->animationID);
            ZHLN::Test::ExpectEq(pose.size(), 4u);
            const FVec3 handPos = ZHLN::DecomposeTransform(pose[2]).translation;
            ZHLN::Test::ExpectTrue((handPos - FVec3 {0.0f, -0.55f, 0.30f}).Length() < 1e-3f);
            return {};
        }
    };
};

} // namespace
