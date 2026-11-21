// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#include <FallbackScene/FallbackScene.hpp>
#include <Scene/Scene.hpp>
#include <TestsFramework.hpp>
#include <UI/HostUI.hpp>
#include <Zahlen/App.hpp>
#include <memory>
#include <Zahlen/COM/EngineFactory.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <cmath>

namespace {
using namespace ZHLN;
using Test::ExpectEq;
using Test::ExpectTrue;
auto FallbackEngine() {
    return COM::CreateEngineRef(
        COM::InChain(
            COM::AppConfig {.appName = String64("Fallback host"), .logLevel = LogLevel::Quiet},
            COM::WindowConfig {.width = 160, .height = 120, .vsync = false, .headless = true},
            COM::RenderConfig {.validationMode = ValidationMode::Off}
        )
    );
}
struct FallbackHostSuite {
    struct Tests {
        std::expected<void, ErrorCode> real_diagnostic_scene_and_animation_are_owned_by_the_host_instance() {
            auto created = FallbackEngine();
            if (!ExpectTrue(created.has_value()))
                return {};
            auto                         engine = std::move(*created);
            FallbackScene::DefaultPreset first, independent;
            ExpectTrue(!first.IsActive() && !independent.IsActive());
            first.BuildFallbackScene(*engine.Get(), {.buildBLAS = false});
            ExpectTrue(first.IsActive() && !independent.IsActive());
            auto scene = Scene::Extract(*engine.Get());
            ExpectEq(scene.entities.size(), size_t {2});
            ExpectEq(scene.lights.size(), size_t {2});
            if (scene.entities.size() == 2)
                ExpectEq(scene.entities[1].name, std::string("FallbackEmblem"));
            first.Update(*engine.Get(), 0.25f);
            scene = Scene::Extract(*engine.Get());
            if (scene.entities.size() == 2)
                ExpectTrue(std::abs(scene.entities[1].transform.position.y - (2.0f + std::sin(0.5f) * 0.25f)) < 1e-5f);
            first.BuildFallbackScene(*engine.Get(), {.buildBLAS = false}); // idempotent on this owner
            ExpectEq(Scene::Extract(*engine.Get()).entities.size(), size_t {2});
            first.ClearFallback();
            ExpectTrue(!first.IsActive());
            engine->ClearScene();
            independent.BuildFallbackScene(*engine.Get(), {.buildBLAS = false});
            ExpectTrue(independent.IsActive() && !first.IsActive());
            ExpectEq(Scene::Extract(*engine.Get()).entities.size(), size_t {2});
            independent.ClearFallback();
            return {};
        }
        std::expected<void, ErrorCode> diagnostic_popup_and_scene_submit_through_application_host_ui() {
            struct Layer {
                FallbackScene::DefaultPreset preset;
                int*                         uiFrames = nullptr;
                std::unique_ptr<HostUI>      ui;
                void Start(COM::IEngine& engine) {
                    ui = std::make_unique<HostUI>(engine);
                    preset.BuildFallbackScene(engine, {.buildBLAS = false});
                }
                auto Tick(COM::IEngine& engine, float dt) -> std::expected<void, ErrorCode> {
                    if (engine.CurrentFrame() >= 2) {
                        engine.RequestClose();
                        return {};
                    }
                    preset.Update(engine, dt);
                    ui->BeginFrame();
                    const auto result = preset.DrawUI(engine, *ui, 1.0f / 60.0f);
                    if (!result.HasValue())
                        return std::unexpected(result.error);
                    ++*uiFrames;
                    return {};
                }
                void Stop(COM::IEngine&) {
                    preset.ClearFallback();
                    ui.reset();
                }
            };
            int        uiFrames = 0;
            const auto result =
                App(EngineConfig {.appName = "Diagnostic popup", .width = 320, .height = 240, .validationMode = ValidationMode::Off, .headless = true})
                    .AddLayer(Layer {.uiFrames = &uiFrames})
                    .Run();
            ExpectTrue(result.has_value());
            ExpectEq(uiFrames, 2);
            return {};
        }
    };
};
} // namespace
