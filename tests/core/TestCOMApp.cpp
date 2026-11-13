// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/COM/App.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/IState.hpp>
#include <Zahlen/Core/ErrorCode.hpp>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ZHLN::Test {
namespace {
struct Layer {
    int id;
    std::vector<std::string>* events;
    std::unique_ptr<int> moveOnly = std::make_unique<int>(1);
    void Start(COM::IEngine&) { events->push_back("start" + std::to_string(id)); }
    void Tick(COM::IEngine&, float dt) {
        ExpectTrue(dt >= 0);
        events->push_back("tick" + std::to_string(id));
    }
    void Stop(COM::IEngine&) { events->push_back("stop" + std::to_string(id)); }
};
} // namespace

void test_com_app_runs_layers_and_update() {
    std::vector<std::string> events;
    COM::App app;
    app.Title("COM App test").Resolution(320, 240).Headless(true).Validation(false)
        .VSync(false).MeshShading(false).MaxFPS(0)
        .PhysicsLimits(100, 200, 200, 8 * 1024 * 1024)
        .AddLayer(Layer{1, &events}).AddLayer(Layer{2, &events})
        .OnStart([&](COM::IEngine& engine) {
            ExpectTrue(engine.Entities()->SingletonEntity("ZHLN::Components::MainCameraTagComponent").HasValue());
            ExpectTrue(engine.Entities()->SingletonEntity("ZHLN::Components::CameraComponent").HasValue());
            ExpectTrue(engine.Entities()->SingletonEntity("ZHLN::Components::InputStateComponent").HasValue());
            ExpectTrue(engine.Entities()->SingletonEntity("ZHLN::Components::DebugSettingsComponent").HasValue());
            ExpectTrue(engine.State() != nullptr && engine.State()->CullingStats().EnableCulling);
            events.push_back("onstart");
        });
    int updates = 0;
    auto result = app.Run([&](COM::IEngine& engine, float dt) {
        ExpectTrue(dt >= 0);
        events.push_back("update");
        if (++updates == 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        } else {
            ExpectTrue(dt == 0.25f);
            engine.RequestClose();
        }
    });
    // Like TestCOMBoundary, permit a real, nonempty initialization error on
    // machines without a Vulkan device; no callbacks should have run then.
    if (!result) {
        ExpectTrue(result.error() != ErrorCode {});
        ExpectTrue(events.empty());
        return;
    }
    const std::vector<std::string> expected {"start1", "start2", "onstart", "tick1", "tick2", "update", "tick1", "tick2", "update", "stop2", "stop1"};
    ExpectTrue(events == expected);
}
} // namespace ZHLN::Test
