// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/App.hpp>
#include <Zahlen/COM/IActionContext.hpp>
#include <Zahlen/COM/IAssetLibrary.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math/Angles.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/COM/ILogger.hpp>
#include <Zahlen/COM/LoggerView.hpp>


struct Game {
    void Start(ZHLN::COM::IEngine& engine) {
        auto& entities = *engine.Entities();
        const auto cameras = entities.EntitiesWith<ZHLN::Components::CameraComponent>();
        if (!cameras.empty()) {
            entities.Patch<ZHLN::Components::CameraComponent>(cameras.front(), [](auto& camera) {
                camera.camera.position = {0.0f, 3.0f, 11.0f};
                camera.camera.yaw = ZHLN::DegF{-90.0f};
                camera.camera.pitch = ZHLN::DegF{-10.0f};
            });
        }

        auto graphics = engine.Graphics()->Settings();
        graphics.environment.ambientExposure = 3.0f;
        graphics.environment.skyZenith = {0.08f, 0.18f, 0.34f, 1.0f};
        graphics.environment.skyHorizon = {0.40f, 0.55f, 0.72f, 1.0f};
        engine.Graphics()->ApplySettings(graphics);

        const auto floor = engine.Assets()->SpawnPlane(
            30.0f, {0.25f, 0.29f, 0.34f, 1.0f},
            {.createPhysics = true, .isStaticPhysics = true});
        const auto box = engine.Assets()->SpawnBox(
            {0.55f, 0.55f, 0.55f},
            {.position = {0.0, 3.0, 0.0},
             .createPhysics = true, .isStaticPhysics = false,
             .color = {0.35f, 0.6f, 0.9f, 1.0f}});
        const auto sun = entities.Spawn(
            ZHLN::Components::SunTagComponent{},
            ZHLN::Components::LightComponent{
                .type = ZHLN::LightType::Sun,
                .color = ZHLN::FVec3{1.0f, 0.95f, 0.85f},
                .intensity = 32.0f,
                .direction = ZHLN::FVec3{0.45f, 1.0f, 0.30f}.Normalized(),
                .castsShadow = true});
        if (floor == ZHLN::Entity::Null() || box == ZHLN::Entity::Null() ||
            sun == ZHLN::Entity::Null()) {
            ZHLN::COM::LoggerView{engine.Logger()}.Error("Could not create the example scene");
            engine.RequestClose();
        }
    }

    void Tick(ZHLN::COM::IEngine& engine, float dt) {
        (void)dt; // seconds since the previous frame; use for time-based logic
        if (engine.Actions()->KeyPressed(ZHLN::KeyCode::Escape, ZHLN::kPrimarySeat)) {
            engine.RequestClose();
        }
    }
};

int main(int argc, char* argv[]) {
    return ZHLN::App()
        .Title("My First Game")
        .Resolution(1280, 720)
        .AddLayer(Game{})
        .Main(argc, argv);
}
