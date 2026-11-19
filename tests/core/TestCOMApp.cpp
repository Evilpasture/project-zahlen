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
            ExpectFalse(engine.Entities()->SingletonEntity("ZHLN::Components::InputStateComponent").HasValue());
            ExpectFalse(engine.Entities()->SingletonEntity("ZHLN::Components::DebugSettingsComponent").HasValue());
            ExpectTrue(engine.State() != nullptr && engine.State()->CullingStats().EnableCulling);
            ExpectEq(engine.State()->PhysicsDrawMode(), 0);
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

void test_com_app_max_frames_and_after_tick() {
    struct PostTickLayer {
        int id;
        std::vector<std::string>* events;
        bool closeOnFirstAfterTick = false;
        void Start(COM::IEngine&) { events->push_back("start" + std::to_string(id)); }
        void Tick(COM::IEngine& engine, float) {
            events->push_back("tick" + std::to_string(id) + "@" + std::to_string(engine.CurrentFrame()));
        }
        void AfterTick(COM::IEngine& engine) {
            events->push_back("after" + std::to_string(id) + "@" + std::to_string(engine.CurrentFrame()));
            if (closeOnFirstAfterTick) engine.RequestClose();
        }
        void Stop(COM::IEngine&) { events->push_back("stop" + std::to_string(id)); }
    };

    std::vector<std::string> events;
    const auto result = COM::App()
        .Title("COM App frame limit")
        .Resolution(64, 48)
        .Headless(true)
        .Validation(false)
        .VSync(false)
        .MeshShading(false)
        .MaxFrames(2)
        .AddLayer(PostTickLayer {1, &events})
        .AddLayer(PostTickLayer {2, &events})
        .AfterTick([&](COM::IEngine& engine, float dt) {
            ExpectTrue(dt >= 0.0f && dt <= 0.25f);
            events.push_back("appAfter@" + std::to_string(engine.CurrentFrame()));
        })
        .Run([&](COM::IEngine& engine) {
            events.push_back("update@" + std::to_string(engine.CurrentFrame()));
        });
    if (!result) {
        ExpectTrue(result.error() != ErrorCode {});
        ExpectTrue(events.empty());
        return;
    }
    const std::vector<std::string> expected {
        "start1", "start2",
        "tick1@0", "tick2@0", "update@0", "after1@1", "after2@1", "appAfter@1",
        "tick1@1", "tick2@1", "update@1", "after1@2", "after2@2", "appAfter@2",
        "stop2", "stop1",
    };
    ExpectTrue(events == expected);

    // RequestClose inside a layer AfterTick skips later post-tick hooks and
    // exits before starting another frame.
    events.clear();
    const auto earlyClose = COM::App()
        .Title("COM App early post-tick close")
        .Resolution(64, 48)
        .Headless(true)
        .Validation(false)
        .VSync(false)
        .MeshShading(false)
        .MaxFrames(5)
        .AddLayer(PostTickLayer {1, &events, true})
        .AddLayer(PostTickLayer {2, &events, false})
        .AfterTick([&](COM::IEngine&) { events.push_back("appAfter"); })
        .Run();
    if (!ExpectTrue(earlyClose.has_value())) return;
    const std::vector<std::string> expectedEarly {
        "start1", "start2", "tick1@0", "tick2@0", "after1@1", "stop2", "stop1",
    };
    ExpectTrue(events == expectedEarly);
}
} // namespace ZHLN::Test
