// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Animation/Animator.hpp>
#include <Animation/Components.hpp>
#include <Animation/IKControls.hpp>
#include <Animation/AnimationContext.hpp>
#include <Animation/AnimationSystem.hpp>
#include <TestsFramework.hpp>
#include <Zahlen/COM/EngineFactory.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/IFramePipeline.hpp>
#include <Zahlen/COM/ISimulation.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/HostRunner.hpp>
#include <cmath>
#include <glTF/GLTFAssets.hpp>

namespace {
ZHLN::Animation::AnimationContext s_testAnimCtx {};
using namespace ZHLN;
using Test::ExpectEq;
using Test::ExpectTrue;
auto RuntimeEngine(bool animated = true) {
    (void)animated;
    return COM::CreateEngineRef(COM::InChain(
        COM::EngineAppConfig {.appName = String64("Engine runtime controls"), .logLevel = LogLevel::Quiet},
        COM::EngineWindowConfig {.width = 160, .height = 120, .vsync = false, .headless = true}, COM::EngineRenderConfig {.validationMode = ValidationMode::Off}
    ));
}
struct RuntimeControlsSuite {
    ProcessScope process;
    struct Tests {
        std::expected<void, ErrorCode> plain_engine_has_no_animation_publications() {
            auto made = RuntimeEngine(false);
            if (!ExpectTrue(made.has_value()))
                return {};
            auto& entities = *(*made)->Entities();
            ExpectTrue(!entities.IsEngineComponent(COM::StringView(Reflect::CanonicalTypeName<Animation::AnimatorComponent>())));
            ExpectTrue(!entities.IsEngineComponent(COM::StringView(Reflect::CanonicalTypeName<Animation::AnimatorControlComponent>())));
            ExpectTrue(!entities.IsEngineComponent(COM::StringView(Reflect::CanonicalTypeName<IK::TwoBoneIKComponent>())));
            return {};
        }

