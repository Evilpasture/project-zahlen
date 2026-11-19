// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#include <Camera/TargetCamera.hpp>
#include <TestsFramework.hpp>
#include <Zahlen/App.hpp>
#include <Zahlen/COM/EngineFactory.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/IFramePipeline.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <Zahlen/COM/ISimulation.hpp>
#include <cmath>
#include <limits>

namespace ZHLN::Test {
enum class StageFailure : uint8_t { Refused = 1 };
}
namespace {
using namespace ZHLN;
using Test::ExpectEq;
using Test::ExpectTrue;
auto StagedEngine() {
    return COM::CreateEngineRef(
        COM::InChain(
            COM::AppConfig {.appName = String64("Staged frames"), .logLevel = LogLevel::Quiet},
            COM::WindowConfig {.width = 160, .height = 120, .vsync = false, .headless = true},
            COM::RenderConfig {.validationMode = ValidationMode::Off}
        )
    );
}
struct FramePipelineSuite {
    struct Tests {
        std::expected<void, ErrorCode> application_preserves_typed_start_failure_and_stops_only_entered_layers_in_reverse() {
            std::vector<int> order;
            struct First {
                std::vector<int>* order;
                void              Start(COM::IEngine&) {
                    order->push_back(1);
                }
                void Stop(COM::IEngine& e) {
                    ExpectEq(e.CurrentFrame(), uint64_t(0));
                    order->push_back(5);
                }
            };
            struct Fails {
                std::vector<int>* order;
                auto              Start(COM::IEngine&) -> std::expected<void, ErrorCode> {
                    order->push_back(2);
                    return std::unexpected(Test::StageFailure::Refused);
                }
                void Tick(COM::IEngine&, float) {
                    order->push_back(99);
                }
                void Stop(COM::IEngine& e) {
                    ExpectEq(e.CurrentFrame(), uint64_t(0));
                    order->push_back(4);
                }
            };
            struct Unvisited {
                std::vector<int>* order;
                void              Start(COM::IEngine&) {
                    order->push_back(98);
                }
                void Stop(COM::IEngine&) {
                    order->push_back(97);
                }
            };
            auto result = App(EngineConfig {.width = 160, .height = 120, .validationMode = ValidationMode::Off, .headless = true})
                              .AddLayer(First {&order})
                              .AddLayer(Fails {&order})
                              .AddLayer(Unvisited {&order})
                              .Run();
            ExpectTrue(!result.has_value());
            if (!result)
                ExpectTrue(result.error().Is(Test::StageFailure::Refused));
            ExpectTrue(order == std::vector<int>({1, 2, 4, 5}));
            return {};
        }
        std::expected<void, ErrorCode> application_tick_failure_stops_before_ui_simulation_and_later_ticks() {
            int later = 0, stopped = 0;
            struct Fails {
                int* stopped;
                auto Tick(COM::IEngine&, float) -> std::expected<void, ErrorCode> {
                    return std::unexpected(Test::StageFailure::Refused);
                }
                void UI(COM::IEngine&) {
                    ExpectTrue(false);
                }
                void Stop(COM::IEngine& e) {
                    ExpectEq(e.CurrentFrame(), uint64_t(0));
                    ++*stopped;
                }
            };
            struct Later {
                int* later;
                void Tick(COM::IEngine&, float) {
                    ++*later;
                }
            };
            auto result = App(EngineConfig {.width = 160, .height = 120, .validationMode = ValidationMode::Off, .headless = true})
                              .AddLayer(Fails {&stopped})
                              .AddLayer(Later {&later})
                              .Run();
            ExpectTrue(!result.has_value());
            if (!result)
                ExpectTrue(result.error().Is(Test::StageFailure::Refused));
            ExpectEq(later, 0);
            ExpectEq(stopped, 1);
            return {};
        }
        std::expected<void, ErrorCode> copied_graphics_info_can_share_a_settings_query_and_survives_recreation() {
            auto created = StagedEngine();
            if (!ExpectTrue(created.has_value()))
                return {};
            auto                    engine = std::move(*created);
            COM::GraphicsDeviceInfo info {};
            GraphicsSettings        settings {};
            const auto              read = engine->Graphics()->GetSettings(COM::OutChain(info, settings));
            ExpectTrue(read.HasValue());
            const std::string copiedName(std::string_view(info.gpuName));
            ExpectTrue(!copiedName.empty());
            ExpectTrue(!std::string_view(info.rendererName).empty());
            const auto generation = engine->DeviceGeneration();
            ExpectTrue(engine->RecreateGraphicsDevice().HasValue());
            ExpectTrue(engine->DeviceGeneration() > generation);
            ExpectEq(std::string(std::string_view(info.gpuName)), copiedName);
            const auto current = engine->Graphics()->Info();
            ExpectEq(std::string(std::string_view(current.gpuName)), copiedName);
            return {};
        }
        std::expected<void, ErrorCode> query_and_strict_frame_protocol_preserve_clock_and_projection() {
            auto created = StagedEngine();
            if (!ExpectTrue(created.has_value()))
                return {};
            auto engine   = std::move(*created);
            auto pipeline = engine.As<COM::IFramePipeline>();
            if (!ExpectTrue(bool(pipeline)))
                return {};
            auto root = pipeline.As<COM::IEngine>();
            ExpectEq(root.Get(), engine.Get());
            ExpectTrue(!pipeline->FramePending());
            const auto initialViewport = engine->Graphics()->Viewport();
            ExpectEq(initialViewport.width, uint32_t {160});
            ExpectEq(initialViewport.height, uint32_t {120});
            const auto noFrame = pipeline->FinishFrame();
            ExpectTrue(!noFrame.HasValue() && noFrame.error.Is(COM::FramePipelineError::NoPendingFrame));
            ExpectTrue(!pipeline->CancelFrame().HasValue());
            for (const float dt: {-1.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
                const auto bad = pipeline->AdvanceFrame(dt);
                ExpectTrue(!bad.HasValue() && bad.error.Is(COM::FramePipelineError::InvalidDeltaTime));
            }
            ExpectEq(engine->CurrentFrame(), uint64_t {0});
            ExpectTrue(pipeline->AdvanceFrame(0).HasValue());
            ExpectTrue(pipeline->FramePending());
            const auto camera = engine->Entities()->GetSingleton<Components::CameraComponent>();
            ExpectTrue(camera != nullptr);
            if (camera)
                ExpectEq(camera->frameCounter, uint32_t {0});
            const auto duplicate = pipeline->AdvanceFrame(10.0f);
            ExpectTrue(!duplicate.HasValue() && duplicate.error.Is(COM::FramePipelineError::FrameInProgress));
            ExpectTrue(!engine->Tick(10.0f).HasValue());
            ExpectTrue(!engine->RecreateGraphicsDevice().HasValue());
            ExpectEq(engine->CurrentFrame(), uint64_t {0});
            ExpectTrue(pipeline->FinishFrame().HasValue());
            ExpectEq(engine->CurrentFrame(), uint64_t {1});
            if (camera)
                ExpectEq(camera->frameCounter, uint32_t {1});
            ExpectTrue(!pipeline->FinishFrame().HasValue());
            ExpectTrue(pipeline->AdvanceFrame(0).HasValue());
            ExpectTrue(pipeline->CancelFrame().HasValue());
            ExpectEq(engine->CurrentFrame(), uint64_t {2});
            if (camera)
                ExpectEq(camera->frameCounter, uint32_t {1});
            ExpectTrue(engine->Tick(0).HasValue());
            ExpectEq(engine->CurrentFrame(), uint64_t {3});
            if (camera)
                ExpectEq(camera->frameCounter, uint32_t {2});
            return {};
        }
        std::expected<void, ErrorCode> orbit_reads_same_frame_physics_world_pose_before_projection() {
            auto created = StagedEngine();
            if (!ExpectTrue(created.has_value()))
                return {};
            auto       engine   = std::move(*created);
            auto       pipeline = engine.As<COM::IFramePipeline>();
            auto&      entities = *engine->Entities();
            const auto body     = engine->Simulation()->CreateBoxBody({0.5f, 0.5f, 0.5f}, {.position = {0, 10, 0}});
            if (!ExpectTrue(body.HasValue()))
                return {};
            const auto target = entities.Spawn(
                Components::TransformComponent {.position = {0, 10, 0}}, Components::WorldTransformComponent {},
                Components::PhysicsComponent {.physicsID = body.value, .isStatic = false}
            );
            const auto cameraID = entities.SingletonEntity<Components::MainCameraTagComponent>();
            if (!ExpectTrue(cameraID.HasValue()))
                return {};
            entities.Attach(
                cameraID.value, CameraRig::TargetComponent {
                                    .target = target, .distance = 2, .targetDistance = 2, .yaw = 0, .pitch = 0, .targetOffset = FVec3::Zero(), .stiffness = 0
                                }
            );
            // Warm committed CPU snapshots without submitting any frame.
            for (int i = 0; i < 2; ++i) {
                ExpectTrue(pipeline->AdvanceFrame(1.0f / 60.0f).HasValue());
                ExpectTrue(pipeline->CancelFrame().HasValue());
            }
            const auto before = entities.Get<Components::WorldTransformComponent>(target)->world;
            ExpectTrue(pipeline->AdvanceFrame(0.025f).HasValue());
            const auto world = entities.Get<Components::WorldTransformComponent>(target)->world;
            ExpectTrue(world[1, 3] < before[1, 3]);
            const auto physicsBeforeFinish = engine->Simulation()->BodyPosition(body.value);
            ExpectTrue(physicsBeforeFinish.HasValue());
            auto* camera = entities.Get<Components::CameraComponent>(cameraID.value);
            ExpectEq(camera->frameCounter, uint32_t {0});
            CameraRig::Apply(entities, {}, 0.025f);
            ExpectTrue(std::abs(camera->camera.position.x - (world[0, 3] - 2.0f)) < 1e-5f);
            ExpectTrue(std::abs(camera->camera.position.y - world[1, 3]) < 1e-5f);
            const auto expectedVP = camera->camera.GetProjectionMatrix(160.0f / 120.0f) * camera->camera.GetViewMatrix();
            ExpectTrue(pipeline->FinishFrame().HasValue());
            ExpectEq(camera->frameCounter, uint32_t {1});
            ExpectTrue(camera->unjitteredViewProj == expectedVP);
            const auto physicsAfterFinish = engine->Simulation()->BodyPosition(body.value);
            ExpectTrue(physicsAfterFinish.HasValue());
            if (physicsBeforeFinish.HasValue() && physicsAfterFinish.HasValue())
                ExpectEq(physicsBeforeFinish.value.y, physicsAfterFinish.value.y); // no double simulation
            const auto* committed = entities.Get<Components::WorldTransformComponent>(target);
            ExpectTrue(committed->world == committed->previous);
            return {};
        }
        std::expected<void, ErrorCode> local_target_edits_are_propagated_before_host_camera_and_cancel_preserves_history() {
            auto created = StagedEngine();
            if (!ExpectTrue(created.has_value()))
                return {};
            auto       engine   = std::move(*created);
            auto       pipeline = engine.As<COM::IFramePipeline>();
            auto&      entities = *engine->Entities();
            const auto target   = entities.Spawn(Components::TransformComponent {.position = {4, 3, 2}}, Components::WorldTransformComponent {});
            const auto camera   = entities.Spawn(
                Components::CameraComponent {},
                CameraRig::TargetComponent {
                    .target = target, .distance = 2, .targetDistance = 2, .yaw = 0, .pitch = 0, .targetOffset = FVec3::Zero(), .stiffness = 0
                }
            );
            ExpectTrue(pipeline->AdvanceFrame(0).HasValue());
            CameraRig::Apply(entities, {}, 0);
            ExpectEq(entities.Get<Components::CameraComponent>(camera)->camera.position.x, 2.0f);
            ExpectTrue(pipeline->CancelFrame().HasValue());
            const auto* world = entities.Get<Components::WorldTransformComponent>(target);
            ExpectTrue(world->previous == Mat4x4::Identity());
            ExpectTrue(world->world != world->previous);
            entities.Get<Components::TransformComponent>(target)->position.x = 9;
            ExpectTrue(pipeline->AdvanceFrame(0).HasValue());
            CameraRig::Apply(entities, {}, 0);
            ExpectEq(entities.Get<Components::CameraComponent>(camera)->camera.position.x, 7.0f);
            ExpectTrue(pipeline->FinishFrame().HasValue());
            return {};
        }
        std::expected<void, ErrorCode> scene_replacement_cancels_pending_render_and_allows_the_next_frame() {
            auto created = StagedEngine();
            if (!ExpectTrue(created.has_value()))
                return {};
            auto       engine   = std::move(*created);
            auto       pipeline = engine.As<COM::IFramePipeline>();
            const auto entity   = engine->Entities()->Spawn(Components::TransformComponent {});
            ExpectTrue(pipeline->AdvanceFrame(0).HasValue());
            engine->ClearScene();
            ExpectTrue(!pipeline->FramePending());
            ExpectTrue(!engine->Entities()->IsAlive(entity));
            ExpectEq(engine->CurrentFrame(), uint64_t {1});
            ExpectTrue(!pipeline->FinishFrame().HasValue());
            ExpectTrue(engine->Tick(0).HasValue());
            ExpectEq(engine->CurrentFrame(), uint64_t {2});
            return {};
        }
        std::expected<void, ErrorCode> application_camera_layer_follows_current_host_edits_not_previous_frame() {
            struct Monitor {
                Entity* target;
                int*    observations;
                void    BeforeRender(COM::IEngine& engine, float) {
                    const auto* world  = engine.Entities()->Get<Components::WorldTransformComponent>(*target);
                    const auto* camera = engine.Entities()->GetSingleton<Components::CameraComponent>();
                    ExpectTrue(world && camera);
                    if (world && camera) {
                        ExpectTrue(std::abs(camera->camera.position.x - (world->world[0, 3] - 2)) < 1e-5f);
                        ExpectEq(world->world[0, 3], engine.CurrentFrame() == 0 ? 5.0f : 8.0f);
                    }
                    ++*observations;
                }
            };
            Entity     target       = Entity::Null();
            int        observations = 0;
            const auto result =
                App(EngineConfig {.appName = "Current frame orbit", .width = 160, .height = 120, .validationMode = ValidationMode::Off, .headless = true})
                    .OnStart([&](COM::IEngine& engine) {
                        auto& entities  = *engine.Entities();
                        target          = entities.Spawn(Components::TransformComponent {}, Components::WorldTransformComponent {});
                        const auto main = entities.SingletonEntity<Components::MainCameraTagComponent>();
                        if (main.HasValue())
                            entities.Attach(
                                main.value,
                                CameraRig::TargetComponent {
                                    .target = target, .distance = 2, .targetDistance = 2, .yaw = 0, .pitch = 0, .targetOffset = FVec3::Zero(), .stiffness = 0
                                }
                            );
                    })
                    .AddLayer(CameraRig::Layer {})
                    .AddLayer(Monitor {&target, &observations})
                    .Run([&](COM::IEngine& engine, float) {
                        if (engine.CurrentFrame() >= 2) {
                            engine.RequestClose();
                            return;
                        }
                        engine.Entities()->Get<Components::TransformComponent>(target)->position.x = engine.CurrentFrame() == 0 ? 5 : 8;
                    });
            ExpectTrue(result.has_value());
            ExpectEq(observations, 2);
            return {};
        }
        std::expected<void, ErrorCode> application_before_render_failure_cancels_before_stop() {
            struct Layer {
                bool* stopped;
                auto  BeforeRender(COM::IEngine& engine, float) -> std::expected<void, ErrorCode> {
                    auto pipeline = COM::Ref<COM::IFramePipeline>::Adopt(engine.Query<COM::IFramePipeline>());
                    ExpectTrue(pipeline->FramePending());
                    ExpectEq(engine.CurrentFrame(), uint64_t {0});
                    return std::unexpected(Test::StageFailure::Refused);
                }
                void Stop(COM::IEngine& engine) {
                    auto pipeline = COM::Ref<COM::IFramePipeline>::Adopt(engine.Query<COM::IFramePipeline>());
                    ExpectTrue(!pipeline->FramePending());
                    ExpectEq(engine.CurrentFrame(), uint64_t {1});
                    const auto camera = engine.Entities()->GetSingleton<Components::CameraComponent>();
                    ExpectTrue(camera && camera->frameCounter == 0);
                    *stopped = true;
                }
            };
            bool       stopped = false;
            const auto result  = App(EngineConfig {.appName = "Before render failure", .width = 160, .height = 120, .validationMode = ValidationMode::Off, .headless = true})
                                     .AddLayer(Layer {&stopped})
                                     .Run();
            ExpectTrue(!result.has_value());
            if (!result)
                ExpectTrue(result.error().Is(Test::StageFailure::Refused));
            ExpectTrue(stopped);
            return {};
        }
    };
};
} // namespace
