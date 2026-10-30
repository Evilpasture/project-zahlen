// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Animation/AnimationContext.hpp>
#include <Animation/AnimationSystem.hpp>
#include <Animation/IK.hpp>
#include <Zahlen/AssetManager.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/ECS/ExtensionServices.hpp>
#include <Zahlen/Frame.hpp>
#include <Zahlen/ModelPrefab.hpp>
#include <Zahlen/PoseUploads.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <Zahlen/ECS/SystemGraph.hpp>
#include <array>
#include <cmath>
#include <expected>
#include <memory>
#include <vector>

enum class TwoBoneIKTestError : uint32_t {
    IKSolverFailed ZHLN_ANNOTATION(ZHLN::Description<"TwoBoneIK solver produced invalid joint orientations or positions."> {}) = 1,
    ExtensionServiceFailed ZHLN_ANNOTATION(ZHLN::Description<"ExtensionServices failed to emplace or resolve typed domain services through SystemGraph."> {}),
    HandleLifecycleFailed ZHLN_ANNOTATION(ZHLN::Description<"AnimationContext generational handle lifecycle or playback transition failed."> {}),
    RestingIKDrift ZHLN_ANNOTATION(ZHLN::Description<"AnimationContext IK drifted on a resting clip or failed to propagate descendant FK."> {}),
    ECSBridgeFailed ZHLN_ANNOTATION(ZHLN::Description<"Stateless Animation ECS systems failed to publish morphs, rigid nodes, or kinematic poses."> {}),
};

