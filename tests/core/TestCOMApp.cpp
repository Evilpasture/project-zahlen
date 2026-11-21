// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/App.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/IPlugin.hpp>
#include <Zahlen/COM/IState.hpp>
#include <Zahlen/Core/COM/Reflect.hpp>
#include <Zahlen/Core/ErrorCode.hpp>
#include <chrono>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ZHLN::Test {

static_assert(COM::AuditInterfaceABI<COM::IAppLayer>());

namespace {
struct Layer {
    int                       id;
    std::vector<std::string>* events;
    std::unique_ptr<int>      moveOnly = std::make_unique<int>(1);
    void Start(COM::IEngine&) { events->push_back("start" + std::to_string(id)); }
    void Tick(COM::IEngine&, float dt) {
        ExpectTrue(dt >= 0);
        events->push_back("tick" + std::to_string(id));
    }
    void Stop(COM::IEngine&) { events->push_back("stop" + std::to_string(id)); }
};

class InlinePlugin final : public COM::Implements<InlinePlugin, COM::IAppLayer> {
  public:
    InlinePlugin(std::vector<std::string>* events, bool* destroyed) noexcept
        : _events(events), _destroyed(destroyed) {}
    ~InlinePlugin() noexcept {
        if (_destroyed != nullptr) *_destroyed = true;
    }

    auto OnStart(COM::IEngine*) noexcept -> COM::Result<void> override {
        _events->push_back("plugin:start");
        return {};
    }
    auto OnTick(COM::IEngine* e, float) noexcept -> COM::Result<void> override {
        _events->push_back("plugin:tick@" + std::to_string(e->CurrentFrame()));
        return {};
    }
    auto OnBeforeRender(COM::IEngine* e, float) noexcept -> COM::Result<void> override {
        _events->push_back("plugin:before@" + std::to_string(e->CurrentFrame()));
        return {};
    }
    auto OnAfterTick(COM::IEngine* e, float) noexcept -> COM::Result<void> override {
        _events->push_back("plugin:after@" + std::to_string(e->CurrentFrame()));
        return {};
    }
    void OnStop(COM::IEngine*) noexcept override { _events->push_back("plugin:stop"); }

  private:
    std::vector<std::string>* _events    = nullptr;
    bool*                     _destroyed = nullptr;
};
} // namespace

void test_com_app_runs_layers_and_update() {
    std::vector<std::string> events;
    App app;
    app.Title("Unified App test")
        .Resolution(320, 240)
        .Headless(true)
        .Validation(false)
        .VSync(false)
        .MeshShading(false)
        .MaxFPS(0)
        .LogLevel(LogLevel::Quiet)
        .PhysicsLimits(100, 200, 200, 8 * 1024 * 1024)
        .AddLayer(Layer {1, &events})
        .AddLayer(Layer {2, &events})
        .OnStart([&](COM::IEngine& engine) {
            ExpectTrue(engine.Entities()->SingletonEntity("ZHLN::Components::MainCameraTagComponent").HasValue());
            ExpectTrue(engine.Entities()->SingletonEntity("ZHLN::Components::CameraComponent").HasValue());
            ExpectFalse(engine.Entities()->SingletonEntity("ZHLN::Components::InputStateComponent").HasValue());
            ExpectFalse(engine.Entities()->SingletonEntity("ZHLN::Components::DebugSettingsComponent").HasValue());
            ExpectTrue(engine.State() != nullptr && engine.State()->CullingStats().EnableCulling);
            ExpectEq(engine.State()->PhysicsDrawMode(), 0);
            events.push_back("onstart");
        });
    ExpectEq(app.Title(), std::string_view {"Unified App test"});
    ExpectEq(app.Width(), uint32_t {320});
    ExpectEq(app.Height(), uint32_t {240});
    ExpectTrue(app.Headless());

    int  updates = 0;
    auto result  = app.Run([&](COM::IEngine& engine, float dt) {
        ExpectTrue(dt >= 0);
        events.push_back("update");
        if (++updates == 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        } else {
            ExpectTrue(dt <= 0.1001f && dt >= 0.099f);
            engine.RequestClose();
        }
    });
    if (!result) {
        ExpectTrue(result.error() != ErrorCode {});
        ExpectTrue(events.empty());
        return;
    }
    const std::vector<std::string> expected {
        "start1", "start2", "onstart", "tick1", "tick2", "update", "tick1", "tick2", "update", "stop2", "stop1"
    };
    ExpectTrue(events == expected);
}

void test_com_app_max_frames_after_tick_and_iapp_layer() {
    struct PostTickLayer {
        int                       id;
        std::vector<std::string>* events;
        bool                      closeOnFirstAfterTick = false;
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
    bool                     pluginDestroyed = false;
    auto                     plugin          = COM::Ref<COM::IAppLayer>::Adopt(new InlinePlugin(&events, &pluginDestroyed));
    ExpectTrue(plugin.As<COM::IObject>().Get() != nullptr);
    ExpectTrue(plugin.As<COM::IAppLayer>().Get() != nullptr);

    const auto result = App()
                            .Title("Unified App frame limit")
                            .Resolution(64, 48)
                            .Headless(true)
                            .Validation(false)
                            .VSync(false)
                            .MeshShading(false)
                            .LogLevel(LogLevel::Quiet)
                            .MaxFrames(2)
                            .AddLayer(PostTickLayer {1, &events})
                            .AddPlugin(std::move(plugin))
                            .AddLayer(PostTickLayer {2, &events})
                            .AfterTick([&](COM::IEngine& engine, float dt) {
                                ExpectTrue(dt >= 0.0f && dt <= 0.1f);
                                events.push_back("appAfter@" + std::to_string(engine.CurrentFrame()));
                            })
                            .Run([&](COM::IEngine& engine) {
                                events.push_back("update@" + std::to_string(engine.CurrentFrame()));
                            });
    ExpectTrue(pluginDestroyed);
    if (!result) {
        ExpectTrue(result.error() != ErrorCode {});
        ExpectTrue(events.empty());
        return;
    }
    const std::vector<std::string> expected {
        "start1",        "plugin:start",   "start2",
        "tick1@0",       "plugin:tick@0",  "tick2@0",  "update@0", "plugin:before@0", "after1@1", "plugin:after@1", "after2@1", "appAfter@1",
        "tick1@1",       "plugin:tick@1",  "tick2@1",  "update@1", "plugin:before@1", "after1@2", "plugin:after@2", "after2@2", "appAfter@2",
        "stop2",         "plugin:stop",    "stop1",
    };
    ExpectTrue(events == expected);

    // RequestClose inside a layer AfterTick skips later post-tick hooks and exits.
    events.clear();
    const auto earlyClose = App()
                                .Title("Unified App early post-tick close")
                                .Resolution(64, 48)
                                .Headless(true)
                                .Validation(false)
                                .VSync(false)
                                .MeshShading(false)
                                .LogLevel(LogLevel::Quiet)
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