        std::expected<void, ErrorCode> motor_moves_without_host_hooks_or_native_modules() {
            auto made = RuntimeEngine(false);
            if (!ExpectTrue(made.has_value()))
                return {};
            auto       engine   = std::move(*made);
            auto&      entities = *engine->Entities();
            const auto body     = engine->Simulation()->CreateCharacter(DVec3 {0, 10, 0});
            ExpectTrue(body != Physics::BodyID::Null());
            const auto entity = entities.Spawn(
                Components::PhysicsComponent {.physicsID = body, .isStatic = false}, Components::TransformComponent {},
                Components::CharacterMotorComponent {.desiredVelocity = {4, 0, 0}}
            );
            for (int i = 0; i < 12; ++i)
                ExpectTrue(engine->Tick(1.0f / 60.0f).HasValue());
            const auto position = engine->Simulation()->BodyPosition(body);
            ExpectTrue(position.HasValue());
            if (position.HasValue())
                ExpectTrue(position.value.x > 0.05);
            const auto* state = entities.Get<Components::CharacterMotorStateComponent>(entity);
            if (ExpectTrue(state != nullptr)) {
                ExpectTrue(state->velocity.x > 0);
                ExpectTrue(state->velocity.y < 0);
            }
            auto* motor            = entities.Get<Components::CharacterMotorComponent>(entity);
            motor->desiredVelocity = {};
            motor->jumpRequested   = true; // airborne jumps are consumed, not queued
            for (int i = 0; i < 12; ++i)
                ExpectTrue(engine->Tick(1.0f / 60.0f).HasValue());
            motor = entities.Get<Components::CharacterMotorComponent>(entity);
            ExpectTrue(!motor->jumpRequested);
            state = entities.Get<Components::CharacterMotorStateComponent>(entity);
            if (ExpectTrue(state != nullptr))
                ExpectTrue(std::abs(state->velocity.x) < 0.01f);
            engine->ClearScene();
            ExpectTrue(!engine->Simulation()->BodyPosition(body).HasValue());
            ExpectTrue(entities.Get<Components::CharacterMotorStateComponent>(entity) == nullptr);
            return {};
        }
        std::expected<void, ErrorCode> real_skinned_prefab_evaluates_with_explicit_animation_extension() {
            Test::SetTimeout(180);
            auto made = RuntimeEngine();
            if (!ExpectTrue(made.has_value()))
                return {};
            auto engine   = std::move(*made);
            auto document = GLTF::Read(ZHLN_PROJECT_ROOT "/tests/render/Uzi.glb", {}, engine->Assets());
            if (!ExpectTrue(document.has_value()))
                return {};
            ExpectTrue(!document->animations.empty());
            auto asset = GLTF::Upload(*engine->Assets(), *engine->Graphics(), *document, {.buildBLAS = false, .buildColliders = false});
            if (!ExpectTrue(asset.has_value()))
                return {};
            const auto parts = engine->Assets()->Instantiate(asset->hash, {.isAnimated = true});
            if (!ExpectTrue(!parts.empty()))
                return {};
            auto&      entities = *engine->Entities();
            const auto root     = parts.front();
            if (auto* camera = entities.GetSingleton<Components::CameraComponent>()) {
                camera->camera.position = {0, 1, 2.5f};
                camera->camera.yaw      = DegF {-90};
                camera->camera.pitch    = DegF {0};
            }
            ExpectTrue(Animator::Play(entities, root, 0));
            for (int i = 0; i < 3; ++i) {
                Animation::Step(*engine, s_testAnimCtx, 1.0f / 60.0f); ExpectTrue(engine->Tick(1.0f / 60.0f).HasValue());
            }
            const auto* animator = entities.Get<Animation::AnimatorComponent>(root);
            if (ExpectTrue(animator != nullptr)) {
                ExpectTrue(animator->animationID.IsValid());
                ExpectTrue(animator->currentTrackTime > 0.0f);
            }
            bool skinningUploaded = false;
            for (const auto entity: parts) {
                if (const auto* skeleton = entities.Get<Components::SkeletalMeshComponent>(entity))
                    skinningUploaded |= skeleton->skinnedScratch != BufferID::Invalid && skeleton->scratchVertexCount > 0;
                if (const auto* mesh = entities.Get<Components::MeshComponent>(entity))
                    ExpectTrue(std::isfinite(mesh->cullRadius) && mesh->cullRadius < 20.0f);
            }
            ExpectTrue(skinningUploaded);
            // Paused playback must not advance and explicit seeks are consumed.
            ExpectTrue(Animator::Speed(entities, root, 0));
            ExpectTrue(Animator::Seek(entities, root, 0.125f));
            Animation::Step(*engine, s_testAnimCtx, 1.0f / 60.0f); ExpectTrue(engine->Tick(1.0f / 60.0f).HasValue());
            animator = entities.Get<Animation::AnimatorComponent>(root);
            if (ExpectTrue(animator != nullptr))
                ExpectTrue(std::abs(animator->currentTrackTime - 0.125f) < 0.0001f);
            engine->ClearScene();
            Animation::Sync(*engine->Entities(), s_testAnimCtx);
            ExpectTrue(!entities.IsAlive(root));
            // Reuse the retained cache after scene reset; the old generational
            // animation must not be reused by the newly created instance.
            const auto recreated = engine->Assets()->Instantiate(asset->hash, {.isAnimated = true});
            ExpectTrue(!recreated.empty());
            Animation::Step(*engine, s_testAnimCtx, 1.0f / 60.0f); ExpectTrue(engine->Tick(1.0f / 60.0f).HasValue());
            return {};
        }
        std::expected<void, ErrorCode> real_skinned_simulation_stage_evaluates_and_recreates_without_presentation() {
            Test::SetTimeout(60);
            auto made = RuntimeEngine();
            if (!ExpectTrue(made.has_value()))
                return {};
            auto engine   = std::move(*made);
            auto pipeline = engine.As<COM::IFramePipeline>();
            if (!ExpectTrue(bool(pipeline)))
                return {};
            auto document = GLTF::Read(ZHLN_PROJECT_ROOT "/tests/render/Uzi.glb", {}, engine->Assets());
            if (!ExpectTrue(document.has_value()))
                return {};
            ExpectTrue(!document->animations.empty());
            auto asset = GLTF::Upload(*engine->Assets(), *engine->Graphics(), *document, {.buildBLAS = false, .buildColliders = false});
            if (!ExpectTrue(asset.has_value()))
                return {};
            auto&      entities = *engine->Entities();
            const auto parts    = engine->Assets()->Instantiate(asset->hash, {.isAnimated = true});
            if (!ExpectTrue(!parts.empty()))
                return {};
            const auto root = parts.front();
            ExpectTrue(Animator::Play(entities, root, 0));
            const auto advance = [&] {
                ExpectTrue(pipeline->AdvanceFrame(1.0f / 60).HasValue());
                Animation::Step(*engine, s_testAnimCtx, 1.0f / 60.0f);
                ExpectTrue(pipeline->CancelFrame().HasValue());
            };
            for (int i = 0; i < 12; ++i)
                advance();
            Animator::PlaybackSnapshot state;
            { auto s = Animator::Read(entities, root); ExpectTrue(s.has_value()); state = *s; }
            ExpectTrue(state.time > 0);
            const auto* runtime = entities.Get<Animation::AnimatorComponent>(root);
            if (!ExpectTrue(runtime && runtime->animationID.IsValid()))
                return {};
            const auto firstHandle = runtime->animationID;
            ExpectTrue(Animator::Speed(entities, root, 0));
            ExpectTrue(Animator::Seek(entities, root, .125f));
            advance();
            { auto s = Animator::Read(entities, root); ExpectTrue(s.has_value()); state = *s; }
            ExpectTrue(std::abs(state.time - .125f) < .0001f);
            runtime = entities.Get<Animation::AnimatorComponent>(root);
            ExpectTrue(runtime && runtime->animationID == firstHandle);
            engine->ClearScene();
            Animation::Sync(*engine->Entities(), s_testAnimCtx);
            ExpectTrue(!entities.IsAlive(root));
            const auto recreated = engine->Assets()->Instantiate(asset->hash, {.isAnimated = true});
            if (!ExpectTrue(!recreated.empty()))
                return {};
            ExpectTrue(Animator::Play(entities, recreated.front(), 0));
            advance();
            runtime = entities.Get<Animation::AnimatorComponent>(recreated.front());
            ExpectTrue(runtime && runtime->animationID.IsValid() && runtime->animationID != firstHandle);
            // This is a separate simulation-stage gate, not a replacement for
            // the retained full rendered skinning workload above.
            return {};
        }
        std::expected<void, ErrorCode> animator_helpers_preserve_runtime_handle_and_refuse_bad_controls() {
            auto made = RuntimeEngine();
            if (!ExpectTrue(made.has_value()))
                return {};
            auto       engine   = std::move(*made);
            Animation::AnimationContext s_testAnimCtx {};
            auto&      entities = *engine->Entities();
            const auto plain    = entities.CreateEntity();
            ExpectTrue(!Animator::Play(entities, plain, 0));
            const auto root = entities.Spawn(Animation::AnimatorComponent {.currentTrackIdx = 2, .currentTrackTime = 3.5f});
            ExpectTrue(Animator::Attach(entities, root));
            Animator::PlaybackSnapshot snapshot;
            { auto s = Animator::Read(entities, root); ExpectTrue(s.has_value()); snapshot = *s; }
            ExpectEq(snapshot.trackIndex, 2);
            ExpectEq(snapshot.time, 3.5f);
            ExpectTrue(!Animator::Play(entities, root, -2));
            ExpectTrue(!Animator::Seek(entities, root, -1));
            ExpectTrue(Animator::Play(entities, root, 1, 0.2f, false));
            ExpectTrue(Animator::Speed(entities, root, 0));
            ExpectTrue(Animator::Seek(entities, root, 0.25f));
            Animation::Step(*engine, s_testAnimCtx, 1.0f / 60.0f); ExpectTrue(engine->Tick(1.0f / 60.0f).HasValue());
            { auto s = Animator::Read(entities, root); ExpectTrue(s.has_value()); snapshot = *s; }
            ExpectEq(snapshot.trackIndex, 1);
            ExpectEq(snapshot.time, 0.25f);
            ExpectEq(snapshot.speed, 0.0f);
            IK::TwoBoneIKComponent ik;
            ExpectTrue(ik.AddChain(IK::TwoBoneIKChain {.upperNodeIndex = 0, .lowerNodeIndex = 1, .endNodeIndex = 2}));
            ExpectTrue(Animator::SetIK(entities, root, ik));
            ik.chains[0].weight = 2;
            ExpectTrue(!Animator::SetIK(entities, root, ik));
            ExpectEq(entities.Get<IK::TwoBoneIKComponent>(root)->chains[0].weight, 1.0f);
            Animator::Detach(entities, root);
            ExpectTrue(entities.Get<Animation::AnimatorComponent>(root) != nullptr);
            ExpectTrue(entities.Get<Animation::AnimatorControlComponent>(root) == nullptr);
            engine->ClearScene();
            Animation::Sync(*engine->Entities(), s_testAnimCtx);
            ExpectTrue(!Animator::Read(entities, root).has_value());
            return {};
        }
        std::expected<void, ErrorCode> playback_and_ik_controls_use_published_typed_storage() {
            auto made = RuntimeEngine();
            if (!ExpectTrue(made.has_value()))
                return {};
            auto  engine   = std::move(*made);
            auto& entities = *engine->Entities();
            ExpectTrue(!entities.IsEngineComponent(COM::IEntities::ComponentName<Animation::AnimatorControlComponent>()));
            ExpectTrue(!entities.IsEngineComponent(COM::IEntities::ComponentName<IK::TwoBoneIKComponent>()));
            IK::TwoBoneIKComponent ik;
            ExpectTrue(ik.AddChain(IK::TwoBoneIKChain {.upperNodeIndex = 0, .lowerNodeIndex = 1, .endNodeIndex = 2}));
            const auto root = entities.Spawn(
                Animation::AnimatorComponent {},
                Animation::AnimatorControlComponent {.trackIndex = 3, .playbackSpeed = 0.5f, .seekTime = 1.25f, .seekRequested = true}, ik
            );
            Animation::Step(*engine, s_testAnimCtx, 1.0f / 60.0f); ExpectTrue(engine->Tick(1.0f / 60.0f).HasValue());
            const auto* animator = entities.Get<Animation::AnimatorComponent>(root);
            const auto* control  = entities.Get<Animation::AnimatorControlComponent>(root);
            if (ExpectTrue(animator && control)) {
                ExpectEq(animator->currentTrackIdx, 3);
                ExpectEq(animator->currentPlaybackSpeed, 0.5f);
                ExpectEq(animator->currentTrackTime, 1.25f);
                ExpectTrue(!control->seekRequested);
            }
            ExpectTrue(entities.GetRaw(root, COM::IEntities::ComponentName<IK::TwoBoneIKComponent>()) != nullptr);
            return {};
        }
    };
};
} // namespace