namespace {

struct TestCounterService {
    uint32_t ticks = 0;
};

struct UnregisteredService {
    uint32_t unused = 0;
};

struct ExtensionGraphServices {
    ZHLN::AssetManager&           assets;
    ZHLN::PoseUploadQueue&        poseUploads;
    ZHLN::ECS::ExtensionServices& extensions;
};

struct ServiceProbeSystem {
    static void Update(
        ZHLN::ECS::ResMut<TestCounterService>          counter,
        ZHLN::Optional<const TestCounterService&>      optPresent,
        ZHLN::Optional<UnregisteredService&>           optMissing
    ) {
        if (optPresent.has_value() && !optMissing.has_value()) {
            counter->ticks += 1u;
        }
    }
};

[[nodiscard]] auto MakeFourNodeLimbPrefab(std::string_view virtualPath) -> std::unique_ptr<ZHLN::ModelPrefab> {
    auto prefab         = std::make_unique<ZHLN::ModelPrefab>();
    prefab->virtualPath = virtualPath;

    // 0: Shoulder (0, 2, 0) -> 1: Elbow (0, 2, 2) -> 2: Hand (0, 2, 4) -> 3: FingerTip (0, 2, 4.5, rigid mesh)
    prefab->nodes.resize(4);
    prefab->nodes[0].name           = ZHLN::String64("Shoulder");
    prefab->nodes[0].parentIndex    = -1;
    prefab->nodes[0].localTransform = JPH::Mat44::sTranslation(JPH::Vec3(0.0f, 2.0f, 0.0f));

    prefab->nodes[1].name           = ZHLN::String64("Elbow");
    prefab->nodes[1].parentIndex    = 0;
    prefab->nodes[1].localTransform = JPH::Mat44::sTranslation(JPH::Vec3(0.0f, 0.0f, 2.0f));

    prefab->nodes[2].name           = ZHLN::String64("Hand");
    prefab->nodes[2].parentIndex    = 1;
    prefab->nodes[2].localTransform = JPH::Mat44::sTranslation(JPH::Vec3(0.0f, 0.0f, 2.0f));

    prefab->nodes[3].name           = ZHLN::String64("FingerTip");
    prefab->nodes[3].parentIndex    = 2;
    prefab->nodes[3].localTransform = JPH::Mat44::sTranslation(JPH::Vec3(0.0f, 0.0f, 0.5f));
    prefab->nodes[3].hasMesh        = true;

    ZHLN::Skeleton skel {};
    skel.name = ZHLN::String64("ArmRig");
    skel.joints.resize(3);
    skel.joints[0] = ZHLN::Joint {
        .name              = ZHLN::String64("Shoulder"),
        .parentIndex       = -1,
        .nodeIndex         = 0,
        .inverseBindMatrix = JPH::Mat44::sTranslation(JPH::Vec3(0.0f, -2.0f, 0.0f)),
    };
    skel.joints[1] = ZHLN::Joint {
        .name              = ZHLN::String64("Elbow"),
        .parentIndex       = 0,
        .nodeIndex         = 1,
        .inverseBindMatrix = JPH::Mat44::sTranslation(JPH::Vec3(0.0f, -2.0f, -2.0f)),
    };
    skel.joints[2] = ZHLN::Joint {
        .name              = ZHLN::String64("Hand"),
        .parentIndex       = 1,
        .nodeIndex         = 2,
        .inverseBindMatrix = JPH::Mat44::sTranslation(JPH::Vec3(0.0f, -2.0f, -4.0f)),
    };
    prefab->skeletons.push_back(std::move(skel));

    // Clip 0: non-looping 1.0 s clip moving shoulder slightly + driving 2 morph weights on node 3
    ZHLN::AnimationClip clip0 {};
    clip0.name     = ZHLN::String64("ReachAndMorph");
    clip0.duration = 1.0f;
    clip0.channels.push_back(
        ZHLN::AnimationChannel {
            .targetNodeIndex = 0,
            .path            = ZHLN::AnimationPathType::Translation,
            .interpolation   = ZHLN::InterpolationType::Linear,
            .keyTimes        = {0.0f, 1.0f},
            .keyValues       = {0.0f, 2.0f, 0.0f, 0.0f, 2.0f, 0.0f},
        }
    );
    clip0.channels.push_back(
        ZHLN::AnimationChannel {
            .targetNodeIndex = 3,
            .path            = ZHLN::AnimationPathType::Weights,
            .interpolation   = ZHLN::InterpolationType::Linear,
            .keyTimes        = {0.0f, 1.0f},
            .keyValues       = {0.0f, 0.0f, 1.0f, 0.75f},
        }
    );
    prefab->animations.push_back(std::move(clip0));

    // Clip 1: keyless degenerate morph channel on node 3
    ZHLN::AnimationClip clip1 {};
    clip1.name     = ZHLN::String64("KeylessMorph");
    clip1.duration = 1.0f;
    clip1.channels.push_back(
        ZHLN::AnimationChannel {
            .targetNodeIndex = 3,
            .path            = ZHLN::AnimationPathType::Weights,
            .interpolation   = ZHLN::InterpolationType::Linear,
            .keyTimes        = {},
            .keyValues       = {0.9f, 0.9f},
        }
    );
    prefab->animations.push_back(std::move(clip1));

    return prefab;
}

} // namespace

namespace {

struct TwoBoneIKTestSuite {
    TwoBoneIKTestSuite() {
        ZHLN::TaskSystem::Init(4, 32, ZHLN::kMinimumFiberStackSize);
    }

    ~TwoBoneIKTestSuite() {
        ZHLN::TaskSystem::Shutdown();
    }

    struct Tests {
        std::expected<void, ZHLN::ErrorCode> two_bone_ik_solver() {
            ZHLN::IK::TwoBoneIKSolverInput input {
                .upperPosition  = JPH::Vec3(0.0f, 2.0f, 0.0f),
                .targetPosition = JPH::Vec3(0.0f, 2.0f, 2.8284f),
                .poleVector     = JPH::Vec3(0.0f, 1.0f, 0.0f),
                .upperLength    = 2.0f,
                .lowerLength    = 2.0f,
            };

            ZHLN::IK::TwoBoneIKSolverOutput output = ZHLN::IK::SolveTwoBoneIK(input);
            if (!ZHLN::Test::ExpectTrue(output.valid) || !ZHLN::Test::ExpectGt(output.midPosition.GetY(), input.upperPosition.GetY())) {
                return std::unexpected(TwoBoneIKTestError::IKSolverFailed);
            }

            const float upperDist = (output.midPosition - input.upperPosition).Length();
            const float lowerDist = (output.endPosition - output.midPosition).Length();
            if (!ZHLN::Test::ExpectLt(std::abs(upperDist - 2.0f), 0.01f) || !ZHLN::Test::ExpectLt(std::abs(lowerDist - 2.0f), 0.01f)) {
                return std::unexpected(TwoBoneIKTestError::IKSolverFailed);
            }

            input.targetPosition = JPH::Vec3(0.0f, 2.0f, 10.0f);
            input.maxExtension   = 0.98f;
            output               = ZHLN::IK::SolveTwoBoneIK(input);
            if (!ZHLN::Test::ExpectTrue(output.valid) || !ZHLN::Test::ExpectTrue(output.reachClamped) ||
                !ZHLN::Test::ExpectLt(std::abs(output.solvedDistance - 3.92f), 0.001f)) {
                return std::unexpected(TwoBoneIKTestError::IKSolverFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> extension_services_resolve_through_system_graph() {
            ZHLN::AssetManager          assets;
            ZHLN::PoseUploadQueue       uploads;
            ZHLN::ECS::ExtensionServices extensions;
            auto&                       counter = extensions.Emplace<TestCounterService>(TestCounterService {.ticks = 3});

            const auto& constExt = extensions;
            if (!ZHLN::Test::ExpectTrue(constExt.Get<TestCounterService>().has_value()) ||
                !ZHLN::Test::ExpectFalse(constExt.Get<UnregisteredService>().has_value())) {
                return std::unexpected(TwoBoneIKTestError::ExtensionServiceFailed);
            }

            ZHLN::ECS::Registry registry;
            ExtensionGraphServices services {.assets = assets, .poseUploads = uploads, .extensions = extensions};
            ZHLN::ECS::SystemGraph<ExtensionGraphServices> graph(registry, services);
            graph.AddSystem<&ServiceProbeSystem::Update>();
            graph.Compile();
            graph.Execute(ZHLN::Frame {.dt = 1.0f / 60.0f});

            if (!ZHLN::Test::ExpectEq(counter.ticks, 4u)) {
                return std::unexpected(TwoBoneIKTestError::ExtensionServiceFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> animation_context_generational_handles_and_playback() {
            constexpr std::string_view kPath = "test_anim_ctx_handles.glb";
            const ZHLN::AssetID        id    = ZHLN::HashAssetPath(kPath);

            ZHLN::AssetManager assets;
            assets.CachePrefab(id, MakeFourNodeLimbPrefab(kPath));

            ZHLN::PoseUploadQueue             uploads;
            ZHLN::Animation::AnimationContext ctx;

            const ZHLN::AnimationHandle h0 = ctx.Create(id);
            const ZHLN::AnimationHandle h1 = ctx.Create(id);
            if (!ZHLN::Test::ExpectTrue(ctx.Valid(h0)) || !ZHLN::Test::ExpectTrue(ctx.Valid(h1)) ||
                !ZHLN::Test::ExpectEq(ctx.ActiveCount(), static_cast<size_t>(2))) {
                return std::unexpected(TwoBoneIKTestError::HandleLifecycleFailed);
            }

            // Destroy h0 -> slot recycled with bumped generation; h0 becomes stale, h1 remains valid after swap-and-pop.
            const std::array doomed {h0};
            ctx.Destroy(doomed);
            const ZHLN::AnimationHandle h2 = ctx.Create(id);
            if (!ZHLN::Test::ExpectFalse(ctx.Valid(h0)) || !ZHLN::Test::ExpectFalse(ctx.Playback(h0).has_value()) ||
                !ZHLN::Test::ExpectTrue(ctx.Valid(h1)) || !ZHLN::Test::ExpectTrue(ctx.Valid(h2)) || !ZHLN::Test::ExpectTrue(h0 != h2)) {
                return std::unexpected(TwoBoneIKTestError::HandleLifecycleFailed);
            }

            const std::array bindings {ZHLN::Animation::SkeletonBinding {.skeletonIndex = 0, .jointOffset = 16, .hasRagdoll = false}};
            ctx.BindSkeletons(h1, bindings);
            ctx.Sync(
                h1, ZHLN::Animation::PlaybackConfig {
                        .prefabAsset = id, .trackIndex = 0, .trackTime = 0.0f, .playbackSpeed = 1.0f, .loop = false, .finished = false
                    }
            );
            ctx.SuppressPaletteUpload(h1);
            ctx.Step(0.5f, assets, uploads);

            // Suppressed upload produces zero palette entries this step, then resets for the next step.
            uint32_t uploadCalls = 0;
            uploads.Drain([&](const ZHLN::PoseUpload&) { ++uploadCalls; });
            if (!ZHLN::Test::ExpectEq(uploadCalls, 0u)) {
                return std::unexpected(TwoBoneIKTestError::HandleLifecycleFailed);
            }

            ctx.Step(0.75f, assets, uploads);
            uploads.Drain([&](const ZHLN::PoseUpload& u) {
                if (u.jointOffset == 16 && u.matrices.size() == 3) {
                    ++uploadCalls;
                }
            });
            const auto playback = ctx.Playback(h1);
            if (!ZHLN::Test::ExpectEq(uploadCalls, 1u) || !ZHLN::Test::ExpectTrue(playback.has_value()) ||
                !ZHLN::Test::ExpectTrue(playback->finished) || !ZHLN::Test::ExpectLt(std::abs(playback->trackTime - 1.0f), 1.0e-5f)) {
                return std::unexpected(TwoBoneIKTestError::HandleLifecycleFailed);
            }

            ctx.Clear();
            if (!ZHLN::Test::ExpectEq(ctx.ActiveCount(), static_cast<size_t>(0)) || !ZHLN::Test::ExpectFalse(ctx.Valid(h1))) {
                return std::unexpected(TwoBoneIKTestError::HandleLifecycleFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> animation_context_resting_clip_ik_and_descendant_fk() {
            constexpr std::string_view kPath = "test_anim_ctx_resting_ik.glb";
            const ZHLN::AssetID        id    = ZHLN::HashAssetPath(kPath);

            ZHLN::AssetManager assets;
            assets.CachePrefab(id, MakeFourNodeLimbPrefab(kPath));

            ZHLN::PoseUploadQueue             uploads;
            ZHLN::Animation::AnimationContext ctx;
            const ZHLN::AnimationHandle       h = ctx.Create(id);

            ctx.Sync(
                h, ZHLN::Animation::PlaybackConfig {
                       .prefabAsset = id, .trackIndex = 0, .trackTime = 1.0f, .playbackSpeed = 1.0f, .loop = false, .finished = true
                   }
            );

            const JPH::Vec3 targetPos(0.0f, 2.0f, 2.8284f);
            const JPH::Quat targetRot = JPH::Quat::sRotation(JPH::Vec3::sAxisY(), 0.35f);
            const std::array chain {
                ZHLN::Animation::IKChain {
                    .upperNodeIndex    = 0,
                    .lowerNodeIndex    = 1,
                    .endNodeIndex      = 2,
                    .targetPosition    = targetPos,
                    .targetRotation    = targetRot,
                    .poleVector        = JPH::Vec3(0.0f, 1.0f, 0.0f),
                    .weight            = 1.0f,
                    .orientEndEffector = true,
                }
            };

            JPH::Mat44 firstSolvedHand   = JPH::Mat44::sIdentity();
            JPH::Mat44 firstSolvedFinger = JPH::Mat44::sIdentity();

            // Step 30 resting frames with the same IK chain: pose must remain bit-identical without compounding drift.
            for (uint32_t frame = 0; frame < 30; ++frame) {
                ctx.SetIKChains(h, chain);
                ctx.Step(1.0f / 60.0f, assets, uploads);
                uploads.Clear();

                const auto modelPose = ctx.ModelPose(h);
                if (!ZHLN::Test::ExpectEq(modelPose.size(), static_cast<size_t>(4))) {
                    return std::unexpected(TwoBoneIKTestError::RestingIKDrift);
                }
                if (frame == 0) {
                    firstSolvedHand   = modelPose[2];
                    firstSolvedFinger = modelPose[3];
                } else if (!modelPose[2].IsClose(firstSolvedHand, 1.0e-5f) || !modelPose[3].IsClose(firstSolvedFinger, 1.0e-5f)) {
                    return std::unexpected(TwoBoneIKTestError::RestingIKDrift);
                }
            }

            // Descendant FK check: node 3 (FingerTip) must sit at HandModel * LocalFingerOffset(0, 0, 0.5).
            const JPH::Vec3 expectedFingerPos = firstSolvedHand * JPH::Vec3(0.0f, 0.0f, 0.5f);
            const auto      rigidFinger       = ctx.RigidNode(h, 3);
            if (!ZHLN::Test::ExpectTrue(rigidFinger.has_value()) ||
                !ZHLN::Test::ExpectTrue(rigidFinger->translation.IsClose(expectedFingerPos, 1.0e-4f))) {
                return std::unexpected(TwoBoneIKTestError::RestingIKDrift);
            }

            // Dropping IK chains on the next frame restores the un-bent bind/clip reach (Hand at z = 4.0).
            ctx.Step(1.0f / 60.0f, assets, uploads);
            const auto restoredModel = ctx.ModelPose(h);
            if (!ZHLN::Test::ExpectTrue(restoredModel[2].GetTranslation().IsClose(JPH::Vec3(0.0f, 2.0f, 4.0f), 1.0e-4f))) {
                return std::unexpected(TwoBoneIKTestError::RestingIKDrift);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> animation_ecs_systems_morphs_rigid_nodes_and_kinematic_uploads() {
            constexpr std::string_view kPath = "test_anim_ecs_bridge.glb";
            const ZHLN::AssetID        id    = ZHLN::HashAssetPath(kPath);

            ZHLN::AssetManager assets;
            assets.CachePrefab(id, MakeFourNodeLimbPrefab(kPath));

            ZHLN::PoseUploadQueue        uploads;
            ZHLN::ECS::ExtensionServices extensions;
            extensions.Emplace<ZHLN::Animation::AnimationContext>();

            ZHLN::ECS::Registry registry;
            registry.RegisterAllComponentsIn<ZHLN::Components>();
            registry.RegisterComponent<ZHLN::IK::TwoBoneIKComponent>();

            ExtensionGraphServices services {.assets = assets, .poseUploads = uploads, .extensions = extensions};
            ZHLN::ECS::SystemGraph<ExtensionGraphServices> graph(registry, services);
            graph.AddSystem<&ZHLN::Animation::AnimationSyncSystem::Update>();
            graph.AddSystem<&ZHLN::Animation::AnimationStepSystem::Update>();
            graph.Compile();

            const ZHLN::Entity root = registry.Create(
                ZHLN::Components::AnimatorComponent {
                    .prefabAsset     = id,
                    .currentTrackIdx = -1, // idle first: authored morph weights must stay untouched
                    .currentLoop     = false,
                },
                ZHLN::Components::RagdollComponent {}
            );
            const ZHLN::Entity skinnedChild = registry.Create(
                ZHLN::Components::HierarchyComponent {.parent = root},
                ZHLN::Components::TransformComponent {.position = JPH::Vec3(7.0f, 8.0f, 9.0f)},
                ZHLN::Components::SkeletalMeshComponent {.jointOffset = 32, .skeletonIndex = 0},
                ZHLN::Components::MeshComponent {.nodeIndex = 2}
            );
            const ZHLN::Entity rigidMorphChild = registry.Create(
                ZHLN::Components::HierarchyComponent {.parent = root},
                ZHLN::Components::TransformComponent {},
                ZHLN::Components::MeshComponent {.nodeIndex = 3},
                ZHLN::Components::MorphTargetComponent {.activeCount = 2, .weights = {0.2f, 0.4f, 0.0f, 0.0f}}
            );

            // 1. Idle tick: handle allocated, kinematic ragdoll pose uploaded, authored morph weights preserved.
            graph.Execute(ZHLN::Frame {.dt = 0.25f});
            const auto idleMorph     = registry.Get<ZHLN::Components::MorphTargetComponent>(rigidMorphChild);
            const auto kinematicSpan = uploads.KinematicPose(32);
            if (!ZHLN::Test::ExpectTrue(idleMorph.has_value()) || !ZHLN::Test::ExpectEq(idleMorph->weights[0], 0.2f) ||
                !ZHLN::Test::ExpectEq(idleMorph->weights[1], 0.4f) || !ZHLN::Test::ExpectEq(kinematicSpan.size(), static_cast<size_t>(3))) {
                return std::unexpected(TwoBoneIKTestError::ECSBridgeFailed);
            }
            uploads.Clear();

            // 2. Degenerate keyless clip (track 1): morph weights still preserved.
            registry.Patch<ZHLN::Components::AnimatorComponent>(root, [](auto& anim) {
                anim.currentTrackIdx  = 1;
                anim.currentTrackTime = 0.0f;
            });
            graph.Execute(ZHLN::Frame {.dt = 0.5f});
            if (!ZHLN::Test::ExpectEq(idleMorph->weights[0], 0.2f) || !ZHLN::Test::ExpectEq(idleMorph->weights[1], 0.4f)) {
                return std::unexpected(TwoBoneIKTestError::ECSBridgeFailed);
            }
            uploads.Clear();

            // 3. Valid clip (track 0) past end (1.2 s > 1.0 s): morph weights clamp to (1.0, 0.75), rigid child TRS updated, skinned child untouched.
            registry.Patch<ZHLN::Components::AnimatorComponent>(root, [](auto& anim) {
                anim.currentTrackIdx  = 0;
                anim.currentTrackTime = 0.0f;
                anim.blendDuration    = 0.0f;
                anim.currentLoop      = false;
                anim.isFinished       = false;
            });
            graph.Execute(ZHLN::Frame {.dt = 1.2f});

            const auto rigidTr   = registry.Get<ZHLN::Components::TransformComponent>(rigidMorphChild);
            const auto skinnedTr = registry.Get<ZHLN::Components::TransformComponent>(skinnedChild);
            const auto animAfter = registry.Get<ZHLN::Components::AnimatorComponent>(root);
            if (!ZHLN::Test::ExpectTrue(animAfter.has_value()) || !ZHLN::Test::ExpectTrue(animAfter->isFinished) ||
                !ZHLN::Test::ExpectEq(idleMorph->weights[0], 1.0f) || !ZHLN::Test::ExpectEq(idleMorph->weights[1], 0.75f) ||
                !ZHLN::Test::ExpectTrue(rigidTr.has_value()) ||
                !ZHLN::Test::ExpectTrue(rigidTr->position.IsClose(JPH::Vec3(0.0f, 2.0f, 4.5f), 1.0e-4f)) ||
                !ZHLN::Test::ExpectTrue(skinnedTr->position.IsClose(JPH::Vec3(7.0f, 8.0f, 9.0f), 1.0e-4f))) {
                return std::unexpected(TwoBoneIKTestError::ECSBridgeFailed);
            }
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> two_bone_ik_ecs_system_tracks_world_target_entity() {
            constexpr std::string_view kPath = "test_two_bone_ik_ecs.glb";
            const ZHLN::AssetID        id    = ZHLN::HashAssetPath(kPath);

            ZHLN::AssetManager assets;
            assets.CachePrefab(id, MakeFourNodeLimbPrefab(kPath));

            ZHLN::PoseUploadQueue        uploads;
            ZHLN::ECS::ExtensionServices extensions;
            auto&                        animCtx = extensions.Emplace<ZHLN::Animation::AnimationContext>();

            ZHLN::ECS::Registry registry;
            registry.RegisterAllComponentsIn<ZHLN::Components>();
            registry.RegisterComponent<ZHLN::IK::TwoBoneIKComponent>();

            ExtensionGraphServices services {.assets = assets, .poseUploads = uploads, .extensions = extensions};
            ZHLN::ECS::SystemGraph<ExtensionGraphServices> graph(registry, services);
            graph.AddSystem<&ZHLN::Animation::AnimationSyncSystem::Update>();
            graph.AddSystem<&ZHLN::Animation::AnimationStepSystem::Update>();
            if (!ZHLN::Test::ExpectTrue(graph.AddSystemBefore<&ZHLN::IK::TwoBoneIKSystem::Update>("AnimationStepSystem"))) {
                return std::unexpected(TwoBoneIKTestError::ECSBridgeFailed);
            }
            graph.Compile();

            // Place the character root at world (10, 0, 5) and a world target entity at (10, 2, 7.6) with local targetOffset (0, 0, 0.2),
            // yielding a model-space IK target of (0, 2, 2.8).
            const ZHLN::Entity targetEntity = registry.Create(
                ZHLN::Components::TransformComponent {.position = JPH::Vec3(10.0f, 2.0f, 7.6f)}
            );

            ZHLN::IK::TwoBoneIKComponent ikComp {};
            const bool                   addedChain = ikComp.AddChain(
                ZHLN::IK::TwoBoneIKChain {
                    .upperNodeIndex    = 0,
                    .lowerNodeIndex    = 1,
                    .endNodeIndex      = 2,
                    .poleVector        = JPH::Vec3(0.0f, 1.0f, 0.0f),
                    .targetEntity      = targetEntity,
                    .targetOffset      = JPH::Vec3(0.0f, 0.0f, 0.2f),
                    .weight            = 1.0f,
                    .orientEndEffector = true,
                }
            );
            if (!ZHLN::Test::ExpectTrue(addedChain)) {
                return std::unexpected(TwoBoneIKTestError::ECSBridgeFailed);
            }

            const ZHLN::Entity root = registry.Create(
                ZHLN::Components::TransformComponent {.position = JPH::Vec3(10.0f, 0.0f, 5.0f)},
                ZHLN::Components::AnimatorComponent {
                    .prefabAsset     = id,
                    .currentTrackIdx = 0,
                    .currentTrackTime = 1.0f,
                    .currentLoop     = false,
                    .isFinished      = true,
                },
                ikComp
            );
            const ZHLN::Entity fingerChild = registry.Create(
                ZHLN::Components::HierarchyComponent {.parent = root},
                ZHLN::Components::TransformComponent {},
                ZHLN::Components::MeshComponent {.nodeIndex = 3}
            );

            for (int frame = 0; frame < 10; ++frame) {
                graph.Execute(ZHLN::Frame {.dt = 1.0f / 60.0f});
                uploads.Clear();
            }

            const auto animComp = registry.Get<ZHLN::Components::AnimatorComponent>(root);
            const auto fingerTr = registry.Get<ZHLN::Components::TransformComponent>(fingerChild);
            if (!ZHLN::Test::ExpectTrue(animComp.has_value()) || !ZHLN::Test::ExpectTrue(fingerTr.has_value())) {
                return std::unexpected(TwoBoneIKTestError::ECSBridgeFailed);
            }

            const auto modelPose = animCtx.ModelPose(animComp->animationHandle);
            if (!ZHLN::Test::ExpectEq(modelPose.size(), static_cast<size_t>(4)) ||
                !ZHLN::Test::ExpectTrue(modelPose[2].GetTranslation().IsClose(JPH::Vec3(0.0f, 2.0f, 2.8f), 1.0e-3f)) ||
                !ZHLN::Test::ExpectTrue(fingerTr->position.IsClose(JPH::Vec3(0.0f, 2.0f, 3.3f), 1.0e-3f))) {
                return std::unexpected(TwoBoneIKTestError::ECSBridgeFailed);
            }
            return {};
        }
    };
};

} // namespace
